# Strings: bytes and characters

A `String` is UTF-8 bytes. The byte view is the primary API — it is what file
formats, protocols, hashing and text layout want, and it is O(1) to index —
and a character (Unicode code point) view sits on top of it for the places
that count what a person sees. Stress case `stress/07_stringIsBytes` asserts
the rules below.

## The byte API (primary)

| call | meaning |
|---|---|
| `length()` | the number of **bytes**. `"héllo👋".length()` is 10 (`é` is 2 bytes, the emoji 4). |
| `byteAt(index:)` | the raw byte at a byte index (0..255; -1 out of range) |
| `at(index:)` | the character (`Char32`) whose UTF-8 sequence **starts** at a byte index |
| `indexOf` / `lastIndexOf` / `startsWith` / `endsWith` / `contains` / `split` | byte-indexed searches; positions they return are byte offsets |
| `sub(start:, len:)` | a substring by **byte** range — never cut inside a character, see below |

### `sub` never cuts a character

`sub` takes byte positions, but the cut never lands inside a multi-byte
sequence: a `start` that points into the middle of a character moves back to
that character's first byte, and an end that points into one moves forward
past its last byte. The range is **widened to whole characters**, so the
result is always valid UTF-8 and never contains a lone continuation byte.

```
"héllo👋".sub(start: 1, len: 1)   → "é"    (bytes 1..3: the whole é)
"héllo👋".sub(start: 0, len: 2)   → "hé"   (the end moved past é)
"héllo👋".sub(start: 7, len: 2)   → "👋"   (start moved back to byte 6)
```

Widening rather than refusing was chosen because byte-stepping callers
(cursor movement, Backspace over a multi-byte character, a parser that
slices at ASCII delimiters) already land on boundaries, and the callers that
do not are asking for "the text around this byte" — giving them the whole
character is what they meant, while an empty result would silently drop
text. Before this rule `sub` returned the raw bytes and printed `�`.

## The character API (on top)

Three calls count and index **characters** (code points). They walk the
string from the start, so they are O(n): right for a label, a menu entry, a
title; for a loop over a large text walk bytes with `at`/`byteAt`.

| call | meaning |
|---|---|
| `charCount()` | the number of characters. `"héllo👋".charCount()` is 6. |
| `charAt(index:)` | the character at a character position (`0` when out of range) |
| `charSub(start:, count:)` | `count` characters from character position `start`, as a new String; out-of-range positions clamp |
| `charByteOffset(index:)` | the byte offset where character `index` starts (`length()` past the end) — the walk the other three share |

```
"héllo👋".charAt(index: 1)             → 'é'
"héllo👋".charSub(start: 1, count: 3)  → "éll"
"héllo👋".charSub(start: 4, count: 2)  → "o👋"
```

A character here is a code point, not a grapheme cluster: a flag emoji or an
accented letter written as base + combining mark is two code points and
counts as two. That is the same rule as Python's `len` and Swift's
`unicodeScalars`; grapheme-aware counting is not provided.

## Elsewhere, for orientation

| language | `"héllo👋"` length | slicing inside a character |
|---|---|---|
| Rae | `length()` 10, `charCount()` 6 | `sub` widens to whole characters |
| Rust | `len()` 10, `chars().count()` 6 | `&s[1..2]` panics |
| Go | `len` 10, `utf8.RuneCountInString` 6 | `s[1:2]` returns the raw byte |
| Python | `len` 6 | slices are by code point |
| JavaScript / Java | 7 (UTF-16 units) | slices can split a surrogate pair |
