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
