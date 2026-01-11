# Rae Source File Encoding and Line Endings

## 1. Line Endings (LF-only)

Rae enforces a strict **LF-only** ("
") policy for line endings in source files.

- **Rule**: Any Carriage Return (``) character found in a Rae source file outside of a **raw string literal** will result in a compilation error.
- **Diagnostics**: The compiler will report the exact line and column where `` was found and suggest running `rae format` to fix it.
- **Fix**: Running `rae format` automatically converts all `
` (CRLF) or standalone `` (CR) sequences to `
` (LF).

## 2. Final Newline

Every Rae source file **MUST** end with a trailing newline character (`
`).

- **Rule**: If the last character of a file is not `
`, compilation will fail.
- **Fix**: Running `rae format` ensures that the resulting file ends with exactly one newline.

## 3. Raw String Exception

Literal `` characters are permitted **only** within raw string literals (e.g., `r"content"`). This allows users to intentionally include CRLF sequences in data strings if required, without violating the source file separator policy. Note that the standard (non-raw) string literals `"..."` still prohibit literal newlines/CRs; they must use escapes like `
` or ``.
