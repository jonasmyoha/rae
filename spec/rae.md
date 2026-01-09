# Rae Language Specification

**Version:** 0.1 (WIP)
**Scope:** Lexer, parser, AST, pretty-printer (no codegen, no typechecking)

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
  * Type names MUST be `PascalCase` (e.g. `List`, `Map`, `Ptr`)
* Case-sensitive

### 1.3 Keywords

```
type func def ret spawn
own view mod opt
if else match case
true false none
and or not is
pub priv extern pack
```

### 1.4 Literals

* **Integer:** `0 | [1-9][0-9]*`
* **String:** `"text"` (supports `\n \t \\ \"`)
* **Boolean:** `true false`
* **None literal:** `none`

### 1.5 Operators and Punctuation

```
=  =>  +  -  *  /  %
<  >  <=  >=
( )  { }  [ ]
,  :  .
```

---

## 2. Core Concepts

### 2.1 Value vs Ownership

* **Plain type (`T`)** → value, copied by `=`
* **`own T`** → owning reference
* **`view T`** → read-only borrowed view
* **`mod T`** → mutable borrowed view
* **`opt T`** → optional wrapper for any type

### 2.2 Assignment and Binding

* **`=`** Always performs a **value copy**, never aliases
* **`=>`** Binds an owning reference (used only with `own`)

---

## 3. Types

### 3.1 Type Definition

A colon (`:`) after the type name is used to introduce properties (like `priv`). If a type has no properties, the colon should be omitted. Type names MUST follow the `PascalCase` convention.

With properties:
```rae
type Point: priv {
  x: Int
  y: Int
}
```

Without properties:
```rae
type Vector {
  x: Float
  y: Float
}
```

Rules:
* Fields use `name: Type`
* `def` is **not allowed** in type fields

### 3.2 Optional Types

```rae
opt Int
opt own Texture
```

---

## 4. Functions

### 4.1 Function Definition

Function names MUST follow the `camelCase` convention.

```rae
# Functions are public by default
func add(a: view Point, b: view Point): ret Point {
  ret (x: a.x + b.x, y: a.y + b.y)
}

# A private function
func internalHelper(a: Int): priv ret Int {
    ret a * 2
}
```

Rules:
* Parameters use `name: Type`
* `def` is **not allowed** in parameters
* Function properties (like `priv` or `spawn`) come **after the parameter list**, space-separated.
* Return declaration (`ret`) comes last.

### 4.2 Multiple Return Values

```rae
func divide(a: Int, b: Int): ret result: Int, error: opt Error {
  if b is 0 {
    ret error: Error(message: "divide by zero")
  }
  ret result: a / b
}
```

### 4.3 Implicit `this`

* In any function, **`this` refers to the first parameter**

### 4.4 External Functions

```rae
extern func tinyexpr_eval(expr: String): ret Int
```

* `extern` declarations have **no body**; they describe functions implemented outside the current Rae module (typically in C).
* The signature follows the regular `func` rules (parameters, optional properties, and `ret` clause).
* Only a single return value is supported. At present, the C backend maps:
  * `Int` → `int64_t`
  * `String` → `const char*` (returning `String` is only allowed on `extern` functions)
* Calls to `extern` functions look like normal calls; it is the builder’s responsibility to link the generated C with the external definition.
* The compiler does **not** validate that an implementation exists—if you forget to link one, the native linker will report an undefined symbol.

---

## 5. Concurrency

### 5.1 Spawn Functions

```rae
func heavyWork(id: Int): spawn {
  log("working")
}
```

* `spawn` is a **function property**

### 5.2 Spawn Calls

```rae
spawn heavyWork(id: 1)
```

---

## 6. Local Bindings

```rae
def x: Int = 10
def p: own Point => (x: 1, y: 2)
```

Rules:
* `def` is used **only** for locals
* Type annotation is required

---

## 7. Control Flow

### 7.1 If / Else

```rae
if x > 0 {
  log("positive")
} else {
  log("negative")
}
```

### 7.2 Match

```rae
match value {
  case 0 {
    log("zero")
  }
  default {
    log("other")
  }
}
```

---

## 8. Expressions

### 8.1 Operators

* Arithmetic: `+ - * / %`
* Comparison: `< > <= >= is`
* Logical: `and or not`

### 8.2 Member Access

```rae
point.x
```

### 8.3 Function Call

```rae
add(a: p, b: q)
spawn heavyWork(id: 2)
```

### 8.4 Logging Built-ins

Rae ships two lightweight logging helpers:

```
log(Any)   # prints the value followed by a newline (like println)
logS(value: Any)  # stream log without a newline (like printf)
```

Both variants flush stdout immediately, so console output appears in the order
you invoke them even under watch mode or long-running examples.

### 8.5 Modules & Imports

- Each `.rae` file automatically becomes a module whose name equals its project-root-relative path without the `.rae` suffix (e.g., `examples/multifile_report/ui/header`).
- Import paths use `/` separators. Absolute imports start at the project root (no leading `./` or drive prefixes). Relative imports may begin with `./` or `../` and are resolved against the current file’s directory.

```rae
import "examples/multifile_report/ui/header"
import "./ui/footer"
import "../shared/time"
export "examples/shared/ui/theme"
```

- Imported modules are compiled before the current file, so their declarations are globally visible.
- `export` re-exports a module for downstream consumers (future semantic passes will surface these in module metadata).

### 8.6 Package Descriptor (`.raepack`)

- Packages (apps, libraries, tool bundles) are described by an optional `*.raepack` file that uses Rae syntax. A colon after the pack name is optional. Example:

```rae
pack MyCoolApp {
  format: "raepack"
  version: 1
  defaultTarget: desktop

  targets: {
    target desktop: {
      label: "Desktop App"
      entry: "src/main.rae"
      sources: {
        source: { path: "src/", emit: both }
      }
    }
  }
}
```

- Key ideas:
  - **`pack <Name>`** defines the canonical package name.
  - **`targets`** describe build outputs (e.g., desktop, mobile, web). Each target is introduced by `target <id>:` and has its own block of properties.
    - `label`: A human-readable name for the target.
    - `entry`: The path to the main `.rae` file for this target.
    - `sources`: A block defining which files or directories are included in the build. Each `source` has a `path` and an `emit` property (`live`, `compiled`, or `both`).
  - Other metadata like `format` and `version` define the package itself.
  - Additional sections (dependencies, assets, release/debug profiles) will live here as the package manager evolves.

- **Defaults:** If no `.raepack` is present, and the compiler is run in a directory containing only one `.rae` file (e.g., `main.rae`), that directory is treated as the package root. As soon as multiple entry points or more structure is needed, creators add a `.raepack` to explicitly describe the package.

---

## 9. Constraints

1. `def` is **only** allowed for local bindings
2. `=` always copies by value
3. `=>` binds owning references
4. No semicolons
5. No implicit aliasing

---

**End of Rae Specification**

---

## Tooling: Formatter CLI

The Phase 1 toolchain ships `rae format` to pretty-print canonical Rae code. By
default the command writes to stdout so you can inspect or pipe the result. Use
`--write` (or `-w`) to rewrite the original file in place, or `--output
<path>` to send the formatted code to a specific destination. The two flags are
mutually exclusive so scripts cannot accidentally clobber multiple files.
