# 09 — Deep recursion

**Verdict: `handles`** — one line says what happened, and the exit code is
non-zero. (Flipped from `footgun`: the process used to die with exit 1 and
nothing on stderr.)

## The foot gun

Ten million frames of recursion; the stack runs out.

```rae
func down(n: view Int) ret Int {
  if n is 0 {
    ret 0
  }
  ret 1 + down(n: n - 1)
}

log("depth 10000000: {down(n: 10000000)}")
```

## What Rae does

```
rae: stack overflow (deep recursion?) in stress/09_deepRecursion/Main.rae
```

Exit 128 + SIGSEGV from the program (`rae run` reports 1). The runtime's
crash handler had always been installed, but it ran on the thread's own
stack — the one that had just run out — so the kernel killed the process
before a byte was written. It now runs on an **alternate signal stack**
(`sigaltstack`, on the main thread and on every spawned worker), and it tells
a stack fault from any other one: a fault inside or just below the faulting
thread's stack is reported as a stack overflow, in one line, with no
backtrace (ten thousand copies of the same frame help nobody); any other bad
address is still `rae: segmentation fault (invalid memory access) in
<program>` followed by the C-level backtrace, so a genuine crash is never
dressed up as recursion.

`<program>` is the entry `rae run` was given, or `argv[0]` of a built binary.
No-op on WASM, where the host reports traps.

## Elsewhere

| language | behaviour |
|---|---|
| Python | `RecursionError` with a traceback (at about 1,000 frames by default) |
| JavaScript | `RangeError: Maximum call stack size exceeded` |
| Java | `StackOverflowError` with a stack trace |
| Go | succeeds — goroutine stacks grow on demand (up to 1 GB) |
| Rust | aborts with `thread 'main' has overflowed its stack` |
| C | segmentation fault, no message |

## Expected outcome

Exit 1 (via `rae run`), stdout empty, stderr matching
`^rae: stack overflow \(deep recursion\?\) in .*Main\.rae$`.
