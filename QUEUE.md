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

- [x] Support type-property stacking in type grammar (`opt`, `view`, `mod`, `id`, `key` in canonical order).

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



## Priority 5: Examples & Documentation

- [x] Integrate example smoke-testing into `run_tests.sh`.

- [ ] Add `.expect` files for non-interactive examples to enable full behavioral testing.

- [ ] Fix any remaining documentation/example inconsistencies with v0.2 spec.



## Priority 6: Editor Support & Visual Polish

- [x] Create `rae.sublime-syntax` for Sublime Text (Sublime Monokai compatible).

- [x] Create `rae.tmLanguage.json` and VSCode extension manifest.

- [x] Create `tools/editor/install.sh` to automate local installation.

- [x] Enhance devtools syntax highlighter logic (types vs keywords, function names, calls).

- [x] Update devtools theme to match Sublime Monokai (background `#272822`).



## Priority 7: Strict File Formatting

- [ ] Enforce LF-only line endings in Lexer (reject CRLF).

- [ ] Support `\r` only inside raw string literals.

- [ ] Enforce mandatory final newline at end-of-file.

- [ ] Update Formatter to automatically fix line endings and EOF newline.

- [ ] Add regression tests for line ending and EOF newline enforcement.

## Priority 8: Raepack Refinements
- [ ] Task: raepack does not contain a thing called "auto_folders". Check how the raepack format really defines folders and files. Fix all raepack files accordingly.
