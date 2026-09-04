# Rae naming conventions

Rae standardizes on the following naming style. It is a **convention**, not a
parser rule — see "Enforcement" below.

| Kind | Style | Examples |
|------|-------|----------|
| Types, structs, enums, modules | `PascalCase` | `RenderSettings`, `List`, `RenderMode` |
| Functions | `camelCase` | `renderScene`, `loadName` |
| Variables and parameters | `camelCase` | `maxDepth`, `count` |
| Constants (`const`) | `camelCase` | `pi`, `tau`, `maxPlayers` |
| Enum cases | `camelCase` | `RenderMode.wireframe`, `RenderMode.pathTraced` |

```rae
type RenderSettings {
  maxDepth: Int
}

enum RenderMode {
  wireframe
  solid
  pathTraced
}

const defaultRenderMode = RenderMode.pathTraced

func renderScene(settings: view RenderSettings) { }
```

## No semantics attached to capitalization

Capitalization carries **no** meaning in the language — it does not affect
parsing, name resolution, visibility, or types. `const` values and enum cases
are values, so they use `camelCase` like other values. (Earlier drafts used
`UPPER_SNAKE_CASE` for constants; that is **not** the Rae convention.)

## Enforcement

Naming style is **not** enforced by the parser or name resolver. A hard parse
error on an "unconventional" name would break:

- generated code,
- bindings to foreign / C APIs (whose names follow other conventions),
- mechanical migrations and refactors,
- intentionally unusual names.

**Enforced as a hard compiler error (#790).** The convention is not optional:
the parser rejects a snake_case / SCREAMING_SNAKE_CASE identifier and a
wrong-first-letter-case name at the declaration site — types/enums/modules must
be PascalCase; functions, parameters, locals, struct fields, enum cases and
constants must be camelCase (`_` anywhere, or the wrong case for the first
letter, is the error).

**C-interop is the one exception**, matching how `extern` is Rae's C-boundary
escape hatch: an `extern` function name and its parameters name C symbols; a
`type X: c_struct` name mirrors a C struct (e.g. `div_t`); and the generated
low-level bindings under `lib/webgpu/` mirror the WebGPU C API verbatim. Those
are skipped. Everything else is a hard error, so a stray `const MAX_RETRIES` or
`func my_func` fails to compile in both the Live and Compiled front ends.
