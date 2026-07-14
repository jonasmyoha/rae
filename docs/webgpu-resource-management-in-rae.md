# WebGPU Resource Management In Rae

Status: design for runtime migration task `#295`.

## Motivation

Rae already has working WebGPU paths:

- `lib/webgpu.rae` exposes a native compute/raytracing bridge.
- `lib/gpu2d.rae` exposes the SDL3/WebGPU 2D UI renderer.
- `compiler/runtime/runtime_webgpu.c` owns raw WGPU device, buffer, compute
  pipeline, bind group, command encoder, and readback calls.
- `compiler/runtime/runtime_gpu2d_*.c` owns gpu2d surface, offscreen texture,
  batching, image textures, text atlases, and render pipelines.

That is useful, but too much policy still lives in C. The long-term runtime
migration direction is:

```text
Rae app / renderer policy
        |
        v
Rae WebGPU resource managers
        |
        v
Thin C raw WebGPU ABI
        |
        v
wgpu-native / browser WebGPU / platform APIs
```

C should provide the ABI boundary and raw platform calls. Rae should own
resource descriptors, lifetime policy, cache keys, resize/reconfigure behavior,
bind tracking, validation, render graph scheduling, and debug labels.

## Current Boundary

Current C-owned policy:

- Global singleton device/queue state.
- Fixed-size handle tables for buffers and compute pipelines.
- Per-call compute bind group creation.
- gpu2d-owned global texture, pipeline, atlas, and image registries.
- Surface/offscreen target reconfiguration and present fallback.
- Ad hoc validation through `if` guards and stderr messages.
- Debug labels mostly absent from Rae-authored resources.

Current Rae-owned policy:

- App-level draw ordering and UI systems.
- High-level gpu2d calls such as `drawBox`, `drawImageKey`, and `drawGlyph`.
- Some image-key naming and cache policy in examples and `lib/ui`.
- Compute examples author WGSL and choose dispatch sizes.

This split works for examples but does not dogfood Rae enough. It also makes
resource behavior hard to test without a window or full renderer.

## Design Principle

Raw API calls stay in C. Decisions stay in Rae.

C should answer questions like:

- "Create a buffer with these numeric flags and byte size."
- "Write these bytes into this handle."
- "Create a shader module from this WGSL string."
- "Create a pipeline from these raw handles."
- "Create a bind group from these raw binding entries."
- "Submit this encoded pass."
- "Release this native handle."

Rae should answer questions like:

- "Is this buffer descriptor valid for this use?"
- "Can this texture be reused after resize?"
- "Does this bind group match the pipeline layout?"
- "Which resources need rebuilding after a surface format changes?"
- "Which pass writes to this texture, and which later pass reads it?"
- "What debug label identifies this resource in GPU tooling?"

## Core Types

### Opaque Native Handles

The permanent C ABI should expose small opaque handles, not policy structs:

```rae
type GpuDeviceHandle { id: Int }
type GpuBufferHandle { id: Int }
type GpuTextureHandle { id: Int }
type GpuTextureViewHandle { id: Int }
type GpuSamplerHandle { id: Int }
type GpuShaderHandle { id: Int }
type GpuPipelineHandle { id: Int }
type GpuBindGroupHandle { id: Int }
type GpuSurfaceHandle { id: Int }
```

Handle ownership follows `docs/native-handle-ownership.md`:

- Owned handles release exactly once.
- Borrowed handles are `view`.
- C validates handle id and generation.
- Rae owns the policy that decides when to drop or rebuild.

### Descriptor Types

Rae-side descriptors should be ordinary comparable structs:

```rae
type GpuBufferDesc {
  label: String
  byteSize: Int
  usage: Int
  mappedAtCreation: Bool
}

type GpuTextureDesc {
  label: String
  width: Int
  height: Int
  depth: Int
  format: Int
  usage: Int
  sampleCount: Int
}

type GpuSamplerDesc {
  label: String
  minFilter: Int
  magFilter: Int
  mipFilter: Int
  addressU: Int
  addressV: Int
}

type GpuShaderDesc {
  label: String
  wgsl: String
}

type GpuPipelineDesc {
  label: String
  shader: GpuShaderKey
  entryPoint: String
  layout: GpuPipelineLayoutDesc
}
```

The exact enum representation can start as `Int` flags to avoid a large enum
design task, then become typed enums once the API stabilizes.

### Resource Keys And Caches

Rae resource managers should cache by stable descriptor keys:

