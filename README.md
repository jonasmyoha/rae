# Rae Programming Language

A programming language focused on explicit ownership semantics and natural syntax.

## Project Structure

- `compiler/` - The Rae compiler (C11 implementation)
- `spec/` - Language specification
- `examples/` - Example Rae programs
- `docs/` - Documentation

## Quick Start

```bash
cd compiler
make build
make test
```

## Status

**Phase 1 (MVP):** Lexer, parser, AST, pretty-printer ✅ In Progress

See `spec/rae.md` for language details.

## Repository statistics

Run the metrics tool before committing to log compiler code size over time:

```bash
./tools/stats/update_metrics.sh
```

Outputs land in `stats/` as JSON lines and can be graphed later.

## CLI commands

```bash
bin/rae lex <file>     # tokenize Rae source
bin/rae parse <file>   # dump AST structure
bin/rae format <file>  # pretty-print canonical Rae (stdout by default)
bin/rae run <file>     # execute via the interpreter/VM (hot reload ready)
bin/rae build --target hybrid <file>   # emit hybrid bundles (Live chunk + C runtime)
bin/rae build --target live <file>     # emit Live (bytecode VM) packages (.vmchunk + manifest)
bin/rae build --target compiled --emit-c <file>  # transpile to experimental C backend
```

Use `bin/rae run --watch <file>` to keep the VM running and recompile whenever the file
changes—great for experimenting with the new interpreted workflow.

`rae format` prints to stdout unless you pass either `--write/-w` to rewrite the
input file in place or `--output <path>` to emit to a different file. The two
flags are mutually exclusive so you can safely script them:

```bash
bin/rae format my_app.rae > my_app.pretty.rae            # stdout pipeline
bin/rae format --write my_app.rae                        # overwrite source
bin/rae format --output build/pretty/app.rae my_app.rae  # write to custom file
```

See `docs/c-backend-plan.md` for the full roadmap that the `rae build` command will follow as the C backend comes online.

### Modules, Packages & Imports

- Every `.rae` file becomes a module whose path mirrors its location relative to the project root (e.g., `examples/todo/ui/list`).
- Import paths always use `/`. Absolute imports start from the repo root (`import "compiler/modules/greetings/banner"`). Relative imports use `./` or `../` to walk from the current file (`import "../shared/time"`).
- When a file omits imports altogether and either a `.raepack` file declares `auto_folders` or the folder contains a single `.rae` entry point, `bin/rae run` automatically pulls in every `.rae` file under that directory tree. This keeps tiny prototypes ergonomic: drop `main.rae` plus helpers in a folder (or add a simple `.raepack`), and run `bin/rae run path/to/main.rae`.
- `.raepack` files use Rae syntax to describe packages (name, branding, auto folders, targets). If they’re missing, the compiler falls back to the single-file heuristic so onboarding stays zero-config.

## Modules & Imports

- Place modules anywhere under the repo root; their canonical name equals the path (without `.rae`), e.g. `examples/multifile_report/ui/header`.
- Import using absolute, Dart-style strings (no `..` segments):

```rae
import "examples/multifile_report/ui/header"
import "compiler/modules/greetings/body"
```

See `spec/rae.md#modules--imports` for the syntax and rules.

## License

MIT
