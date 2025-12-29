# Rae Design Philosophy

## Core Principles

1. **Explicit ownership** - No hidden aliasing or copying
2. **Natural syntax** - Reads like structured English
3. **AI-assisted** - Designed for clarity in AI collaboration

## Ownership System

- `own T` - Exclusive ownership
- `view T` - Read-only borrow
- `mod T` - Mutable borrow
- `opt T` - Optional wrapper

## Assignment Semantics

- `=` always copies values
- `=>` transfers ownership
- No implicit conversions
