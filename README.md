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
bin/rae format <file>  # pretty-print canonical Rae
```

## License

MIT
