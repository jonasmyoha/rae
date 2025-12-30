# Rae Design Philosophy

## Core Principles

1. **Explicit ownership** - No hidden aliasing or copying
2. **Natural syntax** - Reads like structured English
3. **AI-assisted** - Designed for clarity in AI collaboration
4. **Dual-mode execution** - Same source runs interpreted or compiled

## Ownership System

- `own T` - Exclusive ownership
- `view T` - Read-only borrow
- `mod T` - Mutable borrow
- `opt T` - Optional wrapper

## Assignment Semantics

- `=` always copies values
- `=>` transfers ownership
- No implicit conversions

## Execution Modes

Rae supports two complementary execution paths without changing source code:

1. **Compiled mode**
   - Emit native code through the C backend for zero runtime overhead
   - Target performance-critical systems (engine core, rendering, networking)
2. **Interpreted mode**
   - Hot-reload during development, load scripts at runtime, stream updates over the network
   - Ideal for game logic, rapid iteration, modding, and live service updates

The language and tooling are being built so teams can prototype with the interpreter, then ship with the compiler—all from the same codebase.
