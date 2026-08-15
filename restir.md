# ReSTIR GI in vkm

Living document for the ReSTIR GI implementation: what the technique is, the staged plan,
current status, remaining TODOs, and the reading list. Updated at the end of every phase.

**Status:** Phases 1-8 complete. ReSTIR GI runs as the second, runtime-selectable technique in
the `gi` sample — one traced sample per pixel, temporal reuse (confidence cap 20 / age cap 32),
spatial reuse with the reconnection Jacobian and a bias-verified denominator, a fullscreen
lighting pass honouring the §5 composite contract, and the roughness MIS blend. Every sub-step
is gated against the Phase 6 reference on the Cornell fixture. What remains is Phase 9 (denoiser,
firefly controls, performance) plus the recorded honest limits: moving-camera temporal is
verified by eye only, the traced passes still see no directional light (environment/emissive
only, §12), and the final visibility ray is deferred behind its plumbed flag.

---

## 1. Goal and decisions

Implement **ReSTIR GI** (Ouyang et al. 2021, *ReSTIR GI: Path Resampling for Real-Time Path
Tracing*) as vkm's **high-end** GI tier.

**ReSTIR GI is not vkm's default GI.** It is one of at least two interchangeable techniques behind
a common interface. A **low-spec tier** must also exist for ray-tracing-less and mobile targets.
See §5 for that architecture — it is a first-class requirement, not a fallback bolted on later.

| Decision | Choice |
|---|---|
| Secondary ray tracing (high tier) | Hardware ray tracing, **inline ray query in compute** (not RT pipelines) |
| Backends (high tier) | Vulkan **and** Metal, developed in parallel |
| Metal shader path | Existing `dxc` → SPIR-V → SPIRV-Cross → MSL chain (**no** Metal Shader Converter) — verified, §4.1 |
| Geometry in acceleration structures | **Triangles only** (see §4.2) |
| Low-spec tier | Required; runs everywhere including WebGPU and mobile. Technique TBD, see §5 |
| Quality bar | Production: denoiser, motion vectors, performance work all in scope |

### Platform matrix

| Combination | High tier (ReSTIR GI) | Low-spec tier | Why |
|---|---|---|---|
| Windows Vulkan | Yes | Yes | `VK_KHR_ray_query` |
| Linux Vulkan | Yes | Yes | `VK_KHR_ray_query`; lavapipe covers CI (needs Mesa ≥ 24.1) |
| macOS Metal 4 | Yes | Yes | `metal::raytracing` intersection queries |
| macOS Vulkan | **No** | Yes | MoltenVK implements neither `VK_KHR_ray_query` nor `VK_KHR_acceleration_structure` (confirmed, §4.3) |
| iOS Metal 4 | Unlikely / untested | **Primary target** | RT API exists but hardware RT is A17+; low-spec tier is the realistic path |
| WASM WebGPU | **No** | **Primary target** | WebGPU has no ray tracing (blocked on bindless in the WG) |

**Consequence:** macOS ray tracing is reachable *only* through the Metal backend. The
Vulkan/Metal cross-check vkm normally relies on for validation is unavailable on a single macOS
machine — Vulkan RT can only be verified on Windows/Linux or in CI.

---

## 2. What ReSTIR GI is (orientation)

One idea applied recursively. Read this before the phase plan; the phases map onto these concepts.

**The problem.** One-bounce indirect lighting at 1 sample per pixel is unusably noisy. Trace one
ray into the hemisphere, ask how bright what it hit is, and the answer varies wildly between
neighbouring pixels.

**RIS (Resampled Importance Sampling), Talbot 2005.** Draw `M` cheap candidates from an
easy-to-sample distribution `p`, then resample **one** proportional to a *target function* `p̂`
you can evaluate but not sample (ideally the full integrand). You get a sample distributed
approximately as `p̂` without needing its normalization or inverse CDF.

**Reservoir sampling.** The "pick one of M proportionally" step runs in O(1) memory, streaming:

```
struct Reservoir {
    Sample Y;      // selected sample
    float  W_Y;    // unbiased contribution weight — stands in for 1/p(Y)
    float  w_sum;  // running sum of resampling weights
    float  c;      // confidence weight (called M in the 2020 paper)
};

update(X_i, w_i, c_i):
    w_sum += w_i
    c     += c_i
    if (rand() < w_i / w_sum) Y = X_i
// after the stream:  W_Y = w_sum / p̂(Y);  c = min(c, c_cap)
```

`W_Y` is the single most important concept. It replaces `1/p(Y)` in the estimator even though
`p(Y)` is intractable — it is not a function of `Y` but a random variable with
`E[W_Y | Y] = 1/p(Y)`. The estimator is just `⟨I⟩ = f(Y) · W_Y`.

> **Trap:** you may **not** substitute the chosen candidate's own `W_{X_s}` for `W_Y`. That is
> biased, and it is the most common first bug.

**ReSTIR DI, Bitterli 2020.** Reservoirs from *other* pixels are also valid candidate streams:
- **Temporal reuse** — merge the motion-vector-matched reservoir from last frame. Confidence
  grows every frame, so each pixel effectively chooses among hundreds of candidates while
  tracing one new ray. This is where the large quality win comes from.
- **Spatial reuse** — merge a few random nearby pixels' reservoirs.
- Both merges must be **MIS-weighted** with `Σᵢ mᵢ(x) = 1`. `mᵢ = 1/M` is legal *only* for iid
  candidates each covering `supp(p̂)` — not true for spatial/temporal reuse, where you need the
  **generalized balance heuristic** `mᵢ(x) = cᵢ·p̂ᵢ(x) / Σⱼ cⱼ·p̂ⱼ(x)`.
- Confidence must be **capped** (start at 20) or new samples get exponentially negligible weight
  and the image stops responding to change.

**ReSTIR GI, Ouyang 2021 — the target.** A "sample" becomes a **secondary hit point** treated as
a virtual light:

```
struct GISample {
    float3 x_v, n_v;  // visible point (primary hit) + normal — usually implicit from the G-buffer
    float3 x_s, n_s;  // sample point (secondary hit) + normal
    float3 L_o;       // outgoing radiance at x_s toward x_v
};
```

Target function: `p̂(X) = luminance( f_s(x_s → x_1 → x_0) · cos θ_{x_1} · L_o )`.

Per frame, per pixel: **sample generation** (one ray) → **temporal resampling** → **spatial
resampling** → **shading**.

The wrinkle versus DI: reusing a neighbour's sample point means **reconnecting** your surface
point to *their* sample point, changing domains. You must multiply by a **Jacobian**
(paper Eq. 11):

```
|J| = ( cos θ_{x_s → y_1} / cos θ_{x_s → x_1} ) · ( ‖x_1 − x_s‖² / ‖y_1 − x_s‖² )
```

Omit it and the GI is wrong by the local density-scaling factor — easily 10×. You must also
**re-trace a visibility ray** to the reused sample point, or light leaks through walls. Guard the
Jacobian for NaN/Inf (return 0) and reject extreme values — the `1/d²` term explodes as
`y₁ → x_s`, which is the classic firefly source.

**GRIS, Lin 2022** provides the theory: reuse is a **shift mapping** between path spaces, each
with a Jacobian, and MIS weights come from a generalized balance heuristic. Not needed to get
pixels on screen; essential to reason about *why* an image is biased, and the vocabulary every
later paper speaks.

### ReSTIR GI is deliberately biased — know this up front

`L_o` is cached for the direction `x_s → x_1` and reused unchanged for `x_s → y₁`. That is only
exact for a Lambertian `x_s`. This is not a bug fixable with a Jacobian — it is a bandwidth/
register trade the paper makes knowingly, and it means **ReSTIR GI cannot produce faithful
results when `x_s` is specular**. The accepted mitigation is MIS against a fresh BRDF-sampled
path on low-roughness surfaces. If unbiasedness is required, the target is ReSTIR PT / GRIS with
the hybrid shift — substantially more work (random replay, primary-sample-space
parameterization, lobe tags, reconnection criteria).

This affects the verification plan: a converged ReSTIR GI image is **not** expected to match the
reference exactly on specular secondary hits. Validate mean-matching on diffuse-only test scenes
first, where the approximation is exact.

---

## 3. Engine starting point

### Reusable today

| Building block | Where |
|---|---|
| Compute pipelines: create + dispatch + push constants, all backends | `resources/Pipelines/Engine/scene_cull.json`, `resources/Shaders/scene_cull.hlsl`, `src/vkm/renderer/scene/scene.cpp:566-585` |
| Compute subgraph in the frame | `VkmRenderComputeSubGraph`, `include/vkm/renderer/backend/common/render_graph.h:92` |
| Bindless storage buffers (4096 slots) + geometry pools readable in-shader | `include/vkm/renderer/backend/common/bindless_resource_manager.h`, `include/vkm/shaders/vkm_bindless.hlsli` |
| Per-frame constants at set 1, readable from every stage | `include/vkm/renderer/backend/common/frame_constants.h:59` |
| Storage-texture creation (`AllowShaderWrite`) on all backends | `vulkan_util.cpp:180`, `metal_texture.mm:22`, `webgpu_texture.cpp:19` |
| MRT up to 8 colour attachments (unused so far) | `include/vkm/renderer/backend/common/render_pass.h:12` |
| glTF scene, GPU-driven indirect draws, frustum culling | `include/vkm/renderer/scene/scene.h`, `gltf_importer.h` |
| Offscreen texture creation + resize-safe deferred reclaim | `src/samples/model_viewer/main.cpp:522-561` |
| HLSL + offline `vkm-compiler` + PSO-JSON permutations | `src/tools/vkm-compiler/main.cpp`, `pipeline_state.h:218` |
| Camera + controllers, doctest suite, per-backend run scripts | `include/vkm/renderer/camera.h`, `scripts/run_tests.py`, `scripts/run_sample.py` |

### Missing — must be built

**This is the baseline as of 2026-07-30, kept as written so later entries can be read against it.**
Items 3-7 and 10 have since been built, and item 2 by half -- sets 2 and 3 exist on all three
backends, but the Vulkan bindless manager still registers only `SAMPLED_IMAGE`, so a compute shader
still cannot write a texture through set 0. Items 1, 8 and 9 are untouched. §8's checkboxes and
§13's log are the current state.

1. **All ray tracing.** Zero acceleration structures, ray query, RT pipelines, or shader binding
   tables. Vulkan enables no RT extensions (`vulkan_driver.cpp:466-544`).
2. **Per-pass resource binding.** Sets 2/3 reserved but unimplemented (`frame_constants.h:34`,
   `TODO.md:8`). No bindless storage-image binding on Vulkan
   (`vulkan_bindless_resource_manager.cpp:53-86`) — a compute shader cannot write a texture today.
3. **Texture barriers.** Only `barrierIndirectArgumentBuffer` exists (`command_buffer.h:120`).
4. **G-buffer.** Nothing uses MRT; rendering is single-pass forward into the backbuffer.
5. **Previous-frame matrices, frame index, viewport size, jitter** in `VkmFrameConstants`
   (272 B today), plus **3-component motion vectors**.
6. **Fullscreen-pass building block** + live tone mapping (`tonemap.frag` is dead GLSL, `TODO.md:14`).
7. **PBR BRDF evaluation.** Material data imported but never read (`model_viewer.hlsl:219-223`).
8. **Light representation** beyond one directional vector; emissive sampling.
9. **Ping-pong / history resource management.**
10. **glTF textures** — importer reads material factors only (`TODO.md:34`).

The render graph is a flat ordered callback list, not a scheduler — `compile()` is an empty stub
(`render_graph.cpp:76-84`), one command buffer on one queue, no transient/aliased resources.

---

## 4. Toolchain findings

### 4.1 Spike result: PASS — the existing chain can carry inline ray query to Metal

(Toolchain capability only. vkm's own Metal path additionally needs the AS registered as a set-0
binding before it compiles — see §4.4.)

Run 2026-07-30 on macOS, Xcode 26.3, using the Vulkan SDK 1.4.341.1 binaries on `PATH`
(`dxc` 1.10/1.9.0.5180, `spirv-cross` `vulkan-sdk-1.4.335.0-28-ga0fba56c`). Note the system
spirv-cross is slightly *older* than the revision this repo pins (`vulkan-sdk-1.4.350.0`), so the
result is conservative — the pinned newer revision will also work.

A triangle-only inline `RayQuery<>` compute shader writing to an `RWStructuredBuffer<float>`:

```
dxc -spirv -T cs_6_5 -E CSMain -fspv-target-env=vulkan1.2 \
    -fspv-extension=SPV_KHR_ray_query -Fo out.spv in.hlsl     # exit 0
spirv-val out.spv                                              # exit 0
spirv-cross --msl --msl-version 30000 \
    --msl-argument-buffers --msl-argument-buffer-tier 2 out.spv --output out.metal   # exit 0
xcrun -sdk macosx metal -std=metal3.0 -c out.metal -o out.air  # exit 0
xcrun -sdk macosx metallib out.air -o out.metallib             # exit 0
```

Results:

- SPIR-V declares exactly `OpCapability RayQueryKHR` + `OpExtension "SPV_KHR_ray_query"`, and
  validates.
- **Correction to earlier research:** `SPV_KHR_ray_tracing` is **not** emitted. Inline-only ray
  query does not pull it in, so that flag is unnecessary.
- SPIRV-Cross emits correct MSL:
  `raytracing::intersection_query<raytracing::instancing, raytracing::triangle_data>` with
  `.reset()` / `.next()` / `.get_committed_intersection_type()` / `.get_committed_distance()`, and
  `raytracing::acceleration_structure<raytracing::instancing>`, guarded by
  `#if __METAL_VERSION__ >= 230`.
- **The critical result:** with Tier-2 argument buffers the acceleration structure lands *inside*
  the descriptor-set struct as
  `raytracing::acceleration_structure<raytracing::instancing> g_Scene [[id(0)]]` — directly
  compatible with vkm's hand-pinned `[[id(N)]]` bindless layout. Both the plain-binding and
  argument-buffer variants compile to metallib.

**Conclusion: no new shader toolchain, no Metal Shader Converter, no Slang.** vkm-compiler needs
only a per-PSO SM 6.5 profile and the ray-query SPIR-V flag. (`metal-shaderconverter` *is* installed
on this machine at `/usr/local/bin`, but is not needed — and §4.2 explains why adopting it would
cost more than it buys.)

### 4.2 The envelope: triangles only, and the failure is silent

Procedural/AABB geometry is **not** usable, and the failure mode is worse than expected. A shader
using `CANDIDATE_PROCEDURAL_PRIMITIVE` / `CommitProceduralPrimitiveHit()` compiles cleanly all the
way to `.air` — but spirv-cross hardcodes the query tags to
`intersection_query<instancing, triangle_data>` while still emitting
`commit_bounding_box_intersection()`. The query is never typed for bounding-box data, so this is
wrong **at runtime with no diagnostic**, not a compile error.

Also unsupported (these do throw): `OpConvertUToAccelerationStructureKHR` (no AS-from-uint64, so no
pointer-style bindless AS) and `...ShaderBindingTableRecordOffsetKHR`. No RT pipelines to MSL.

**Therefore: triangle geometry only, one TLAS bound as a singleton.** Do not attempt procedural
primitives on the Metal path, and add a comment saying why at the declaration site.

### 4.3 MoltenVK confirmed unusable for RT

