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

So any name that is otherwise valid is accepted. The convention is intended to
be enforced as **optional compiler/linter warnings**, never as compilation
errors, and externally-imported (`extern`) names should be exempt or easily
suppressed.

Rae does not yet have a general lint/warning framework, so for now the
convention is documented here and the lint is tracked as a focused follow-up
(see QUEUE.md). The parser's previous PascalCase/camelCase hard checks have been
removed.
