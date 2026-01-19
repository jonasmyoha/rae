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
view mod opt
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

## 2. Memory Safety and Initialization

Rae guarantees that all memory is initialized before use. Every variable and type member is automatically assigned a default value based on its type if no explicit initializer is provided. This eliminates entire classes of bugs related to garbage values and uninitialized memory.

### 2.1 Default Values

| Type | Default Value |
|------|---------------|
| `Int` | `0` |
| `Float` | `0.0` |
| `Bool` | `false` |
| `String` | `""` (empty string) |
| `List(T)` | `[]` (empty list) |
| `Char` | `'\0'` (null character) |
| `mod T`, `view T` | Invalid without explicit binding |

### 2.2 Type Initialization

When a complex type (struct) is instantiated, all members are recursively initialized to their respective default values.

```rae
type State {
  score: Int
  active: Bool
}

func main() {
  # score is 0, active is false
  def s: State
}
```

---

## 3. Core Language Rules

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

## 5. Values, References, Optionality, Identity (id/key), and Lifetimes

Rae uses a strict reference model designed for performance and safety, prioritizing explicit semantics over implicit magic. Rae's model is aliasing-friendly (multiple references to the same storage are allowed) but enforces lifetime safety.

### 5.1 Assignment vs. Binding

*   **Assignment (`=`)**: **Always copies**. Observable semantics are deep copy. No implicit moves or sharing.
*   **Binding (`=>`)**: **Never copies**. Binds or rebinds a reference-like slot (alias) to a storage location. Only works with bindable slot types (references or optionals).

### 5.2 Canonical Type Properties

Type properties stack in a single canonical order:
`opt` → `{view, mod, id, key}` → `T`

#### 5.2.1 Value Types
*   **`T`**: Self-contained value (value semantics). Stored inline.
*   **`opt T`**: Optional value. Self-contained (none/some).
*   **`id T`**: Typed identity token. Underlying representation is `Int`. Copyable value.
*   **`key T`**: Typed identity token. Underlying representation is `String`. Copyable value.
*   **`opt id T` / `opt key T`**: Optional identity handles.

#### 5.2.2 Reference Types (Aliases)
*   **`view T`**: Read-only reference.
*   **`mod T`**: Modifiable reference.
*   **`opt view T`**: Optional read-only reference.
*   **`opt mod T`**: Optional modifiable reference.

### 5.3 Composition Matrix

| Type | Storable in Fields | Copyable (`=`) | Bindable (`=>`) | Underline Type |
| :--- | :---: | :---: | :---: | :--- |
| `T` | Yes | Yes | No | Internal |
| `opt T` | Yes | Yes | Yes | Internal |
| `view T` | No | No | Yes | Pointer |
| `mod T` | No | No | Yes | Pointer |
| `id T` | Yes | Yes | No | `Int` |
| `key T` | Yes | Yes | No | `String` |

### 5.4 Identity: `id` and `key`

Identity types are opaque handles to objects, typically managed by a `Store` or `Pool`.
*   `id T` is for fast local gameplay/UI handles.
*   `key T` is for stable external identifiers (URLs, DB keys).

### 5.5 Lifetime and Escape Rules

References are zero-overhead but must be safe.
1.  **Scope**: A reference must not outlive the storage it points to.
2.  **No Escaping**: References (`view`/`mod`) **MUST NOT** be stored in long-lived containers (e.g., `List(view T)` is illegal) or struct fields.
3.  **Return Restriction**: A function can only return a reference if it is derived from an input parameter. Returning a reference to a local variable or a temporary is prohibited.

### 5.6 Member-Call Syntax Sugar

Any function whose first parameter matches a type `T` can be called using member syntax on an expression of type `T`:
*   `p.x()` desugars to `x(p)`.
*   Resolution is based on the receiver type. Ambiguous matches result in a compile-time error.

### 5.7 Call Syntax: Positional First Arguments

The first argument of a function call can be passed positionally if it is unambiguous.
*   `getX(p)` is equivalent to `getX(p: p)`.
*   Subsequent arguments must be named.

Standard functions like `log()` and `logS()` are typically called positionally:
*   `log("Hello")` is the preferred style.
*   `log(message: "Hello")` is technically allowed but discouraged.

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