# Rae Implementation Queue (v0.2 Strict + Reference Model)

## Priority 1: Reference Model & Terminology
- [x] Rename "borrow" to "reference/alias" in all diagnostics, comments, and internal naming (where it doesn't break external deps).
- [x] Update specifications (`spec/rae.md`) to reflect the new reference semantics (aliasing-friendly, lifetime-restricted).
- [x] Support positional first arguments in calls (`getX(p)` instead of `getX(p: p)`).
- [x] Implement member-call syntax sugar (`p.x()` -> `x(p)`).
- [x] Implement lifetime checks for references:
    - [x] Reject returning a reference to a local variable.
    - [x] Reject taking a reference to a temporary/rvalue.

## Priority 2: Parser & AST Refinements
- [ ] Support type-property stacking in type grammar (`opt`, `view`, `mod`, `id`, `key` in canonical order).
- [ ] Implement `=>` bind/rebind operator refinements.
- [ ] Support `view`/`mod` prefixes in expression positions (for returning/passing references).

## Priority 3: Type Checker & Semantic Analysis
- [ ] Implement position rules:
    - [ ] `T`, `opt T` allowed everywhere.
    - [ ] `view`/`mod` allowed in locals/params.
    - [ ] `id`/`key` allowed everywhere.
    - [ ] Reject `view`/`mod` in struct fields.
- [ ] Implement reference return rules:
    - [ ] References can only be returned if derived from params/`this`.
- [ ] Implement escape diagnostics:
    - [ ] Reject storing references in long-lived containers (`List`, `Map`).

## Priority 4: Runtime & Interpreter (VM)
- [ ] Define `id T` as `int64_t` underlying.
- [ ] Define `key T` as `String` underlying.
- [ ] Implement "bindable slots" in the VM for `view`/`mod`.
- [ ] Implement runtime lifetime validation (debug builds).
- [ ] Update `=` to ensure deep/observable copy semantics.