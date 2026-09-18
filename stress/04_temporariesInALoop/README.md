# 04 — Temporaries in a loop

**Verdict: `handles`** — the process does not grow.

## The foot gun

Two million short-lived strings built and dropped in a hot loop. Whether the
program stays small depends on who frees them: a garbage collector (with its
pauses and its headroom), the programmer (with the leaks that follow), or the
language, at the end of each scope.

```rae
loop var i: Int = 0, i < 2000000, ++i {
  let s: String = "item {i}"
  total = total + s.length()
}
```

## What Rae does

Each `s` is freed when the loop body ends. The program prints the resident
set before and after the loop:

```
total 22888890
rss 2704 -> 2816 KB, grew 112 KB
```

The runner asserts the growth is under 4 MB (`stdoutMatches`), not the exact
figure — the OS decides the page numbers.

## Elsewhere

| language | behaviour |
|---|---|
| Python | fine, via reference counting — but every iteration pays an allocation and a refcount dance |
| JavaScript | fine, after garbage-collection pauses; the heap grows between collections |
| Java | fine, after garbage-collection pauses; the young generation churns |
| Go | fine, with the collector running alongside; the heap grows to the GC target first |
| Rust | fine — dropped at scope end, exactly like Rae |
| C | leaks unless every string is freed by hand; one missing `free` is 2,000,000 leaks |

## Expected outcome

Exit 0; stdout matches `^total 22888890\nrss N -> N KB, grew <0..3999> KB$`.
