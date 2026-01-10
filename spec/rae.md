# Rae Language Specification

**Version:** 0.2 (Strict)
**Scope:** Lexer, parser, AST, pretty-printer, VM, C backend

---

## 1. Lexical Structure

### 1.0 Source Files

*   **Encoding:** Rae source files (`.rae`) must be encoded in **UTF-8**.
*   **Indentation:** Rae uses **2 spaces** for indentation. Tabs are prohibited for indentation.

### 1.1 Comments

* **Line comment:** `# comment until newline`
* **Multiline comment:** `#[ comment across multiple lines ]#`

### 1.2 Identifiers

* Pattern: `[a-zA-Z_][a-zA-Z0-9_]*`
* **Naming Conventions:**
  * Function names MUST be `camelCase` (e.g. `add`, `removeLast`)
  * Type names MUST be `PascalCase` (e.g. `List`, `Map`, `Point`)
* Case-sensitive

### 1.3 Keywords

```
type func def ret spawn
own view mod opt
if else match case
true false none
and or not is
pub priv extern pack default
```

### 1.4 Literals

* **Integer:** `0 | [1-9][0-9]*`
* **Float:** `[0-9]+\.[0-9]*`
* **String:** `"text"` (supports `\n \t \\ \" {expression}`)
* **Char:** `'c'`
* **Boolean:** `true false`
* **None literal:** `none`

### 1.5 Operators and Punctuation

```
=  =>  +  -  *  /  %
<  >  <=  >=  is
( )  { }  [ ]
,  :  .
```

---

## 2. Core Language Rules

### 2.1 Declarations

* `def` is used exclusively for **bindings** (variables, constants, locals).
* `func` is used exclusively for **functions**.
* `def` is never used for functions.

### 2.2 Functions

* All function parameters are **named**.
* There are **no positional arguments**.
* Functions with a return type must use an explicit `ret` statement.
* Nothing is implicitly returned.

Example:
```rae
func add(a: Int, b: Int): ret Int {
  ret a + b
}
```

Usage:
```rae
def sum: Int = add(a: 10, b: 20)
```

### 2.3 Indexing

* `[]` is reserved **exclusively for indexing**.
* `[]` is never used for generics or type application.

Example:
```rae
def x: Int = values[0]
```

### 2.4 Type Application (Generics)

* Generic types use **parentheses** for type application.
* Parentheses in type positions mean *type application*, not calls.

Examples:
```rae
List(Int)
Map(String, Int)
List(List(Int))
```

### 2.5 Object and List Literals

* Typed literals use the form: `Type { ... }`
* Untyped literals `{ ... }` are allowed **only** when the expected type is explicitly known from context (assignment or return).

Examples:
```rae
def v1: Point = Point { x: 1, y: 2 }
def v2: Point = { x: 3, y: 4 } # OK: type known from annotation
def values: List(Int) = { 1, 2, 3 } # OK: type known
```

Invalid:
```rae
def bad = { x: 1, y: 2 } # ERROR: type required for `{}` literal
```

### 2.6 No Constructors - Use Factory Functions

* Rae does **not** have constructors.
* Creation logic is expressed using normal functions.

Example factory function:
```rae
func createPoint(x: Int, y: Int): ret Point {
  ret Point { x: x, y: y }
}
```

---

## 3. Symbol Meanings (Strict)

* `:` → type annotation, parameter typing, field assignment
* `()` → application (type application in types, calls in values)
* `{}` → literal (object or collection)
* `[]` → indexing exclusively

Each symbol has **exactly one meaning**. No lookahead-based ambiguity.

---

## 4. Expressions & Control Flow

### 4.1 Operators

* Arithmetic: `+ - * / %`
* Comparison: `< > <= >= is`
* Logical: `and or not`

### 4.2 Control Flow

```rae
if condition { ... } else { ... }

loop def i: Int = 0, i < 10, ++i { ... }

match value {
  case 0 { ... }
  default { ... }
}
```

### 4.3 Modules & Imports

```rae
import "path/to/module"
export "path/to/shared"
```

---

**End of Rae Specification v0.2**

```

## Tooling: Formatter CLI

The Phase 1 toolchain ships `rae format` to pretty-print canonical Rae code. By
default the command writes to stdout so you can inspect or pipe the result. Use
`--write` (or `-w`) to rewrite the original file in place, or `--output
<path>` to send the formatted code to a specific destination. The two flags are
mutually exclusive so scripts cannot accidentally clobber multiple files.

```