# Rae Compiler Test Suite

Each test in `tests/cases/` is powered by the Bash runner in
`tools/run_tests.sh` and consists of up to three files sharing the same stem:

- `NNN_name.rae` – Rae source that is passed to the compiler CLI
- `NNN_name.expect` – canonical stdout/stderr expected from the command
- `NNN_name.cmd` (optional) – overrides the CLI invocation used for the test

## `.cmd` files

A `.cmd` file contains a single command line that replaces the default
`parse` invocation. The line is split just like a normal shell command, so you
can specify multi-word invocations:

```
format             # default parse replacement
format --output {{TMP_OUTPUT}}
lex --some-flag
```

The runner automatically appends the `.rae` path to the end of the argument
list. If the file is empty (or missing entirely) the test falls back to
`rae parse`.

## Temporary output placeholder

When the command line includes the literal token `{{TMP_OUTPUT}}` the runner
creates a temporary file, replaces the placeholder with that file path and,
after the command finishes, compares the contents of the temp file against the
`.expect` file. This is how format tests assert that `--output`/`--write` write
the canonical program without producing stdout.

Rules enforced for placeholder runs:

1. The command must not print anything to stdout/stderr. Any output causes the
   test to fail with a helpful message so format regressions surface quickly.
2. Temporary files are deleted after each test so the suite remains hermetic.

## Adding new tests

1. Drop a new `.rae` and `.expect` pair into `tests/cases/`.
2. Add a `.cmd` if you need specialized invocation (e.g., `format --write`).
3. Run `make test` from `compiler/` to execute the full suite.

The runner also performs extra checks for plain `format` tests (idempotence and
AST comparisons) so leaning on these helpers is encouraged.
