# 14 — A window-system call from a worker thread

**Verdict: `handles`** — the spawn is a compile error (since 0.3.110). Before,
it compiled, and the call ran on a worker thread while the window system
expects the main one.

## The foot gun

A spawned task asks the window system a question. SDL's window and event
functions, and AppKit / UIKit under them, may only be called on the main
thread. From a worker the call races the event pump, or AppKit stops the
program; which one happens depends on the platform and the timing.

```rae
func probeInput() ret Bool {
  ret Device.hasTouch()
}

let task: Task(Bool) = spawn probeInput()
```

## What Rae does

The C under `Device.hasTouch` is marked `mainThread`, and the realtime
analysis carries that up the call graph. A spawned function, a parallelLoop
body, a `realtime` function or a `cExport` function not marked `mainThread`
that reaches such a call is refused, with the chain:

```
`probeInput` runs on another thread (spawned at Main.rae:15), but it reaches
`touchDeviceCount`, which only the main thread may call.
```

The fix is to ask on the main thread (in the frame loop) and send the answer
to the worker, or send the request to the main loop.
