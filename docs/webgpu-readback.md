# Nonblocking buffer readback

`open webgpu/ReadRequest` provides an ordinary Rae owner for an asynchronous
read request. This is an internal lifecycle pilot for shipped `create`/`drop`;
its native `Ptr` field and bridge calls now live inside the implemented unsafe
boundary. It does not establish pointers as Rae's public ownership model.
Staging allocation, command encoding and queue submission remain in Rae over
`webgpu/Webgpu`. The small runtime bridge supplies the C callback pointer and
retains its request state; the generated callback-info field is currently `Ptr`,
not a callable Rae callback.

1. Allocate a buffer with `MapRead + CopyDst`, copy GPU output into it and submit.
2. Construct directly in its owning scope:

   ```rae
   if let request: ReadRequest = ReadRequest.create(
     buffer: staging,
     range: { offset: 0 as UInt64, size: bytes }
   ) {
     # use request here; drop runs when this branch ends
   }
   ```

   `none` reports allocation, usage or range failure detected while starting.
   Construction only reads the initial status and never polls device events.
3. Each frame call `poll(this: request)`: `0` pending, `1` success, `-1`
   failure. This advances the context once with `wait: 0`, never waits for GPU
   completion, and does not require timestamp-query support.
4. On success, call `copyTo(this: request, destination: output)`, where `output`
   is a mutable `List(T)`. It returns true only when the List's initialized
   elements provide enough bytes for the entire requested range. It returns
   false for pending/failed requests or insufficient capacity. The safe wrapper
   exposes the backing pointer only inside its unsafe block, and the native call
   never retains it. The map stays open until release, so copying again is
   allowed.
5. Scope exit consumes the owner and calls `drop`. `request.drop()` is available
   for explicit cancellation, and passing `own request` transfers cleanup to the
   callee. A `ReadRequest` has no `copy` function. It can be moved into another
   owner field or a supported owner container such as `List(ReadRequest)`; those
   owners recursively drop it.

The raw functions remain in `webgpu/Readback` for existing manager-owned slot
protocols and blocking diagnostics. New direct use should prefer `ReadRequest`
so every successful start has one Rae owner.

Offsets must be multiples of 8; sizes must be positive multiples of 4. The
requested range must fit in the buffer and the platform's `size_t`. Invalid
ranges or usage produce a failed request that must still be released. Allocation
failure returns null, which polls as failure. Only the requested bytes are copied.

The native request is uniquely owned: **do not copy its raw handle or release an
alias**. The `ReadRequest` wrapper enforces this through the ordinary Rae
destructor/no-copy lifecycle. All operations, including event polling, run on
the WebGPU context thread.
The callback uses `AllowProcessEvents`, so owner and callback references are
serialized on that thread. The request retains the buffer: releasing the
caller's original buffer reference while a read is pending is safe. Start only
on an unmapped buffer and give that request exclusive control of its mapping;
do not map/unmap it or submit it for GPU use until release. The pinned native
backend exports `wgpuBufferGetMapState` but aborts when called, so this precondition
is not queried. Device loss or buffer destruction is reported by the map callback.

Cancellation marks the request failed before unmapping. Its callback reference
keeps both request state and the retained buffer alive even if the callback runs
later; an inline cancellation callback is safe too. Continue normal context
polling after cancellation so deferred callbacks can retire their references.
The bridge stores no pointer into a Rae collection. A destination List may grow
while waiting, provided its current data pointer and capacity are supplied at
copy time. On browser builds the application must also return control to the
browser event loop; this API introduces no blocking sleep to force completion.

`Context.webgpuMapRead` and `GpuTiming.collect` remain available as existing
blocking capture diagnostics. This change does not add adaptive water scheduling.

## Validation

Run deterministic callback-order and ownership checks through the official log:

```sh
TEST=webgpuReadback perl -e 'alarm shift; exec @ARGV' 120 bash compiler/tools/watch-tests.sh
```

The test compiles the actual bridge against a mock callback ABI under ASan/UBSan.
It checks independent completion order, pending copies, destination bounds,
inline failure, inline and delayed cancellation, exact request/buffer reference
cleanup, invalid ranges and that polling always receives `wait == 0`. It also
runs in the default test suite.

Run the headless hardware check (requires the native WebGPU and SDL3 libraries;
SDL3 links the existing single-command submission ABI, but creates no window):

```sh
perl -e 'alarm shift; exec @ARGV' 120 compiler/bin/rae run --target compiled examples/zz_readback_check/Main.rae
```

Verified on Apple M1 Max / Metal: 32 pairs of independent reads with known Int
contents and an offset subrange, destinations grown after starting reads,
releasing original buffer references while requests remain alive, rejected short
copies and invalid ranges, owner fields, explicit transfer, a two-element owner
List, cancellation followed by 32 map/unmap cycles on the same buffer, the
existing blocking read diagnostic, and failure from destruction before the
callback. The deterministic sanitizer test covers late-callback cleanup even
when hardware delivers early.
Browser execution has not been verified by this hardware check.
