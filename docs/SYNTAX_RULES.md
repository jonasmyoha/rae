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
*   In a literal written one element per line, a binary operator at the start
    of a line ENDS the element above it: `0.5` / `-0.25` on two lines are two
    elements, never `0.5 - 0.25` (fixtures 1090, 1091). An element that
    continues over lines keeps its operator inside parentheses (`(a` / `- b)`).

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
*   A foreign function is `unsafe extern`; one whose C call may wait for long
    is `blocking unsafe extern` (`blocking` first, on externs only).

## 3. Generic Syntax

Rule: **Use parentheses `()` for generics, not square brackets `[]`.**
*   `List(Int)` instead of `List[Int]`
*   `func create(T)()` instead of `func create[T]()`

## 4. Updating a number: `++`, `--`, `+=`, `-=`

Rule: **`x += e` and `x -= e` are statements, the same as `x = x + e` and `x = x - e`** (Rae
0.4.28, docs/loop-step-options.md). The target is a name or a field (`count += 1`,
`player.score -= 2`); a `let` cannot take one, like any assignment. They are not expressions:
`a = b += 1` does not parse. `+=` also appends to a String (`line += "x"`). Only `+=` and `-=`
exist, to match `++` and `--`; there is no `*=` or `/=`.

A counted loop's step is one of `++i`, `--i`, `i += n` or `i -= n`, on the loop's own variable,
a number:

*   `loop var degrees: Int = 0, degrees <= 90, degrees += 5 { }`
*   `loop var value: Int = 10, value >= 0, value -= 2 { }`
*   `loop var t: Float = 0.0, t <= 1.0, t += 0.25 { }`

Anything else as the step is an error. To walk a chain, use the condition loop:
`loop node >= 0 { ... node = nextNode(node: node) }`.
