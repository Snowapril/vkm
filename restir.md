# ReSTIR GI in vkm

Living document for the ReSTIR GI implementation: what the technique is, the staged plan,
current status, remaining TODOs, and the reading list. Updated at the end of every phase.

**Status:** Phase 0 (scaffolding). No implementation work started.

---

## 1. Goal and decisions

Implement **ReSTIR GI** (Ouyang et al. 2021, *ReSTIR GI: Path Resampling for Real-Time Path
Tracing*) as production-usable real-time indirect lighting.

| Decision | Choice |
|---|---|
| Secondary ray tracing | Hardware ray tracing, **inline ray query in compute** (not RT pipelines) |
| Backends | Vulkan **and** Metal, developed in parallel |
| Metal shader path | Existing `dxc` → SPIR-V → SPIRV-Cross → MSL chain (**no** Metal Shader Converter) |
| WebGPU | GI gated off — WebGPU has no ray tracing |
| Quality bar | Production: denoiser, motion vectors, performance work all in scope |

### Platform matrix for GI

| Combination | GI support | Why |
|---|---|---|
| Windows Vulkan | Yes | `VK_KHR_ray_query` |
| Linux Vulkan | Yes | `VK_KHR_ray_query`; lavapipe works for CI (needs Mesa ≥ 24.1) |
| macOS Metal 4 | Yes | `metal::raytracing` intersection queries |
| macOS Vulkan | **No** | MoltenVK implements neither `VK_KHR_ray_query` nor `VK_KHR_acceleration_structure` |
| iOS Metal 4 | Untested | API present; perf unknown |
| WASM WebGPU | **No** | WebGPU has no ray tracing (blocked on bindless in the WG) |

**Consequence:** macOS ray tracing is reachable *only* through the Metal backend. The
Vulkan/Metal cross-check that vkm normally relies on for validation is unavailable on a single
macOS machine — Vulkan RT can only be verified on Windows/Linux or in CI.

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

## 4. Toolchain findings (settled by research; spike still required)

### SPIRV-Cross → MSL supports ray query

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

### MoltenVK will not help

