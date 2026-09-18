# Stress tests — language foot guns, honestly scored

A third bucket next to `examples/` and `compiler/tests/`: small, human-readable
programs that each poke ONE classic language foot gun — the kind of thing that
goes wrong in many languages — and say, in the open, whether Rae handles it or
still gets it wrong. Half the point is marketing-by-honesty: the devtools tab
shows the wins AND the losses side by side, and the suite is built so a loss
cannot quietly stay on the list once the compiler fixes it.

Not a benchmark, not a fuzzer. "Stress" here means *semantic* stress: the
program is trivial, the question is what the language does with it.

## The two shelves

Every case sits on one of two shelves, declared in its pack file and shown as a
badge in the devtools:

| verdict | meaning | the test asserts |
|---|---|---|
| `handles` | Rae does the right thing — usually by refusing to compile, sometimes by a defined runtime result | the good outcome (a diagnostic, a specific output) |
| `footgun` | Rae still does the wrong thing | **the current bad behaviour, exactly** |

The second row is the ratchet. A `footgun` case passes while the bug is alive.
When someone fixes the compiler, that case FAILS with a message saying "this
foot gun no longer reproduces — move it to `handles` and assert the fix". So
the shelf is always the truth as of the checked-in compiler, and a fix is
celebrated by an explicit commit that flips a case, never by a test silently
going green.

## The first cases

Chosen by one rule: **each must be a foot gun in several mainstream
languages**, so the comparison is fair — it is possible to make any language
fall over, that is not what this is. Each case's README carries a short
"elsewhere" table (what Python / JavaScript / Java / Go / Rust / C do), written
from memory of the language rules, not run; the Rae column is the one that is
executed.

All results below were probed against the compiler as of 2026-09-18 — the
`handles` shelf is what it does, the `footgun` shelf is what it does wrong.

### Shelf 1 — Rae handles it

| # | case | the foot gun | what Rae does | elsewhere |
|---|---|---|---|---|
| 01 | `mutateWhileIterating` | add to a list inside a loop over that list | **compile error** `cannot mutate a List while a collection loop borrows its elements` | Java throws `ConcurrentModificationException` at run time; Python silently skips or repeats elements; JS silently loops forever or skips; C++ is undefined behaviour |
| 02 | `forgottenNoneCheck` | use an optional result as if it were a value | **compile error** `optional not unwrapped — use if let` | Java `NullPointerException`; Go nil-pointer panic; JS `undefined is not a function` at run time; Kotlin only if you write `!!` |
| 03 | `hiddenAliasing` | `var b: Box = a`, then `b.items.add(…)` | `a` is untouched: value semantics, no shared mutable container behind two names | Python, JS, Java, C# alias — `a` changes too; Go slices share a backing array; Rust refuses the second binding while the first is live; C++ copies |
| 04 | `temporariesInALoop` (optional 4th) | two million short-lived strings in a hot loop | RSS flat (2.7 → 2.8 MB), no pause, no manual free | Python/JS/Java/Go pay a GC; C leaks unless every `free` is right; Rust and C++ match Rae |

### Shelf 2 — Rae still gets it wrong