```rae
type GpuBufferResource {
  key: String
  desc: GpuBufferDesc
  handle: GpuBufferHandle
  revision: Int
}

type GpuResourceCache {
  buffers: StringMap(GpuBufferResource)
  textures: StringMap(GpuTextureResource)
  shaders: StringMap(GpuShaderResource)
  pipelines: StringMap(GpuPipelineResource)
  bindGroups: StringMap(GpuBindGroupResource)
}
```

The cache policy belongs in Rae:

- Create on first use.
- Reuse when descriptor is identical.
- Rebuild when descriptor changes.
- Invalidate dependents when a resource revision changes.
- Release old owned handles when replacing.

C only creates and releases handles.

## Raw C ABI Shape

The thin C ABI should look like a narrow handle API:

```rae
func gpuRawCreateBuffer(desc: view GpuBufferDesc) extern ret GpuBufferHandle
func gpuRawWriteBuffer(buffer: view GpuBufferHandle, offset: Int, bytes: view Buffer(Any), byteLen: Int) extern
func gpuRawReleaseBuffer(buffer: own GpuBufferHandle) extern

func gpuRawCreateTexture(desc: view GpuTextureDesc) extern ret GpuTextureHandle
func gpuRawCreateTextureView(texture: view GpuTextureHandle, desc: view GpuTextureViewDesc) extern ret GpuTextureViewHandle
func gpuRawReleaseTexture(texture: own GpuTextureHandle) extern

func gpuRawCreateShader(desc: view GpuShaderDesc) extern ret GpuShaderHandle
func gpuRawCreatePipeline(desc: view GpuPipelineRawDesc) extern ret GpuPipelineHandle
func gpuRawCreateBindGroup(desc: view GpuBindGroupRawDesc) extern ret GpuBindGroupHandle
func gpuRawReleasePipeline(pipeline: own GpuPipelineHandle) extern
func gpuRawReleaseBindGroup(bindGroup: own GpuBindGroupHandle) extern
```

Do not start by exposing every WebGPU field. Add fields when a Rae renderer or
compute example needs them. The ABI should be thin, but not vague.

## Ownership Model

Rae resource managers own handles. Renderer passes borrow handles.

Rules:

- A cache entry owns exactly one current handle.
- Replacing a cache entry drops the old handle after dependent users are no
  longer recording commands.
- A render pass receives `view` handles only.
- A command encoder owns transient command state until submit.
- Surface swapchain textures are borrowed frame resources and must not escape
  the frame.
- Offscreen textures are owned cache resources and may persist.

This makes resource lifetime visible in Rae and keeps C from becoming a hidden
garbage collector for GPU objects.

## Resize And Reconfigure Policy

Surface and offscreen resize behavior should move to Rae:

```rae
type GpuSurfaceState {
  surface: GpuSurfaceHandle
  width: Int
  height: Int
  format: Int
  revision: Int
}

type GpuResizePolicy {
  minWidth: Int
  minHeight: Int
  recreateOffscreenOnResize: Bool
  preserveContent: Bool
}
```

On resize:

1. Rae reads the new drawable size from the platform layer.
2. Rae validates non-zero dimensions.
3. Rae calls raw C reconfigure for the surface.
4. Rae bumps the surface revision.
5. Rae invalidates offscreen targets and pipelines that depend on the old
   format or sample count.
6. Rae rebuilds resources lazily on the next render graph execution.

C should not decide which offscreen targets or pipelines to rebuild.

## Bind Tracking

Bind groups should be described and validated in Rae before C sees them.

```rae
type GpuBindingDesc {
  binding: Int
  resourceKind: Int
  buffer: GpuBufferHandle
  textureView: GpuTextureViewHandle
  sampler: GpuSamplerHandle
  offset: Int
  size: Int
}

type GpuBindGroupDesc {
  label: String
  layoutKey: String
  bindings: List(GpuBindingDesc)
}
```

Validation:

- Binding numbers must be unique.
- Required bindings for a pipeline layout must be present.
- Buffer size and usage must satisfy the layout.
- Texture format and sampler kind must satisfy the layout.
- A bind group revision depends on all resource revisions it references.

This replaces ad hoc C-side "if handle invalid, return" with testable Rae
diagnostics.

## Render Graph

The render graph should be a Rae data structure, not C control flow.

