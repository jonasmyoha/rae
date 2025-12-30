# Rae Compiler Test Suite

Each test in `tests/cases/` is powered by `tools/run_tests.sh` and may include
up to three files with the same stem:

- `NNN_name.rae` – Rae source fed to the CLI
- `NNN_name.expect` – Expected stdout/stderr or formatted code
- `NNN_name.cmd` – Optional override for the CLI command/arguments

## `.cmd` overrides

The `.cmd` file contains a single line that replaces the default `parse`
invocation. The runner splits the line into arguments before executing
`bin/rae`, so multi-word commands are supported:

```
format
format --output {{TMP_OUTPUT}}
format --write {{TMP_INPUT}}
```

`{{TMP_OUTPUT}}` and `{{TMP_INPUT}}` are placeholders handled by the runner:

- `{{TMP_OUTPUT}}` tells the runner to create a temp file, substitute its path
  into the command, and then compare the file contents to `.expect` (while also
  asserting the formatter produced no stdout/stderr).
- `{{TMP_INPUT}}` copies the `.rae` source to a temp file, substitutes that path
  into the command, and skips auto-appending the original `.rae`. After the run
  the modified temp file must match `.expect`, again with no stdout/stderr.

All temporary files are removed after each test.

## Writing new tests

1. Create a new `.rae`/`.expect` pair under `tests/cases/`.
2. Add a `.cmd` if the test needs a special invocation.
3. Run `make test` from `compiler/` to execute the suite.

Pure `format` tests automatically run the extra idempotence + AST checks baked
into the runner, so prefer `format` wherever possible.
