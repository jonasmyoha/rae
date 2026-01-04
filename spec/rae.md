# Rae Language Specification

**Version:** 0.1 (WIP)
**Scope:** Lexer, parser, AST, pretty-printer (no codegen, no typechecking)

---

## 1. Lexical Structure

### 1.1 Comments

* **Line comment:** `# comment until newline`
* **Multiline comment:** `#[ comment across multiple lines ]#`

### 1.2 Identifiers

* Pattern: `[a-zA-Z_][a-zA-Z0-9_]*`
* Case-sensitive

### 1.3 Keywords

```
type func def ret spawn
own view mod opt
if else match case
true false none
and or not is
pub priv extern
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

```rae
type Point: pub {
  x: Int
  y: Int
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

```rae
func add(a: view Point, b: view Point): pub ret Point {
  ret (x: a.x + b.x, y: a.y + b.y)
}
```

Rules:
* Parameters use `name: Type`
* `def` is **not allowed** in parameters
* Function properties (pub, spawn) come **after the parameter list**, space-separated
* Return declaration (`ret`) comes last

### 4.2 Multiple Return Values

```rae
func divide(a: Int, b: Int): pub ret result: Int, error: opt Error {
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
- When a file does not declare any explicit imports and either a `.raepack` file enables auto folders or the folder contains only one `.rae` entry point, the CLI automatically scans that directory (and subdirectories) for `.rae` files and includes them. This keeps tiny apps ergonomic—drop `main.rae` plus helpers in a folder, or add a `.raepack`, and run `rae run path/to/main.rae` without bookkeeping.

### 8.6 Package Descriptor (`.raepack`)

- Packages (apps, libraries, tool bundles) are described by an optional `*.raepack` file that uses Rae syntax. Example:

```rae
package MyCoolApp {
  brand {
    name: "My Cool App"
    identifier: "com.example.cool"
  }
  auto_folders: ["./src", "./ui"]
  targets {
    desktop {
      main: "src/main.rae"
    }
  }
}
```

- Key ideas:
  - **`package <Name>`** defines the canonical package name. This value can be referenced from source code later (e.g., `import package.name`) so renaming happens in one place.
  - **`brand` block** stores marketing metadata (display name, identifiers, icons) so UI code can reference `package.brand.name`.
  - **`auto_folders`** lists directories that should behave as implicit import roots. If omitted, the CLI falls back to the single-file heuristic (one `.rae` entry point) so tutorial projects still Just Work™.
  - **`targets`** describe build outputs (desktop, mobile, web). Each target can set its own `main`, build flags, and bundling directives.
  - Additional sections (dependencies, assets, versioning, release/debug profiles) will live here as the package manager evolves.

- **Defaults:** If no `.raepack` is present, and the compiler is run in a directory containing only one `.rae` file (e.g., `main.rae`), that directory is treated as the package root and auto-imports are enabled implicitly. As soon as multiple entry points or more structure is needed, creators add a `.raepack` to explicitly describe the package.

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
