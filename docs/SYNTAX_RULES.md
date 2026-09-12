# Rae Language Formatting & Syntax Rules

This document tracks established rules for the Rae compiler and formatter.

## 1. List Formatting (Commas vs. Newlines)

Established: 2026-01-28

Rule: **Strict Comma vs. Newline enforcement globally.**

1.  **Single-line lists REQUIRE commas.**
2.  **Multi-line lists MUST NOT use commas.**
3.  **No trailing commas allowed.**

This applies to:
*   Function parameter lists `func f(a: Int, b: Int)`
*   Function call arguments `f(1, 2)`
*   Enum variants `enum Color { Red, Green, Blue }`
*   Type members `type T { a: Int, b: Int }`
*   Return type records `ret { a: Int, b: Int }`
*   Object/Record literals `{ a: 1, b: 2 }`
*   List literals `{ 1, 2, 3 }`

### 1b. The canonical layout (`rae format`, #911–#919)

Established: 2026-09-12. `rae format` is the one authority on layout and the
compiler runs it first (`rae run` / `build` / `watch` rewrite changed files in
place; `--check-format` / `RAE_FORMAT=check` refuse and fail). The rules:

*   2-space indent, 100 columns, LF, one final newline, no trailing whitespace,
    braces on the declaration line, files capped at 1,000 lines (refused, never
    auto-split).
*   Parameters, arguments, return items: 1–3 items share the line when the whole
    header/call fits in 100 columns; from FOUR items, or whenever it does not
    fit, one item per line, two spaces deeper, no commas, `)` back at the
    declaration's indent.
*   Object / collection literals and enum members: the same from FIVE items.
    A `type` declaration is ALWAYS one field per line.
*   A lone argument that is itself a vertical list hugs its delimiters
    (`ret Point {` … `}`); `ret a, b` never goes vertical; explicit parentheses
    are kept; at most one blank line is kept where the source had one; a
    comment keeps its place (before what it precedes, trailing a one-line
    statement, below a statement that went vertical); `# ` prose wraps at 100.
*   `# raefmt: off` / `# raefmt: on` fence a verbatim region (rare).

```rae
func three(a: copy Int, b: copy Int, c: copy Int) ret Int {
  ret a + b + c
}

func four(
  a: copy Int
  b: copy Int
  c: copy Int
  d: copy Int
) ret Int {
  let total: Int = four(
    a: a
    b: b
    c: c
    d: d
  )
  let point: Point = { x: 1, y: 2 }
  ret total + point.x
}
```

## 2. Function Header Ordering

Established: 2026-01-28

Rule: **Canonical order for function declarations.**

A function declaration must follow this exact order:
1.  `func` keyword
2.  Function name
3.  Parameter list `(...)`
4.  **Zero or more modifiers** (space-separated, e.g., `extern`, `priv`, `spawn`)
5.  Optional `ret` clause
6.  Function body `{ ... }`

**Notes:**
*   Modifiers appear after the parameter list and before `ret`.
*   No colon (`:`) is allowed between the parameter list and the `ret` clause.
*   `extern` is now a function-level modifier, not a prefix to the `func` keyword.

## 3. Generic Syntax

Rule: **Use parentheses `()` for generics, not square brackets `[]`.**
*   `List(Int)` instead of `List[Int]`
*   `func create(T)()` instead of `func create[T]()`
