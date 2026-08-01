# ReSTIR GI in vkm

Living document for the ReSTIR GI implementation: what the technique is, the staged plan,
current status, remaining TODOs, and the reading list. Updated at the end of every phase.

**Status:** Phases 1-3 complete (toolchain, shared infrastructure, G-buffer + deferred lighting).
Next: Phase 4 — the GI technique interface and the low-spec probe tier, the first real GI.

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
      *is* this concept, and WebGPU's own model already treats one as immutable. **Compile-verified
      only**, see the risk below
- [ ] ⚠ **Make WebGPU runtime-verifiable — blocks trusting Phase 4 there.** The WebGPU suite does
      run locally (headless Chrome, and it passes), but no shader can be *compiled* for it:
      `VKM_COMPILER_ENABLE_WGSL` is OFF and turning it on builds Tint through a full Dawn
      ExternalProject that no local or CI configuration provides (`TODO.md:11,23`). So the set-2
      test is excluded on WebGPU, and its group-2 path has never executed. This is a pre-existing
      engine limitation affecting *all* WebGPU shader work, not something set 2 introduced — but
      Phase 4 puts real weight on that backend, so it should be resolved before, not after
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
- [ ] MRT G-buffer: linear depth, world/shading normal, **geometric normal** (for ray offsetting),
      albedo, roughness/metallic, material ID, **3-component motion vectors**
- [ ] **Previous-frame G-buffer** (double-buffered) — temporal reuse reads last frame's
      normal/depth/material to reject taps and evaluates `p̂` in the previous domain
- [x] **Extended `VkmFrameConstants`** (272 -> 368 bytes, stride unchanged at 512): added
      `_prevViewProjection`, `_viewportSize` (xy = pixels, zw = reciprocal) and `_frameIndex`
      (monotonic, distinct from the 0..FRAME_COUNT-1 slot index). The engine owns the two
      frame-to-frame fields since a camera holds no such state, and seeds `_prevViewProjection`
      with the current matrix on the first frame so reprojection starts as the identity rather
      than a violent cut. `TestCamera` now pins every member offset — the C++ struct, the HLSL
      mirror and vkm-compiler's Metal pin are three halves of one ABI that nothing else checks.
      Deliberately **not** added: jitter and previous camera position, which have no consumer
      until TAA (Phase 9)
- [ ] Reconstruct world position from depth + inverse view-projection rather than storing it
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
- [ ] Hash-based stateless RNG seeded per (pixel, frame, pass)

**Gate:** G-buffer channels visualizable via a debug view; reprojection debug view stable under
camera motion with no drift. Works on all five platform combinations.

### Phase 4 — GI technique interface + low-spec tier ⭐ first working GI
The de-risking phase. Delivers visible GI on **all five** platform combinations with no ray tracing.
Technique decided in §5: **raster-updated dynamic probe volume + SSGI contact term.**

- [x] Decide the low-spec technique (§5)
- [ ] Define the **technique interface**: a technique owns its passes and resources and produces an
      indirect-radiance texture (+ optional confidence channel) for one shared composite pass
- [ ] Runtime selection gated on `VkmDriverCapabilityFlags::RayTracing` + a quality setting, not `#ifdef`
- [x] **4.1 Probe volume storage.** `VkmProbeVolume`: probe grid, both atlases (8×8 irradiance,
      16×16 distance/moments), double-buffered for hysteresis blending, with a **one-texel border
      per probe** so bilinear taps near an octahedral seam read the right neighbour rather than the
      far side of the map. The border is in from the start because the atlas layout is what every
      addressing helper and both shaders depend on. Distance uses RGBA16F with two channels unused —
      the engine has no two-channel format, same constraint the G-buffer hit
- [ ] **4.2 Probe update pass** — round-robin a per-frame probe budget; rasterize a very low-res cube
      view per probe reusing `VkmScene`'s existing cull/emit draw path, shade it, convert to
      octahedral, blend into the atlases with hysteresis.
      ✅ **Unblocked:** `setViewportAndScissor` now exists on all three backends, so a probe's six
      cube faces can share one render pass instead of needing six
- [x] **4.3 Probe sampling.** `probe_lighting.hlsl`: trilinear over the 8 surrounding probes,
      weighted by the **Chebyshev visibility test** against the distance atlas, plus normal bias and
      a smoothed backface term. Addressing and the octahedral mapping live in
      `vkm_probe_volume.hlsli`, shared with whatever fills the atlases. Tested by authoring the
      atlases from the CPU, which isolates the read path — the two cases are "a visible grid returns
      its irradiance" and "occluded probes contribute nothing", the second being exactly the wall
      leak the distance atlas exists to prevent
- [ ] **4.4 SSGI contact term** — depth-buffer ray march, added on top of the probe result. Keep it
      strictly additive and strictly in this tier (see the warning in §5)
- [ ] **4.5 GI sample** with a runtime technique switcher and per-term debug views (probe irradiance,
      probe depth/moments, Chebyshev weight, SSGI only, composite). Must build on WebGPU
