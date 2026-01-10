# Rae Implementation Queue (v0.2 Strict + Memory Model)

## Priority 1: Parser & AST
- [ ] Support type-property stacking in type grammar (`opt`, `view`, `mod`, `id`, `key` in canonical order).
- [ ] Implement `=>` bind/rebind operator in lexer, parser, and AST.
- [ ] Support `view`/`mod` prefixes in expression positions (for returning/passing borrows).

## Priority 2: Type Checker
- [ ] Implement position rules:
    - [ ] `T`, `opt T` allowed everywhere.
    - [ ] `view`/`mod` allowed in locals/params.
    - [ ] `id`/`key` allowed everywhere (locals, params, fields, returns).
    - [ ] Reject `view`/`mod` in struct fields.
- [ ] Implement composition matrix:
    - [ ] Disallow `view id T`, `mod key T`, etc.
- [ ] Implement `=>` legality checks:
    - [ ] LHS must be a bindable slot (`view`, `mod`, or `opt ...`).
    - [ ] LHS cannot be a plain value `T`.
- [ ] Implement borrow return rules:
    - [ ] Borrows can only be returned if derived from params/`this`.
- [ ] Implement escape diagnostics:
    - [ ] Reject storing borrows in long-lived containers (`List`, `Map`).

## Priority 3: Runtime & Interpreter (VM)
- [ ] Define `id T` as `int64_t` underlying.
- [ ] Define `key T` as `String` underlying.
- [ ] Implement "bindable slots" in the VM for `view`/`mod`.
- [ ] Add runtime lifetime tracking for borrows.
- [ ] Implement `mod` exclusivity checks at runtime.
- [ ] Update `=` to ensure deep/observable copy semantics.

## Priority 4: Stdlib & Tooling
- [ ] Stdlib skeleton:
    - [ ] Identity logging helpers (e.g. `log(id T)`).
    - [ ] Identity extraction functions (`id.int`, `key.string`).
    - [ ] Minimal Store/Cache API stubs.
- [ ] Lints: add warning for "opt soup" (excessive optional parameters).
- [ ] C Backend:
    - [ ] Map `id T` to `int64_t`.
    - [ ] Map `key T` to `const char*`.

