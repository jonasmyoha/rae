# stress/ — language foot guns, honestly scored

Small, readable programs that each poke ONE classic language foot gun — the
kind of thing that goes wrong in several mainstream languages — and say in the
open whether Rae handles it or still gets it wrong. Not a benchmark and not a
fuzzer: every program is trivial, the question is what the language does with
it. The design and the full case list are in `docs/stress-tests.md`.

## The two shelves

Every case declares its shelf in its pack file, and the runner holds it to it:

| verdict | meaning | the case asserts |
|---|---|---|
| `handles` | Rae does the right thing — usually by refusing to compile, sometimes by a defined run-time result | the good outcome |
| `footgun` | Rae still does the wrong thing | **the current bad behaviour, exactly** |

The second row is the ratchet. A `footgun` case passes while the bug is alive.
When the compiler fixes it, the case FAILS with `FIXED? … move this case to
verdict "handles"` — so the shelf is always the truth as of the checked-in
compiler, and a fix is an explicit commit that flips a case, never a test
silently going green.

## Running

```sh
make stress                              # from the repo root (builds the compiler first)
bash stress/run.sh                       # the runner itself
RAE_STRESS_FILTER="01_mutateWhileIterating" bash stress/run.sh
```

One line per case:

```
HANDLES  01_mutateWhileIterating      compile error as expected
FOOTGUN  05_integerOverflow           still reproduces (exit 0, output as expected)
FIXED?   08_silentNoOpWrite           expected exit 0, got 1 — the foot gun no longer reproduces: move this case to verdict "handles", …
```

`FIXED?` is a failing exit. The full test suite (`make test`, both runners)
runs `stress/run.sh` as one pre-suite case right after the banned-terms gate.

## Anatomy of a case

```
stress/NN_camelCaseName/
  Main.rae             the program — the foot gun in the first ten lines, comments
                       written for a reader who does not know Rae
  README.md            what it shows, the "elsewhere" table, the expected outcome
  NN_camelCaseName.raepack
```

The pack is an ordinary `.raepack` (the compiler validates it) plus one block
the runner reads:

```
stress: {
  verdict: "handles"                 # or "footgun"
  expect: {
    outcome: "compile-error"         # or "run"
    diagnostic: "…substring of the diagnostic…"      # compile-error
    exitCode: 0                                      # run
    stdout: "line one\nline two"                     # run: exact (or stdoutMatches: a regex)
    stderr: ""                                       # run, optional: exact — "" asserts silence
  }
  elsewhere: {                        # what the same program does in other languages
    python: "…"
    javascript: "…"
    java: "…"
    go: "…"
    rust: "…"
    c: "…"
  }
  fixedBy: ""                         # the commit / task that flipped a footgun, once it is
}
```

## Rules for adding a case

- One foot gun per case. If the program shows two things, split it.
- It must bite in at least three mainstream languages, and the README must say
  which and how. "Rae rejects something nobody else accepts" is not a case.
- Write `expect` from an actual run, never from what the behaviour *should*
  be — for a `footgun`, the assertion IS the bug.
- A fixed foot gun is flipped in the same commit as the fix: verdict to
  `handles`, `expect` to the good outcome, `fixedBy` filled in.
- No assets, no windows, no timing-dependent output. Every case runs in well
  under a second.