- [ ] Measure and record the light-propagation latency (frames for a moving light to converge) — this
      is the known weak point of raster-updated probes and needs a number, not an impression

**Gate:** low-spec GI renders correctly on Vulkan, Metal **and** WebGPU, with zero validation errors;
the technique switcher works at runtime; no visible leaking through walls in a two-room test scene.
This is the interface's real test — one consumer proves nothing.

### Phase 5 — Acceleration structures in the RHI
Where the high tier starts.
- [ ] New resource type + `VkmDriverCapabilityFlags::RayTracing` (pattern at `driver.h:34-59`)
- [ ] Vulkan: enable `VK_KHR_acceleration_structure`, `VK_KHR_ray_query`,
      `VK_KHR_deferred_host_operations`; BLAS per mesh, TLAS per scene
- [ ] Metal: `MTL4PrimitiveAccelerationStructureDescriptor` / instance descriptors via
      `MTL4ComputeCommandEncoder` (Metal 4 folded the AS encoder into the compute encoder)
- [ ] Build BLAS from the existing `VkmSceneGeometryPool` so no vertex data is duplicated.
      **Triangles only** — document why at the declaration site (§4.2)
- [ ] Refit on transform change; full rebuild only on topology change
- [ ] Bind one TLAS as a bindless singleton — extend `VkmBindlessSingletonBuffer`, all three
      backends' managers, and `vkm_bindless.hlsli`. **On Metal it must also get an
      `add_msl_resource_binding` entry** (`basetype = SPIRType::AccelerationStructure`, `count = 1`,
      using the `msl_buffer` index category): `pad_argument_buffer_resources` walks *every*
      registered set-0 binding to synthesize padding members, so an unregistered one shifts the whole
      argument-buffer layout for every shader (`main.cpp:376-420`)
