# Binding modes: the relation lives on the name, never on the value

Status: **design draft, decisions settled 2026-09-28, nothing implemented**.
Origin: the canvas-hover leak investigation (#13186639) exposed that the
current ownership rules put the same idea in three different places
(`=`, `=>`, `own x`), and that the C backend decides ownership with mutable
flags flipped during emission. This document proposes one rule for how a
name takes a value, and one place to write it.

Supersedes the syntax parts of `docs/ownership-model.md` if adopted; the
drop/cascade machinery described there stays.

## The problem, in one paragraph

Today a value reaches a name through four spellings that do not share a
shape: `let b: T = a` (silent deep copy), `let v: view T => a` (alias),
`let c: T = own a` (transfer, the word on the value), and `f(x: a)` with
`x: own T` (transfer, the word on the callee's declaration, nothing at the
call). The reader has to know which of the four applies, and the expensive
one, the copy, is the only one with no word at all. The compiler mirrors the
confusion: whether a local is still owned is a per-function flag array
mutated while emitting C, with no notion of control-flow paths, which is
exactly what leaked one String per hierarchy refresh in the editor.

## The model

**A name declares how it takes values. Every value it is given is taken
that way. The relation is written on the name's type, and nowhere else.**

Two binders with fixed meanings, and they never trade places:

- **`:` feeds a receiver.** A `let` initialiser, a parameter, a struct-literal
  field and a `ret` all feed through `:` (or the `ret` keyword), and what
  happens is decided by the receiver's declared mode.
- **`=` copies.** It is the copy operator, as it has always been in Rae. It
  writes an independent value into an existing `var`. It never transfers and
  never aliases.

Four words, all already in the language: `own`, `copy`, `view`, `mod`.

| word | the name ... | the source afterwards |
|---|---|---|
| `own` | takes over the value | a whole name ends; a new value has no source |
| `copy` | takes an independent value equal to the source | unchanged |
| `view` | aliases the source, read-only, fixed at binding | unchanged, observed |
| `mod` | aliases the source, writable, fixed at binding | unchanged, changed through the alias |

There is no fifth case. A bare type in a binding position is an error,
except a struct field, whose relation is fixed: owned by the struct.

### Declarations carry the word

```rae
let a: own Holder: makeHolder()      # a takes over a new value
let b: copy Holder: a                # b takes an independent copy; a stays
let c: own Holder: a                 # c takes over; a ends
let d: view Holder: a                # d aliases a
var count: own Int: 0
let e: own Int: 5
let f: copy Int: 5                   # allowed: a copy of a literal is a value

func consume(holder: own Holder)
func keep(holder: copy Holder)
func inspect(holder: view Holder)
func step(world: mod UiWorld)

type Holder {
  label: String                      # a field is always owned by its struct
  items: List(String)
}

func makeLabel(seed: view Int) ret own String
func firstItem(holder: view Holder) ret view String
```

The binder is `:` in every declaration, so a `let`, a parameter and a
return type all read `name: mode Type`, and a `let` adds `: value`. This is
the shape a named argument and a struct-literal field already have.

A struct field carries no mode, because it has only one possible relation:
the struct owns it. A field can neither alias nor copy on its own; the
literal or assignment that feeds it decides where the value comes from. So
a field declaration stays `name: Type`, and every field is an `own`
receiver when fed.

### Feeding sites carry nothing

A call argument, a struct-literal field and a returned value have no
vocabulary. Each does what its receiver declared.

```rae
consume(holder: a)                   # holder is own: a ends
keep(holder: a)                      # holder is copy: a stays
inspect(holder: a)                   # holder is view: alias for the call
ret Holder { label: name, items: items }   # fields are own: name and items end
ret flat                             # taken as the return type declares
```

`= own x`, `= copy x`, `x => y` and `field: own x` no longer exist.

### `let` binds once; `var` rebinds by copying

A `let` name is bound at its declaration and never again. A `var` name can
be assigned with `=`, and `=` copies:

```rae
let a: own Holder: makeHolder()      # bound once; a never rebinds
var count: own Int: 0
count = count + 1                    # a new value copied into count
var current: own List(Int): make()
current = other                      # an independent copy of other; current's old value is dropped
d = a                                # ERROR: d is a view; aliases are fixed
```

There is no move statement. `c = b` never ends `b`, and a `c: b` statement
does not exist. The three cases that would want one each have a plain
function instead, which is where the `own` word already lives:

- a local built up and then stored into a field of an existing struct: a
  setter with an `own` parameter (`componentSet(data: own T)` is this);
- swapping two owned values (double buffering): `swap(a: mod T, b: mod T)`;
- taking a value out of an optional and leaving `none`:
  `take(this: mod opt T) ret own T`.

Writing `world.thing = local` copies `local`, visibly, because `=` is the
copy operator; when the copy is not wanted, the code says so by going
through the setter.

### Rules

1. **A transfer source is a whole name, fed through `:`.** `own` receivers
   may take a new value (a call result, a literal, an operator expression, a
   struct literal) or a whole local / parameter name, which then ends. A
   field or an element (`outer.inner`, `items[0]`) is never a transfer
   source; it can only be copied or aliased. There are no partial moves.
   Since `=` copies and a field is only ever fed through `:` in a literal,
   a transfer out of a field cannot be spelled at all.
2. **A name that ended cannot be read.** Sema rejects any later use in the
   same function, on every path that reaches the transfer (a use on a path
   that skips it is fine).
3. **Aliases are fixed at binding.** A `view` / `mod` name is bound once;
   assigning to it is an error. Writing *through* a `mod` alias
   (`state.count = 1`, `state = value` for a primitive pointee) is a copy
   into the pointee, as `=` always is.
4. **`copy` and `own` accept new values.** `let f: copy Int: 5` and
   `keep(holder: makeHolder())` are legal: an independent copy of a value
   that has no other name is the value itself. A `view` / `mod` of a new
   value is an error (there is nothing to alias).
5. **The return type is a receiver.** `ret x` takes `x` as the return
   type declares, with the same rules as any other receiver: `ret own T`
   takes a new value or a whole name (a local, an `own` parameter), which
   ends; a field or an element is an error there, per rule 1. A function
   that hands out an independent copy of something it only names declares
   `ret copy T`, and the copy is visible in its signature:

   ```rae
   func labelOf(holder: view Holder) ret copy String {
     ret holder.label
   }
   ```

   `ret view T` / `ret mod T` alias, which sema already restricts to
   sources that outlive the call.
6. **A type with no identity** (Int, Float, Bool, an enum, a struct of such
   fields) takes the same words. Nothing is exempt; `copy Int` and `own Int`
   are both legal and mean the same thing at runtime. The words are still
   information: `own` says "mine", `copy` says "I took this from
   somewhere".

## What goes away

- `=` in declarations. A `let` / `var` / `const` initialiser is fed through
  `:`; `=` is only ever an assignment to an existing `var`, and it copies.
- `=>` for reference bindings. `let v: view T: x` replaces `let v: view T
  => x`, including in `if let` (`if let v: view T: opt`).
- `own x` in value position, and with it the asymmetry that transfer was
  spelled on the value in a `let` but on the declaration at a call.
- The "plain `T` parameter copies" rule. A parameter with no mode is an
  error.
- The C backend's `local_moved[]` / `local_struct_owns_heap[]` flags as the
  source of truth. Ownership becomes a sema result the backend reads.

## Decisions (settled 2026-09-25)

- **No exemptions.** Loop headers and consts take the same form as every
  binding: `loop var i: own Int: 0`, `const maxRetries: own Int: 5`. A bare
  type in a binding position is always an error.
- **Return types follow the receiver rules.** `ret own T` transfers a whole
  name or takes a new value; `ret copy T` copies; a field returned under
  `ret own T` is an error, fixed by declaring `copy`. No return copies
  silently.
- **Reading a field into an `own` local is an error**, by rule 1; the
  author writes `copy` or `view`. This will be the most common error in
  the sweep, and the diagnostic must name the fix.
- **`=` is the copy operator, always.** It assigns to an existing `var` and
  copies. It never transfers; there is no move statement (`c = b` does not
  end `b`, and `c: b` does not exist). `let` binds once. The cases that
  want a transfer into an existing place go through a function with an
  `own` parameter (a setter, `swap`, `take`).

## Migration (measured on lib + examples, 122K lines, 2026-09-25)

| form today | count | becomes |
|---|---|---|
| `let` / `var` / `const` bindings with `=` | ~10,700 | `name: own T: value` or `copy T` (sema tells which: a place source needs `copy` or `own`) |
| `let x: view/mod T => place` | 639 | `let x: view/mod T: place` |
| `if let x: T = call()` | 672 | `if let x: own T: call()` |
| `if let x: view T => opt` | 113 | `if let x: view T: opt` |
| `= own x` | 33 | a `let x: own T: value` where the target was only initialised, otherwise a setter call with an `own` parameter |
| `field: own x` in struct literals | 476 | `field: x`; a field is always an `own` receiver, the word is dropped |
| `ret own x` | 14 | `ret x`; the return type says `own` |
| parameters `copy T` / `own T` | 912 / 56 | unchanged |
| parameters with a bare `T` | ~218 | `copy T` (today's meaning) or `own T`, the author decides |
| `ret T` return types | most functions | `ret own T` when every `ret` hands back a new value or a whole name; `ret copy T` when one returns a field |
| assignments into an existing name | 5,800 | unchanged text and unchanged meaning: `=` copies, as today |
| of which from a bare place `x = y` | 1,191 | unchanged: a copy, as today; sema only checks that the target is a `var` and not an alias |

Every rejected line gets a diagnostic that names the exact rewrite, so the
sweep is compiler-driven. `rae format` does not rewrite semantics; the
mechanical forms (`=>` to `:`, `= own` removal, `ret own` removal, bare `T`
params) can be a one-off migration tool under `compiler/tools/`.

## Implementation plan

Phases, each landing green with the full suite and the example gates.

1. **Sema: ownership as a result.** Add a per-function pass over the
   typed AST that computes, per local and per control-flow path, whether
   the name is live, ended, or an alias; rejects reads after a transfer
   (rule 2), transfers from fields (rule 1), aliasing a new value, and
   assignment to an alias (rule 3). Output: a per-statement table the
   backend reads ("drop x here", "x ended on this path"). This phase runs
   on the *current* syntax first, replacing `local_moved[]` /
   `local_struct_owns_heap[]` as the source of truth, and must reproduce
   today's behaviour byte-for-byte in emitted C for the suite. This is the
   part that fixes the class of bug behind #13186639, independent of the
   syntax change, and it is the phase to do even if the syntax is not
   adopted.
2. **Parser + AST: the new binding grammar.** `name: mode Type: value` in
   `let` / `var` / `const` / `if let` / loop headers; modes on return
   types; fields stay `name: Type`; reject bare types in the other binding
   positions; reject `=>`,
   value-side `own`, and `= own`. Keep the old forms parseable behind a
   `--legacy-bindings` flag only for the migration tool's own run, then
   delete it.
3. **Sema: the feeding rule.** Every `:` feeding site (initialiser,
   argument, literal field, `ret`) checks the source against the receiver's
   mode: whole-name-or-new for `own`, anything for `copy`, place for
   `view`/`mod`. An `=` assignment checks only that the target is a `var`
   that is not an alias; it always copies. Diagnostics name the fix.
4. **Backend.** Emit copies, transfers and drops from the phase-1 table.
   Delete the flag arrays and the pre-passes that mark moves during
   emission (the assignment pre-pass, the `ret` mark, the call-arg mark).
5. **Formatter.** Canonical layout for the new headers; `rae format` stays
   semantics-free.
6. **Migration tool + sweep.** `compiler/tools/migrate-bindings.sh` rewrites
   the mechanical forms; the remaining errors are fixed by hand, guided by
   the diagnostics. lib first (it is globals-clean and the prelude), then
   examples, then the fixtures. New fixtures for each rule, each with the
   outstanding-allocation check fixture 918/919/920 use.
7. **Docs.** `docs/ownership-model.md` syntax sections replaced by a pointer
   here; AGENTS.md's "NO type inference" section gains the binding grammar;
   `rae init` scaffold updated.

Order matters: 1 before anything else, because it is the safety net for
the sweep; 2–4 together on a branch; 6 is the long tail.

## Verification

- The full suite and every example gate green after each phase.
- Fixtures 916–920 unchanged in meaning, rewritten in the new grammar.
- The hover soak (`examples/121_ui_editor/tools/hover-soak.sh`) and the
  run_examples.sh hover-leak check stay at the boot baseline.
- A sabotage fixture per rule: a read after transfer, a transfer from a
  field, an alias of a new value, an assignment to an alias, a bare-type
  binding, each rejected with the documented diagnostic.
- Emitted C for the suite is byte-identical between the end of phase 1
  and the start of phase 1 (ownership computed in sema must change no
  code until the syntax phases begin).
