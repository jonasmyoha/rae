# 13 — One place as `mod` and as `view` in one call

**Verdict: `footgun`** — it compiles, and the `view` sees the change made
through the `mod`.

## The foot gun

```rae
grow(output: numbers, input: numbers)       # mod and view of one list
addBoth(first: pair, second: pair)          # two mods of one list
```

`view` reads as "nobody changes this while I look", and `mod` as "only I
change this". Passing one place as both breaks both promises, silently.

## What Rae does

```
the view read 1 items, then 101
one list as two mods: 2 items
```

There is no rule against overlapping arguments of one call: a place (a local,
a field, an element) may be passed as `mod` together with itself, a part of
it or a whole containing it, as another `mod` or a `view`.

## Why it matters beyond correctness

Such a rule is what would let the compiler promise C that a `mod` and a
`view` parameter never overlap (`restrict`), so a loop over them vectorizes
without run-time overlap checks (docs/realtime-language-design.md §4.6, the
audit of 2026-11-12). Until it exists, emitting `restrict` would be wrong:
the compiler could cache `input.length()` across the writes above.

## Decided (2026-11-12): no rule for now

A rule against overlapping arguments was measured (46 call sites in the tree
would be refused) and not adopted: a whole and its part passed together, even
both as `mod`, is a legitimate pattern, as in C, C++, Java and Go. This case
stays as the record of what Rae does; `restrict` will only come where the
compiler proves no overlap from every call site (docs/realtime-language-design.md
§4.6).

## Elsewhere

Rust refuses it at compile time; Swift checks "exclusive access" (at compile
time where it can, at run time otherwise); C makes it undefined behavior only
when the parameters are `restrict`; Python, JavaScript and Java share the
object without a promise.
