# Rae Programming Language

[![XKCD 927: Standards](https://imgs.xkcd.com/comics/standards.png)](https://xkcd.com/927/)

## WIP: EARLY STAGE

Rae is in early development. Many promised features are not built yet.
Treat everything below as direction, not a guarantee.

## Why Rae?

Rae is a language and toolchain designed for humans and AI agents to share the same codebase.
It focuses on clarity, explicit semantics, and tooling that works the same in Live and Compiled workflows.

What makes Rae different:
- One language, two runtimes: Live (bytecode VM) for fast iteration and Compiled (C backend) for production builds.
- Hybrid mode bridges them, so a compiled host can load Live code bundles.
- Hot reload is a first-class workflow, not an afterthought.
- Ownership is explicit: no hidden aliasing, no surprise copies.
- Syntax aims to be structured and machine-friendly, without clever ambiguity.

## Execution Modes

Live (bytecode VM)
- Fast iteration and hot reload during development.
- Ideal for tooling, scripted behavior, and rapid feedback loops.

Compiled (C backend)
- Targets performance and distribution by emitting C.
- The backend is still incomplete, but the pipeline is in progress.

Hybrid
- A compiled host can package and load Live code bundles.
- Designed for shipped apps that download new behavior and reload without restarting.

## Hot Reload

Development hot reload
- Live mode supports watch-based reload for quick iteration.

Shipped hot reload (planned)
- Hybrid mode is intended to allow downloading code updates at runtime.
- Current demos are early and focus on packaging and staging, not full production behavior.

## Memory Model

Rae uses explicit ownership qualifiers:
- own T: owning reference
- view T: read-only borrow
- mod T: mutable borrow
- opt T: optional wrapper

There is no general-purpose garbage collector today.
The Live VM uses simple managed allocations, and future work will add better tracking,
especially to support hot reload and long-running sessions.

## Current State

Done or usable today:
- [x] Lexer, parser, AST, and formatter
- [x] Live (bytecode VM) execution for examples
- [x] File watching for Live hot reload
- [x] Basic module loading and imports
- [x] Hybrid packaging outputs (experimental)

In progress or planned:
- [ ] Ownership and type checking
- [ ] Robust C backend with full codegen
- [ ] Stable runtime host API for shipped hot reload
- [ ] Debugger and editor tooling
- [ ] Performance profiling and optimization passes

## Planned Use Cases

Rae is being built for:
- Games and gameplay scripting
- Apps with live-updated logic
- UI tooling and dynamic workflows

These are targets, not finished products.
Expect sharp edges while the language and toolchain mature.

## Learn More

- Language spec: spec/rae.md
- Design notes and plans: docs/
- Example programs: examples/

## License

MIT
