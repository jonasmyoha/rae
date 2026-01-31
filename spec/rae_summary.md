# Rae Language Summary

## Goals
Rae is designed for humans and AI agents to share the same codebase with maximum clarity.

## Core Rules
1. **Binding**: Use `let` for local variables. `def` is reserved for future top-level usage or discarded.
2. **Type Visibility**: Type must be on the left side of `let`. RHS must be type-free for direct assignments.
3. **Initialization**: Everything is default-initialized. `Type {}` is the default literal.
4. **Aliasing**: `=` always copies. `=>` binds a reference (`mod` or `view`). References are bind-once.
5. **Optionality**: `opt T` is allowed for owned values. `opt view T` and `opt mod T` are prohibited.
6. **Naming**: `camelCase` for functions and variables, `PascalCase` for types.

## Syntax Cheat Sheet
```rae
# Binding
let x: Int = 10
let p: Point = { x: 1, y: 2 }
let p2: Point                   # Default init

# Aliasing
let r: view Point => p
let m: mod Point => p
m.x = 20                        # Copy into aliased storage
# m => other                   # ERROR: re-binding alias

# Control Flow
if x > 0 { ... }
loop let i: Int in list { ... }
match val {
  case 1 => "one"
  default => "other"
}

# Functions
func add(a: Int, b: Int): ret Int {
  ret a + b
}
```