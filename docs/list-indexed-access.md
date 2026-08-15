# List indexed access

Status: implemented for the Compiled (C backend) target. Live is deprecated and intentionally unchanged.

## Public API

All public indexed reads and element borrows are checked against logical `List.length`:

```text
at(index: Int)     -> opt T
viewAt(index: Int) -> opt view T
modAt(index: Int)  -> opt mod T
```

Negative indices and indices greater than or equal to `length` return `none`. Capacity is not part of the check. There is no public raw, unsafe, or unchecked accessor.

`get`, `viewGet`, and `modGet` remain temporary compatibility aliases with the same optional semantics. New code should use the `At` names; the aliases can be removed after callers have migrated.

Owned optionals narrow with `=`; reference optionals narrow with `=>`:

```rae
if let value: Track = tracks.at(index: index) {
  consume(track: value)
}

if let value: view Track => tracks.viewAt(index: index) {
  log(value.title)
}

if let value: mod Track => tracks.modAt(index: index) {
  value.plays = value.plays + 1
}
```

The C backend lowers these exact `if let` accessor forms directly. It evaluates the List and index once, performs an unsigned logical-length comparison, and creates the binding only on success. No `RaeAny` box or generic accessor call remains in the generated hot path.

## Collection loops

`loop` remains Rae's only loop keyword. Collection iteration is:

```rae
loop track: view Track in tracks {
  log(track.title)
}

loop track: mod Track in tracks {
  track.plays = track.plays + 1
}
```

The C backend snapshots the List header and logical length once, then emits a direct counted loop over `data`. It does not construct an optional or perform an accessor bounds check per element. Value, `view`, and `mod` bindings are supported.

Direct mutation of the iterated List through `add`, `set`, `insert`, `remove`, `swapRemove`, `clear`, `drop`, `grow`, or `pop` is rejected in the loop body. This prevents reallocation or competing element access while the loop's element borrow is live. The check is deliberately conservative and lexical.

## Performance

The reproducible suite is in `benchmarks/list_access`. On the recorded Apple M1 Max run, optimized Rae was within measurement noise of the equivalent optimized C checked access and pointer loops:

- sequential `at` + `if let`: 0.97x C checked;
- pseudo-random `at` + `if let`: 1.00x C checked;
- collection `view` loop: 1.03x C pointer loop;
- 64-byte struct `viewAt` + `if let`: 0.98x C pointer access.

These results depend on the direct narrowing optimization. They do **not** measure the general owned-optional ABI: storing, passing, or returning `opt T` can still materialize the 48-byte `RaeAny` representation. A wide payload may be heap-allocated, while a heap-owning payload may also require deep copy and drop work. That path needs separate correctness and performance work before drawing benchmark conclusions from it. Hot indexed code should currently narrow immediately; sequential code should use a collection loop.

The measurements do not justify an unchecked public API.

## Remaining lifetime work

An element `view` or `mod` points into List backing storage. The collection-loop body rule closes that form's immediate invalidation hazard. General lexical borrow tracking for standalone `viewAt`/`modAt` results is still required: structural mutation of the source List must be rejected while such a binding remains live. This should remain a conservative local analysis, not grow into non-lexical lifetime inference.
