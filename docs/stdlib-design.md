# Rae Standard Library Design Proposal (v0.3)

This document outlines the standard library design, utilizing Rae's "extension method" syntax where functions are resolved based on their first parameter (`this`).

## 1. Core (`lib/core.rae`) - Always available implicitly

### Execution & Time
- `nowMs(): ret Int` - Current monotonic time in milliseconds.
- `sleep(ms: Int)` - Suspend execution.
- `nextTick(): ret Int` - Global tick counter.

### Collections: List(T)
Factory: `createList(T)(capacity: Int): ret List(T)`

Methods (resolved by type):
- `add(this: mod List(T), value: T)`
- `get(this: view List(T), index: Int): ret T`
- `set(this: mod List(T), index: Int, value: T)`
- `length(this: view List(T)): ret Int`
- `free(this: mod List(T))`
- `clear(this: mod List(T))`
- `pop(this: mod List(T)): ret T`
- `remove(this: mod List(T), index: Int)`
- `insert(this: mod List(T), index: Int, value: T)`

## 2. Math & Random (`lib/math.rae`)

### Basic Math (Overloaded by type)
- `abs(n: Int): ret Int`
- `abs(n: Float): ret Float`
- `min(a: Int, b: Int): ret Int`
- `max(a: Int, b: Int): ret Int`
- `clamp(val: Int, low: Int, high: Int): ret Int`

### Randomness
- `seed(n: Int)`
- `random(): ret Float` (0.0 to 1.0)
- `random(min: Int, max: Int): ret Int`

## 3. String Utilities (`lib/string.rae`)

Methods (resolved by type):
- `length(this: String): ret Int`
- `compare(this: String, other: String): ret Int`
- `concat(this: String, other: String): ret String`
- `sub(this: String, start: Int, len: Int): ret String`
- `contains(this: String, sub: String): ret Bool`
- `toFloat(this: String): ret Float`
- `toInt(this: String): ret Int`

## 4. Diagnostics & I/O (`lib/io.rae`)

- `log(value: Any)` - Newline.
- `logS(value: Any)` - Stream (no newline).
- `readLine(): ret String`
- `readChar(): ret Char`

## 5. Call Ergonomics & Parameter Naming

Rae follows a "Named-by-Default" philosophy but provides two specific affordances for conciseness.

### The Rules
1.  **The Receiver Rule**: The receiver in a member call (`obj.method`) is always positional.
2.  **The Parens-First Rule**: The first argument appearing inside the parentheses `(...)` of any call is allowed to be positional.
3.  **The Explicit Rule**: All other arguments must be explicitly named.

### Examples in Practice

Using `func add(this: mod List(T), value: T)`:

| Syntax Style | Code | Analysis |
| :--- | :--- | :--- |
| **Member Positional** | `myList.add(10)` | `myList` is receiver (Rule 1), `10` is first in parens (Rule 2). |
| **Member Named** | `myList.add(value: 10)` | `myList` is receiver, `value` is named (optional but allowed). |
| **Regular Positional** | `add(myList, value: 10)` | `myList` is first in parens (Rule 2), `value` must be named (Rule 3). |
| **Regular Full-Named** | `add(this: myList, value: 10)` | Both named (always allowed). |

**Note**: Under these rules, `add(myList, 10)` is **invalid** because `10` is the second argument in the parentheses and thus must be named. However, `myList.add(10)` is **valid** because `10` is the first argument inside the parentheses in that specific syntax.

---

## Dispatch and C-Backend Mangling

Since Rae allows multiple functions with the same name if their signatures (specifically the first parameter) differ, the C backend must generate unique names to avoid collisions.

### Mangling Strategy
The compiler should transform Rae signatures into C names using the pattern:
`[TypeName]_[FunctionName]` (using camelCase or snake_case for the prefix).

**Example:**
| Rae Signature | Generated C Name |
| :--- | :--- |
| `func length(this: List(T))` | `List_length` |
| `func length(this: String)` | `String_length` |
| `func add(this: mod List(T))` | `List_add` |
| `func abs(n: Int)` | `Int_abs` |

### Member Syntax Resolution
When the compiler encounters `myList.length()`:
1. It determines the type of `myList` (e.g., `List(Int)`).
2. It searches for a function named `length` where the first parameter is compatible with `List(Int)`.
3. It emits a call to the mangled C function `List_length`.

### Positional First Argument
`add(myList, 10)` is identical to `myList.add(10)`. The resolution logic remains the same.