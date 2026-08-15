# List access benchmark

This suite tests Rae's public List design:

```text
at(index:)     -> opt T
viewAt(index:) -> opt view T
modAt(index:)  -> opt mod T
```

It compares optional indexed access, collection loops, access locality, invalid-index rates, and value-versus-reference access across Rae, C, Rust, JavaScript, and Python. C/Rust unchecked cases are controls only; Rae deliberately exposes no unchecked List accessor.

## Run

From this directory:

```sh
./run.sh
```

The script builds the compiler, compiles the Rae/C/Rust implementations, runs those plus Node.js and Python, performs nine repetitions, discards two warmups during aggregation, writes `results/raw.csv` and `results/summary.json`, captures Rae's generated C in `build/rae_generated.c`, and regenerates `site/index.html`.

The page is self-contained and can be opened directly. The committed results describe one machine and should be regenerated after compiler/codegen changes.

## Fairness

- Every timed case processes the same deterministic values and prints a checksum.
- Allocation and data initialization happen before timing.
- Sequential, constant, strided, pseudo-random, 99.9%-valid, and 75%-valid patterns are covered.
- The struct is a 64-byte data-oriented object with eight numeric fields.
- Rae/C/Rust store that struct inline. JavaScript uses heap objects and Python uses tuples, so their struct results compare idiomatic runtime access rather than identical layout/codegen.
- Results report median, minimum, and population standard deviation.
- Rae is compiled in release mode, C with `-O3`, and Rust with `opt-level=3`; exact versions and platform are recorded in `results/metadata.json`.
- The website shows concise five-language comparisons and embeds every complete benchmark program used to produce the numbers.

## Owned optionals and `RaeAny`

`opt view T` and `opt mod T` lower to nullable references. General owned `opt T` currently uses the 48-byte `RaeAny` ABI record; a payload wider than its inline union may require heap allocation, and heap-owning values may require deep copy/drop work.

That cost is **not measured by the current Particle results**. The benchmark uses immediate narrowing:

```rae
if let particle: Particle = particles.at(index: index) {
    ...
}
```

The C backend recognizes this shape and lowers it directly to a logical-length check plus a local value copy, without constructing `RaeAny`. Stored, passed, or returned owned optionals need a separate correctness/performance benchmark once that general path is ready; do not attribute the current Particle timings to `RaeAny`.
