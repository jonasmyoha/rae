# List access benchmark

This suite tests Rae's public List design:

```text
at(index:)     -> opt T
viewAt(index:) -> opt view T
modAt(index:)  -> opt mod T
```

It compares optional indexed access, collection loops, access locality, invalid-index rates, and value-versus-reference access against equivalent optimized C and Rust programs. C/Rust unchecked cases are controls only; Rae deliberately exposes no unchecked List accessor.

## Run

From this directory:

```sh
./run.sh
```

The script builds the compiler, compiles all three implementations, performs nine repetitions, discards two warmups during aggregation, writes `results/raw.csv` and `results/summary.json`, captures Rae's generated C in `build/rae_generated.c`, and regenerates `site/index.html`.

The page is self-contained and can be opened directly. The committed results describe one machine and should be regenerated after compiler/codegen changes.

## Fairness

- Every timed case processes the same deterministic values and prints a checksum.
- Allocation and data initialization happen before timing.
- Sequential, constant, strided, pseudo-random, 99.9%-valid, and 75%-valid patterns are covered.
- The struct is a 64-byte data-oriented object with eight numeric fields.
- Results report median, minimum, and population standard deviation.
- Rae is compiled in release mode, C with `-O3`, and Rust with `opt-level=3`; exact versions and platform are recorded in `results/metadata.json`.
