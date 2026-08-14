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
view mod val opt
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
    *   `=>` is **required, and only legal, when the target type contains `view` or `mod` at any level** — `view T`, `mod T`, and also `opt view T` and `opt mod T`.
    *   Alias bindings are **bind-once**. Rebinding an alias (using `=>` again on the same name) is illegal.
    *   Copying *through* a `mod` alias using `=` is legal and modifies the underlying target.

The "at any level" rule keeps `=` honest. `=` produces an INDEPENDENT value; a
type whose payload is a reference cannot be copied independently, because
duplicating it would yield a second name onto the same storage. Requiring `=>`
for those types means `=` never has to be explained away for a special case.

Examples:
```rae
let v: Pos = { x: 5, y: 12 }
v = { x: 9, y: 9 }          # Legal: value copy

let w: mod Pos => transform.position()
w = { x: 1, y: 2 }          # Legal: copies value into aliased storage
# w => other.position()     # ERROR: alias rebinding is illegal
```

### 2.3 Semantic stability of `=` and `=>`

The meaning of `=` and `=>` is **fixed by the operator alone**. It does not vary
with the type on either side, with the size of that type, or with what the
compiler is able to optimise.

*   `=` means **copy**. The programmer reads `=` and knows a separate value is
    produced, whether the type is `Int` or a struct holding lists and strings.
*   `=>` means **bind**. The programmer reads `=>` and knows no independent value
    is produced; the name refers to storage that already exists.

Rae has no type-dependent assignment rule. There is no category of type that is
silently referenced by `=`, and none that is silently copied by `=>`.

**Optimisation is permitted; reinterpretation is not.** A compiler may implement
a binding however it likes, provided every observable Rae semantic is preserved:

*   `view T` is a **window**, not a snapshot. If the underlying storage changes
    while the view is alive, reads through the view observe the new value. A
    compiler may implement a `view` as a physical copy ONLY where it can
    establish that no such change is observable during the binding's lifetime.
*   `mod T` requires **true aliasing**. Writes through the alias must be
    observable in the original. This is the meaning of `mod`, not a strategy for
    implementing it.
*   `=` may have its physical copy elided whenever the result is
    indistinguishable.

### 2.4 Type Visibility Rule

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
*   **`opt view T` and `opt mod T` are allowed**, in exactly the positions any
    reference is allowed: function parameters, return types, and local bindings
    (§4.3). They are bound with `=>`, per §2.2.

`opt` composes with the reference modes rather than competing with them.
Optionality adds no lifetime obligation to a reference: the `none` case has no
referent at all, and the `some` case carries exactly the obligation `view T` or
`mod T` already carries. The rule that keeps stored references from outliving
their target is §4.3, which excludes ALL references from type members — optional
or not — so `opt` is not what creates that risk.

```rae
func selected(list: view Playlist) ret opt view Track
func draw(scene: view Scene, highlight: opt view Track)

let sel: opt view Track => selected(list: playlist)
```

An earlier version of this specification prohibited these types, citing hidden
lifetime complexity. That rationale did not hold: the complexity it named is
handled by §4.3, and forbidding them made optionality behave differently
depending on whether its payload was a value or a reference — the type-dependent
rule §2.3 exists to rule out.

### 4.2 Binding the payload of an optional (`if let`)

`if let` conditionally introduces a binding to the value inside an optional, for
the duration of the branch.

```rae
if let track: view Track => library.current {
    log("{track.title}")
} else {
    log("nothing playing")
}
```

*   The binding target MUST be `view T` or `mod T`, and the operator MUST be
    `=>`. This follows from §2.2: `=>` is the binding operator, and it is legal
    only for those two target types.
*   **`=` is NOT permitted in `if let`.** `=` means copy (§2.3), and `if let`
    exists to give access to a value, not to duplicate one. A copy is written as
    an ordinary statement inside the branch, where it is visible as a copy:

```rae
if let track: view Track => library.current {
    let mine: Track = track          # explicit copy, if one is wanted
}
```

*   The binding is in scope only within the `if` branch. It is not in scope in
    the `else` branch, where by definition there is no value.
*   Bind-once applies (§2.2): the name introduced by `if let` may not be rebound.
*   `mod` narrowing writes through to the optional's payload:

