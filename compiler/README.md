# Rae Compiler

C11 implementation of the Rae compiler (Phase 1: parse and pretty-print).

## Building

```bash
make build
```

## Testing

```bash
make test
```

## Usage

```bash
bin/rae lex <file>     # tokenize
bin/rae parse <file>   # parse + dump AST
bin/rae format <file>  # pretty-print canonical Rae
```

See `../spec/rae.md` for language specification.
