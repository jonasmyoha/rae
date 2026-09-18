# 07 — Strings are bytes

**Verdict: `handles`** — bytes by default, characters on request, and a cut
never lands inside a character. (Flipped from `footgun`: `sub` used to hand
out a lone byte of `é`.)

## The foot gun

A string that is not plain ASCII, measured and sliced. `"héllo👋"` is six
characters; in UTF-8 it is ten bytes (`é` is two, the emoji four).

```rae
let s: String = "héllo👋"
log("length {s.length()}  charCount {s.charCount()}")
log("sub(1,1) [{s.sub(start: 1, len: 1)}]")
log("charSub(1,1) [{s.charSub(start: 1, count: 1)}]  charAt(5) {s.charAt(index: 5)}")
```

## What Rae does

```
length 10  charCount 6
sub(1,1) [é]
charSub(1,1) [é]  charAt(5) 👋
```

`length()` and `sub` are byte-indexed (`docs/strings.md`) — a defensible,
fast default that Go and Rust share — but `sub` **never cuts a character**:
a byte range that starts or ends inside a multi-byte sequence is widened to
whole characters, so the result is valid text. `charCount()`, `charAt(index:)`
and `charSub(start:, count:)` count and index characters (code points, O(n))
when that is what the code means.

## Elsewhere

| language | behaviour |
|---|---|
| Python | `len` is 6 (code points); `s[1]` is `é` |
| JavaScript | `length` is 7 (UTF-16 units — the emoji counts twice); `s[1]` is `é` |
| Java | `length()` is 7 (UTF-16 units); `charAt(1)` is `é` |
| Go | `len` is 10 (bytes) like Rae, but `range` walks runes and `utf8.RuneCountInString` is 6 |
| Rust | `len()` is 10 (bytes) like Rae, but `&s[1..2]` **panics** at a non-boundary |
| C | `strlen` is 10; `s[1]` is one byte of `é` |

## Expected outcome

Exit 0, stdout exactly the three lines above.
