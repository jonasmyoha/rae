# 07 — Strings are bytes

**Verdict: `footgun`** — `length()` counts bytes and `sub` can cut a character
in half.

## The foot gun

A string that is not plain ASCII, measured and sliced. `"héllo👋"` is six
characters; in UTF-8 it is ten bytes (`é` is two, the emoji four).

```rae
let s: String = "héllo👋"
log("length {s.length()}")
log("sub(1,1) [{s.sub(start: 1, len: 1)}]")
```

## What Rae does today

```
length 10
sub(1,1) [�]
```

Exit 0. `length()` is the byte count, and `sub(start: 1, len: 1)` returns the
first byte of `é` on its own — invalid UTF-8, shown as the replacement
character. Counting bytes is a defensible choice (Go and Rust do it); handing
out half a character is the foot gun.

## Elsewhere

| language | behaviour |
|---|---|
| Python | `len` is 6 (code points); `s[1]` is `é` |
| JavaScript | `length` is 7 (UTF-16 units — the emoji counts twice); `s[1]` is `é` |
| Java | `length()` is 7 (UTF-16 units); `charAt(1)` is `é` |
| Go | `len` is 10 (bytes) like Rae, but `range` walks runes and `utf8.RuneCountInString` is 6 |
| Rust | `len()` is 10 (bytes) like Rae, but `&s[1..2]` **panics** at a non-boundary instead of producing garbage |
| C | `strlen` is 10; `s[1]` is one byte of `é` |

## Expected outcome (the current bug, asserted)

Exit 0; stdout matches `^length 10\nsub\(1,1\) \[.\]$` — the second line's
bracket holds exactly one (lone, invalid) byte. A fixed `sub` that returns
`é` (two bytes) or refuses the cut no longer matches, and the case flips.
