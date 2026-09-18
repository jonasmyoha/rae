# 09 — Deep recursion

**Verdict: `footgun`** — the process dies with exit 1 and no message.

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

## What Rae does today

Nothing printed on stdout, nothing on stderr, exit code 1. The stack
overflow is a bare crash: no "stack overflow", no function name, nothing to
search for.

## Elsewhere

| language | behaviour |
|---|---|
| Python | `RecursionError` with a traceback (at about 1,000 frames by default) |
| JavaScript | `RangeError: Maximum call stack size exceeded` |
| Java | `StackOverflowError` with a stack trace |
| Go | succeeds — goroutine stacks grow on demand (up to 1 GB) |
| Rust | aborts with `thread 'main' has overflowed its stack` |
| C | segmentation fault, no message — like Rae |

## Expected outcome (the current bug, asserted)

Exit 1, stdout empty, stderr empty — the empty stderr is the assertion. When
the runtime prints a stack-overflow message this case flips to `handles`
(exit non-zero AND the message).
