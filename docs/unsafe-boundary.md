# The unsafe boundary (#868 / #891 / #877)

Since #877 the pointer-operation boundary is **unconditional in every source
file**. There is no per-file opt-in any more (the #868 "adoption" staging —
a file only checked once it contained an `unsafe` token — is gone).

## The rules (what the compiler guarantees)

1. **Every foreign declaration says so.** `func name(...) unsafe extern("sym")`
   (or bare `unsafe extern`). A declaration that omits the distinct
   post-parameter `unsafe`, or writes the modifiers in another order, is a
   parse error — in declaration-only files too (fixture 798).
2. **Unsafe calls need a block.** Calling an `unsafe extern`, or a Rae function
   marked `unsafe`, outside `unsafe { ... }` is an error — in files with no
   `unsafe` token at all, across files (fixture 799) and modules, and inside
   generic bodies (fixture 800): a generic instantiated from within the
   caller's `unsafe { }` block does **not** inherit that block — its own raw
   operations still need their own block (the specialization is analysed with
   its own origin, unsafe depth 0 and loop depth 0). Generic functions whose
   bodies the backend binds by name (positional `T: type` parameters) get a
   by-name obligation scan of the template body, using the same rule as #897:
   a call is unsafe when every visible function of that name is unsafe.
3. **Raw pointer operations need a block.** Reading, writing, copying,
   forwarding or producing a raw `Ptr`, constructing a value that stores one
   (use a safe factory whose body wraps the literal), and formatting /
   reflecting / serialising a value with `Ptr` storage.
4. **Obligations are not erased** through fields, defaults, generics (above),
   reflection or serialization; `unsafe` is statement-level (`let x = unsafe
   { }` is rejected) and an `unsafe` block is a *transparent scope* for the
   rest of the compiler: escape analysis (a `ret` of a container-aliased
   value from inside the block is still an alias — fixture 801), element
   reference invalidation, drop insertion and defers all see through it.
5. **Explicit interop stays available in every user module**: any file may
   declare `unsafe extern` and open `unsafe { }` blocks.

## The safe-wrapper convention

Public APIs are **safe Rae functions**; the foreign symbol behind them is a
renamed `unsafe extern` with an explicit symbol:

```
func nativeSqrt(x: Float) unsafe extern("rae_ext_Math_sqrt") ret Float
func sqrt(x: copy Float) ret Float {
  unsafe { ret nativeSqrt(x: x) }
}
```

`Math`, `Sdl3`, `Gpu2d`, `Gpu3d`, `Gbuffer*`, `TransparentForward`,
`GpuTiming`, `Shadow3d`, `RenderScale`, `SdfText`, `Gpu`, `Time`,
`Filesystem`, `Image` and the prelude timers (`nowMs`, `nowNs`, `nextTick`,
`sleep`) follow this. Call sites did not change (`Math.math_log` became
`Math.naturalLog`; `log` is the prelude print). The C symbol of a bare extern
is `rae_ext_<Module>_<name>` (`rae_ext_<name>` in the prelude; `sleep` is the
historical `rae_ext_rae_sleep`), and the backend generates prototypes for
explicit `rae_ext_*` symbols exactly as it did for bare ones, so platform-
guarded runtime headers are not required for the wrappers to compile.

What stays `unsafe` (no wrapper): any extern whose signature carries a raw
`Ptr` or `Any` (handle accessors such as `gbFrameUbuf`, the WebGPU bindings,
readback bridges), the raw buffer intrinsics (`rae_ext_rae_buf_get/set/copy`),
`Sys.encrypt/decrypt`, `String.fromCStr`, and `sizeof(T)`. Their callers are
the opted-in modules (`gpu/*`, `water/WaterGpuBridge`, `GrassCompute`,
`GbufferSprite`, the render modules) and wrap at the source site.

## Identity and lifecycle (unchanged by #877)

`create`/`drop`/`copy`, owner containers and the no-stored-borrows rule are
as shipped. GPU objects are identified by manager IDs (`gpu/GpuResources`):
tag + slot + generation, validated on every use (see
docs/gpu-resource-identity-design.md). Limitations: an ID is only meaningful
against the manager that issued it; adopted externals are referenced, never
owned; a raw handle obtained inside `unsafe` carries no identity.

## Remaining platform constraints

- The legacy blocking `lib/GpuTiming.rae` keeps the last global handle slot
  table (`g_gt_handles[8]` / `rae_gt_set/get`); it folds into `gpu/GpuTiming`
  under #906. Every other global GPU slot table (water, grass, sprites) is
  gone (#873/#874).
- The 3D renderer's C-owned frame state and the 2D renderer's C pipelines are
  behind `unsafe extern` accessors and migrate under #904–#906 / #907–#910.
- `examples/legacy/` is unmaintained and not gated; it still contains plain
  `extern` declarations and does not compile under the boundary.
- Fixtures: 779/780/781/786/789/798/799/800 are the negative cases; 626/776/
  778/791/801 the positive ones.