| # | case | the foot gun | what Rae does today | elsewhere |
|---|---|---|---|---|
| ~~05~~ | `integerOverflow` | `Int.max + 1`, and `7 / 0` | **FLIPPED to `handles`** (docs/integer-semantics.md): `/ 0` is a runtime error with a source position (exit 70), a constant zero divisor is a compile error, overflow traps in dev and wraps in release. It used to print `0` for `7 / 0` with exit 0 | C: undefined; Java: wraps, throws on `/ 0`; Rust: panics in debug, wraps in release, always panics on `/ 0`; Python: arbitrary precision, throws on `/ 0`; Go: wraps, panics on `/ 0` |
| ~~06~~ | `useAfterMove` | pass a list to an `own` parameter, keep using it | **FLIPPED to `handles`**: `own` is a move and a later use is a compile error `use of moved value 'xs'` (dataflow: branches, reassignment, spawn exempt). It used to compile and read a dangling caller copy | Rust: compile error `use of moved value`; C++ `std::move` leaves a valid-but-unspecified object (a classic foot gun); GC languages have no move at all |
| 07 | `stringIsBytes` | `"héllo👋".length()` and `sub(start: 1, len: 1)` | length is **10** (bytes), and `sub` cuts inside a code point and prints `�` | Python and Swift count characters; JS counts UTF-16 units (`"👋".length` is 2); Go counts bytes like Rae but its `range` walks runes; Rust counts bytes and panics on a non-boundary slice rather than producing garbage |
| 08 | `silentNoOpWrite` | `xs.set(index: 10, value: 5)` on a 3-element list | nothing happens: no error, no growth, exit 0 — the write is dropped | Java, Python, Rust, Go throw or panic; JS grows the array (a different foot gun); C writes past the buffer. Silently dropping the write is the one behaviour no language chooses |
| 09 | `deepRecursion` | recurse ten million frames deep | exits with code 1 and **no message** — a bare stack overflow | Python `RecursionError` with a traceback; Java `StackOverflowError`; Rust prints `thread 'main' has overflowed its stack`; C segfaults like Rae; Go grows goroutine stacks and simply succeeds |

Held back as a note, not a case, because it is tooling rather than language: two
`.rae` files in the same folder are compiled as one program, and the failure
surfaces as a *C compiler* error (`redefinition of 'main'`) instead of a Rae
diagnostic.

Cases 05 and 08 are the ones the devil's advocate leads with — silent wrong
answers with exit code 0 are worse than any crash on the other shelf.

## Layout on disk

```
stress/
  README.md                        ← the two shelves, in prose, kept in sync with this doc
  01_mutateWhileIterating/
    Main.rae                       ← the program; comments explain the foot gun in plain words
    README.md                      ← what it shows, the "elsewhere" table, the expected outcome
    01_mutateWhileIterating.raepack
  02_forgottenNoneCheck/
  …
  09_deepRecursion/
```

- A top-level `stress/` folder, a sibling of `examples/` — it is neither an
  example (nothing to look at, nothing to learn from as a pattern) nor a
  compiler test (a compiler test asserts the spec; a stress case is allowed to
  assert a bug). Number-prefixed case folders follow the `examples/` bucket
  convention; the name after the number is camelCase (a folder is a package).
- One `Main.rae` per case, short enough to read in one screen, with the foot
  gun in the first ten lines and comments written for a reader who does not
  know Rae. Every case is a plain program with no assets and no window.