```rae
if let track: mod Track => library.current {
    track.plays = track.plays + 1    # the stored track is updated
}
```

`if let` NARROWS an optional: inside the branch, the name denotes a `view T` or
`mod T` onto the payload, and the branch is entered only when there is one. The
optional it narrows may be an `opt T`, an `opt view T` or an `opt mod T` — in the
last two, narrowing yields a reference to the same storage the optional
referenced, not a second level of indirection.

Narrowing is the preferred way to reach the value inside an optional even where
a local of reference-optional type would be legal, because absence is handled
structurally by the branch rather than by a check the programmer must remember
to write.

The right-hand side may be any expression producing an optional, including a
function call:

```rae
if let seconds: view Int => durations.get(k: "intro") { ... }
```

A binding to a produced value obliges the implementation to give that value
storage for the lifetime of the binding. That is an implementation requirement,
not a restriction on the programmer (§2.3).

`is none` and `is not none` remain available and are preferred where the value
itself is not wanted:

```rae
if found is none {
    ret noEntity()
}
```

### 4.3 Aliasing Scope

`mod T` and `view T` references are short-lived handles.
*   They may appear in: function parameters, return types, and local bindings.
*   They **MUST NOT** be used as members of types.

#### 4.3.1 A returned reference must outlive the call

A function that returns `view T` or `mod T` hands the caller a reference, so
whatever it names **MUST** still exist after the function returns. Three
things satisfy that, and only those three:

*   a **parameter** (the caller owns that storage),
*   a **global**, and
*   anything reached through one of those — a field, an element, or a
    reference returned by another function and forwarded on.

A `let`/`var` in the function's own body does not: its storage dies at `ret`.
Returning a reference to one is an error — *reference escapes local storage*.

Neither does a **temporary** — a value produced by the expression itself,
with no storage of its own. So a reference to a value-returning call is an
error too:

```rae
func pick(tracks: view List(Track)) ret opt view Track {
  ret view tracks.at(index: 0)   # ERROR: `at` returns T — a value
}
```

`at` is declared `ret T`, so its result is a fresh value that dies with
`pick`. To hand back a window into the list, the accessor itself has to
return a reference. Forwarding one is fine, because the reference then names
storage that already outlives both calls:

```rae
func forward(lib: view Library) ret opt view Track {
  ret view nowPlaying(lib: view lib)   # OK: nowPlaying returns a reference
}
```

The same rule applies to arguments: if a call returns a reference, passing it
a temporary is an error, because the returned reference may point into
something that dies at the end of the statement.

```rae
let a: mod Int => xMod({ x: 1 })   # ERROR: reference to a temporary literal
```

Passing a temporary to a `view` parameter of a **value**-returning function is
fine — the temporary outlives the call.

---

## 5. Core Language Rules

### 5.1 Functions

* All function parameters are **named**.
* **Positional First Argument:** The first argument of a function call can be passed positionally if it is unambiguous (e.g. `log("Hi")`).
* Functions with a return type must use an explicit `ret` statement.

#### 5.1.1 Parameter Passing Semantics

Rae uses "borrow-by-default" for function parameters to prevent accidental performance overhead from copying large structures.

*   **`view` by default:** Any parameter declared as `x: T` is semantically a `view` reference. It is read-only and does not transfer ownership.
*   **`mod T`**: Explicitly allows mutation of the caller's value.
*   **`val T`**: Explicitly forces the parameter to be passed by value (copied). This is useful when the function needs its own owned copy to mutate locally without affecting the caller, or for small primitive-like types.
*   **Optimization (SVO)**: The compiler may internally pass small, trivially copyable types (e.g., `Int`, `Float`, `Vec2`, `Color`) by value even if declared as `view` (default), provided it does not change observable semantics.

Examples:
```rae
func draw(p: Point)           # Semantically: view Point (read-only)
func move(p: mod Point)       # mutable reference
func update(p: val Point)     # explicitly passed by value (copy)
```

### 5.2 Indexing

* `[]` is reserved **exclusively for indexing**.

### 5.3 Member-Call Syntax Sugar

Any function whose first parameter matches a type `T` can be called using member syntax on an expression of type `T`: `p.x()` desugars to `x(p)`.

---

**End of Rae Specification v0.3**

