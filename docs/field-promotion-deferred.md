# Decision: field promotion (`using`) is deferred (#648)

**Status: deferred.** Rae does not have field promotion, and there is no plan to
add it in the near term. This note records why, and the one hard constraint any
future design must satisfy.

## What "field promotion" would mean

Promoting a struct field would let a containing type expose the field's members
as if they were its own — e.g. `using size: Vec2` on

```rae
alias Size = Vec2
type Rect {
  position: Vec2
  size: Size
}
```

so that `rect.x` reads `rect.size.x` without naming `size`.

## Why it is deferred — and the constraint if it is ever added

Bare promotion (`using size: Vec2`) is **broken by construction** the moment a
struct has two fields of the same type, and type aliases (#647/#660) guarantee
that case is common: `Rect` above has `position: Vec2` and `size: Size` (= Vec2),
so promoting both makes `rect.x` ambiguous — is it `position.x` or `size.x`? An
alias is the *same* type, so there is no way to tell the two apart by type.

Therefore, **if field promotion is ever added it must ship with per-field rename
in the same change** — promotion without rename must not exist. The spelling to
adopt is an explicit rename of the promoted members:

```rae
using size: Vec2 as { width: x, height: y }   # rect.width -> rect.size.x, rect.height -> rect.size.y
```

This makes the promoted names unambiguous and intentional, and side-steps the
collision entirely: two `Vec2` fields promote to distinct names the author chose.

## What already covers the common need (read-only promotion)

Rae's dot-syntax (UFCS) functions already give **read-only** promotion today,
with no new feature and no collision risk:

```rae
func width(rect: view Rect) ret Float { ret rect.size.x }
# usage: rect.width()
```

Any first-parameter function is callable as `value.func(...)`, so a hand-written
accessor promotes a nested field under whatever name you pick. This is enough for
the vast majority of "I want `rect.width`" cases, is explicit, and never
collides. Field promotion would only add sugar (dropping the `()` and allowing
writes), which is not worth the ambiguity surface right now.

## Summary

- No `using` / field promotion for now.
- If added later: per-field rename is mandatory in the same change
  (`using size: Vec2 as { width: x, height: y }`), because `alias` makes
  same-type field collisions unavoidable.
- Read-only promotion is already available via dot-syntax accessor functions.
