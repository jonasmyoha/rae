# Type aliases (`alias`)

`alias Name = Type` gives an existing type a second name. It is a **naming
convenience, not a new type**: the alias resolves to the *same* `TypeInfo` as its
target, so the two names are fully interchangeable — identical layout, identical
generated C, and all of the target's dot-syntax methods work on the alias.

```rae
import vec2
alias Size = Vec2

type Rect {
  position: Vec2
  size: Size          # inline Vec2 — Rect is 16 bytes (four floats), no rae_Size
}

func area(size: view Size) ret Float { ret size.x * size.y }   # callable on any Vec2
```

`Size` *is* `Vec2`: `r.size.x` works, `vec2.*` functions apply, `Size` and `Vec2`
pass to each other's parameters freely. There is no conversion and no wrapper
struct in the generated C.

## Not type safety

Because an alias is the same type, it does **not** create a distinct type the
compiler can keep apart. `func area(size: Size)` accepts any `Vec2`, and a `Vec2`
accepts any `Size`. Use an alias when you want a domain-flavored *name* for a
shared shape (`Size = Vec2`, `Rgba = Vec4`); use a real `type` when you want the
compiler to reject mixing two concepts.

## Spelling

- Contextual keyword: `alias` is recognized only at the top-level declaration
  position, spelled `alias <PascalName> = <type>`. It is **not** a reserved word
  — `let alias: String = ...` and other identifier uses are unaffected.
- The alias name must be `PascalCase` (it names a type).
- The target may be any type, including a generic instance (`alias Ints =
  List(Int)`). A short alias-of-alias chain resolves through.

## Error messages print the canonical name

An alias rewrites to its target before any diagnostic is produced, so error
messages name the **canonical** type, not the alias:

```
let v: Vec3 = someSize        # someSize: Size (= Vec2)
# error: cannot convert Vec2 to Vec3; they are different types
```

This is deliberate and cheap: the alias is resolved away at the point of use
(the type reference is rewritten to the target's name), so there is no alias name
left to print. If a future change wants diagnostics to echo the alias the author
wrote, it must thread the alias name through resolution — not free, and out of
scope for the convenience feature.
