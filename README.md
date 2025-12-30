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
```

`rae format` prints to stdout unless you pass either `--write/-w` to rewrite the
input file in place or `--output <path>` to emit to a different file. The two
flags are mutually exclusive so you can safely script them:

```bash
bin/rae format my_app.rae > my_app.pretty.rae            # stdout pipeline
bin/rae format --write my_app.rae                        # overwrite source
bin/rae format --output build/pretty/app.rae my_app.rae  # write to custom file
```

## License

MIT
