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
bin/rae format <file>  # pretty-print canonical Rae (stdout by default)
```

Formatting options:

```bash
bin/rae format --write file.rae             # overwrite the input in place
bin/rae format --output tmp.pretty file.rae # write to a different file
```

Only one of `--write/-w` or `--output` may be supplied per invocation. If
neither flag is passed the formatter simply streams the canonical code to
stdout, which keeps shell pipelines simple.

See `../spec/rae.md` for language specification.