- [ ] Confirm `MTL4ArgumentTable` AS binding empirically (no documented `setAccelerationStructure`)
- [ ] Prefer `MTLResidencySet` over per-encoder `useResource:` (fits vkm's existing residency code)

**Gate:** a compute shader ray-casts a loaded glTF scene and writes hit/miss + `t` matching a CPU
reference for known rays, on Vulkan and Metal.

**RHI contract note (Phases 2 and 5):** `backend/common/AGENTS.md:556` requires that no new pure
virtual is added without implementing it in **all** backends — so every RT and barrier entry point
needs a **WebGPU error-logging stub**, per the `copyTexture`/`registerTexture` precedent
(`TODO.md:29,42`). Gate on the capability flag, not `#ifdef`. Public base-class signatures must not
change.

### Phase 6 — Reference path tracer
**Do not skip.** Without ground truth you cannot distinguish a Jacobian bug from noise.
- [ ] Accumulating brute-force path tracer in compute using Phase 5's ray query
- [ ] Area/emissive light representation — one `_lightDirection` (`scene.h:63`) is not enough
- [ ] MSE/RelMSE comparison utility, so later phases produce a *number*
- [ ] Split-screen accumulation mode: ground truth vs live pipeline

**Gate:** white furnace test passes (uniform environment, albedo 1 → output equals input); energy
conservation holds on a small diffuse test scene.

### Phase 7 — 1-spp indirect, no resampling (baseline to beat)
- [ ] Single indirect bounce, one ray per pixel, no reservoirs. Deliberately noisy
- [ ] Direct lighting at the secondary hit (`L_o` = emission + NEE + optional continued path)
- [ ] Hit-point encoding, **forward-compatible with the LoD paper**: reserve
      `(instanceID, float2 uv)` alongside `(primitiveID, barycentrics)`, and put "locate this
      surface point" behind one `vertexMapping()` function rather than inlining it. Free now,
      expensive to retrofit
- [ ] Ray origin offset along the **geometric** normal, scale-relative epsilon; reconnection
      visibility rays use `t_max = (1-ε)·distance`

**Gate:** converges to the Phase 6 reference when accumulated. Proves sampling and BRDF are right
*before* reservoirs — if this is biased, ReSTIR will be, and far harder to see.

### Phase 8 — ReSTIR GI core
Incremental, with measured RelMSE against Phase 6 at every sub-step.

- [ ] **8.1 Reservoir buffer.** RTXDI-style packing is worth copying:
      `position` fp32 (feeds a `1/d²` Jacobian — fp16 is not viable), `normal` as octahedral
      `snorm2x16`, `radiance` as **LogLuv or RGB9E5** (not fp16×3 — HDR indirect clips and
      quantizes badly), `weight` fp32, and `M`/`age` as 8-bit fields (so `M ≤ 255`, `age ≤ 255`).
      ~32 B/reservoir; ~16 MB per slice at half-res 1080p.
      Use **one buffer with multiple array slices** and per-pass input/output slice indices rather
      than separate buffers — it makes bypass/validation modes trivial.
      **Triple-buffering hazard:** `FRAME_COUNT = 3` (`base/common.h:21`), so frames N-1/N-2 may
      still be executing when N is recorded. Either allocate `FRAME_COUNT` slices or rely on the
      per-slot `ensureCompleted()` deliberately — decide and document before writing the passes.
- [ ] **8.2 Neighbour offset LUT** — a small buffer of precomputed low-discrepancy disk offsets,
      indexed with a mask. Cheap, avoids per-pixel disk sampling, gives a stable pattern
- [ ] **8.3 Sample generation pass** — trace one ray, fill a fresh reservoir (`c = 1`, `age = 0`)
- [ ] **8.4 Spatial resampling first** (easier to validate than temporal — no scene change between
      samples): merge `k` neighbours (start 3–5, radius ~30 px) with normal/depth/material
      rejection (relative depth ~10%), the reconnection Jacobian, Jacobian validation, and a
      visibility ray per accepted neighbour. Then the second loop over accepted neighbours for the
      bias-correction denominator
- [ ] **8.5 Temporal resampling** — camera and scene **static first**, then moving. Reproject via
      motion vectors, ring of jittered fallback taps, optional zero-motion fallback, confidence
      cap (start 20), separate **age cap**
- [ ] **8.6 Shading/resolve** — `f_s · cos · L_o · W`, optional final visibility ray, composite
      with direct light
- [ ] **8.7 Final-shading MIS** on low-roughness surfaces (see §2's bias note). Cyberpunk's cheap
      version: `BRDF·(1−roughness²) + ReSTIR·roughness²`

> **Hard rule (course Tip 4.1):** choose which neighbours to reuse based **only** on the G-buffer.
> Never on the samples or weights stored *in* the neighbours' reservoirs — that conditions the
> probability space and biases the result. Same rule for resetting confidence on disocclusion.

**Gate per sub-step:** RelMSE versus Phase 6 must improve or at least not regress, and the mean
must match **on a diffuse-only scene** (specular secondary hits are legitimately biased — see §2).
A shifted mean on diffuse means a broken MIS weight or missing Jacobian. Add a debug view per pass
(reservoir `M`, `W`, `age`, chosen sample position, temporal-vs-spatial contribution) — these bugs
are invisible in the final image.

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

- [ ] **Test scene.** No Cornell-box-style scene exists; `scripts/download_scenes.py` offers only
      `DamagedHelmet` and `Sponza`. Hand-author a small `.gltf` in `resources/tests/` (following
      `gltf_triangle.gltf`) for analytic validation.
- [ ] **CI runner bump** to get lavapipe ≥ Mesa 24.1 for Vulkan ray-query coverage; also allows
      un-pinning `dxc-linux`.
- [ ] **Confirm `MTL4ArgumentTable` AS binding** empirically — no documented
      `setAccelerationStructure`; binding is inferred to go through `gpuResourceID`.
- [x] ~~**Low-spec technique choice.**~~ **Decided 2026-07-30:** fully dynamic (no bake), so the
      technique is a **raster-updated dynamic probe volume + SSGI contact term** (§5). Baked
      lightmaps and voxel cone tracing ruled out by the fully-dynamic + mobile constraints.
- [ ] **Probe budget and propagation latency** are the low tier's real risk. Measure frames-to-converge
      for a moving light early in Phase 4; if it is unacceptable on mobile, the options are a larger
      per-frame probe budget, fewer/coarser probes, or revisiting the no-bake constraint.
- [ ] **Multi-bounce for the high tier.** Once Phase 4's probe volume exists, decide whether ReSTIR GI
      queries it for `L_o` at the secondary hit (kajiya-style world-space irradiance cache) rather
      than continuing a path. This is the cheap route to multi-bounce and reuses Phase 4 directly.
- [ ] **Where GI lives.** Recommendation: technique passes and their resources in the engine
      (`src/vkm/renderer/`, they need RHI-level resources and barriers), debug UI and scene setup in
      a **GI sample** with a runtime technique switcher — mirroring how `VkmScene` owns cull/emit
      while `model_viewer` owns presentation. The sample must build on WebGPU (unlike `skybox`,
      which is excluded at `CMakeLists.txt:489`), since the low-spec tier targets it.
- [ ] **glTF textures.** Importer reads material factors only (`TODO.md:34`). Not needed early;
      needed for the production bar. The texture upload/bindless path already exists.
- [ ] **Direct lighting strategy.** ReSTIR GI needs `L_o` shaded at the secondary hit. Decide
      whether that is plain NEE or a full ReSTIR DI implementation — DI usually lands before GI,
      and ReSTIR PT Enhanced argues for unifying them into one reservoir set.
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
| 2026-07-30 | 4 | Low-spec tier must be **fully dynamic** (no bake) → technique decided: raster-updated dynamic probe volume (DDGI-style octahedral irradiance + distance moments, Chebyshev visibility) plus an additive SSGI contact term. Storage/sampling are update-mechanism-agnostic, so Phase 5's rays can later refresh the same volume, and ReSTIR GI can query it for multi-bounce `L_o`. |