- The pack file is the one place a case NEEDS `.raepack` (the "no pack unless
  needed" rule): it carries the verdict and the expectation.

### The pack block

```
stress: {
  verdict: "footgun"            # or "handles"
  expect: {
    outcome: "run"              # "compile-error" | "run"
    exitCode: 0                 # for "run"
    stdout: "wrapped: -9223372036854775808\ndiv0: 0"   # exact, for "run"
    diagnostic: "optional not unwrapped"               # substring, for "compile-error"
  }
  elsewhere: {                   # what the same program does in other languages
    python: "OverflowError never; ZeroDivisionError on 7 / 0"
    rust: "panics in debug, wraps in release; always panics on / 0"
    …
  }
  fixedBy: ""                    # filled in when a footgun flips: the commit / task that fixed it
}
```

The `expect` block is the test. For a `footgun`, `expect` describes the bad
behaviour verbatim — that is what makes the ratchet work.

## The runner

`stress/run.sh` (mirrors `compiler/tools/run_examples.sh`, minus windows and
screenshots): for every `stress/*/`, build with `rae build --emit-c`
(compile-error cases stop here and grep the diagnostic), else `rae run` with a
timeout, compare exit code and stdout to `expect`, print one line per case:

```
HANDLES  01_mutateWhileIterating   compile error as expected
HANDLES  02_forgottenNoneCheck     compile error as expected
HANDLES  03_hiddenAliasing         ok
FOOTGUN  05_integerOverflow        still reproduces (7 / 0 = 0)
FOOTGUN  06_useAfterMove           still reproduces
FIXED?   08_silentNoOpWrite        expected exit 0 with dropped write, got exit 1 — move this case to `handles`
```

`FIXED?` is a failure exit code, on purpose. Wired in as `make stress` and as
one extra pre-suite case of the full test run (a few seconds; every case is
tiny). The `RAE_STRESS_FILTER` variable scopes it like `RAE_EXAMPLE_FILTER`.

## The devtools tab

A new top-level tab **Stress** between *Raytracer* and *Test log* — its own
collection, not a category under Examples, because the page is different:

- **Left column:** the cases in two groups with a badge each — a green
  `handles` group on top, a red `footgun` group below — and a scoreboard
  header: *"Rae handles 4 · still gets 5 wrong · last run 2026-09-18"*.
- **Detail pane** for the selected case, top to bottom:
  1. the README rendered (the plain-words explanation and the "elsewhere"
     table from the pack, as a real table with the Rae row highlighted);
  2. the source (the existing example source viewer);
  3. **Run** and the live output (the existing example runner), plus a
     verdict line under it: *"expected: exit 0, stdout … — actual: same
     (still reproduces)"* or *"FIXED? the foot gun no longer reproduces"*.
- **Run all** runs the shelf top to bottom with the existing batch machinery
  and fills the scoreboard; a `FIXED?` result is shown as a call to action,
  not as an error.

Implementation outline (small; everything reuses the examples plumbing):

1. `config.ts`: `stressPath: "stress"` next to `examplesPath`. The examples
   scanner (`examples.ts`) takes a root parameter and is run twice; each
   record gets `origin: "examples" | "stress"` and, for stress, the parsed
   `stress` pack block.
2. `types.ts`: `stress?: { verdict, expect, elsewhere, fixedBy }` on the
   example record; a `stress-verdict` field on the batch result.
3. `app.js`: `EXAMPLE_COLLECTIONS.stress` (title "Stress", the scoreboard
   subtitle), membership by `origin === "stress"`, the two-group list
   renderer, the verdict line computed from the run's exit code + captured
   stdout against `expect` (the same comparison the shell runner does, so the
   two never disagree), Run-all reusing `runAllExamples` with the stress
   collection.
4. `index.html` / `style.css`: the tab button, the badge styles, the
   scoreboard header.

## Rules for adding a case

- One foot gun per case. If the program shows two things, split it.
- It must bite in at least three mainstream languages, and the README must say
  which and how. "Rae rejects something nobody else accepts" is not a case.
- Write the `expect` block from an actual run, never from what the behaviour
  *should* be — for a `footgun`, the assertion is the bug.
- A fixed foot gun is flipped in the same commit as the fix: verdict to
  `handles`, `expect` to the good outcome, `fixedBy` filled in. The runner's
  `FIXED?` line is what reminds you.
- No assets, no windows, no timing-dependent output. Every case runs in well
  under a second on the run shelf.

## Suggested queue split

1. **stress/ + runner + the nine cases** (this doc's shelves, `stress/run.sh`,
   `make stress`, pre-suite hook) — self-contained, no devtools change.
2. **devtools Stress tab** — scanner root, collection, two-group list,
   verdict line, scoreboard, Run-all.
3. Follow-ups the shelves point at, each a normal compiler task that ends by
   flipping a case: trap or diagnose integer overflow and division by zero
   (05); make `own` a real move with a use-after-move diagnostic (06); a
   documented character-vs-byte string API (07); an error (or a diagnostic)
   for `set` past the end (08); a stack-overflow message (09).