```rae
type RenderResourceRef {
  key: String
  kind: Int
  access: Int # read, write, readWrite
}

type RenderPassNode {
  label: String
  passKind: Int # compute, render, copy
  reads: List(RenderResourceRef)
  writes: List(RenderResourceRef)
  pipelineKey: String
  bindGroupKeys: List(String)
}

type RenderGraph {
  resources: GpuResourceCache
  passes: List(RenderPassNode)
}
```

The first implementation can execute passes in author order and validate:

- No read-before-write for graph-owned transient resources.
- No write-after-read hazard without an explicit pass boundary.
- No missing pipeline or bind group.
- No pass writes to a texture while it is also used as a sampled input.

Later, the graph can add sorting, transient aliasing, pass merging, and
automatic barriers if the backend requires them.

## Debug Labels

Every descriptor should have a `label`.

Policy:

- Rae assigns labels from app/resource keys.
- C forwards labels to WebGPU descriptors.
- Generated fallback labels are deterministic:
  `module.resourceName#revision`.
- Failed creation diagnostics include the label and descriptor summary.

This is ordinary app value, not runtime-only magic. It helps examples, tools,
debug GPU captures, and future editor inspection.

## Validation Strategy

Validation should live in Rae first, C second.

Rae validation catches:

- zero or negative sizes;
- incompatible usage flags;
- duplicate binding numbers;
- missing layout bindings;
- stale resource revisions;
- illegal resize dimensions;
- graph dependency errors.

C validation catches:

- invalid native handles;
- WebGPU creation failure;
- backend-specific limits not modeled yet in Rae;
- platform/device loss.

The C layer returns explicit failure handles or status codes. Rae turns those
into diagnostics with resource labels.

## Migration Phases

### R9a: Document And Name The Boundary

- Add this document.
- Mark current C WebGPU/gpu2d resource tables as compatibility bridges.
- Add queue tasks for raw handle ABI and first Rae cache.

### R9b: Raw Handle ABI

- Add typed opaque handle wrappers and generation checks in C.
- Add create/release APIs for buffers, textures, shader modules, pipelines,
  bind groups, and surface configuration.
- Keep existing gpu2d APIs working.

### R9c: Rae Buffer/Shader/Pipeline Cache

- Implement `lib/gpu/resource_cache.rae`.
- Move compute buffer and pipeline cache policy from `runtime_webgpu.c` to Rae.
- Update compute examples to use descriptor keys.

### R9d: Rae Bind Group Descriptors

- Implement bind group descriptors and layout validation in Rae.
- C creates only the raw bind group from already-validated entries.

### R9e: gpu2d Resource Policy

- Move gpu2d image-key registry, atlas registry, failed-load throttling, and
  pipeline cache descriptors into Rae.
- C keeps decode/upload/raw draw calls.

### R9f: Resize/Reconfigure Ownership

- Move surface/offscreen resize decisions into Rae.
- C exposes raw surface configure, acquire, present, and offscreen texture
  creation/release calls.

### R9g: Render Graph

- Introduce a Rae render graph for 2D and compute passes.
- Start with validation and author-order execution.
- Later add transient resource aliasing and scheduling.

## Test Gates

Each phase needs tests before replacing C policy:

- descriptor equality and cache reuse;
- release-on-replace and release-on-cache-drop;
- invalid descriptors produce deterministic diagnostics;
- bind group validation catches missing/duplicate bindings;
- resize invalidates dependent offscreen targets;
- compute examples still run;
- gpu2d examples still produce deterministic screenshots;
- ASan or leak diagnostics show no leaked native handles on reset.

## What Not To Do

- Do not change `WGPU*` usage semantics in C and Rae at the same time.
- Do not expose raw `WGPU*` pointers to Rae.
- Do not make C decide cache keys or renderer policy.
- Do not rewrite gpu2d batching as part of the first resource-cache slice.
- Do not require new Rae syntax for this migration.

## Open Questions

- Should usage flags stay as `Int` bitmasks or become typed enums first?
- Does the first Rae cache live in `lib/gpu` or `lib/webgpu`?
- How should device loss be represented in Rae: `Result`, `opt`, or explicit
  status components?
- Should render graph resources be keyed by strings initially or typed handles
  once `HashMap(K,V)` is stronger?
- How much of gpu2d batching becomes render graph policy versus staying in a
  specialized 2D renderer module?

## Decision

Move WebGPU resource management policy into Rae in stages. Keep C as the raw
handle/platform ABI: create, write, configure, submit, present, release. Rae
owns descriptors, caches, resize decisions, bind tracking, graph validation,
debug labels, and renderer-specific rebuild policy.
