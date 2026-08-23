# Texture streaming in vkm

How vkm decides which mip levels of a material texture to keep resident, and what it costs to change
that decision. Written the way `restir.md` is: decisions and their reasons first, toolchain findings
next, then the phase plan and a progress log.

---

## 1. Goal and decisions

Material textures used to hold their whole mip chain for the life of a scene. A wall covering twelve
pixels cost exactly what one filling the screen did.

Two questions run the system, and they are **orthogonal**:

| | Question | Answered by |
|---|---|---|
| **Target** | Which level does this texture need? | Tier 0: a projected bounding sphere. Tier 1+: what the pixel shader actually sampled. |
| **Residency** | What does changing it cost? | Tier 0/1: rebuild the texture. Tier 2: bind and unbind levels in place. |

Decisions taken:

- **Tiers are a runtime choice, never an `#ifdef`** — matching `RayTracing`, `TextureUpload` and
  `BindlessTextures`. Tier 0 must stay reachable on a tier-2 machine or it rots.
- **Sparse means per-mip residency, not virtual texturing.** No page table, no indirection texture,
  no UV translation in-shader. That is a different renderer and is out of scope.
- **Feedback before sparse.** Feedback is the bigger correctness win and needs no device features;
  sparse is the bigger cost win and needs real capability work.

## 2. The tiers