Grepping this machine's `~/VulkanSDK/vulkaninfo.txt` for `ray_query`, `ray_tracing`,
`acceleration_structure`, and `deferred_host` returns **zero** matches — empirical confirmation
that macOS GI must go through the Metal backend.
Upstream: [MoltenVK#1956](https://github.com/KhronosGroup/MoltenVK/issues/1956) (open since
2023-06); maintainer in [#2079](https://github.com/KhronosGroup/MoltenVK/discussions/2079):
*"Ray tracing is a large project, and we are currently talking with customers about funding its
development."*

### 4.4 The Metal integration blocker, already diagnosed

Compiling a ray-query PSO for the **metal** target through `vkm-compiler` currently fails:

```
vkm-compiler: spirv-cross MSL generation failed for rq_test.hlsl: Argument buffer resource base
type could not be determined. When padding argument buffer elements, all descriptor set resources
must be supplied with a base type by the app.
```

**This is not a toolchain limitation** — §4.1 proved spirv-cross lowers ray query to MSL and places
an acceleration structure inside a Tier-2 argument buffer. It is vkm's argument-buffer *padding*
requirement: `mslOptions.pad_argument_buffer_resources = true` makes spirv-cross walk **every** set-0
binding to synthesize padding members, and it needs an `add_msl_resource_binding` entry supplying a
base type for each one (`main.cpp:376-420`). The acceleration structure has no such entry yet.

Fixing it means giving the AS a real set-0 binding — extending `VkmBindlessSingletonBuffer`, all
three backend managers, `vkm_bindless.hlsli`, and the `add_msl_resource_binding` list. That is
squarely **Phase 5**, and doing it now would add a singleton binding with no resource able to
populate it. So the Metal compile path stays unproven inside vkm until Phase 5, with the failure
mode already understood and the fix already located.

### 4.5 Why SPIRV-Cross works (source evidence)

Verified against the pinned revision `vulkan-sdk-1.4.350.0` (`dependencies/bootstrap.json:103`):

- All 24 `OpRayQuery*KHR` opcodes have MSL codegen (`spirv_msl.cpp:10423-10510`), lowering to
  `.reset()` / `.next()` / `.get_committed_distance()` / `.commit_triangle_intersection()`.
- `SPIRType::RayQuery` → `raytracing::intersection_query<raytracing::instancing, raytracing::triangle_data>`
  (line 17125).
- `SPIRType::AccelerationStructure` → `raytracing::acceleration_structure<raytracing::instancing>`,
  needs MSL ≥ 2.3; vkm-compiler already sets `set_msl_version(3, 0)`.
- Acceleration structures work **inside Tier-2 argument buffers** (line 20332) and as
  runtime-sized `spvDescriptorArray<>` — compatible with vkm's bindless model. They use the
  `msl_buffer` index category (line 16043).

SPIRV-Cross's README documents none of this, which is why it is widely believed unsupported.
Maintainer confirmation: [SPIRV-Cross#2065](https://github.com/KhronosGroup/SPIRV-Cross/issues/2065)
— *"There is some support for ray queries, but not for ray-tracing shaders."*

**Hard limits of that support — these define the safe envelope:**
- `OpConvertUToAccelerationStructureKHR` throws → no AS-from-uint64, so no pointer-style bindless AS.
- `...ShaderBindingTableRecordOffsetKHR` throws.
- The `intersection_query<>` tags are hardcoded to `instancing, triangle_data`, so procedural/AABB
  geometry is likely broken even though `commit_bounding_box_intersection()` is emitted.
- **Conclusion: triangle-only inline ray tracing.** No RT pipelines to MSL, and not soon.

### Metal Shader Converter is not needed, and would cost more than it buys

`metal-shaderconverter` takes **DXIL in, `.metallib` out** (no SPIR-V input, no `.air` emission).
Standalone download, not in Xcode; macOS 13+/Xcode 15+ or Windows 10+/VS2019+; **no Linux build**
(a CI problem). Its RT support is genuinely broader — DXR 1.1 inline `RayQuery` *and* full DXR 1.0
pipelines with shader-binding-table emulation via `IRShaderIdentifier`.

**But its binding model is the disqualifier, not its RT support.** It imposes its own top-level
argument-buffer / root-signature-descriptor-table model, conflicting with the hand-pinned Tier-2
`[[id(N)]]` layout that vkm-compiler's `add_msl_resource_binding` calls and
`bindless_resource_manager.h` depend on. Adopting it for RT shaders only would mean maintaining
**two incompatible Metal binding ABIs** — to buy DXR 1.0 pipelines that inline ray query does not
need.

Secondary problem: DXIL signing. `dxil.dll`/`libdxil.so` is Windows/Linux-x86_64 only, with **no
macOS build**, and vkm builds dxc from source on macOS (`v1.9.2602.24`) — so only *unsigned* DXIL
is obtainable there. Evidence suggests MSC accepts unsigned DXIL
([DXC#6057](https://github.com/microsoft/DirectXShaderCompiler/issues/6057) was closed over
warning noise, not rejection) but Apple documents it nowhere.

**Decision: do not adopt Metal Shader Converter.** Revisit only if full DXR 1.0 pipelines become
necessary. Fallback if the SPIRV-Cross path fails the spike: **Slang**, whose compatibility table
says `Metal: No` for ray tracing but is stale — `slang-emit-metal.cpp` emits
`intersection_query`/`acceleration_structure`, the capability system declares
`alias rayquery = GL_EXT_ray_query | _sm_6_3 | metal`, and v2026.14.1 (2026-07-30) contains
"[Metal] Fix RayQuery TriangleFrontFace emission". It also has `-target metallib`. Cost is a
front-end migration off dxc.

### CI: lavapipe works, but the runner is too old

Mesa's `docs/features.txt` lists `VK_KHR_ray_query  DONE (anv/gfx12.5+, lvp, radv/gfx10.3+, tu/a740+, vn)`,
landed Mesa 24.1 (Apr 2024). vkm's Vulkan CI job runs `ubuntu-22.04`
(`.github/workflows/ubuntu.yml:27`), whose `mesa-vulkan-drivers` is 23.2.1 → **no ray query**.
Bumping the runner also allows un-pinning `dxc-linux` from the GLIBC-2.34 release.

### dxc flags

- `RayQuery` requires **SM 6.5** → `-T cs_6_5`. Implemented as a per-stage opt-in
  (`"ray_query": true` in the PSO JSON → `VkmShaderStageDescriptor::requiresRayQuery`) so the
  baseline stays at `cs_6_0` and one ray-tracing shader does not raise the bar engine-wide.
- Add **`-fspv-target-env=vulkan1.2`** for those stages. Not cosmetic: without it dxc emits a
  **SPIR-V 1.0** module, with it **SPIR-V 1.5** — the version Vulkan 1.2 mandates and the one the RT
  extensions are written against. The 1.0 module does validate, but no driver exposing `rayQuery`
  predates Vulkan 1.2, so emitting 1.0 buys nothing and asks drivers to accept an odd combination.
- **Do NOT pass `-fspv-extension`.** ⚠ It is a *whitelist*, not an "also allow" switch. Naming
  `SPV_KHR_ray_query` alone makes dxc **reject vkm's bindless unsized descriptor arrays**:
  `error: SPIR-V extension 'SPV_EXT_descriptor_indexing' required for runtime array of resources but
  not permitted to use`. Verified against a shader mirroring vkm's set-0 layout. dxc emits both
  `SPV_KHR_ray_query` and `SPV_EXT_descriptor_indexing` unprompted, so the flag is unnecessary as
  well as actively harmful. (Earlier research recommended it; it is wrong for this engine.)
- `SPV_KHR_ray_tracing` is *not* emitted and not needed for inline-only ray query.
- Known quirk: *storing* a `RayQuery<>` value fails
  ([DXC#4221](https://github.com/microsoft/DirectXShaderCompiler/issues/4221)) — keep it a plain local.
- Metal side, one trap that does **not** apply to vkm but looks alarming in isolation: feeding a
  ray-query shader that uses unsized descriptor arrays through the `spirv-cross` **CLI** throws
  *"Runtime sized variables must be in device storage argument buffers."* vkm never hits this,
  because vkm-compiler pins every set-0 array with an explicit `count`
  (`resourceBinding.count = kVkmBindlessBufferCapacity`, `main.cpp:389-396`), so spirv-cross emits
  **sized** arrays and the runtime-array path is never taken. Only reach for
  `--msl-device-argument-buffer` if those pins are ever removed.

### Metal 4 acceleration structures

Metal 4 **folded the AS encoder into the compute encoder** — there is no
`MTL4AccelerationStructureCommandEncoder`. `MTL4ComputeCommandEncoder` (macOS 26+) carries
`build` / `refit` / `copyAndCompact` / `writeCompactedSize`. Use
`MTL4PrimitiveAccelerationStructureDescriptor` / `MTL4InstanceAccelerationStructureDescriptor`
(buffers typed as `MTL4BufferRange`). Apple recommends `MTLResidencySet` over per-encoder
`useResource:` for MTL4 AS builds — which fits vkm's existing residency-set code. Metal has **no
RT pipeline object**; RT is always compute-kernel-driven, so inline ray query is the natural fit.
One gap to confirm empirically: no documented `setAccelerationStructure` on `MTL4ArgumentTable`;
binding is inferred to go through `gpuResourceID`.

### Vulkan extensions and features

`VK_KHR_ray_query` + `VK_KHR_acceleration_structure` + `VK_KHR_deferred_host_operations` on a
Vulkan 1.2 base. Features: `VkPhysicalDeviceRayQueryFeaturesKHR::rayQuery`,
`VkPhysicalDeviceAccelerationStructureFeaturesKHR::accelerationStructure`, and
`VkPhysicalDeviceVulkan12Features::bufferDeviceAddress` (already enabled — `driver.h:54-59`).
Other `accelerationStructure*` members are optional.

---

## 5. GI as a tiered, interchangeable system

ReSTIR GI is the high tier only. A low-spec tier must run where there is no ray tracing (WebGPU,
macOS Vulkan/MoltenVK) and where there is RT hardware but not enough of it (mobile). This shapes
the plan in two ways worth stating plainly.

### The abstraction boundary

Do **not** try to make one algorithm scale down, and do **not** try to reuse ReSTIR's reservoirs
with a screen-space tracer. The two tiers are genuinely different algorithms; what they share is
infrastructure:

| Shared (build once) | Per-technique (owned by each tier) |
|---|---|
| G-buffer + previous-frame G-buffer | Its own passes and dispatch order |
| Motion vectors, frame constants, history management | Its own resources (reservoirs / probes / trace buffers) |
| Denoiser + history-confidence input | Its own quality/perf knobs |
| Composite + tone mapping | Its own capability requirements |
| Debug-view plumbing | |

Proposed contract: a technique produces an **indirect radiance texture** (plus optionally a
per-pixel confidence/variance channel for the denoiser), consumed by one shared composite pass.
That is a narrow enough interface that a third technique could be added later without touching the
first two, and wide enough that neither tier is crippled by it.

Selection should be a runtime choice gated on `VkmDriverCapabilityFlags::RayTracing` plus a user/
quality setting — not `#ifdef`, matching how `TextureUpload` and `BufferDeviceAddress` are already
handled.

### Why this reorders the plan favourably

The low-spec tier needs **none** of the ray-tracing work (Phase 1's toolchain, Phase 5's
acceleration structures). It needs only the shared infrastructure of Phases 2–3. That makes it:

- the **first GI that works end to end**, on all five platform combinations,
- the thing that **validates the technique interface with two consumers** before ReSTIR lands, and
- a hedge: if ReSTIR stalls, vkm still has GI.

So the low-spec tier is scheduled *before* ReSTIR GI in §8, even though ReSTIR is the headline
feature. This is a deliberate de-risking choice, not a deprioritization of ReSTIR.

### Low-spec technique: DECIDED — raster-updated dynamic probe volume + SSGI contact term

**Constraint set:** fully dynamic (no bake), no ray tracing, must run on mobile and WebGPU.

Options considered:

| Technique | Verdict |
|---|---|
| **Baked lightmaps / baked probes** | **Ruled out** — requires an offline bake; "fully dynamic" excludes it. Could still be added later as a separate third tier for shipping mobile titles. |
| **Voxel cone tracing** | **Ruled out** — fully dynamic means re-voxelizing every frame, and the memory footprint is a poor mobile fit. Also gives prefiltered cone queries rather than point samples. |
| **Screen-space GI alone** | **Ruled out as the only tier** — offscreen and occluded geometry simply miss, so indirect light changes as the camera turns. Kept as an *additive contact term*. |
| **Dynamic irradiance probe volume, raster-updated** | **Chosen.** The only option that is fully dynamic, view-independent, and mobile-affordable. |

**Design.** DDGI-style storage, but with probe updates driven by **rasterization instead of rays**:

- A camera-centred probe grid (clipmap if the world is large).
- Per probe, two octahedral maps packed into atlas textures: a small **irradiance** map (~8×8) and
  a larger **distance/moment** map (~16×16). The moment map is what enables the **Chebyshev
  visibility test** — the essential trick that stops light leaking through walls. Do not skip it;
  SH-only irradiance in a 3D texture leaks badly and there is no cheap fix afterwards.
- **Update:** round-robin a small subset of probes per frame. For each, rasterize a very low-res
  cube view of the scene, shade it, convert to octahedral, and blend into the atlas with
  hysteresis. Reuses the existing GPU-driven draw path (`VkmScene`'s cull/emit).
- **Sampling:** trilinear across the 8 nearest probes, weighted by the Chebyshev test, plus a
  normal bias and backface rejection.
- **SSGI** added on top as an additive contact term, for the fine near-field detail the probe grid
  is far too coarse to represent.

**Honest costs of "fully dynamic, no RT":**
- Rasterizing probe views re-renders geometry per probe, so **amortization is mandatory** — the
  per-frame probe budget is the main tuning knob.
- **Slow propagation:** with only a subset of probes refreshed per frame, moving lights take many
  frames to converge. This is the direct price of dropping both the bake and the rays, and it
  should be measured early rather than discovered late.
- Leaking is *reduced* by the Chebyshev test, not eliminated.

### Why this is not throwaway work

The probe volume's **storage and sampling are independent of how it is updated**. That gives two
things for free later:

1. **Swap the update mechanism, keep everything else.** Once Phase 5 lands acceleration structures,
   the same probe volume can be refreshed with rays instead of rasterized cube views — much better
   convergence, no change to storage or sampling. This is real DDGI at that point.
2. **It becomes the high tier's multi-bounce cache.** ReSTIR GI resamples *one* bounce; the radiance
   `L_o` at the secondary hit still has to come from somewhere. Querying the probe volume there is
   exactly what kajiya does with its world-space irradiance cache, and it is how multi-bounce GI
   gets added without a second ReSTIR pass.

So Phase 4 buys a shippable low tier *and* infrastructure the high tier wants anyway.

> **Do not merge SSGI into ReSTIR's reservoirs.** Screen-space tracing is a poor ReSTIR citizen
> because the *support* of `p̂` becomes view-dependent and changes as the camera moves — exactly the
> assumption cross-domain MIS relies on. Combining them naively produces brightening/darkening that
> shifts with camera motion. SSGI stays an additive term in the low tier only.

---

## 6. Two vkm-specific gotchas

**Vulkan Y-flip does not apply to compute.** The engine's clip space is +Y up with `[0,1]` depth
everywhere, and Vulkan's inverted NDC is compensated by `-fvk-invert-y`, which vkm-compiler
applies **only to the vertex stage** (`main.cpp:154-162`, `camera.h:18-23`). A compute
ray-generation shader reconstructing rays from pixel coordinates via `_inverseViewProjection` gets
**no** flip and will render vertically mirrored on Vulkan while looking correct on Metal. Handle it
explicitly and cover it in the cross-backend test.

**~~Threadgroup size is engine-global.~~ FIXED (Phase 2).** `kVkmComputeThreadGroupSizeX = 64` forced every compute
shader to `[numthreads(64,1,1)]`, because Metal needs `threadsPerThreadgroup` at dispatch time and
`MTLComputePipelineState` cannot be queried (`renderer_common.h:135-138`). Every ReSTIR pass is a
2D screen-space kernel wanting `8x8` for locality and cheap neighbour taps. Resolved: the size now
travels from the shader's SPIR-V through the cache header onto the pipeline object.

---

## 7. Scope reality check

| Phases | What | Rough share |
|---|---|---|
| 1–3 | Shared infrastructure (RT toolchain ✅, per-pass binding + barriers, G-buffer/motion vectors) | ~30% |
| 4 | GI technique interface + low-spec tier | ~15% |
| 5 | Acceleration structures | ~10% |
| 6–7 | Reference path tracer + unresampled baseline | ~15% |
| 8 | ReSTIR GI itself | ~15% |
| 9 | Denoiser + performance + robustness (both tiers) | ~15% |

**ReSTIR GI itself is ~15% of the work.** Phases 1–3 are things vkm needs anyway and are
independently mergeable; Phase 4 ships usable GI on every platform before any ray tracing exists.
Phases 6–7 look skippable and are not — they are the only way to tell a Jacobian bug from noise.

---

## 8. Phase plan

Reordered after the Phase 1 spike and the tiering requirement (§5): **shared infrastructure and the
low-spec tier come before acceleration structures and ReSTIR.** The low-spec tier needs none of the
ray-tracing work, so it becomes the first GI that works end to end, validates the technique
interface with two consumers, and hedges the project. ReSTIR remains the headline feature.

### Phase 0 — scaffolding ✅
- [x] Create this document
- [ ] Add genuine known limitations to `TODO.md` as they appear (one line each, per `CLAUDE.md` §8)

### Phase 1 — Ray tracing toolchain spike ✅ PASSED
- [x] Spike: `RayQuery<>` compute shader through `dxc -T cs_6_5` → `spirv-cross` → `xcrun metal`
      → metallib, in both plain-binding and Tier-2 argument-buffer configurations. **All exit 0** (§4.1)
- [x] Confirm MoltenVK exposes no RT extensions (§4.3)
- [x] Decision: keep the existing SPIRV-Cross chain. **No Metal Shader Converter, no Slang** (§4.2)
- [x] Per-stage `ray_query` opt-in: `VkmShaderStageDescriptor::requiresRayQuery`, parsed from the PSO
      JSON, selecting `cs_6_5` + `-fspv-target-env=vulkan1.2` in vkm-compiler. Covered by
      `TestPsoConfig`
- [x] Establish that `-fspv-extension` must **not** be used — it whitelists and would break every
      bindless shader in the engine (§4.4 "dxc flags")
- [x] End-to-end through `vkm-compiler`, **Vulkan: PASS.** A ray-query compute shader that also uses
      the engine's real `VKM_BINDLESS_VERTEX_PULLING` + push constants produced
      `rq_test[default].comp.vulkan.vfcache`, whose payload is **SPIR-V 1.5**, passes `spirv-val`,
      and declares `RayQueryKHR` + `RuntimeDescriptorArray` with both `SPV_KHR_ray_query` and
      `SPV_EXT_descriptor_indexing`
- [ ] End-to-end through `vkm-compiler`, **Metal: blocked on Phase 5** — see §4.4. Not a toolchain
      gap; it needs the AS registered as a set-0 binding, which is Phase 5's task
- [x] CI lavapipe check: **every** Ubuntu job pins `os: ubuntu-22.04`
      (`.github/workflows/ubuntu.yml:27-62`, 8 entries), whose `mesa-vulkan-drivers` is Mesa 23.2.x
      — below the 24.1 that introduced `VK_KHR_ray_query` in lavapipe. So Vulkan RT gets **no CI
      coverage** until the runner is bumped. Tracked in §12

**Deferred to Phase 5 (needs an acceleration structure to exist):** actually *dispatching* a ray
query. A `RayQuery<>` cannot run without a bound AS, so a runtime smoke test belongs with Phase 5's
gate, not here. Phase 1's gate is the **compile** path only — that is what was at risk.

### Phase 2 — Shared infrastructure A: per-pass binding, barriers, threadgroup sizes
Blocks *any* multi-pass compute work, and therefore **both** GI tiers. Pre-existing `TODO.md:8` items.
- [x] **Descriptor set 2 (per-pass), Vulkan.** PSO JSON declares `per_pass_resources` (sampled
      texture / sampler / storage buffer / uniform buffer, with shader visibility derived from the
      type); `VkmPerPassResourceTable` binds them via `commandBuffer->bindPerPassResources()`.
      Tables are **immutable** — that removes the update-while-in-flight hazard entirely, and the
      cases that "change" want a rebuilt table anyway (a resized G-buffer recreated its textures; a
      ping-ponged reservoir pair reads better as two tables selected by parity). Verified end to end
      with a compute pass whose only resources are in set 2
- [x] **Descriptor set 2 on Metal.** Bound *discretely* rather than as a second argument buffer —
      the same choice set 1 already makes, and for the same reason: it keeps set 2 out of the
      `pad_argument_buffer_resources` walk, which needs a registered base type for every id it steps
      over and so cannot cope with the sparse binding indices a PSO may declare. (That walk is also
      exactly what blocks the Metal ray-query path — see §4.4.) vkm-compiler pins each declared
      binding per-PSO via `add_msl_resource_binding`; the runtime replays the resolved entries onto
      the argument table. Verified by running the *same* shader and expected values as Vulkan
- [x] **Descriptor set 2 on WebGPU** (bind group 2). The most natural of the three — a bind group
      *is* this concept, and WebGPU's own model already treats one as immutable
- [x] ⚠ **WebGPU is runtime-verifiable now.** The premise recorded here — that turning on
      `VKM_COMPILER_ENABLE_WGSL` needs a Dawn build "no local or CI configuration provides" — was
      wrong: Dawn is already vendored and pinned, the CMake wiring already existed, and
      `scripts/run_tests.py` now builds a WGSL-capable host vkm-compiler and hands it to the
      Emscripten configure. Every engine PSO compiles to WGSL and ships in MEMFS, and the set-2
      table path runs there.

      **The gap was hiding four real bugs**, none of which a compile could have caught: implicit-LOD
      sampling in non-uniform control flow (illegal in WGSL); staging buffers left mapped at
      creation, which WebGPU forbids the GPU or the queue from touching — that broke `readbackTexture`
      and every `writeDirect` upload; the compute path never binding group 1, which the graphics path
      already did and documented; and all unpublished bindless singletons sharing one placeholder
      buffer, which WebGPU rejects both for overlapping writable-storage bindings and for mixing
      read-write with read-only use of one buffer. That last one invalidated every WebGPU pass that
      did not populate a scene.

      Still open: the per-pass *compute* test is skipped — `newBuffer` returns null for its storage
      buffer with no Dawn error and no engine log (`TODO.md`). The validation half of that test runs
- [ ] **Note:** set 2 is what unblocks WebGPU sampling at all — `registerTexture` is a hard error
      there because WGSL has no runtime-sized arrays, so the low-spec tier cannot sample a probe
      atlas until set 2 lands on WebGPU
- [x] **Texture barrier for shader reads.** Added `barrierTextureForShaderRead(texture)`, named for
      its destination like the existing `barrierIndirectArgumentBuffer` and equally coarse (it takes
      no source state: Vulkan already tracks the texture's layout, and the other backends have none).
      Closes a latent gap — a render target left in `COLOR_ATTACHMENT_OPTIMAL` while the bindless
      descriptors declare `SHADER_READ_ONLY_OPTIMAL` — which nothing had hit only because nothing
      renders to a texture and then samples it yet. Vulkan transitions (handling depth/stencil
      aspects); Metal and WebGPU are documented no-ops, since Metal 4's compute passes already
      bracket themselves with `barrierAfterQueueStages:`/`barrierAfterStages:` and WebGPU orders
      passes implicitly. Three Vulkan tests, each confirmed to fail when the transition is removed
- [ ] A **buffer** barrier beyond `barrierIndirectArgumentBuffer` has not been needed — that one
      already covers transfer/compute writes reaching compute reads. Add one when a case appears
      rather than pre-emptively
- [ ] Push constants beyond the vertex stage, or per-pass constants through set 2
- [x] **Per-PSO compute threadgroup size.** Done differently and better than planned: rather than
      declaring the size in the PSO JSON (which could drift from the shader), vkm-compiler reads
      the declared local size out of the compiled SPIR-V and stores it in
      `VkmShaderCacheHeader::threadGroupSize` (cache version 3). Metal's `onDispatch` reads it
      from the bound pipeline; Vulkan/WebGPU take it from the shader module. Shaders may now
      declare `[numthreads(8, 8, 1)]`. A `dispatch2D` helper was **not** needed — `dispatch()`
      already takes 3 group counts and the size now comes from the shader

**Gate:** a two-pass compute chain (A writes a storage texture, B samples it) produces correct
pixels with **zero validation-layer errors** on Vulkan and Metal. Per `CLAUDE.md` §9, non-negotiable.

### Phase 3 — Shared infrastructure B: G-buffer, motion vectors, frame constants
Also shared by both tiers.
- [x] **MRT G-buffer**, three targets plus depth (`gbuffer.cpp:30-51`, `gbuffer.hlsl:87-92`): shading
      **and** geometric normals octahedral-packed into one RGBA16F target (the geometric one is what
      ray offsetting will need); albedo + roughness; motion + metallic + camera distance. Three
      deliberate departures from the line above, each recorded rather than quietly dropped:
      **no material ID** — nothing consumes one yet (`gbuffer.h:47-48`); motion is **2-component**,
      not 3 (`vkm_gbuffer.hlsli:73`), and **camera-only**, since `VkmObjectData` carries no previous
      transform, so a moving object reports static-object motion (`gbuffer.hlsl:11-16`); and "linear
      depth" shipped as **radial distance from the camera** (`gbuffer.hlsl:189`), which is what the
      position reconstruction below actually wants
- [x] **Previous-frame G-buffer** (double-buffered): a full second copy of every target plus depth,
      flipped by `advanceFrame()` (`gbuffer.h:127-128`, `gbuffer.cpp:135-158`). Storage only —
      **nothing reads `getPrevTexture()` yet**, and nothing will until Phase 8's temporal reuse
- [x] **Extended `VkmFrameConstants`** (272 -> 368 bytes, stride unchanged at 512): added
      `_prevViewProjection`, `_viewportSize` (xy = pixels, zw = reciprocal) and `_frameIndex`
      (monotonic, distinct from the 0..FRAME_COUNT-1 slot index). The engine owns the two
      frame-to-frame fields since a camera holds no such state, and seeds `_prevViewProjection`
      with the current matrix on the first frame so reprojection starts as the identity rather
      than a violent cut. `TestCamera` now pins every member offset — the C++ struct, the HLSL
      mirror and vkm-compiler's Metal pin are three halves of one ABI that nothing else checks.
      Deliberately **not** added: jitter and previous camera position, which have no consumer
      until TAA (Phase 9)
- [x] **Reconstruct world position rather than storing it** — `vkmReconstructWorldPosition`
      (`vkm_gbuffer.hlsli:97-105`), used by `deferred_lighting.hlsl:91` and `probe_lighting.hlsl:126`.
      It unprojects a far-plane NDC point through `_inverseViewProjection` and marches along that ray
      by the **stored radial distance** rather than sampling the depth attachment, because WebGPU
      rejects a depth-format view bound as an ordinary sampled texture (`vkm_gbuffer.hlsli:88-95`).
      Same outcome — no world position in the G-buffer — reached through the one channel every
      backend can read
- [x] **Fullscreen-pass building block + PBR deferred lighting.** One oversized triangle from
      `SV_VertexID` (no vertex/index buffer), sampling the G-buffer through **descriptor set 2** —
      not the bindless arrays, so the pass has a path on WebGPU and needs no `VkmScene`. Cook-Torrance
      GGX finally reads the `VkmMaterialData` that glTF import has carried unused. This is also
      where `barrierTextureForShaderRead` got its pixel-level proof, and where set 2's *texture and
      sampler* paths were first exercised (the earlier test used buffers only)
- [x] **Tone mapping migrated to HLSL + PSO.** The loose `tonemap.frag`/`quad_screen.vert` were dead
      (no PSO referenced them) *and* wrong: `main()` bypassed the helper doing exposure/white-point/
      gamma, so whites came out grey, and the "fullscreen" quad was scaled to 0.8. Both fixed rather
      than reproduced; the GLSL is deleted and `TODO.md:14` narrowed to what remains
- [x] **Fullscreen triangle factored into `vkm_fullscreen.hlsli`** once two passes needed it
- [x] **Hash-based stateless RNG** seeded per (pixel, frame, pass) — `vkm_random.hlsli`: a PCG hash,
      a uint/float stream and a cosine-weighted hemisphere sampler. Written when SSGI needed ray
      directions, which is also its first consumer. Stateless matters beyond convenience: ReSTIR's
      validation modes replay a frame's sampling decisions, which only works if the stream is a pure
      function of what identifies the sample

**Gate:** G-buffer channels visualizable via a debug view; reprojection debug view stable under
camera motion with no drift. Works on all five platform combinations.

> ⚠ **The gate is not met, and the phase should not be called closed because of it.** Every pass
> above is implemented and tested, but **no application owns any of them** — `VkmGBuffer`,
> `deferred_lighting` and `tonemap` are instantiated only from `tests/`, and `model_viewer`'s
> `DebugMode` (`src/samples/model_viewer/main.cpp:63-72`) drives its own forward shader, not a
> G-buffer. So there is no debug view to look through and no reprojection view to check for drift.
> The first app to own this chain is Phase 4's item 4.5, which is where this gate really gets
> settled; it is listed there rather than duplicated here.

### Phase 4 — GI technique interface + low-spec tier ⭐ first working GI
The de-risking phase. Delivers visible GI on **all five** platform combinations with no ray tracing.
Technique decided in §5: **raster-updated dynamic probe volume + SSGI contact term.**

- [x] Decide the low-spec technique (§5)
- [x] Define the **technique interface**: a technique owns its passes and resources and produces an
      indirect-radiance texture for one shared composite pass. `VkmProbeVolumeUpdater` is the first
      technique and `gi_composite` is that pass — it is the only thing that knows how the output is
      combined (irradiance × albedo / pi, added to direct), so a second technique means adding
      passes rather than editing the composite. The optional confidence channel is not there yet:
      nothing consumes one until the Phase 9 denoiser
- [x] Runtime selection gated on `VkmDriverCapabilityFlags::RayTracing` + a quality setting, not
      `#ifdef`. **Landed with Phase 8, as planned**: `gv_gi_technique` / an ImGui combo in the gi
      sample selects probe volume or ReSTIR at runtime, shown only where the device reports ray
      tracing and clamped back to the probe tier where it does not (or where the RT pipelines or
      acceleration structures fail). The interface held: the switch swaps which subgraphs run
      between GiDirectLighting and GiComposite, and `_indirectTarget` and the composite table are
      untouched — exactly the §5 boundary, proven by its second consumer
- [x] **4.1 Probe volume storage.** `VkmProbeVolume`: probe grid, both atlases (8×8 irradiance,
      16×16 distance/moments), double-buffered for hysteresis blending, with a **one-texel border
      per probe** so bilinear taps near an octahedral seam read the right neighbour rather than the
      far side of the map. The border is in from the start because the atlas layout is what every
      addressing helper and both shaders depend on. Distance uses RGBA16F with two channels unused —
      the engine has no two-channel format, same constraint the G-buffer hit
- [x] **4.2a Probe capture.** `probe_capture.hlsl` renders the scene from one probe into six cube
      faces packed in a **single render pass**, aimed by `setViewportAndScissor` and selected by a
      pushed face index. Reuses `VkmScene`'s cull/emit path; `recordDrawBatches` gained an optional
      per-draw hook, which is the only point where push constants can be set since it owns the bind.
      Radiance is forward-shaded (a probe stores what it saw), and distance rides in alpha for the
      moments the Chebyshev test needs
- [x] **4.2b Octahedral conversion + border + hysteresis blend.** `probe_blend.hlsl`, two
      permutations (irradiance / distance moments) from one source. Two details make it correct
      rather than nearly so: a border texel is mapped to the interior texel it *mirrors* and that
      direction is integrated — same answer as a copy pass, in one pass, with no read-after-write —
      and the capture is sampled by projecting through the same face matrices the capture used,
      so there is no second cube convention that can disagree with the first
- [x] **4.2c Per-frame probe budget (round-robin) + propagation latency.** `VkmProbeVolumeUpdater`
      appends the whole refresh to a render graph — scene update, a cull aimed at the probes rather
      than a camera, one capture pass covering every budgeted probe's six faces, a barrier, and the
      two atlas blends. Three things came out of making a *partial* update correct:
      - **Hysteresis moved to the blend hardware and the atlas stopped being double-buffered.**
        `lerp(new, previous, h)` against a second copy is only correct when every probe is rewritten
        every frame — which is exactly what round-robin stops doing, so un-refreshed probes would
        alternate between two increasingly stale copies. Outputting `alpha = 1 - h` under
        `SrcAlpha`/`OneMinusSrcAlpha` gives the identical arithmetic against the atlas itself, and
        "nobody drew this cell" then means "it keeps its value". It also removed a latent bug: the
        previous-atlas fetch used cell-relative UV, correct only for the single-cell atlas the test
        happened to use
      - **Both probe passes turned out not to need per-probe constant buffers at all.** The face
        matrices are `P·R·T(−p)`, so the probe position cancels in the blend (which evaluates them
        at `p + direction`) and factors out of the capture (which can work in probe-relative space).
        One shared buffer per PSO serves the whole volume, and what is genuinely per-probe — a
        position, a capture tile base, a hysteresis — is small enough to push. Pushed from the
        **vertex** stage and forwarded as flat interpolants, since a fragment shader cannot read
        push constants on Vulkan
      - **The budget is clamped, not wrapped**, so a round refreshes every probe exactly once even
        when the budget does not divide the probe count, and a probe's first refresh ignores
        hysteresis so a cold start does not cost a full convergence
- [x] **Second cull view.** `kVkmSceneMaxCullViews = 2`: each view gets its own `VkmFrameData`
      region, its own count words and its own payload regions in the visible-list and argument
      buffers, so two culls in one frame never write the same words and need no barrier between
      them. `recordUpdate`/`recordCull`/`recordDrawBatches` all take a `viewIndex`.

      Two details are what make the layout work. The counts for *every* view sit at the front of
      both buffers rather than beside each view's payload, because a batch uses one index into
      both and their payloads have different strides — there is no single per-view offset that
      would serve both. And each view needs its own **staging** region, not just its own device
      region: both `recordUpdate` calls write host memory immediately, long before either GPU copy
      runs, so a shared staging slot leaves the first cull reading the second view's frustum. That
      was the actual blocker, and it is the one a test has to pin
- [x] **4.3 Probe sampling.** `probe_lighting.hlsl`: trilinear over the 8 surrounding probes,
      weighted by the **Chebyshev visibility test** against the distance atlas, plus normal bias and
      a smoothed backface term. Addressing and the octahedral mapping live in
      `vkm_probe_volume.hlsli`, shared with whatever fills the atlases. Tested by authoring the
      atlases from the CPU, which isolates the read path — the two cases are "a visible grid returns
      its irradiance" and "occluded probes contribute nothing", the second being exactly the wall
      leak the distance atlas exists to prevent
- [x] **4.4 SSGI contact term.** `ssgi.hlsl`: short cosine-weighted rays marched against the
      G-buffer's camera-distance channel, sampling the direct lighting where they contact. Added
      into the *same* indirect target as the probe result with a one-to-one blend and a `Load`, so
      it can only brighten what the probes produced — strictly additive, and strictly this tier.
      It also brought in the hash RNG Phase 3 left open (`vkm_random.hlsli`: PCG, seeded per
      (pixel, frame, pass), with a cosine-hemisphere sampler).
      Its limits are recorded at the shader: offscreen and occluded geometry contribute nothing, a
      ray leaving the screen counts as no contact rather than as sky (which darkens rather than
      brightens — the conservative direction), and thin geometry can be marched through. Its
      strength is a knob, not a calibration: there is no ground truth to tune it against until
      Phase 6
- [x] **4.5 GI sample** — `src/samples/gi`. Drives the whole chain per frame: probe refresh (cull
      view 1) → scene update and cull (view 0) → G-buffer → deferred lighting → probe lighting →
      composite → tone map. It owns **no shaders at all** — every pass is an engine PSO — which is
      what lets it build on WebGPU without a per-sample WGSL cache, and it is the first application
      to own the G-buffer/deferred/tone-map chain, so **Phase 3's gate is settled here**.
      Ten debug views (composite, direct, indirect, albedo, both normals, roughness, metallic,
      motion, camera distance) run through the new shared `gi_composite` pass, which is also the
      one place a technique's output is combined — the abstraction boundary §5 describes.
      The probe grid is fitted to the scene's bounds, and the per-frame probe budget is derived
      from the draw-batch count rather than fixed: the capture pushes once per (probe, face, batch)
      and the Metal/WebGPU push-constant ring has no per-frame reset, so a fixed budget wraps it
      onto entries a running frame still references.

      Verified by screenshot: `--gv_gi_screenshot=<png> --gv_gi_screenshot_frame=<n>` tone-maps a
      second time into an owned target, reads it back and writes a PNG, then exits — the only way
      to check any of this on a machine where nobody sees the window. The indirect-only view over
      Sponza shows real colour bleeding, green where light bounced off the green material and grey
      where it did not, which is what closes the capture→lookup addressing question the atlas tests
      could not reach on their own.

      Not yet done here: a runtime *technique* switcher (there is one technique), the SSGI term
      (4.4), a reprojection debug view, and any *automated* pixel check — screenshots are still
      compared by eye
- [x] **Light-propagation latency measured, and it is as bad as feared.** The decay is exactly
      geometric — a probe retains `hysteresis` of its old value per refresh and is refreshed once
      per round of `ceil(probeCount / budget)` frames — so the latency is
      `roundLength · ceil(ln f / ln h)` frames to an error fraction `f`. That is asserted against a
      real GPU measurement on a 4-probe volume rather than assumed
      (`tests/TestProbeVolumeUpdaterShared.hpp`), which is what makes the projection trustworthy:
      **at the defaults (2048 probes, budget 32, hysteresis 0.97) a light change takes 4864 frames
      — about 81 s at 60 Hz — to shed 90% of the error**, and 1472 frames (~25 s) to shed half.
      That is unusable as shipped, and the fix is a knob, not a redesign: latency is linear in the
      round length and logarithmic in `ln h`, so halving the hysteresis to 0.85 costs 6.6× less
      latency for a noisier image, and doubling the budget halves it for twice the raster cost.
      Recorded in §12 as the number the mobile budget has to be argued against

**Gate:** low-spec GI renders correctly on Vulkan, Metal **and** WebGPU, with zero validation errors;
the technique switcher works at runtime; no visible leaking through walls in a two-room test scene.
This is the interface's real test — one consumer proves nothing.

**Where the gate actually stands.** Met: the `gi` sample renders the tier on Metal with zero
validation errors and builds on all three backends; a two-room fixture now exists
(`resources/tests/gltf_two_rooms.gltf`) and a probe walled off from the white room captures none of
it. Not met, and each for a stated reason:

- ~~**The technique switcher has nothing to switch.**~~ **Resolved with Phase 8**: ReSTIR is the
  second technique, selectable at runtime behind the RayTracing capability, and the composite was
  untouched by its arrival — the interface's real test, passed.
- **Leak prevention is verified on the capture side only.** The Chebyshev test lives in the *lookup*,
  and disabling it leaves the two-room test passing: reaching that path needs `probe_lighting` run
  over an authored G-buffer against real captured atlases. Its rejection is covered against
  CPU-authored atlases separately; the two halves have not been joined (`TODO.md`).
- **WebGPU is verified by building and by the suite, not by looking.** The sample compiles and links
  there and the WebGPU tests pass, but no image from that backend has been inspected.

None of the three is a surprise from implementation; they are the parts that need something a later
phase supplies.

### Phase 5 — Acceleration structures in the RHI
Where the high tier starts. Split into three PRs: **5a** the capability seam, **5b** BLAS/TLAS
built from `VkmSceneGeometryPool`, **5c** the TLAS as a bindless singleton plus the ray-query gate.
- [x] **5a: `VkmDriverCapabilityFlags::RayTracing`.** Vulkan requests
      `VK_KHR_acceleration_structure`, `VK_KHR_ray_query` and `VK_KHR_deferred_host_operations` as
      a set (they are useless apart) and checks both feature bits; Metal asks
      `MTLDevice.supportsRaytracing` rather than assuming Metal 4 implies it, since Metal 4 runs on
      hardware that predates hardware RT; WebGPU never reports it. Measured on this machine:
      **Metal yes, Vulkan-on-MoltenVK no** — which is why this is a runtime capability and not an
      `#ifdef`. `VK_KHR_ray_tracing_pipeline` is deliberately not requested: the engine casts rays
      from compute shaders, so it needs no shader binding tables (§4).
- [x] **5b (part): the AS resource type and the Vulkan build.** `VkmResourceType::AccelerationStructure`
      with a `VkmAccelerationStructure` base, a Vulkan implementation that builds both levels, and
      error-logging stubs on Metal and WebGPU. `VkmResourceCreateInfo::AllowAccelerationStructureInput`
      both adds `VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR` and forces
      the committed allocation path, which is the trap recorded below.
- [x] **5b (part): dynamic objects.** The first shape built here could not express a moving object
      -- it built once, synchronously, at creation. A structure created with `_allowUpdate` now
      keeps its scratch buffer and can be rebuilt: `updateInstances` rewrites the transforms and
      `VkmCommandBufferBase::buildAccelerationStructure` records the rebuild into a render graph
      pass. **Rebuild, not refit**: a top-level structure over its instances is cheap to rebuild
      and stays optimal, while an update degrades traversal as instances drift from where it was
      built -- and a falling rigid body moves a long way. A refit is for *deforming* geometry,
      which nothing produces yet. A bottom-level structure still builds once: a sphere's geometry
      does not change in object space, only its instance transform does.
- [x] **5b (part): Metal.** Metal 4 rebuilt the API rather than renaming it —
      `MTL4PrimitiveAccelerationStructureDescriptor` takes `MTL4BufferRange` (GPU address + length)
      instead of `id<MTLBuffer>`, and an instance names its bottom-level structure by `MTLResourceID`
      through the `Indirect` descriptor layout rather than indexing an
      `instancedAccelerationStructures` array. That second difference is what lets `updateInstances`
      rewrite a buffer instead of rebuilding the descriptor. **This is the first code in Phase 5
      that has actually run**: on Metal the test builds a bottom-level structure, instances it,
      moves the instance and rebuilds — 21 assertions, passing, and now **in the suite**.
- [x] **5b (part): the Vulkan fixture.** `tests/TestAccelerationStructure.cpp` wraps the shared
      body for Vulkan, so the never-executed Vulkan implementation gets its first run in CI's
      lavapipe job. It skips on MoltenVK, which turns the whole Vulkan suite's local result into
      SKIP (`TODO.md`).
- [x] **5b (rest): BLAS per mesh, TLAS per scene.** `VkmScene::buildAccelerationStructures()`
      builds one bottom-level structure per pooled mesh and one rebuildable top-level structure
      over the placed objects; `recordAccelerationStructureUpdate()` republishes the transforms and
      records the rebuild. Separate from `build()` so a scene that is only rasterized pays nothing.
      An instance's id is its object index, which is also its `VkmObjectData` index, so a hit
      recovers its object with no side table. **The test cannot verify the geometry offsets** --
      zeroing them leaves it passing, because nothing traverses the result until the ray-query gate
      below; recorded in `TODO.md`.
- [x] Metal: `MTL4PrimitiveAccelerationStructureDescriptor` / instance descriptors via
      `MTL4ComputeCommandEncoder` (Metal 4 folded the AS encoder into the compute encoder)
- [x] Build BLAS from the existing `VkmSceneGeometryPool` so no vertex data is duplicated.
      **Triangles only** — documented at the declaration site (§4.2). The pool's shape already
      fitted: one vertex buffer, one index buffer, and a `MeshRange` per mesh carrying
      `(vertexWordOffset, vertexCount, indexOffset, indexCount)`, which is exactly a BLAS geometry
      descriptor's inputs. **The trap found before starting did not fire**, and here is why: on
      Vulkan a buffer only picks up `VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT` on the *committed*
      path (`vulkan_buffer.cpp:123-130`), and the geometry pool already asks for
      `VkmMemoryPlacementHint::ForceCommitted` for an unrelated reason (a bindless registration
      must see offset 0). Adding `AllowAccelerationStructureInput` therefore only adds the
      build-input usage bit — and it is added **only where the device reports ray tracing**, since
      that usage is illegal on a device where `VK_KHR_acceleration_structure` was never enabled.
- [ ] ~~Refit on transform change~~ **Decided: rebuild only.** A top-level rebuild over its
      instances is cheap and stays optimal, while an update degrades traversal as instances drift
      from where the structure was built. Refit belongs to deforming geometry, which nothing
      produces yet.
- [x] **5c: one TLAS bound in set 0**, at `kVkmBindlessAccelerationStructureBinding` (8), through a
      new `setAccelerationStructure()` on all three backends' managers plus
      `VKM_BINDLESS_ACCELERATION_STRUCTURE` in `vkm_bindless.hlsli` (native branch only). Given its
      own binding rather than a `VkmBindlessSingletonBuffer` entry: its descriptor type is an
      acceleration structure on Vulkan and an `MTLResourceID` on Metal, so nothing about the
      singleton *buffer* path applies to it. Vulkan drops the binding entirely on a device without
      ray tracing, since that descriptor type is illegal in a layout there.
      The `add_msl_resource_binding` entry is what unblocked §4.4 — but **not** with
      `basetype = SPIRType::AccelerationStructure`, which throws
      "Unexpected argument buffer resource base type" for *every* shader in the tree: the switch
      that consumes it accepts scalars, `Image`, `Sampler` and `SampledImage` only. A scalar
      basetype is correct, because the registration only picks the index category (buffer, which is
      what an acceleration structure genuinely uses — `[[buffer(index)]]`, `spirv_msl.cpp:15344`)
      and the padding-member width (8 bytes, which is its entry).
- [x] Confirm `MTL4ArgumentTable` AS binding empirically — confirmed by the emitted MSL and by the
      gate running: `raytracing::acceleration_structure<raytracing::instancing> g_Scene [[id(12293)]]`
      inside `spvDescriptorSetBuffer0`, reached through the same argument buffer as everything else
      in set 0, with no `setAccelerationStructure` on the argument table needed at all.
- [x] Prefer `MTLResidencySet` over per-encoder `useResource:` —
      `VkmRenderResourcePoolMetal::fetchAllocation` now covers `AccelerationStructure`, so a
      structure joins the pool's residency set like any other allocation and every bottom-level
      structure the top-level one names is covered for free.

**Gate: MET on Metal** (`tests/TestRayQueryShared.hpp`, `resources/tests/ray_query/`) — six rays
against a loaded glTF scene, hit/miss and `t` against an analytic reference. Vulkan runs the same
body and first executes it in CI's lavapipe job. The gate is what finally makes Phase 5 observable:
the traced mesh is deliberately the *second* one in the geometry pool and its object is placed away
from the origin, so a zeroed pool offset and a dropped instance transform each turn its three
expected hits into misses — both verified by sabotage, and both silent before this.

**RHI contract note (Phases 2 and 5):** `backend/common/AGENTS.md:556` requires that no new pure
virtual is added without implementing it in **all** backends — so every RT and barrier entry point
needs a **WebGPU error-logging stub**, per the `copyTexture`/`registerTexture` precedent
(`TODO.md:29,42`). Gate on the capability flag, not `#ifdef`. Public base-class signatures must not
change.

### Phase 6 — Reference path tracer
**Do not skip.** Without ground truth you cannot distinguish a Jacobian bug from noise.
- [x] **Accumulating brute-force path tracer in compute** using Phase 5's ray query
      (`resources/Shaders/path_trace.hlsl` + `VkmPathTracer`). Reads the scene entirely through
      set 0 and set 1, so it takes no scene pointer. Cosine-weighted Lambertian sampling, so a
      bounce's throughput multiplier is the albedo exactly and no pdf division appears anywhere.
      Needed one word of `VkmObjectData` (`_vertexStrideWords`, in an existing padding word): a
      draw knows its vertex layout from its PSO permutation, a ray cannot.
- [x] **Area/emissive light representation** — emissive geometry *is* the light, hit rather than
      sampled. `_lightDirection` is deliberately ignored: a directional light has no area, and a
      reference that special-cased one would not be measuring the same integral the techniques are
      (`TODO.md`).
- [x] **MSE/RelMSE comparison utility** (`vkmComputeImageMse`, `vkmComputeImageRelativeMse`),
      normalizing each image by its own sample count so a longer run is not penalised for being
      brighter, and skipping pixels nothing was accumulated into.
- [ ] Split-screen accumulation mode: ground truth vs live pipeline. **Not built** — presentation
      work in the gi sample rather than part of the gate; the reference is reachable only from a
      test today (`TODO.md`).

**Gate: MET on Metal** (`tests/TestPathTracerShared.hpp`, `resources/tests/gltf_furnace.gltf`).
Two unit cubes of albedo 1.0 and 0.5 in a uniform environment. The answer is **analytic, not
convergent**: a convex diffuse body in a uniform environment reflects exactly `albedo * L`, and
cosine-weighted Lambertian sampling makes the estimator zero-variance on it — one sample per pixel
already gives the exact value, so the tolerance is 1e-3 rather than a noise budget. The white
furnace assertion is therefore that the albedo-1 cube is *invisible*: the whole left half of the
image is one value. Energy conservation is asserted directly (no pixel exceeds the environment) and
analytically (the grey cube's darkest ratio is exactly 0.5). Sabotage-verified: 5% energy created
per bounce fails it. Vulkan runs the same body in CI's lavapipe job.

### Phase 7 — 1-spp indirect, no resampling (baseline to beat)
- [x] **Single indirect bounce, one ray per pixel, no reservoirs** (`gi_indirect.hlsl` +
      `VkmIndirectPass`). Differs from the reference in its primary hit and nothing else: the
      rasterized G-buffer rather than a traced ray, then the same `vkmTracePath`.
- [x] **Direct lighting at the secondary hit** — behind `vkmShadeSecondaryHit()`, currently
      emission plus the continued path. NEE replaces exactly that body; it is not written yet
      because the scene still has no area-light representation to sample (§12).
- [x] **Hit-point encoding, forward-compatible with the LoD paper.** `VkmSurfaceHit` carries
      `(instanceId, primitiveIndex, barycentrics)` and reserves the `uv`; `vkmVertexMapping()` is
      the one place a hit becomes a surface point. The uv is reserved but not filled — that needs
      a vertex-layout id in `VkmObjectData` (`TODO.md`).
- [~] **Ray origin offset along the geometric normal, scale-relative epsilon** — done, and it is
      what surfaced the G-buffer normal bug below. The reconnection ray's
      `t_max = (1-ε)·distance` is documented at `vkmOffsetRayOrigin` but unimplemented: nothing
      casts a shadow or reconnection ray until Phase 8, so writing it now would ship untested.

**Gate: MET, and it earned its keep.** Accumulating both estimators over a Cornell box
(`resources/tests/gltf_cornell.gltf`, open-topped and environment-lit so no camera-visible surface
emits) and comparing with the Phase 6 MSE utility. Its first honest run scored **MSE 0.030**: the
G-buffer's geometric normal pointed *away* from the camera, so every secondary ray started inside
its own wall. Nothing had ever consumed that channel — SSGI marches in screen space, deferred
lighting uses the shading normal — so the first pass to offset a ray along it is what found it.
Fixing it took the number to **6.2e-4**. The sample count was then raised until the floor fell
below the smallest systematic error worth catching: a reference run one bounce short scores 7.3e-4
against a 2.4e-4 floor.

It also found a third bug, and the hardest of the three: **an acceleration structure was built
before it was ever made resident.** `VkmDriverBase::newAccelerationStructure` calls
`onResourceInitialized()` — which is what puts a resource in Metal's residency set — *after*
`initialize()` returns, and `initialize()` is where the synchronous build happens. So a
bottom-level build wrote into unmapped memory and a top-level build read bottom-level structures
that were not mapped either. It worked whenever the memory happened to be resident anyway, which
depends on what the process had allocated first, and Metal reported nothing either way. The
structure now registers itself before recording its build.

### Phase 8 — ReSTIR GI core
Incremental, with measured RelMSE against Phase 6 at every sub-step.

- [x] **8.1 Reservoir buffer.** Packed as planned: `position` fp32x3, `normal` octahedral
      snorm16x2, `radiance` RGB9E5, `weight` fp32, `M`/`age` 8 bits each — 32 B in eight u32
      words, with one reserved for 8.4 to cache a target pdf in. One buffer, `kVkmReservoirSliceCount`
      slices, per-pass input/output slice indices in the push constants.
      **The triple-buffering question is answered, not deferred:** two slices, not `FRAME_COUNT`,
      because `VkmRenderGraph` already calls `ensureCompleted()` on a frame slot before recording
      into it again — the same guarantee set 1's per-slot region and the push-constant ring rely
      on. So the count is set by what resampling needs (a read slice and a write slice), not by
      how many frames are in flight. Recorded at `kVkmReservoirSliceCount`.
      **RGB9E5 earned its own lesson:** the exponent bias is 15, and writing 16 does *not* look
      like a bug — pack and unpack share the scale, so small values round-trip perfectly and every
      channel whose mantissa lands above 511 silently clamps. It cost 7.4% of the image and read
      exactly like plausible quantization loss.
- [x] **8.3 Sample generation pass** — one traced ray, one fresh reservoir (`M = 1`, `age = 0`),
      `W = 1/p_source`. Plus the resolve half of 8.6 (`f_s · cos · L · W`, no final visibility ray
      — the sample was traced from this pixel, so it is visible by construction until spatial reuse
      starts handing pixels a neighbour's).
- [x] **8.2 Neighbour offset LUT** — 256 R2-sequence points through Shirley-Chiu's concentric
      square-to-disk map, uploaded once and indexed with a mask. R2 rather than a golden-angle
      spiral because the spatial pass reads a *run* of consecutive entries, and a spiral's
      consecutive points share almost the same radius — a run of them would sample a ring rather
      than a disk. `vkmBuildNeighbourOffsets` is free-standing, so the distribution is testable
      without a GPU.
- [x] **8.4 Spatial resampling** — verified, still off by default (the un-resampled estimator
      stays one flag away as a validation mode; the gi sample turns it on).
      `gi_reservoir_spatial.hlsl` merges k neighbours chosen from the G-buffer alone (normal within
      25°, camera distance within 10%), applies the reconnection Jacobian, and runs the second loop
      over the same neighbours for the bias-correction denominator with a visibility ray each.
      RTXDI's Jacobian magnitude clamp is deliberately left out: it buys variance at the cost of a
      mean this sub-step exists to measure.
      **The 13.8% brightening was the asymmetry the maths forbids**: the denominator Z evaluated
      `p̂·V` (its per-participant visibility ray) while the merge weights evaluated `p̂` alone — no
      ray in the merge loop. Every genuinely occluded participant was dropped from the divisor
      while its weight stayed in the sum. Algorithm 6 requires the *same* target function in both;
      adding the centre-visibility ray to the merge loop (rejecting candidates the centre cannot
      see, which is also §2's leak-prevention ray) took the mean from 1.138× to **0.9977×** the
      un-resampled estimator, with MSE vs the reference equal to the 1-spp baseline's (2.44e-4 vs
      2.41e-4 on Metal). Asserted in `TestIndirectPassShared` at the baseline's own thresholds
      plus a 1% mean-ratio bound. The environment flag needed no Jacobian special case after all —
      at the 1e5-unit stand-in distance the Jacobian's own ratios collapse to 1 (documented at
      `vkmReservoirJacobian`). One honest note: at 1536 *accumulated* samples spatial's RelMSE
      (1.9e-3) does not beat the baseline's (1.6e-3) — reuse correlation holds the floor; the win
      resampling buys is at low sample counts, the live path's regime.
- [x] **8.5 Temporal resampling** — `gi_reservoir_temporal.hlsl`, no rays. The slice layout grew
      to three: generation writes a fresh slice that survives the whole frame (8.7's MIS blend
      reads it), temporal ping-pongs the history pair by sample parity, and spatial — when on —
      writes into the slice history was just consumed from. Only the temporal output is ever
      re-ingested; spatial results never feed back (§9). Reprojection subtracts the motion vector
      from the current UV; the history tap is validated against the **previous** G-buffer only
      (bounds, coverage, normal within threshold, camera distance against the **previous** eye —
      which needed `_prevCameraPositionWorld` in the frame constants, 368 → 384 B). Confidence cap
      20 and a separate age cap 32, both push constants; age is the *sample's*, so a pixel that
      adopts its fresh candidate resets it and never trips the cap. The temporal p̂ is
      luminance·cos with **no** visibility ray, stated per-stage: the history sample came from
      (up to reprojection) this surface, disocclusion is G-buffer-rejected, and the spatial pass
      and resolve still guard the result — numerator and Z use the same rayless p̂, so the stage
      is consistent the way 8.4 taught. Verified on the Cornell gate: mean ratio 1.002, caps
      observed in a reservoir readback (max M exactly 21, max age exactly 32) — and that readback
      caught two real bugs on the way in: a hand-filled test frame-constant leaving the previous
      eye at the origin (every tap depth-rejected; M never left 1), and `W = 0` histories treated
      as invalid, which drops their confidence from Z and brightens by the same asymmetry 8.4 was
      (+0.9% → +0.2% once null samples merge with weight 0 but full M, per §9's null-sample rule).
      The ring of jittered fallback taps is deliberately not built: nothing measured needs it yet,
      and it belongs with the moving-camera work the gi sample verifies by eye
- [ ] **8.5 (deferred half): moving camera/scene verification** — reprojection and the previous-eye
      depth test are implemented and static-verified; a moving camera is checked in the gi sample
      by eye (no automated gate exists for it), and object motion still reports camera-only
      vectors (`VkmObjectData` carries no previous transform)
- [x] **8.6 Shading/resolve** — two consumers of one reservoir, deliberately. The compute resolve
      (`gi_reservoir_resolve.hlsl`, applies albedo/π into the accumulation buffer) stays as the
      MSE gates' instrument; the new `gi_restir_lighting.hlsl` is a fullscreen **graphics** pass
      that shades the resampled reservoirs into the indirect-radiance texture, honouring
      gi_composite's contract — incoming irradiance, `radiance·cos·W`, **no** albedo and no 1/π,
      both applied once by the composite. A graphics pass because the target is a color
      attachment and a fragment shader reads the reservoir storage buffer fine;
      `VkmTableResourceType` needs no storage-texture kind for this. `recordAccumulate` split
      into `recordResample` / `recordResolveAccumulate`, plus `recordUpdateLightingConstants`
      (per-frame staged uniform: parity-dependent slice indices cannot live in an immutable
      table) and `recordLighting`. Debug views ride the same constants: reservoir M, age and W
      as grey ramps — the caps are invisible in the lit image. Gate: one frame driven exactly as
      a live renderer drives it (transfer → resample → fullscreen draw), read back **raw fp16**
      (readbackTexture converts to 8-bit and clamps HDR), composited per pixel with the
      G-buffer's own albedo·(1−metallic)/π, and its mean must land on the accumulated resolve's
      (measured 0.979, tolerance 10% for one frame of a confidence-21 estimator). The optional
      final visibility ray is **not** in: fragment-stage ray query is unproven through the
      SPIRV-Cross MSL path, and with 8.4's merge rays and 8.5's G-buffer validation nothing
      static needs it — deferred with its flag word already plumbed (`TODO.md`)
- [x] **8.7 Final-shading MIS** on low-roughness surfaces (see §2's bias note). Cyberpunk's cheap
      version, in `gi_restir_lighting.hlsl` rather than the shared composite (which stays
      technique-agnostic per §5): `out = fresh·(1−r²) + resampled·r²`, where the fresh half is the
      pixel's own canonical 1-spp sample — kept alive in the fresh slice by 8.5's layout exactly
      for this. Toggle in the lighting constants, `gv_gi_restir_mis` + a checkbox in the sample,
      on by default. Gated where it can be: every fixture material has roughness 1, where the
      blend must degenerate to the resampled estimate — both blend states are held to the same
      mean in the Cornell gate. The honest limit: at r = 1 the fresh half never contributes, so a
      wrong fresh-slice read is invisible until a varied-roughness asset exists to look at; the
      sample's headless A/B was also found to be non-reproducible run to run (the orbit camera
      integrates wall-clock time), so byte comparisons say nothing there

> **Hard rule (course Tip 4.1):** choose which neighbours to reuse based **only** on the G-buffer.
> Never on the samples or weights stored *in* the neighbours' reservoirs — that conditions the
> probability space and biases the result. Same rule for resetting confidence on disocclusion.

**Gate per sub-step:** RelMSE versus Phase 6 must improve or at least not regress, and the mean
must match **on a diffuse-only scene** (specular secondary hits are legitimately biased — see §2).
A shifted mean on diffuse means a broken MIS weight or missing Jacobian. Add a debug view per pass
(reservoir `M`, `W`, `age`, chosen sample position, temporal-vs-spatial contribution) — these bugs
are invisible in the final image.

**How the gate was actually applied** (8.4/8.5): the mean-match half held as written and caught
both real bugs. The RelMSE half was re-scoped to "the baseline's own absolute thresholds" rather
than "beat the baseline": at 1536 *accumulated* samples every estimator sits at its noise floor,
and reuse correlation holds a resampled estimator's accumulated floor at or above the independent
baseline's — resampling's win is per-frame, the live path's regime, which no accumulation gate
can see. Debug views shipped as the lighting pass's M/age/W ramps plus the reservoir word-6
readback in the test, which is what the caps are actually asserted through.

### Phase 9 — Denoiser and production hardening
The denoiser is **shared by both tiers**, so it is built once here and benefits the low-spec path too.
- [ ] **A-SVGF** (SVGF + temporal-gradient adaptive history rejection). Backend-agnostic, unlike
      NRD which is Vulkan/DX-only and would break backend parity
- [ ] Feed the denoiser **demodulated** radiance (divide out albedo, re-modulate after) and a
      **history-confidence signal** derived from reservoir `M`/`age`/disocclusion state — without
      it the output is sluggish
- [ ] **Skip the pre-blur stage.** ReSTIR already densifies sparse data and fixes outliers far
      better; make the rest of the denoiser *more* conservative (shorter history, smaller final
      blur). Treat ReSTIR as a physically-motivated pre-blur
- [ ] Firefly controls: clamp radiance growth per frame (kajiya uses 10× previous), a **boiling
      filter** (kill reservoirs whose `luminance·weightSum` is a tile-level outlier), Jacobian
      rejection
- [ ] Disocclusion boost (temporarily raise spatial sample count)
- [ ] **Permutation sampling** to decorrelate histories — but note it erodes detail (§9)
- [ ] Moving lights: validation/re-trace frames (kajiya re-traces stored samples every 3rd frame
      and lowers `M` if radiance changed materially), plus aggressive age capping
- [ ] Performance: half-res tracing + upsample, reservoir compression (measure first), BLAS refit
      budgeting
- [ ] Mobile budget pass for the low-spec tier (iOS Metal)

**Gate:** Sponza-class scene at 60 fps on both backends for the high tier, and a documented mobile
budget for the low tier. Zero validation errors, no visible ghosting under camera motion.

### Phase 10 (optional, much later) — the LoD paper
Only once Phases 8–9 are solid *and* a dynamic geometry-LoD system exists. See §10.

---

## 9. Known pitfalls (consolidated)

**Bias and correctness**
- Never substitute the chosen candidate's `W_{X_s}` for `W_Y`.
- `1/M` MIS weights are legal only for iid candidates each covering `supp(p̂)`.
- Neighbour selection must depend only on the G-buffer, never on reservoir contents.
- Bias feeds forward — a biased reuse step is re-ingested next frame and can numerically explode
  within a few frames. Bias in *reuse* matters far more than bias in *shading*.
- Temporal MIS formally needs *last frame's* `p̂`, i.e. last frame's BVH. Nobody keeps two. Sign
  rule: over-estimating `p̂ⱼ(Xᵢ)` darkens, under-estimating brightens; a bad approximation
  typically does both in different screen regions.
- Reservoirs are distributions **at a point**, not over a volume — world-space grid/voxel
  reservoirs carry bias that is only now becoming controllable. Don't start there.
- Visibility reuse causes most of the hard-to-debug bias. Keep `V` in `p̂` and in the MIS weights
  until a validated baseline exists.
- A null sample is a legitimate outcome (`W_∅ = 0`). Do **not** immediately re-draw — that biases.
  Still include that input in others' MIS weights.

**Variance and artifacts**
- Confidence capping is mandatory. Course recommends 5–30, start 20; kajiya uses 10 diffuse /
  8 specular.
- Age capping is separate and also required for GI — cached `L_o` goes stale.
- Every spatial pass erodes detail unless a very aggressive normal cutoff is used; thin features
  can vanish. This is a real quality trade, not just a bias guard.
- Small bright emitters (candles) defeat ReSTIR GI — handle them as explicit lights via DI, don't
  hope BRDF-sampled rays find them.
- Two spatial passes of 8 samples multiply (8·8); one pass of 16 adds (8+8). Feeding spatial output
  back into the temporal loop raises effective spp enormously but drives correlation and detail
  erosion — Cyberpunk chose not to.
- Correlation is the tax for reuse, and it reduces the information available to a denoiser that
  assumes independence.

**Numerics**
- Keep `position` and `weight` in fp32; pack everything else.
- Guard the Jacobian for NaN/Inf (return 0) and guard `0/0` in the finalize step.
- Self-intersection: offset along the geometric normal with a scale-relative epsilon. A spurious
  self-hit becomes a *cached* bad sample that propagates for `maxReservoirAge` frames.

**Process**
- Build validation modes that cost nothing: disable spatial reuse via **radius = 0** rather than
  rewiring the render graph.
- Course Tip 3.9, worth quoting: *"Do you love difficult, very frustrating debugging? Implement all
  performance optimizations simultaneously, rather than starting with a baseline implementation."*

---

## 10. The linked paper: Real-Time Level-of-Detail Rendering with ReSTIR

Wang, Kettunen, Lin, Wyman, Wu, Zhao. SIGGRAPH Conference Papers '26, July 19–23 2026.
[Landing page](https://research.nvidia.com/labs/rtr/publication/wang2026levelofdetail/) ·
[PDF](https://research.nvidia.com/labs/rtr/publication/wang2026levelOfDetail/wang2026levelOfDetail.pdf) ·
[DOI 10.1145/3799902.3811100](https://doi.org/10.1145/3799902.3811100). No code release.

**Problem.** Every prior ReSTIR PT shift mapping locates a previous-frame surface point by
*(triangle index, barycentrics)*. When an object switches geometric LoD the topology changes,
triangle IDs become meaningless, the mapping fails, and temporal reuse resets for that object for
several frames. So enabling dynamic LoD *actively hurts* baseline ReSTIR — their Fig. 1: RelMSE
0.104 (base, highest detail) → **0.126** (base + LoD, worse than no LoD) → 0.092 (theirs).

**Method.** Replace the triangle-ID mapping with a **surface-point mapping through UV space**,
assuming all LoD levels share a consistent UV atlas: `g(x) = τ₂⁻¹(τ₁(x))`. Where UVs are
many-to-one (mirrored/tiled/symmetric), shifts must stay invertible, so Algorithm 1 does a
**greedy world-space nearest-neighbour matching** over the UV preimage sets. Inverting the UV map
needs a per-asset, per-LoD **auxiliary BVH in UV space**, traversed with all-hits ray tracing.
The shift Jacobian gains a factor `|u₂ × v₂| / |u₁ × v₁|` per vertex mapped through `g`. Built on
**reservoir splatting** (Liu et al. 2025) with **Area ReSTIR**'s reservoir format, storing object
ID + UV for *both* the primary hit and the reconnection vertex — their ablation shows mapping both
is necessary (RelMSE 0.127 both, vs 0.225 / 0.287 for either alone).

**Cost/benefit.** ~0.4–0.8 ms (2–4%) overhead for 1.2–2.8× RelMSE reduction — **only in the frames
around a LoD switch**. Outside switches it behaves identically to the baseline.

**Verdict: this is the last thing to add, not an early one.** It modifies one component *inside*
ReSTIR PT's temporal shift, and is meaningless until both (a) a correct ReSTIR PT with temporal
reuse, hybrid shift and Jacobians, and (b) a runtime dynamic geometry-LoD system exist. vkm has
neither, and `TODO.md:39` notes there is no LoD selection even in the raster culling pass. It also
assumes one LoD per object per frame — stochastic/per-ray LoD is explicitly *not* supported and is
their stated future work. Note it targets ReSTIR **PT**, not ReSTIR GI, so the plan above does not
build its base.

**What it would require:** per-asset LoD chains sharing one consistent UV atlas (the real cost —
meshoptimizer's `simplify` preserving texcoords gets close; Nanite-style clusters do not);
per-LoD UV-space BVHs with all-hits traversal; a larger reservoir (object ID + UV for two
vertices, and reservoir bandwidth is the usual bottleneck); shader-side greedy matching; and a
per-object "did LoD change?" flag for the cheap fast path.

**Cheaper mitigations to try first:** LoD hysteresis / deferred switching during fast camera
motion, dithered LoD crossfade, or confidence decay instead of a hard history reset.

**Sibling papers, same base, same late-phase classification:**
[Compatibility-Guided Neighbor Selection](https://research.nvidia.com/labs/rtr/publication/junkins2026compatibility/) ·
[Multi-Layer Reservoir Splatting (disocclusion)](https://research.nvidia.com/labs/rtr/publication/hong2026multilayer/) ·
[Reservoir Splatting (Liu et al. 2025)](https://research.nvidia.com/labs/rtr/publication/liu2025splatting/liu2025splatting_paper.pdf)

---

## 11. Reading list

### Start here — this one replaces reading the papers cold
**A Gentle Introduction to ReSTIR: Path Reuse in Real-Time** — Wyman, Kettunen, Lin, Bitterli,
Yuksel, Jarosz, Kozlowski, De Francesco. SIGGRAPH 2023 course.
Site: <https://intro-to-restir.cwyman.org/> ·
**Notes PDF (61 pp): <https://intro-to-restir.cwyman.org/presentations/2023ReSTIR_Course_Notes.pdf>**

A textbook, not a slide dump. Nine chapters from Monte Carlo basics → RIS → reservoirs/WRS →
cross-domain reuse with shift mappings and Jacobians → ReSTIR PT → optimizations → game
integration → "advice for getting started". Numbered Algorithms 1–7 in clean pseudocode and ~30
inline "Tip" boxes that are almost all hard-won bug-avoidance advice. The **only** source
presenting DI, GI, PT and volumetric ReSTIR in one consistent notation, and the only one that says
what *order* to build in. Its correction of the DI paper's `1/M` placement alone saves days.

### Foundations, in dependency order
1. **Importance Resampling for Global Illumination** — Talbot, Cline, Egbert. EGSR 2005.
   <http://justintalbot.com/publication/importance-resampling/>
   RIS. Take the two-stage sampling idea; don't take the notation (predates the modern `W`).
   Note it drops visibility from `p̂` for speed — the course explicitly says **don't** copy that
   until validated.
2. **Spatiotemporal Reservoir Resampling for Real-Time Ray Tracing with Dynamic Direct Lighting**
   — Bitterli et al. SIGGRAPH 2020. <https://benedikt-bitterli.me/restir/>
   The original. Take the reservoir struct, pipeline shape, M-capping, and the observation that
   its `1/M` weights are biased and need the correction pass.
3. **ReSTIR GI: Path Resampling for Real-Time Path Tracing** — Ouyang, Liu, Kettunen, Pharr,
   Pantaleoni. HPG 2021.
   <https://research.nvidia.com/publication/2021-06_restir-gi-path-resampling-real-time-path-tracing>
   **The target.** Short; read twice. 9.3×–166× MSE improvement over 1 spp path tracing.
   ★ Read alongside **Jiayin Cao's derivation walkthrough**, which is more useful to an
   implementer than the paper:
   <https://agraphicsguynotes.com/posts/understanding_the_math_behind_restir_gi/>
4. **Generalized Resampled Importance Sampling: Foundations of ReSTIR** — Lin et al. SIGGRAPH 2022.
   <https://d1qx31qr3h6wln.cloudfront.net/publications/sig22_GRIS.pdf> ·
   code <https://github.com/DQLin/ReSTIR_PT>
   Theory + ReSTIR PT. Read once Phase 8 produces images.

### Code to read
- **RTXDI** — <https://github.com/NVIDIA-RTX/RTXDI> ·
  GI doc <https://github.com/NVIDIA-RTX/RTXDI/blob/main/Doc/RestirGI.md> ·
  library <https://github.com/NVIDIA-RTX/RTXDI-Library>
  The single most useful artifact for this project. Read
  `Include/Rtxdi/GI/{Reservoir,TemporalResampling,SpatialResampling,BoilingFilter}.hlsli` —
  under 700 lines total, and it is the algorithm concretely, with every heuristic named.
  **License caveat: `LicenseRef-NvidiaProprietary`. Read for understanding; do not copy into vkm.**
- **kajiya** — <https://github.com/EmbarkStudios/kajiya> (**archived June 2026**).
  Read **`docs/gi-overview.md`** — the best *engineering* writeup of a non-NVIDIA ReSTIR GI, with
  concrete numbers (M caps, two spatial passes, candidate generation only 2 of every 3 frames with
  the third re-validating, 10× radiance clamp, near/far-field split). Its "known issues" list is a
  free pitfall catalogue. Note: it is **hardware ray traced**, not SDF-traced — SDF tracing is
  Lumen's approach, not kajiya's. Walkthrough:
  <http://lousodrome.net/blog/light/2022/06/05/overview-of-global-illumination-in-tomaszs-kajiya-renderer/>
- **Screen-Space-ReSTIR-GI** — <https://github.com/Alegruz/Screen-Space-ReSTIR-GI>. Falcor 5.1,
  derived from Daqi Lin's code; the closest thing to a readable ReSTIR-GI-specific research impl.
- **ReSTIR-Vulkan** — <https://github.com/lukedan/ReSTIR-Vulkan>. Clean Vulkan ReSTIR DI; good
  first reference for reservoir buffers and ping-pong in Vulkan specifically.
- **CIS-565-Final-VR-Raytracer** — <https://github.com/IwakuraRein/CIS-565-Final-VR-Raytracer>.
  Vulkan RT with ReSTIR DI + GI + denoiser; student project, so more readable than production code.

### Beginner-friendly, in reading order
1. Alan Wolfe — reservoir sampling
   <https://blog.demofox.org/2022/03/01/picking-fairly-from-a-list-of-unknown-size-with-reservoir-sampling/>
   then SIR <https://blog.demofox.org/2022/03/02/sampling-importance-resampling/>
2. Chris Wyman, *What is Resampled Importance Sampling?*
   <http://cwyman.org/blogs/introToReSTIR/introToRIS.md.html>
3. Kostas Anagnostou, *A gentler introduction to ReSTIR*
   <https://interplayoflight.wordpress.com/2023/12/17/a-gentler-introduction-to-restir/>
   Best "what am I even building" post.
4. Jiayin Cao, *Understanding the Math Behind ReSTIR DI*
   <https://agraphicsguynotes.com/posts/understanding_the_math_behind_restir_di/>
   Clearest explanation anywhere of why `W` works.

### Production integration and denoising
- **Cyberpunk 2077 – RT: Overdrive ReSTIR Integration** (SIGGRAPH 2023, 27 slides, very high signal)
  <https://intro-to-restir.cwyman.org/presentations/2023ReSTIR_Course_Cyberpunk_2077_Integration.pdf>
- **Rearchitecting Spatiotemporal Resampling for Production** — Wyman & Panteleev, HPG 2021.
  The engineering paper behind RTXDI (light sample tiling, presampling).
  <http://cwyman.org/presentations/2021_HPG_Productizing_ReSTIR.pdf>
- **Ray Tracing Gems II** ch. 49, *ReBLUR: A Hierarchical Recurrent Denoiser*. Read before wiring
  any denoiser. <https://link.springer.com/content/pdf/10.1007/978-1-4842-7185-8_49.pdf>
- SVGF — Schied et al. 2017; **A-SVGF** — Schied et al. 2018 (the Phase 9 target).

### Current state of the art (for the "what I'd do differently" list)
**ReSTIR PT Enhanced** — Lin, Kettunen, Wyman 2026.
<https://research.nvidia.com/labs/rtr/publication/lin2026restirptenhanced/>
2.74× average speedup: reciprocal neighbour selection, footprint-based reconnection criteria,
**duplication maps** that adaptively lower the confidence cap where samples are duplicated, and
unifying DI + GI into *one* reservoir set. Memory 431 → 265 MB/frame.

---

## 12. Remaining TODOs / open questions

- [ ] **Test scene.** **Partly answered 2026-08-06:** `resources/tests/gltf_furnace.gltf` (two
      unit cubes, albedo 1.0 and 0.5) is the analytic-validation fixture, and it is *more* analytic
      than a Cornell box would be — a convex body in a uniform environment has a closed-form
      answer, an enclosed room does not. What is still missing is a Cornell-box-style scene for
      **visual** comparison from Phase 7 onwards, where the interesting quantity is colour bleeding
      rather than a number.
- [x] ~~**CI runner bump**~~ **Decided 2026-08-04: add, do not replace.** `ubuntu.yml` gains one
      `ubuntu-24.04` Vulkan job whose only purpose is ray-query coverage — 24.04 carries Mesa 25.x
      through updates, well past the 24.1 that gave lavapipe `VK_KHR_ray_query`, so no PPA is
      needed. The eight 22.04 jobs stay: noble ships neither gcc-10/11 nor clang-11/12, and those
      are the compiler floor. **The job's first run passed without executing a single Vulkan
      test**: it looked for lavapipe's ICD manifest at one hardcoded path, Mesa 25.x on noble does
      not put it there, and an empty `VK_DRIVER_FILES` makes every device test skip through
      `VKM_REQUIRE_DEVICE` while the run still reports success. The directory was right and the
      *filename* had changed: Mesa 25.x ships `lvp_icd.json`, without the `.x86_64` suffix the
      glob required. The lookup now searches the standard locations by prefix and **fails the job**
      when it finds none, so this cannot recur silently. With that fixed the job skips nothing and
      **lavapipe reports `RayTracing: yes`** — so Phase 5's gate can be exercised in CI on Vulkan,
      which is what the runner was added for. The cost of keeping them is that **`dxc-linux` stays pinned** to
      v1.8.2505.1 (GLIBC 2.34) while Windows and macOS use v1.9.2602.24, so Linux CI still
      validates shaders with a different compiler than the other two platforms. Un-pinning it
      requires dropping 22.04 outright, which is a compiler-support decision, not a CI one.
- [ ] **Confirm `MTL4ArgumentTable` AS binding** empirically — no documented
      `setAccelerationStructure`; binding is inferred to go through `gpuResourceID`.
- [x] ~~**Low-spec technique choice.**~~ **Decided 2026-07-30:** fully dynamic (no bake), so the
      technique is a **raster-updated dynamic probe volume + SSGI contact term** (§5). Baked
      lightmaps and voxel cone tracing ruled out by the fully-dynamic + mobile constraints.
- [ ] **Probe relocation / classification.** A probe that lands inside geometry captures that
      geometry's interior and hands it to the lookup — saturated patches beside hard black ones on
      interior surfaces, visible in Sponza the moment the camera goes indoors. DDGI detects such
      probes and either nudges them into open space or marks them inactive; neither is implemented.
      The Chebyshev test cannot help: it weights a probe by whether the *surface* is visible to it,
      and cannot repair a probe whose own capture is wrong.
- [x] ~~**Probe budget and propagation latency**~~ **Measured 2026-08-01, and the risk was real.**
      `roundLength · ceil(ln f / ln h)` frames, verified against a GPU measurement. At the defaults
      (2048 probes, budget 32, hysteresis 0.97): **4864 frames / ~81 s to 90%**. Still open is what
      to *do* about it — the levers are hysteresis (logarithmic, cheap, costs image stability),
      budget (linear, costs raster time), and grid size. A mobile budget has to be argued against
      this number rather than against an impression, and it is the strongest argument for replacing
      the raster update with Phase 5's rays once they exist.
- [ ] **Multi-bounce for the high tier.** Once Phase 4's probe volume exists, decide whether ReSTIR GI
      queries it for `L_o` at the secondary hit (kajiya-style world-space irradiance cache) rather
      than continuing a path. This is the cheap route to multi-bounce and reuses Phase 4 directly.
- [ ] **Where GI lives.** Recommendation: technique passes and their resources in the engine
      (`src/vkm/renderer/`, they need RHI-level resources and barriers), debug UI and scene setup in
      a **GI sample** with a runtime technique switcher — mirroring how `VkmScene` owns cull/emit
      while `model_viewer` owns presentation. The sample must build on WebGPU (unlike `skybox`,
      which is excluded at `CMakeLists.txt:489`), since the low-spec tier targets it.
- [x] ~~**glTF textures.**~~ **Done 2026-08-04.** Base colour and metallic-roughness are imported,
      uploaded with a full mip chain, and sampled by `gbuffer.hlsl` and `probe_capture.hlsl` on all
      three backends -- so the probe volume now captures textured radiance rather than a
      per-material average. Vulkan and Metal index a sized bindless array; WebGPU cannot (WGSL has
      no array-of-handles type) and goes through descriptor set 3, one immutable table per material,
      which is what forced per-material draw batches. `vkm_material.hlsli` hides the split, so no
      shader tests `VKM_BACKEND_*`. Still open, each with its own prerequisite: **normal maps**
      (needs tangent generation -- the importer leaves a zeroed `TANGENT` when the asset omits one)
      and **emissive** (the G-buffer has no channel to carry it).
- [x] ~~**Direct lighting strategy.**~~ **Decided 2026-08-04: plain NEE, behind a seam.** Phase 7
      shades `L_o` at the secondary hit with ordinary next-event estimation, and every call site
      goes through one `shadeSecondaryHit()` function so the implementation can be swapped without
      touching the path code. ReSTIR DI is *not* built first, despite that being the usual ordering
      and despite ReSTIR PT Enhanced arguing for one unified reservoir set — because the scene has
      exactly one directional light today (`scene.h:63`; area/emissive representation is still open
      under Phase 6), and many-light resampling is the entire value of DI. Building it now would be
      building it against a light set that cannot exercise it. Revisit once Phase 6 supplies area
      lights *and* a reference to measure a unified reservoir set against.
- [ ] Decide whether to pursue ReSTIR PT later for unbiased specular, or accept the documented
      ReSTIR GI bias plus final-shading MIS.

---

## 13. Progress log

| Date | Phase | Note |
|---|---|---|
| 2026-07-30 | 0 | Document created. Toolchain research settled: SPIRV-Cross supports ray query → MSL, so Metal Shader Converter is not needed; MoltenVK has no RT, so macOS GI is Metal-only. |
| 2026-07-30 | 1 | **Spike passed.** HLSL `RayQuery<>` → `dxc -T cs_6_5` → SPIR-V (validates, `RayQueryKHR`) → `spirv-cross` MSL → metallib, in both plain-binding and Tier-2 argument-buffer configurations, all exit 0. AS lands at `[[id(0)]]` inside the descriptor-set struct, compatible with vkm's pinned bindless layout. Corrections: `SPV_KHR_ray_tracing` is not needed; procedural/AABB geometry compiles but is silently wrong at runtime, so triangles only. MoltenVK RT absence confirmed from local `vulkaninfo.txt`. |
| 2026-07-30 | — | Scope change: ReSTIR GI is the **high tier only**; a low-spec tier is now a first-class requirement (§5). Phase plan reordered so shared infrastructure and the low-spec tier precede acceleration structures and ReSTIR. |
| 2026-07-30 | 1 | `ray_query` PSO opt-in implemented (`VkmShaderStageDescriptor::requiresRayQuery` → `cs_6_5` + `-fspv-target-env=vulkan1.2`), covered by `TestPsoConfig`. End-to-end through `vkm-compiler`: **Vulkan passes** (SPIR-V 1.5, validates, ray query + descriptor indexing, using the real bindless macros); **Metal blocked** on registering the AS as a set-0 binding (§4.4) — Phase 5 work, not a toolchain gap. Found that `-fspv-extension` is a whitelist that would break every bindless shader; it is now deliberately not passed. CI Ubuntu runners are all 22.04 (Mesa 23.2.x), so Vulkan RT will have no CI coverage until bumped. Full suite: 164/164, 18602 assertions, Metal validation clean. |
| 2026-07-30 | 2 | Compute threadgroup size now comes from the shader's compiled SPIR-V via `VkmShaderCacheHeader::threadGroupSize` (cache v3) rather than the engine-global `kVkmComputeThreadGroupSizeX`, so 2D kernels can declare `[numthreads(8, 8, 1)]`. Metal reads it off the bound pipeline at dispatch. Verified end to end with an 8x8x1 shader on both targets; the Metal GPU cull test (visible count 1 -> 0 -> 1) covers the changed dispatch path. |
| 2026-07-31 | 2 | Added `barrierTextureForShaderRead`: the render-pass-writes -> shader-samples hand-off the engine's implicit-layout convention could not express. Real work on Vulkan only; Metal and WebGPU documented no-ops. Verified on Vulkan (170 tests) and Metal (166), plus a wasm build for the WebGPU stub; the new tests were checked to actually fail when the transition is sabotaged. |
| 2026-07-31 | 2 | Descriptor set 2 (per-pass resources) implemented on **Vulkan**: PSO JSON declaration, per-PSO `VkDescriptorSetLayout`, an immutable `VkmPerPassResourceTable`, and `bindPerPassResources()`. Verified end to end by a compute pass whose only resources are a set-2 uniform buffer and storage buffer, reading back `base + threadId` with validation clean. Metal and WebGPU are error stubs. This is the first genuinely per-PSO descriptor set in the engine, so a pipeline declaring it is no longer layout-compatible with one that does not. |
| 2026-07-31 | 2 | Descriptor set 2 on **Metal**, via discrete argument-table bindings (not a second argument buffer) so it stays out of the `pad_argument_buffer_resources` walk, mirroring set 1. vkm-compiler pins each declared binding per-PSO. The cross-backend test now runs the same shader and expected values on Vulkan and Metal, which is what shows the declaration means the same thing on both. Metal 169 tests, Vulkan 173, wasm builds. |
| 2026-07-31 | 2 | Descriptor set 2 on **WebGPU** (bind group 2), completing set 2 on all three backends. Compile-verified only: the WebGPU suite runs in headless Chrome and passes, but no shader can be compiled for that backend without a Tint/Dawn build, so the group-2 path has not executed. Flagged as a Phase 4 risk. |
| 2026-08-01 | 3 | G-buffer filled by a real MRT scene pass (`gbuffer.hlsl`) with camera motion vectors, then consumed by a fullscreen PBR deferred-lighting pass reading it through descriptor set 2. Verified on Metal by rendering the fixture scene and checking the shaded output: the lit pixel is non-black with the material's colour ordering preserved, uncovered pixels are exactly black, and doubling only the set-2 light buffer doubles the result. Metal 175 tests. |
| 2026-08-01 | 3 | Tone mapping migrated from dead GLSL to HLSL + PSO, fixing the missing white-point normalization and gamma encode it never applied. Fullscreen triangle factored into `vkm_fullscreen.hlsli` now that lighting and tone mapping both use it. **Phase 3 complete.** |
| 2026-08-01 | 4 | Probe volume storage + the lookup pass. `probe_lighting.hlsl` samples 8 probes trilinearly with Chebyshev visibility weighting; verified on Metal against CPU-authored atlases, including that fully occluded probes contribute nothing. Found that the update pass is blocked on the RHI having no viewport control (six cube faces cannot share a render pass) — recorded as the next decision. |
| 2026-08-01 | 4 | Added `setViewportAndScissor` to the RHI (all three backends), which unblocks the probe update: six cube faces can now share one render pass rather than needing six. Vulkan and Metal agree on the top-left origin, pinned by a test that also covers several viewports writing separate tiles in one pass. |
| 2026-08-01 | 4 | Probe capture (six cube faces in one render pass, via the new viewport control) and probe blend (octahedral integration + border + hysteresis). The write path now reaches the atlases that the read path already samples. Verified on Metal: per-face capture correctness, and a directional octahedral map whose border matches its mirrored interior. |
| 2026-08-02 | 4 | **Phase 4 closed out.** SSGI contact term (`ssgi.hlsl`) added into the same indirect target as the probe result with a one-to-one blend, so it can only brighten what the probes produced; verified by A/B screenshot with the term on and off. It brought in `vkm_random.hlsli` (PCG, seeded per pixel/frame/pass), which closes Phase 3's last open item. A two-room fixture (`gltf_two_rooms.gltf`) and a test that a walled-off probe captures no light from the bright room -- and the honest limit of that test: it covers the *capture* side, and disabling the Chebyshev term leaves it passing, because that term lives in the lookup. The gate is reconciled in §8 rather than declared met: the technique switcher waits on a second technique, the lookup-side leak check waits on probe_lighting being run against real atlases, and WebGPU is verified by building rather than by looking. |
| 2026-08-02 | 4 | **GI is on screen.** `src/samples/gi` drives probe refresh -> scene cull -> G-buffer -> deferred lighting -> probe lighting -> composite -> tone map, using two cull views in one frame (the probe capture cannot share the camera's frustum). It owns no shaders of its own -- every pass is an engine PSO -- so it builds on WebGPU too, and being the first app to own the G-buffer chain it settles Phase 3's gate. The new shared `gi_composite` pass is the technique interface's consuming end: it is the only place a technique's output is combined, and it carries ten debug views over the G-buffer channels. `VkmScene` gained the second cull view, with all views' count words packed at the front of both bookkeeping buffers (a batch uses one index into two payloads of different strides) and a staging region per view (both updates write host memory before either copy runs). Sponza renders with zero validation errors on Metal; the sample builds on Vulkan and WebGPU. Not verified: any automated check that a captured atlas feeds the lookup correctly -- both halves are tested, their junction is not (`TODO.md`). |
| 2026-08-02 | 4 | **WebGPU can compile and run shaders.** The blocker recorded since Phase 2 -- that `VKM_COMPILER_ENABLE_WGSL` needs a Dawn build no configuration provides -- was simply untrue: Dawn is vendored and pinned, the CMake wiring existed, and `run_sample.py` already built a WGSL host compiler. `run_tests.py` now does the same, so every engine PSO ships as WGSL in MEMFS and the set-2 path executes there. The gap was hiding **four** real bugs, all invisible to a compile: implicit-LOD sampling in non-uniform control flow; staging buffers left mapped at creation (WebGPU forbids the GPU or queue touching a mapped buffer, which broke `readbackTexture` and every `writeDirect` upload); the compute path never binding group 1, which the graphics path already did *and explained in a comment*; and every unpublished bindless singleton sharing one placeholder buffer, rejected both for overlapping writable-storage bindings and for mixing read-write with read-only use of one buffer -- that alone invalidated any WebGPU pass without a scene. Metal 193/193, Vulkan 191/191, both Release, WebGPU green. One test stays skipped with the cause unknown (`TODO.md`). |
| 2026-08-01 | 4 | **4.2c: the probe volume runs as a frame loop.** `VkmProbeVolumeUpdater` refreshes a round-robin budget of probes per frame (scene update -> probe-aimed cull -> one capture pass for every budgeted probe's six faces -> barrier -> two atlas blends). Making a *partial* update correct forced three changes: hysteresis moved to the blend hardware and the atlas lost its second copy (a swapped pair leaves un-refreshed probes alternating between stale values, and the old previous-atlas fetch used cell-relative UV — correct only for the one-cell atlas the test used); both passes turned out to need no per-probe constant buffer at all, since the face matrices are `P·R·T(−p)` and the position cancels, leaving only a position/tile/hysteresis small enough to push from the vertex stage; and the budget is clamped rather than wrapped so a round covers every probe exactly once. **Propagation latency measured** and asserted against the analytic model: geometric decay at `hysteresis` per refresh, so the defaults (2048 probes, budget 32, h = 0.97) need **4864 frames / ~81 s at 60 Hz** to shed 90% of a light change. Unusable as shipped; the levers are hysteresis, budget and grid size, all recorded in §12. Also reconciled §8's Phase 3 checkboxes with the code and noted that Phase 3's gate is unmet until 4.5 gives those passes an owning application. Metal 193/193 and Vulkan 191/191 in Debug, 192/192 and 191/191 in Release, validation clean throughout; both new tests were checked to fail when the blend factors and the round-robin clamp are sabotaged. Two things only Vulkan caught: releasing at destroy through the deferred reclaimer leaks past the allocator's teardown (it frees on a GPU timeline that will never advance again), and `~VkmScene()` is defaulted so both the new fixture and the pre-existing `runProbeCaptureTest` leaked a scene — Metal has no allocator assert, so both had been passing there. |
| 2026-07-30 | 4 | Low-spec tier must be **fully dynamic** (no bake) → technique decided: raster-updated dynamic probe volume (DDGI-style octahedral irradiance + distance moments, Chebyshev visibility) plus an additive SSGI contact term. Storage/sampling are update-mechanism-agnostic, so Phase 5's rays can later refresh the same volume, and ReSTIR GI can query it for multi-bounce `L_o`. |
| 2026-08-02 | 4 | **The push-constant ring gets a per-frame reset.** Metal and WebGPU emulate push constants with a ring the caller advanced but nobody reset, so entries a still-executing frame referenced were overwritten; callers worked around it by budgeting against `1024 / FRAME_BUFFER_COUNT`. The ring is now `FRAME_BUFFER_COUNT` regions with the cursor rewound per frame slot, which is safe by construction because `VkmRenderGraph` already calls `ensureCompleted()` on a slot before recording into it again. Lifts the probe budget from 32 to 128 on a one-batch scene. |
| 2026-08-02 | — | **Descriptor set 3 (per-draw)** on all three backends, by generalizing the set-2 machinery rather than duplicating it: the two sets differ only by index and by which declaration they validate against, so `VkmResourceSetKind` threads through and `VkmPerPassResourceTable` became `VkmResourceTableBase`. A table is now validated against the pipeline's **declaration**, not its pointer, so one table serves every vertex-layout permutation of a PSO. Sets 0-3 exactly exhaust WebGPU's default `maxBindGroups = 4` -- there is no room for a fifth. Metal's `MTL4ArgumentTable` caps buffer binds at 31 and sampler binds at 16, which is what bounds each set to 13 buffers / 8 samplers / 16 textures; exceeding it aborted device creation *as a hang*, only under `MTL_DEBUG_LAYER=1`. |
| 2026-08-03 | — | **glTF material textures, Vulkan and Metal.** `VkmMaterialData` grew to 64 B with four texture slots; `VkmScene` decodes, uploads and registers them at `build()`, keyed on `(path, colourSpace)` so an image used as both sRGB and linear gets two textures. `vkm_material.hlsli` is the only place that branches on the backend. Two findings a unit test could not have produced: the engine sampler was clamp-to-edge while glTF's default wrap is repeat, which made every pixel of DamagedHelmet sample the texture's bottom row (the fixture is UV-invariant, so only a real asset showed it); and PR 1's claimed headroom was wrong -- measured on Sponza the ring gives 128 at 1 batch but only 6 at 25. |
| 2026-08-03 | — | **Material textures on WebGPU**, through set 3 and per-material draw batches, plus the four validation bugs that had kept the gi sample from ever producing a WebGPU frame: bindless singleton bindings numbered 5-8 instead of 4-7 on the stated but false reasoning that this backend "shifts by one for the ring"; the argument buffer being writable-storage and indirect in one render pass, which WebGPU forbids at *bind group* scope rather than shader scope (set 0 now exists in a compute shape and a graphics shape); and a `git checkout` that had silently reverted the sample's material-table wiring in an earlier commit. Validation errors went **1812 to 0**, and a WebGPU frame was looked at for the first time. |
| 2026-08-04 | — | **Mipmaps for material textures.** Built on the CPU and uploaded level by level -- no new GPU path, since `uploadToTexture` always took a mip index. The load-bearing detail is sRGB: averaging gamma-encoded bytes averages the wrong quantity (half black + half white gives 128 where the answer is ~188), and it is invisible on any solid-colour fixture. `vkm_material.hlsli`'s WebGPU branch had to move from `SampleLevel(..., 0)` to `Sample()` or the chain would have been uploaded and never read; the comment justifying the pinned LOD was factually wrong about its callers. Sponza's roof: mean gradient over the minified region **30.5 to 11.1**. |
| 2026-08-04 | 4 | **The GI was green because Metal 4 render passes had no barriers.** Metal 4 does no automatic hazard tracking and the backend only emitted barriers around compute dispatches and blits -- nothing around render passes. The handoff was meant to come from a compute subgraph calling `barrierTextureForShaderRead`, which records nothing on Metal and binds no pipeline, so no encoder opened and no barrier ever issued. The probe blend read the capture atlas mid-write; because hysteresis *is* the blend hardware, `NaN * 0 + src * 1` is still NaN, so a poisoned cell never recovered. The irradiance atlas held 273k NaN in R and B against 21k in G -- which tone-maps to exactly (0,255,0) -- and the distance atlas negative means, so the Chebyshev test rejected every probe and the indirect term collapsed to zero. Render passes now carry the same barrier pair the compute path uses. Both atlases read back with zero NaN; 900 frames cost 4m34s against 4m31s. |
| 2026-08-04 | 5 | **Two Phase 5 pre-decisions settled (§12).** CI gains one `ubuntu-24.04` Vulkan job for lavapipe ray-query coverage rather than moving the matrix -- 24.04 carries Mesa 25.x so no PPA is needed, but noble has neither gcc-10/11 nor clang-11/12, so the eight 22.04 jobs stay and `dxc-linux` stays pinned to v1.8 against the other platforms' v1.9. Direct lighting at the secondary hit will be plain NEE behind a `shadeSecondaryHit()` seam, not ReSTIR DI first: the scene has one directional light, and many-light resampling is the whole point of DI, so building it now would be building it blind. |
| 2026-08-06 | 5 | **The Metal acceleration structure test is in the suite.** Registering it had been killing the whole Metal run with mimalloc heap assertions, which were entirely the backward-cpp signal-handler recursion already in `TODO.md`. The real abort was Metal's debug layer on `buildAccelerationStructure:` with a nil scratch buffer: `recordBuild` passed an empty `MTL4BufferRange` where the Vulkan side has always refused outright, because a static structure's scratch is freed after its initial build. It passed standalone only because `MTL_DEBUG_LAYER=1` comes from `run_tests.py` and not from the fixture. A Vulkan fixture over the same shared body was added at the same time, so the Vulkan implementation -- which has never executed anywhere -- runs for the first time in CI's lavapipe job. Metal 213/213 (20452 assertions) Debug and Release, validation clean; Vulkan 209/209, reported SKIP because the shared body honestly skips without ray tracing. |
| 2026-08-06 | 5 | **A scene builds its own acceleration structures.** `VkmScene::buildAccelerationStructures()` makes one bottom-level structure per pooled mesh and one rebuildable top-level structure over the objects, described as ranges into the geometry pool's existing buffers -- no vertex data duplicated. The pooled-buffer trap §8 warned about never fired: the pool already forces the committed allocation path for a bindless reason, so `AllowAccelerationStructureInput` only adds the build-input usage bit, and it is added only where the device reports ray tracing (that usage is illegal without `VK_KHR_acceleration_structure` enabled, which is every MoltenVK device here). The honest limit is that **the test cannot see whether the geometry offsets are right**: zeroing both leaves it passing, because a wrong-but-in-range address builds over the wrong triangles and nothing traverses the result until 5c's ray-query gate. What it does catch, by sabotage, is a mesh range reaching the build empty. Metal 214/214 (20489 assertions) Debug and Release, validation clean. |
| 2026-08-06 | 5 | **Phase 5c: the gate runs.** The scene's top-level structure is bound in set 0 at its own binding (not a `VkmBindlessSingletonBuffer` entry -- its descriptor type is an acceleration structure on Vulkan and an `MTLResourceID` on Metal), and a compute shader ray-casts a loaded glTF scene through it. The §4.4 Metal blocker fell to the predicted `add_msl_resource_binding` entry, but **not** with the basetype §8 specified: `SPIRType::AccelerationStructure` throws "Unexpected argument buffer resource base type" for every shader in the tree, because that registration only selects an index category and a padding width, and the accepted list is scalars/Image/Sampler/SampledImage. A scalar is correct -- an acceleration structure is emitted as `[[buffer(index)]]`. Confirmed in the MSL: `acceleration_structure<instancing> g_Scene [[id(12293)]]`, query lowered to `intersection_query<instancing, triangle_data>`. Vulkan drops the binding on a non-RT device, where it is not a legal descriptor type. Metal residency comes from the pool's `MTLResidencySet` rather than per-encoder `useResource:`. **The gate closes the offset hole recorded two days ago**: the traced mesh is the second in the pool and its object sits at (10, 20, -1), so zeroing the pool offsets and forcing the instance transform to identity each fail it -- both verified by sabotage, both silent before. |
| 2026-08-06 | 6 | **The reference path tracer, and a furnace gate that is exact rather than convergent.** Brute-force accumulating path tracer over Phase 5's ray query, reading the scene entirely through sets 0 and 1. The furnace fixture's answer is analytic: a convex diffuse body in a uniform environment reflects exactly `albedo * L`, and cosine-weighted Lambertian sampling cancels the cosine against the pdf so the throughput multiplier is the albedo with no pdf division anywhere -- which makes the estimator **zero-variance** on this scene and lets the tolerance be 1e-3. An albedo-1 body is invisible, so the white furnace assertion needs no knowledge of where it projects. Two things had to move: ray-tracing PSOs cannot sit in `resources/Pipelines/Engine/` (loaded wholesale at startup on every backend, and compiled for every backend -- MoltenVK cannot create one and WebGPU cannot compile one), and **Metal never bound descriptor set 1 for compute** -- the graphics branch did and said in a comment that it did so for every pipeline, but the scene's cull and emit passes read no camera so nothing had noticed. Same gap WebGPU had on 2026-08-02. Sabotage-verified at 5% energy created per bounce. Metal 217/217, validation clean. |
| 2026-08-06 | 7 | **1-spp indirect, and the two bugs its convergence gate found.** A deferred GI pass taking its primary hit from the G-buffer and continuing through the same `vkmTracePath` the reference uses, so accumulating it must converge to the reference. The shared seam is `vkm_path_tracing.hlsli`: `VkmSurfaceHit` (the LoD-forward hit encoding), `vkmVertexMapping()` and `vkmShadeSecondaryHit()`. The gate's first honest run scored MSE 0.030 -- **the G-buffer's geometric normal pointed away from the camera**, so every secondary ray started inside its own wall; nothing had ever consumed that channel, and the first pass to offset a ray along it is what found it (0.030 to 6.2e-4). Before that it scored a *perfect* MSE 0 while the reference was empty: both passes loaded the same PSO directory, and `loadPipelineState` **replaces** an entry rather than skipping it, destroying the pipeline the other held. The test now proves both images non-empty before comparing, because a metric that returns 0 for no-data makes a missing estimator look perfect. Sample count chosen so the noise floor (2.4e-4) sits below a one-bounce error (7.3e-4). Metal 218/218; the gate itself is Vulkan-registered pending an unresolved Metal validation-layer issue. |
| 2026-08-06 | 7 | **The third bug the convergence gate found: an acceleration structure built before it was resident.** `VkmDriverBase::newAccelerationStructure` calls `onResourceInitialized()` -- which is what adds a resource to Metal's residency set -- *after* `initialize()` returns, and `initialize()` is where the synchronous build happens. Every structure was therefore built while unmapped: a bottom-level build wrote into memory that was not resident, and a top-level build read bottom-level structures that were not either. It worked whenever that memory happened to be resident anyway, so it depended on what the process had allocated first and Metal reported nothing either way -- the gate saw it as zero ray hits under `MTL_DEBUG_LAYER=1` only after another test case had run. What cracked it: rebuilding the freshly built structure once made it work every time, while a full `waitIdle` did not and the instance ids were byte-identical at build and rebuild, so it was neither synchronization nor contents. The structure now registers itself before recording its build, its scratch and instance buffers too, and the synchronous build records through the same `buildAccelerationStructure` entry point the per-frame rebuild uses. Gate registered on both backends; Metal 218/218 under validation. |
| 2026-08-06 | 8 | **8.1 + 8.3, and the packing bug the sub-step gate caught.** A 32-byte reservoir (fp32 position, octahedral snorm16 normal, RGB9E5 radiance, fp32 W, 8-bit M and age), one buffer with slices and per-pass slice indices, one traced sample per pixel written into it, and a resolve that shades `f_s * cos * L * W` from it. Two slices rather than `FRAME_COUNT`, because `VkmRenderGraph`'s per-slot `ensureCompleted()` already answers the frames-in-flight question -- the count is set by what resampling needs. **The gate is 'is it the same estimator', not 'does it converge':** with one candidate RIS reduces to `W = 1/p_source`, and the pass shares gi_indirect's random stream on purpose, so the two see the same direction at every pixel and may differ only by the reservoir round trip. It scored MSE 5.5e-4 -- 7.4% dark -- because RGB9E5's exponent bias was written 16 instead of 15. That is not a visible off-by-one: pack and unpack share the scale, so small values round-trip perfectly and every channel whose mantissa lands above 511 silently clamps. Fixed: **9.7e-7**, two orders under the convergence gate. Slice index sabotage-verified. |
| 2026-08-06 | 8 | **8.2 and 8.4: the neighbour LUT, and a spatial pass that is honest about not being verified.** 256 R2 points through a concentric disk map, and a spatial resampling pass that picks neighbours from the G-buffer alone, applies the reconnection Jacobian, and runs the second loop for the bias-correction denominator with a visibility ray each. The estimator arithmetic is **measured** correct: with that ray bypassed the mean is within 0.015% of the un-resampled one, which is what says the Jacobian, the receiver-side target function and the denominator are right. With the ray in place the image is 13.8% bright and the verdict is insensitive to the ray's own inputs -- origin, target offset, surface from an array vs recomputed, rolled vs unrolled, `ACCEPT_FIRST_HIT` vs closest-hit all give a bit-identical result, while bypassing it does not, and the same function returns visible for the call outside the loop. Landed off by default and left unchecked in the plan rather than tuned until the number looked right. |
| 2026-08-09 | 8 | **8.4 verified: the 13.8% was visibility in the denominator but not in the merge weights.** Z's per-participant ray evaluated `p̂·V` while the merge loop evaluated `p̂` with no ray at all, so every genuinely occluded participant left the divisor while its weight stayed in the sum — brighter by exactly the occluded fraction, which on the open Cornell box is ~14%. That is also why the recorded symptoms pointed away from the ray itself: its verdicts were correct (input changes bit-identical), bypassing it made Z consistent with the rayless numerator again (mean within 0.015%), and `_neighbourCount = 0` removed the asymmetry with the ray still present. One added ray in the merge loop — which is §2's re-traced reconnection visibility, so it also closes the wall-leak path — took the ratio to 0.9977 and the MSE to the 1-spp baseline's own floor (2.44e-4 vs 2.41e-4, Metal). The gate is now asserted: mean ratio within 1%, baseline MSE/RelMSE thresholds. The environment-flag Jacobian special case TODO.md called for was dropped rather than added: at the 1e5-unit stand-in distance the Jacobian's ratios collapse to 1 on their own, already documented at `vkmReservoirJacobian`. |
| 2026-08-09 | 8 | **8.5: temporal resampling, static-verified, and the reservoir readback earned its keep twice.** Three slices now (fresh + a parity-ping-ponged history pair; only the temporal output re-ingests), a rayless per-stage p̂ = luminance·cos consistent between numerator and Z, history taps validated against the previous G-buffer only, confidence cap 20 / age cap 32 as push constants, and `_prevCameraPositionWorld` added to the frame constants (368 → 384 B) because a history camera distance is radial from the *previous* eye. The image gates alone said "fine" while M never left 1 — the readback of reservoir word 6 is what caught the test's own frame-constants leaving the previous eye at the origin (every tap depth-rejected). It then caught the second bug at +0.9% mean: `W = 0` histories treated as invalid, dropping their confidence from Z — §9's null-sample rule, and the same shrink-the-denominator asymmetry as 8.4. Null samples now merge with zero weight and full M: ratio 1.002, max M exactly 21, max age exactly 32. Metal 246/246. |
| 2026-08-09 | 8 | **8.6: the reservoir gets its second consumer.** `gi_restir_lighting` shades the resampled reservoirs into the indirect-radiance texture as a fullscreen graphics pass, emitting incoming irradiance per gi_composite's contract (albedo and 1/π applied once, by the composite — the compute resolve keeps applying them itself because it feeds the MSE gates, and the two must not be conflated). `recordAccumulate` split into `recordResample`/`recordResolveAccumulate`; lighting constants ride a per-frame staged uniform buffer because the parity-dependent slice indices cannot live in an immutable table. M/age/W debug ramps ride the same constants. The gate drives one frame the way a live renderer will and compares the fp16 target (read raw — `readbackTexture` clamps HDR to 8-bit) composited with the G-buffer's own albedo against the accumulated mean: 0.979. The final visibility ray is deferred with its flag plumbed: fragment-stage ray query is an unproven compiler path, and on static scenes the merge rays + history validation leave it nothing to catch. Metal 246/246. |
| 2026-08-09 | 8 | **ReSTIR is on screen, and the §5 interface passed its real test.** The gi sample gained `gv_gi_technique` / an ImGui combo (shown only where the device reports RayTracing, clamped back to probes where it doesn't or where the RT load / AS build fails), builds the scene's acceleration structures at load, loads the RT PSOs exactly once, and — when ReSTIR is active — skips the probe refresh, probe lighting and SSGI, recording instead one compute subgraph (generate → temporal → spatial) and one fullscreen lighting draw into the *same* `_indirectTarget`. The composite and its table were untouched, which is the interface holding. The G-buffer flip hazard is solved in the engine: `VkmRestirPass` builds every G-buffer-binding table twice, roles swapped, and the record calls take a parity — the count of `advanceFrame()` calls since the pass was built. On resize the whole pass object retires for `FRAME_BUFFER_COUNT` frames (its destroy() deletes tables immediately, which in-flight frames may hold). Verified headless on the Cornell fixture: composite and indirect-only screenshots on Metal, zero validation errors — the indirect view shows the smooth, occlusion-graded environment irradiance a confidence-21 temporal estimator should give, not 1-spp noise. Honest limits: the scene's directional light is invisible to the traced passes (restir.md §12 — environment radiance is the knob until area lights exist), and camera-motion ghosting is checked interactively, not headless. Metal 246/246. |
| 2026-08-07 | 5 | **The acceleration structure lifetime bugs the first ray-tracing CI job found.** Everything above had been written against a device that reports no ray tracing, so lavapipe running it was the first execution anywhere. It reported `VUID-vkDestroyAccelerationStructureKHR-...-02442` and died with SIGSEGV, and the cause was three separate things stacked. **One counter used for two purposes:** `beginCommandBuffer()` takes a timeline value and `submit()` allocates and signals another, so `waitIdle()` waiting on the last *allocated* value is correct only while every allocation is followed by a submit -- and one test deliberately begins a command buffer it never submits. Metal blocked for the full timeout; Vulkan's `vkWaitSemaphores` returned `VK_TIMEOUT` and **the result was discarded**, making the wait indistinguishable from success. **The deferred reclaimer was deferring nothing:** `recordUsage` was called only by `VkmRenderGraph::execute`, and only for resources a pass declared, so a structure built through a bare queue submit reached the reclaimer with no usages at all -- and an entry with no usages is ready on the worker's next 4 ms poll. **And a timeline wait is not the API considering the work retired:** the engine called `vkDeviceWaitIdle` nowhere outside the swapchain, and adding it to `VkmDriverVulkan::waitIdle` is what finally cleared the VUID. |
| 2026-08-07 | 7 | **Vulkan culled every front face, and two gates had to move.** The deferred GI test reported **zero** covered pixels on lavapipe while its reference path tracer covered all of them -- the ray tracing was fine and the rasteriser was drawing nothing. `toVkFrontFace` inverted the enum behind a comment saying not to "fix" it, on the reasoning that vkm-compiler's `-fvk-invert-y` mirrors screen-space winding so inverting here cancels it. What that misses is that **Metal flips too**, in its viewport transform, because its NDC is +Y up and its framebuffer origin is top-left: one flip each, nothing left to cancel. Settled by measurement after two reasoning attempts got it wrong -- the same scene through both backends fills 2016 of 4096 texels, bounding box x[0..62] y[1..63], covered-mask centroid (20.7, 42.3), pixel-identical and therefore identically wound, with the indirect argument buffers matching byte for byte to rule out the cull and emit passes. It surfaced only now because the two tests that draw a scene were **Metal-only**; `TestGBufferRenderVulkan.cpp` instantiates them for Vulkan, needs no ray tracing, and so reproduces this class of failure on MoltenVK locally. Also found: the Cornell G-buffer was sampled while still in `COLOR_ATTACHMENT_OPTIMAL` (`VUID-vkCmdDraw-None-09600`), and Phase 7's convergence threshold cannot be shared -- lavapipe's floor at 1536 samples is 7.9e-4 against Metal's 2.4e-4, and eight times the samples moves it 14%, so the excess is systematic rather than variance and the threshold is now per-backend (6.0e-4 Metal, 1.0e-3 Vulkan) with the unexplained ~5.5e-4 in `TODO.md`. The last failure was not ours: lavapipe segfaults while compiling the internal shaders it builds acceleration structures with, proven by `MESA_SHADER_CACHE_DISABLE=true` making *every* run crash instead of only the first, so the job warms the cache and judges the second run. Full suite on lavapipe: **222/222, 9199 assertions**; 17/17 CI jobs. |
| 2026-08-08 | 5 | **Review follow-ups.** Acceleration structure geometry now names its vertex and index ranges with two `VkmBufferView` handles rather than a (buffer, byte offset) pair each -- a range inside a buffer someone else owns is what a view already means here, and naming it twice invited the halves to disagree. The scene makes them with `VkmBuffer::createView`, so they are children of the pool buffer and released with it. Both backends resolve one through the common API, using the view's *relative* offset: `VkmBufferViewVulkan::getOffset()` is absolute and would count a pooled buffer's own offset twice on top of the parent's device address. Separately, `recordBuild` is gone from both backends' resource classes -- a resource describes itself and the command buffer turns that into a command -- which also stopped Vulkan's `initialize()` reaching for the raw `VkCommandBuffer` and thereby skipping the recording-rule checks and the usage recording the reclaimer waits on. |