Neither `VK_KHR_ray_query` nor `VK_KHR_acceleration_structure` is implemented
([MoltenVK#1956](https://github.com/KhronosGroup/MoltenVK/issues/1956), open since 2023-06;
maintainer in [#2079](https://github.com/KhronosGroup/MoltenVK/discussions/2079): *"Ray tracing is
a large project, and we are currently talking with customers about funding its development."*).

### CI: lavapipe works, but the runner is too old

Mesa's `docs/features.txt` lists `VK_KHR_ray_query  DONE (anv/gfx12.5+, lvp, radv/gfx10.3+, tu/a740+, vn)`,
landed Mesa 24.1 (Apr 2024). vkm's Vulkan CI job runs `ubuntu-22.04`
(`.github/workflows/ubuntu.yml:27`), whose `mesa-vulkan-drivers` is 23.2.1 → **no ray query**.
Bumping the runner also allows un-pinning `dxc-linux` from the GLIBC-2.34 release.

### dxc flags

- `RayQuery` requires **SM 6.5** → `-T cs_6_5`. vkm-compiler hardcodes `cs_6_0`
  (`main.cpp:48,142,605`), so profiles become per-PSO configuration.
- Add `-fspv-target-env=vulkan1.2 -fspv-extension=SPV_KHR_ray_query -fspv-extension=SPV_KHR_ray_tracing`.
  DXC emits `SPV_KHR_ray_tracing` unconditionally when an AS is present
  ([DXC#4113](https://github.com/microsoft/DirectXShaderCompiler/issues/4113)); harmless,
  SPIRV-Cross does not reject the capability.
- Known quirk: *storing* a `RayQuery<>` value fails
  ([DXC#4221](https://github.com/microsoft/DirectXShaderCompiler/issues/4221)) — keep it a plain local.

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

## 5. Two vkm-specific gotchas

**Vulkan Y-flip does not apply to compute.** The engine's clip space is +Y up with `[0,1]` depth
everywhere, and Vulkan's inverted NDC is compensated by `-fvk-invert-y`, which vkm-compiler
applies **only to the vertex stage** (`main.cpp:154-162`, `camera.h:18-23`). A compute
ray-generation shader reconstructing rays from pixel coordinates via `_inverseViewProjection` gets
**no** flip and will render vertically mirrored on Vulkan while looking correct on Metal. Handle it
explicitly and cover it in the cross-backend test.

**Threadgroup size is engine-global.** `kVkmComputeThreadGroupSizeX = 64` forces every compute
shader to `[numthreads(64,1,1)]`, because Metal needs `threadsPerThreadgroup` at dispatch time and
`MTLComputePipelineState` cannot be queried (`renderer_common.h:135-138`). Every ReSTIR pass is a
2D screen-space kernel wanting `8×8` for locality and cheap neighbour taps. This must become
per-PSO (Phase 3).

---

## 6. Scope reality check

| Phases | What | Rough share |
|---|---|---|
| 1–4 | Engine prerequisites (RT toolchain, acceleration structures, per-pass binding + barriers, G-buffer/motion vectors) | ~50% |
| 5–6 | Reference path tracer + unresampled baseline | ~15% |
| 7 | ReSTIR GI itself | ~15% |
| 8 | Denoiser + performance + robustness | ~20% |

Phases 1–4 are all things vkm needs anyway and are independently mergeable. Phases 5–6 look
skippable and are not — they are the only way to tell a Jacobian bug from noise later.

---

## 7. Phase plan

### Phase 0 — scaffolding
- [x] Create this document
- [ ] Add genuine known limitations to `TODO.md` as they appear (one line each, per `CLAUDE.md` §8)

### Phase 1 — Ray tracing toolchain spike (go/no-go)
Highest risk, so it goes first. Time-boxed.
- [ ] **~1 hour spike:** one `RayQuery<>` compute shader through `dxc -T cs_6_5` → `CompilerMSL`
      → `xcrun metal`. Validates or kills the whole recommendation before any runtime work.
- [ ] `vulkaninfo` on MoltenVK and on CI lavapipe, grepping for the RT extensions
- [ ] Per-PSO shader profile in vkm-compiler (SM 6.5 for RT shaders)
- [ ] Vulkan SPIR-V flags for ray query
- [ ] Decide: SPIRV-Cross (expected) vs Slang (fallback). Record outcome here and in
      `implementation-notes.md`

**Gate:** a trivial HLSL compute shader with a `RayQuery<>` against a hardcoded triangle compiles
and runs on Vulkan *and* Metal, writing a known value. New `TestRayQuerySmoke` + Metal variant.

### Phase 2 — Acceleration structures in the RHI
- [ ] New resource type + `VkmDriverCapabilityFlags::RayTracing` (pattern at `driver.h:34-59`)
- [ ] Vulkan: enable the three extensions; BLAS per mesh, TLAS per scene
- [ ] Metal: `MTL4PrimitiveAccelerationStructureDescriptor` / instance descriptors via
      `MTL4ComputeCommandEncoder`
- [ ] Build BLAS from the existing `VkmSceneGeometryPool` so no vertex data is duplicated
- [ ] Refit on transform change; full rebuild only on topology change
- [ ] Bind the AS to a compute shader (new bindless singleton binding is least invasive)

**Gate:** a compute shader ray-casts a loaded glTF scene and writes hit/miss + `t` matching a CPU
reference for known rays, on both backends.

**RHI contract note (Phases 2–3):** `backend/common/AGENTS.md:556` requires that no new pure
virtual is added without implementing it in **all** backends — so every RT and barrier entry point
needs a **WebGPU error-logging stub**, per the `copyTexture`/`registerTexture` precedent
(`TODO.md:29,42`). Gate on the capability flag, not `#ifdef`. Public base-class signatures must not
change.

### Phase 3 — Per-pass resource binding, barriers, threadgroup sizes
Blocks *any* multi-pass compute work. Both are pre-existing `TODO.md:8` items.
- [ ] Implement **descriptor set 2 (per-pass)** for pass-owned buffers/storage images, plus its
      PSO-JSON representation (`TODO.md:16`). Note every pipeline layout currently declares sets 0
      and 1 and binds both unconditionally
- [ ] Add a **general texture/buffer barrier** entry point. The engine's stated convention is that
      operations manage layout implicitly (`command_buffer.h:62-66`), which cannot express
      compute-write → sample-read — so this is a deliberate extension
- [ ] Push constants beyond the vertex stage, or per-pass constants through set 2
- [ ] **Per-PSO compute threadgroup size** + a `dispatch2D` helper

**Gate:** a two-pass compute chain (A writes a storage texture, B samples it) produces correct
pixels with **zero validation-layer errors** on both backends. Per `CLAUDE.md` §9, non-negotiable.

### Phase 4 — G-buffer, motion vectors, frame constants
- [ ] MRT G-buffer: linear depth, world/shading normal, **geometric normal** (for ray offsetting),
      albedo, roughness/metallic, material ID, **3-component motion vectors**
- [ ] **Previous-frame G-buffer** (double-buffered) — temporal resampling reads last frame's
      normal/depth/material to reject taps and evaluates `p̂` in the previous domain
- [ ] Extend `VkmFrameConstants`: `_prevView`, `_prevViewProjection`, inverses, `_frameIndex`,
      `_viewportSize`/`_invViewportSize`, jitter, previous camera position. Lockstep across
      `frame_constants.h`, `vkm_frame_constants.hlsli`, the `static_assert`, and vkm-compiler's
      Metal binding pins (`frame_constants.h:27-31`)
- [ ] Reconstruct world position from depth + inverse view-projection rather than storing it
- [ ] Fullscreen-pass HLSL building block + live tone mapping
- [ ] PBR BRDF evaluation reading the already-imported `VkmMaterialData`

**Gate:** G-buffer channels visualizable via a debug view; reprojection debug view stable under
camera motion with no drift.

### Phase 5 — Reference path tracer
**Do not skip.** Without ground truth you cannot distinguish a Jacobian bug from noise.
- [ ] Accumulating brute-force path tracer in compute using Phase 2's ray query
- [ ] Area/emissive light representation — one `_lightDirection` (`scene.h:63`) is not enough
- [ ] MSE/RelMSE comparison utility, so later phases produce a *number*
- [ ] Split-screen accumulation mode: ground truth vs live pipeline

**Gate:** white furnace test passes (uniform environment, albedo 1 → output equals input); energy
conservation holds on a small diffuse test scene.

### Phase 6 — 1-spp indirect, no resampling (baseline to beat)
- [ ] Single indirect bounce, one ray per pixel, no reservoirs. Deliberately noisy
- [ ] Direct lighting at the secondary hit (`L_o` = emission + NEE + optional continued path)
- [ ] Hash-based stateless RNG seeded per (pixel, frame, pass)
- [ ] Hit-point encoding, **forward-compatible with the LoD paper**: reserve
      `(instanceID, float2 uv)` alongside `(primitiveID, barycentrics)`, and put "locate this
      surface point" behind one `vertexMapping()` function rather than inlining it. Free now,
      expensive to retrofit
- [ ] Ray origin offset along the **geometric** normal, scale-relative epsilon; reconnection
      visibility rays use `t_max = (1-ε)·distance`

**Gate:** converges to the Phase 5 reference when accumulated. Proves sampling and BRDF are right
*before* reservoirs — if this is biased, ReSTIR will be, and far harder to see.

### Phase 7 — ReSTIR GI core
Incremental, with measured RelMSE against Phase 5 at every sub-step.

- [ ] **7.1 Reservoir buffer.** RTXDI-style packing is worth copying:
      `position` fp32 (feeds a `1/d²` Jacobian — fp16 is not viable), `normal` as octahedral
      `snorm2x16`, `radiance` as **LogLuv or RGB9E5** (not fp16×3 — HDR indirect clips and
      quantizes badly), `weight` fp32, and `M`/`age` as 8-bit fields (so `M ≤ 255`, `age ≤ 255`).
      ~32 B/reservoir; ~16 MB per slice at half-res 1080p.
      Use **one buffer with multiple array slices** and per-pass input/output slice indices rather
      than separate buffers — it makes bypass/validation modes trivial.
      **Triple-buffering hazard:** `FRAME_COUNT = 3` (`base/common.h:21`), so frames N-1/N-2 may
      still be executing when N is recorded. Either allocate `FRAME_COUNT` slices or rely on the
      per-slot `ensureCompleted()` deliberately — decide and document before writing the passes.
- [ ] **7.2 Neighbour offset LUT** — a small buffer of precomputed low-discrepancy disk offsets,
      indexed with a mask. Cheap, avoids per-pixel disk sampling, gives a stable pattern
- [ ] **7.3 Sample generation pass** — trace one ray, fill a fresh reservoir (`c = 1`, `age = 0`)
- [ ] **7.4 Spatial resampling first** (easier to validate than temporal — no scene change between
      samples): merge `k` neighbours (start 3–5, radius ~30 px) with normal/depth/material
      rejection (relative depth ~10%), the reconnection Jacobian, Jacobian validation, and a
      visibility ray per accepted neighbour. Then the second loop over accepted neighbours for the
      bias-correction denominator
- [ ] **7.5 Temporal resampling** — camera and scene **static first**, then moving. Reproject via
      motion vectors, ring of jittered fallback taps, optional zero-motion fallback, confidence
      cap (start 20), separate **age cap**
- [ ] **7.6 Shading/resolve** — `f_s · cos · L_o · W`, optional final visibility ray, composite
      with direct light
- [ ] **7.7 Final-shading MIS** on low-roughness surfaces (see the bias section). Cyberpunk's cheap
      version: `BRDF·(1−roughness²) + ReSTIR·roughness²`

> **Hard rule (course Tip 4.1):** choose which neighbours to reuse based **only** on the G-buffer.
> Never on the samples or weights stored *in* the neighbours' reservoirs — that conditions the
> probability space and biases the result. Same rule for resetting confidence on disocclusion.

**Gate per sub-step:** RelMSE versus Phase 5 must improve or at least not regress, and the mean
must match **on a diffuse-only scene** (specular secondary hits are legitimately biased — see §2).
A shifted mean on diffuse means a broken MIS weight or missing Jacobian. Add a debug view per pass
(reservoir `M`, `W`, `age`, chosen sample position, temporal-vs-spatial contribution) — these bugs
are invisible in the final image.

### Phase 8 — Denoiser and production hardening
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
- [ ] **Permutation sampling** to decorrelate histories — but note it erodes detail (see TODOs)
- [ ] Moving lights: validation/re-trace frames (kajiya re-traces stored samples every 3rd frame
      and lowers `M` if radiance changed materially), plus aggressive age capping
- [ ] Performance: half-res tracing + upsample, reservoir compression (measure first), BLAS refit
      budgeting

**Gate:** Sponza-class scene at 60 fps on both backends, zero validation errors, no visible
ghosting under camera motion.

### Phase 9 (optional, much later) — the LoD paper
Only once Phases 7–8 are solid *and* a dynamic geometry-LoD system exists. See §9.

---

## 8. Known pitfalls (consolidated)

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

## 9. The linked paper: Real-Time Level-of-Detail Rendering with ReSTIR

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

## 10. Reading list

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
   Theory + ReSTIR PT. Read once Phase 7 produces images.

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
- SVGF — Schied et al. 2017; **A-SVGF** — Schied et al. 2018 (the Phase 8 target).

### Current state of the art (for the "what I'd do differently" list)
**ReSTIR PT Enhanced** — Lin, Kettunen, Wyman 2026.
<https://research.nvidia.com/labs/rtr/publication/lin2026restirptenhanced/>
2.74× average speedup: reciprocal neighbour selection, footprint-based reconnection criteria,
**duplication maps** that adaptively lower the confidence cap where samples are duplicated, and
unifying DI + GI into *one* reservoir set. Memory 431 → 265 MB/frame.

---

## 11. Remaining TODOs / open questions

- [ ] **Test scene.** No Cornell-box-style scene exists; `scripts/download_scenes.py` offers only
      `DamagedHelmet` and `Sponza`. Hand-author a small `.gltf` in `resources/tests/` (following
      `gltf_triangle.gltf`) for analytic validation.
- [ ] **CI runner bump** to get lavapipe ≥ Mesa 24.1 for Vulkan ray-query coverage; also allows
      un-pinning `dxc-linux`.
- [ ] **Confirm `MTL4ArgumentTable` AS binding** empirically — no documented
      `setAccelerationStructure`; binding is inferred to go through `gpuResourceID`.
- [ ] **Where GI lives.** Recommendation: passes and reservoir management in the engine
      (`src/vkm/renderer/`, they need RHI-level resources and barriers), debug UI and scene setup
      in a `restir_gi` sample — mirroring how `VkmScene` owns cull/emit while `model_viewer` owns
      presentation. Exclude the sample from the WebGPU build as `skybox` does
      (`CMakeLists.txt:489`).
- [ ] **glTF textures.** Importer reads material factors only (`TODO.md:34`). Not needed early;
      needed for the production bar. The texture upload/bindless path already exists.
- [ ] **Direct lighting strategy.** ReSTIR GI needs `L_o` shaded at the secondary hit. Decide
      whether that is plain NEE or a full ReSTIR DI implementation — DI usually lands before GI,
      and ReSTIR PT Enhanced argues for unifying them into one reservoir set.
- [ ] Decide whether to pursue ReSTIR PT later for unbiased specular, or accept the documented
      ReSTIR GI bias plus final-shading MIS.

---

## 12. Progress log

| Date | Phase | Note |
|---|---|---|
| 2026-07-30 | 0 | Document created. Toolchain research settled: SPIRV-Cross supports ray query → MSL, so Metal Shader Converter is not needed; MoltenVK has no RT, so macOS GI is Metal-only. |
