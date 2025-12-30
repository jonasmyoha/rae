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
pub priv
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
  case _ {
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