| Tier | Requires | Target from | Residency by | Status |
|---|---|---|---|---|
| 0 | `BindlessTextures` | CPU bounding sphere | Rebuild | **Shipped** (PR #69) |
| 1 | + fragment-visible storage buffer | GPU feedback | Rebuild | **Shipped** |
| 2 | + sparse residency | GPU feedback | Bind/unbind levels | Planned |

WebGPU is tier 0 permanently: it has no bindless texture array to key feedback by, and Tint has no
`OpImageQueryLod` case — WGSL has no LOD-query builtin at all.

## 3. Toolchain findings

The HLSL → SPIR-V → MSL/WGSL path has surprised this codebase before. What was actually verified,
against the generated artifacts in `resources/Shaders/ShaderCache/`:

**`CalculateLevelOfDetail` survives to Metal, including through the bindless array.** DXC emits
`OpImageQueryLod` at `ps_6_0` with no profile bump; SPIRV-Cross emits `calculate_clamped_lod`, gated
on MSL ≥ 2.2 and vkm sets 3.0. The combination nobody had exercised — a runtime-array element inside
a Tier-2 argument buffer, `NonUniformResourceIndex`, and a LOD query — compiles and produces:

```metal
_254.x = spvDescriptorSet0.g_VkmMaterialTextures[_43]
             .calculate_clamped_lod(spvDescriptorSet0.g_VkmMaterialSampler, in.in_var_TEXCOORD4);
```

**`InterlockedMin` from a fragment shader works**, and SPIRV-Cross casts the storage buffer correctly:

```metal
uint _260 = atomic_fetch_min_explicit(
    (device atomic_uint*)&(*spvDescriptorSet0.g_VkmTextureFeedback)._m0[...], ..., memory_order_relaxed);
```

**A fallback exists and was not needed.** `GetDimensions` (`OpImageQuerySizeLod`) plus the `ddx`/`ddy`
`gbuffer.hlsl` already uses gives a manual LOD on every backend including WebGPU. Reach for it only if
the query path breaks.

### The trap: singleton bindings are positional

`VkmBindlessSingletonBuffer` is a dense enum, and `kVkmBindlessAccelerationStructureBinding` is
derived as *first singleton + Count*. Adding `TextureFeedback` moved the acceleration structure from
binding 8 to 9 — and `VKM_BINDLESS_ACCELERATION_STRUCTURE` in `vkm_bindless.hlsli` **hardcodes its
binding number**. The shader kept declaring 8, which the runtime now used for feedback, so every ray
query silently missed. Five ray-tracing tests caught it; nothing else would have.

**Adding a singleton means auditing every hardcoded `vk::binding(N, 0)` in `vkm_bindless.hlsli`.**
There is no compile-time link between the enum and those numbers.

Also note: `vkm_ray_tracing_shaders` is a **separate build target** from `vkm_engine_shaders`.
Building the library does not rebuild either. A set-0 layout change means building both.

## 4. Tier 1: how feedback works

**The write.** The G-buffer pass — and only it — reports what each pixel wanted:

```hlsl
const float lod = tex.CalculateLevelOfDetail(sampler, uv);
InterlockedMin(g_VkmTextureFeedback[slot], (uint)clamp(round(lod), 0, 15));
```

- **Keyed by bindless slot**, the granularity the streamer already works at.
- **`InterlockedMin`** because the finest level any pixel needed is the one that matters. A plain
  store lets an arbitrary distant pixel win and under-resolves the surface.
- **One pixel in 16 votes** (`VKM_TEXTURE_FEEDBACK_PIXEL_STRIDE`). Every pixel voting would serialise
  a great many atomics onto the few slots a large surface covers and buy nothing, since the answer
  wanted is a minimum over a surface. This is what makes the write cheap enough to leave on
  unconditionally instead of behind a PSO permutation.
- **The probe capture deliberately does not vote.** Probes look in every direction; letting them vote
  would drag every texture to full resolution and defeat streaming entirely. This is why the write
  lives in `gbuffer.hlsl` rather than in `vkmSampleMaterialTexture`, which eight shaders expand.

**The reading is relative.** A streamed texture is already reduced, so the shader's "level 0" is the
chain's level *n*. `vkmStreamingBaseMipFromFeedback` adds the resident base back. That makes the loop
a fixed point rather than a chase: stream out four levels and the next reading comes back four
higher, naming the same absolute level again. `TestTextureStreaming.cpp` pins exactly that property.

**The readback never stalls.** The buffer is copied into a ring of `FRAME_BUFFER_COUNT + 1` staging
buffers and mapped at the top of `updateTextureStreaming`. The ring is one longer than the frame count
on purpose: at that point this frame's own wait has not happened yet, so a ring of exactly
`FRAME_BUFFER_COUNT` would hand back a slot still in flight. Feedback is therefore several frames
stale by construction. **That is fine for streaming and must not be "fixed" with a stall.**

**No feedback means fall back.** A slot still holding `kVkmTextureFeedbackUnused` was sampled by
nothing this frame and keeps the CPU estimate — which is what stops a texture evicting the instant it
leaves the screen. `VkmTextureStreamingStats::_feedbackCount` reports how many targets came from
feedback; zero with streaming on means the readback is not arriving and the tier fell back silently.

## 5. Measured

Sponza, `gi` sample, texture-category bytes from the shutdown memory report:

| Camera | Tier 0 | Tier 1 |
|---|---|---|
| distance 0.6 (close) | 559.6 MiB | — |
| distance 2.5 (building on screen) | 389.1 MiB | **349.2 MiB** |
| distance 12 (speck) | 372.5 MiB | — |

Feedback saves a further ~40 MiB at the same viewpoint, in the predicted direction: the bounding
sphere cannot see UV density, grazing angles or occlusion, and keeps those surfaces too sharp.

## 6. Tier 2 plan: sparse per-mip residency

Not started. The shape, from the capability exploration:

**Why it matters.** A sparse texture's view covers the whole chain forever while residency changes
underneath. That deletes what tier 0/1 needs: no second texture, **no new bindless slot**, no material
rewrite, no retire delay, no re-upload of levels already held. The reason a rebuild needs a fresh slot
— the set is `UPDATE_AFTER_BIND | PARTIALLY_BOUND` but not `UPDATE_UNUSED_WHILE_PENDING` — stops
applying.

**Vulkan.** `sparseBinding` / `sparseResidencyImage2D` are already enabled (the driver queries every
supported feature and hands the same chain to `vkCreateDevice`) and are core 1.0, no extension. Create
flags go on one line in `vulkan_texture.cpp`. The one real gap is submission: `vkQueueBindSparse`
takes `VkBindSparseInfo`, not `VkSubmitInfo2`, so it needs its own entry point taking a value from the
same timeline semaphore, or ordering against normal submits breaks.

**Metal.** MTL4 has no resource-state encoder and cannot create one — but does not need one. macOS 26
is this build's floor, and there mapping updates are a queue method:
`MTL4CommandQueue updateTextureMappings:heap:operations:count:`, in tile units. vkm already creates
`MTLHeapTypePlacement` heaps and registers raw heaps for residency. New work: `MTLStageResourceState`
appears in neither `vkmToMTLStages` nor the encodable-stage masks, and a mapping update needs a
barrier against it. Copy the `onAcquireAliasedTexture` latch pattern, and never open an encoder just
to hold a barrier — that caused progressive queue timeouts before.

**The seam.** A driver/queue-level virtual with a no-op default, not a command-buffer operation.

**What per-mip granularity avoids.** Uploads stay whole-mip, so `uploadToTexture` is untouched. Both
backends hardcode origin `(0,0)` and full-mip extents in `writeRegion` / `onCopyBufferToTexture` —
sub-rect upload does not exist. Full virtual texturing would have needed it.

## 7. Progress log

- **2026-08-22** — Tier 0: `VkmTextureStreamer`, rebuild-based, CPU bounding-sphere estimate. PR #69.
- **2026-08-23** — Tier 0 measurement: resident-vs-full-chain readout, honest `computeTextureByteSize`,
  per-asset texture debug names.
- **2026-08-23** — Tier 1: GPU feedback. Toolchain spiked first and passed, so no fallback was needed.
  Fixed `toVkShaderStageFlags` making set-2 storage buffers fragment-visible, which was also a latent
  bug for `gi_restir_lighting` on Vulkan. Moved the acceleration-structure binding to 9.

## 8. Tier 2 measurements — what this hardware actually offers

Probed directly on an **Apple M3 Pro**, against a 2048x2048 RGBA8 12-level chain:

| Page size | Tier | Tile | First mip in tail | Tail bytes |
|---|---|---|---|---|
| 16 KiB | 2 | 64x64 | 6 | 16 KiB |
| 64 KiB | 2 | 128x128 | 5 | 64 KiB |
| 256 KiB | 2 | 256x256 | 4 | 256 KiB |

A placement heap declaring `maxCompatiblePlacementSparsePageSize = 64KB` creates without complaint.

Three things follow:

- **Tier 2 is available**, which is more than per-mip residency needs. Tier 1 already gives partial
  backing and defined reads of unbacked texels; Tier 2 adds per-tile access counters, i.e. Metal's
  own texture feedback. That is a possible future replacement for the shader-side feedback of
  tier 1, not something this design requires.
- **16 KiB pages are the right choice**, and the reason is the tail rather than the tile. Every level
  too small to fill one tile lives in an indivisible, permanently resident mip tail, so the page size
  sets the floor on what streaming can ever give back. 16 KiB pages stream two more levels than
  256 KiB ones and leave a sixteenth of the residue.
- **Per-mip residency stays worthwhile.** With 16 KiB pages a 2048-wide chain streams levels 0-5
  independently, and level 0 alone is three quarters of the chain's bytes. The 16 KiB tail is noise.

`VkmDriverCapabilityFlags::SparseResidency` now reports this: yes on the M3 Pro, no under MoltenVK
(which has neither the feature bits nor ray tracing).

## 9. Tier 2 progress: the mapping contract, verified

Probed the full placement-sparse cycle on an M3 Pro before writing engine code — create, map the
tail, map level 0, unmap it — under `MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1`. All of it is legal
as written. Three things the probe settled that the headers do not say plainly:

- **A placement-sparse texture is created standalone**, with `newTextureWithDescriptor:`, not out of
  a heap. The page size buys a texture whose levels have addresses but no memory; the heap only
  enters later, as the source of tiles in `updateTextureMappings:heap:operations:count:`. So a sparse
  texture must *skip* the engine's heap-placement path rather than take it.
- **A tail operation is addressed differently from a level one.** `textureLevel` is
  `firstMipmapInTail` and the region is `(0, 0, 1, 1)` — one tile, not the tail's pixel extent.
- **`allocatedSize` is not a residency signal.** It reported 192 KiB for a 2048x2048 12-level chain
  and did not move across a full map and unmap of level 0's 32x32 tiles. That is page-table
  footprint. The bytes live in the tile heap, which is where tier 2 has to account for them — and it
  is why `VkmTextureMetal` reports zero rather than a number that never changes.

Landed so far: `VkmResourceCreateInfo::Sparse` with the request-vs-grant contract the `Transient` and
`Aliasable` flags already carry, `VkmTexture::isSparse()` / `getMipTailFirstLevel()`, and Metal
creation. `TestSparseTextureShared.hpp` asserts the request is answered *coherently* — granted with a
tail above level 0, or refused and downgraded to a fully backed texture — rather than asserting a
value, since whether a device can do it is a property of the machine.

Still to come: the tile heap, the `updateSparseTextureMapping` seam and its `MTLStageResourceState`
barrier, and the streamer's residency path.

## 10. Tier 2: residency works

The tile heap and the mapping seam are in, and a level's memory can now be taken and given back
without touching the texture, its view or its bindless slot.

**`VkmDriverBase::updateSparseMipResidency(texture, level, resident)`** is deliberately a driver call
rather than a command-buffer one: Metal updates mappings on the queue and Vulkan through
`vkQueueBindSparse`, neither of which is an encoder operation, and routing it through the command
buffer would mean opening an encoder to hold a barrier — something this backend has been bitten by
before. Asking about a level in the mip tail succeeds and does nothing, so a caller can walk a whole
chain without first finding where the tail starts.

**`VkmSparseTileHeapMetal`** allocates *runs*, not tiles. One mapping operation covers a rectangular
tile region of one level and draws it from a contiguous span starting at one tile offset, so the unit
of allocation is a run of N tiles. It is a first-fit free list with coalescing on release: without the
coalesce the list fragments into single tiles as levels stream in and out, and a level needing a
contiguous run would then fail against a heap that is mostly empty.

**Residency is measured in the heap, not on the texture.** A sparse texture's `allocatedSize` is
page-table footprint and does not move, so the tile heap's own totals go into
`VkmGpuMemoryStats::_poolUsedBytes`. That is the only place the cost of streaming a level in becomes
visible, and it is what the test asserts against.

### Two things the hardware taught us here

- **A sparse texture must never be host-writable.** On unified memory the engine's storage-mode
  policy would otherwise give it `MTLStorageModeShared`, and `uploadToTexture` would then take the
  `replaceRegion:` path straight into sparse memory. That **hangs the queue** rather than failing —
  far harder to diagnose than a copy that returns an error. Its pages come from a Private placement
  heap and arrive one mapping at a time, so there is no CPU-visible allocation to write into.
- **A copy into a freshly mapped level did not need an explicit `MTLStageResourceState` barrier** to
  complete, tested with and without. The header still asks for one, so this is a "not observed"
  rather than a "not required" — but it was not the cause of the hang above, which is worth recording
  because it was the obvious suspect.
