# Rae Language Specification

**Version:** 0.3 (Strict)
**Scope:** Lexer, parser, AST, pretty-printer, VM, C backend

---

## 1. Lexical Structure

### 1.0 Source Files

*   **Encoding:** Rae source files (`.rae`) must be encoded in **UTF-8**.
*   **Indentation:** Rae uses **2 spaces** for indentation. Tabs are prohibited for indentation.
*   **Line Endings:** Rae files must use LF (`\n`) line endings.

### 1.1 Comments

* **Line comment:** `# comment until newline`
* **Multiline comment:** `#[ comment across multiple lines ]#`

### 1.2 Identifiers

* Pattern: `[a-zA-Z_][a-zA-Z0-9_]*`
* **Naming Conventions:**
  * Function names MUST be `camelCase` (e.g. `add`, `removeLast`)
  * Type names MUST be `PascalCase` (e.g. `List`, `Map`, `Point`)
  * Variable names MUST be `camelCase`.
* Case-sensitive

### 1.3 Keywords

```
type func let ret spawn
view mod opt
if else match case
true false none
and or not is
pub priv extern pack default enum loop in
```

### 1.4 Literals

* **Integer:** `0 | [1-9][0-9]*`
* **Float:** `[0-9]+\.[0-9]*`
* **String:** `"text"` (supports `\n \t \\ \" {expression}`)
* **Raw String:** `r"text"` or `r#"text"#`
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

## 2. Bindings and Aliasing

Rae distinguishes between **value replacement** and **storage aliasing**.

### 2.1 The `let` keyword

`let` is the only keyword for introducing local bindings. `def` is prohibited for locals.

### 2.2 Value replacement (`=`) vs Alias binding (`=>`)

*   **`=` (Copy/Replace)**: Replaces the value in the current storage. SEMANTIC: Always a deep copy.
*   **`=>` (Alias Binding)**: Creates a stable alias to an existing storage location. 
    *   `=>` is **only** legal when the target type is `mod T` or `view T`. 
    *   Alias bindings are **bind-once**. Rebinding an alias (using `=>` again on the same name) is illegal.
    *   Copying *through* a `mod` alias using `=` is legal and modifies the underlying target.

Examples:
```rae
let v: Pos = { x: 5, y: 12 }
v = { x: 9, y: 9 }          # Legal: value copy

let w: mod Pos => transform.position()
w = { x: 1, y: 2 }          # Legal: copies value into aliased storage
# w => other.position()     # ERROR: alias rebinding is illegal
```

### 2.3 Type Visibility Rule

With `let`, the binding's type MUST appear on the left side only. The right side must be type-free for the top-level expression.

With `ret`, structural literals MUST be explicitly typed at the return site for clarity.

Legal:
```rae
let i: Int = 5
let v: Pos = { x: 5, y: 12 }
let v: Pos = {}
let v: Pos
ret Color { r: 255, g: 0, b: 0, a: 255 }
```

Illegal:
```rae
let i = Int { 5 }               # ERROR: type on wrong side
let v: Pos = Pos { x: 5 }       # ERROR: redundant type on RHS
ret { r: 255, g: 0, b: 0 }      # ERROR: structural literal must be typed in ret
```

**Exception:** Nested structural literals MUST be typed when their type is not otherwise known from immediate context.
```rae
let t: Transform = {
  position: Pos { y: 12 }       # REQUIRED: Pos type is introduced here
}
```

---

## 3. Memory Safety and Initialization

Rae guarantees that all memory is initialized before use. Every variable and type member is automatically assigned a default value.

### 3.1 Default Values

| Type | Default Value | Literal Syntax |
|------|---------------|----------------|
| `Int` | `0` | `0` |
| `Float` | `0.0` | `0.0` |
| `Bool` | `false` | `false` |
| `String` | `""` | `""` |
| `opt T` | `none` | `none` |
| `T` (struct) | All members defaulted | `T {}` |

### 3.2 Explicit Default Literal

The canonical expression for a default value of a type `T` is `T {}`.
```rae
let result: Int = v.some(that: (o or Pos {}))
```

---

## 4. Optionality and References

### 4.1 The `opt` rule

*   `opt T` represents an optional owned value.
*   `opt` members in types are allowed.
*   `opt` parameters are allowed.
*   **`opt view T` and `opt mod T` are NOT allowed.** Optionality and aliasing must be handled separately to avoid hidden lifetime complexity.

### 4.2 Aliasing Scope

`mod T` and `view T` references are short-lived handles.
*   They may appear in: function parameters, return types, and local bindings.
*   They **MUST NOT** be used as members of types.

---

## 5. Core Language Rules

### 5.1 Functions

* All function parameters are **named**.
* **Positional First Argument:** The first argument of a function call can be passed positionally if it is unambiguous (e.g. `log("Hi")`).
* Functions with a return type must use an explicit `ret` statement.

### 5.2 Indexing

* `[]` is reserved **exclusively for indexing**.

### 5.3 Member-Call Syntax Sugar

Any function whose first parameter matches a type `T` can be called using member syntax on an expression of type `T`: `p.x()` desugars to `x(p)`.

---

**End of Rae Specification v0.3**

