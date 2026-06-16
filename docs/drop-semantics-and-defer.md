# Drop semantics, destructors, and defer in Rae

> Design notes on how cleanup should work in Rae. Captures the
> reasoning so future implementation has a coherent target rather
> than ad-hoc cases.
>
> Three statuses are used throughout:
>
> * **Decided** — already a requirement or already in the language.
> * **Likely** — the direction the reasoning points toward, but
>   not committed to.
> * **Open** — a real design question, with alternatives still on
>   the table.
>
> Where this document gives a "preferred" or "natural" rule, that
> reflects the current best guess only.

## Rae's goal isn't Rust

Before anything else, the goal Rae is aiming at:

> Rae should automatically handle ordinary ownership cleanup, warn
> or restrict obvious hazards, and allow explicit unsafe or manual
> patterns when needed.

Rae does **not** aim for Rust-level memory safety or
compile-time proof that misuse is impossible. The goal is a
pleasant, practical systems language that avoids leaks and
cleanup mistakes when used properly, with helpful compiler and
runtime support and sensible defaults.

That framing matters because most of the open questions below
have a "Rust-style" answer and a "looser, more ergonomic" answer.
This document treats Rust as one comparison point, not the
target. Specifically:

* **Automatic structural drop** for `String`, `List`, buffers,
  structs, and generic types is about preventing the ordinary
  leak ("I forgot to free this") and reducing the amount of
  cleanup the programmer has to write by hand. It is not about
  proving the absence of leaks.
* Rae does **not** need to prove that every resource has exactly
  one owner at all times. It needs to make the common case work
  and make the obvious mistakes hard.
* `own`, `view`, and `mod` are useful **vocabulary and tooling**
  for talking about who is currently responsible for a value.
  They do not need to expand into a full Rust-style borrow checker.
* Copying and moving still need understandable semantics —
  especially to avoid accidental double cleanup of an external
  resource — but the answer may be a mix of conventions,
  warnings, limited compiler checks, explicit annotations, or
  simple restrictions rather than complete static soundness.
* Resource wrappers with custom drop may use **stronger rules**
  than ordinary value types (for example, no implicit copy of a
  texture handle), but Rae should preserve explicit escape
  hatches for advanced or FFI code.
* The language should prevent the common mistakes without making
  every uncommon or potentially unsafe operation impossible. "Hard
  to do by accident, possible to do on purpose" is the target.

The rest of the document should be read with that in mind: when
two paths exist and one is more permissive, the more permissive
one is not automatically wrong.

The key point is that Rae already has the beginnings of a destruction
model, even if it does not yet expose “destructors” as a user-facing
feature.

When an owned value goes out of scope, Rae must eventually know
whether that value contains anything requiring cleanup. For example:

```rae
type Person {
  name: String
  aliases: List(String)
}
```

Dropping a `Person` cannot mean “forget the struct memory.” It must
mean roughly:

```rae
drop(person.name)
drop(person.aliases)
```

And dropping `aliases` must recursively drop each owned `String`
before freeing the list buffer.

So Rae already needs this general compiler question:

> What cleanup operation belongs to this type when an owned value
> dies?

That applies even without files, fonts, sockets, or GPU objects.

## At a glance: Decided / Likely / Open

**Decided**

* Owned built-in types (`String`, `List(T)`, buffers) must be
  cleaned up when they die.
* Cleanup must compose through user structs and generic
  instantiations — a `String` inside a struct cannot leak just
  because it is one level of indirection deeper.
* Rae already has `own`, `view`, and `mod` to talk about who
  currently holds responsibility for a value.
* **Implemented (Stage 1, both backends)** — the compiler
  synthesises structural drop for user types that contain owned
  fields. The base case for "this type needs cleanup" is "any
  field needs cleanup." Cleanup fires at scope exit, before every
  `ret`, and before reassignment, in reverse binding order. `ret
  <bare-ident>` moves rather than drops. The C backend has done
  this since Phase 1–3 (May 2026); the Live VM caught up across
  commits `e31bb9b` → step 6 (see
  `docs/structural-drop-stage1-plan.md` for the per-commit
  changelog). Both backends drop bare String/List/Buffer/Map
  locals, non-generic user structs, and — on the VM side —
  concrete generic struct specializations the module instantiates.

**Likely**

* External resources (files, GPU handles, sockets) get their own
  release hook somehow — separate from structural drop.
* `defer` (lexical, control-flow-scoped cleanup) earns its place
  alongside drop rather than being subsumed by it.
* The font case used as the trigger for this document is **not**
  a great motivating example for adding destructors — see the
  "font example revisited" section below.

**Open**

* **The general copy/move/assignment model for owned values.**
  This is the real gating question. `own` / `view` / `mod`
  provide vocabulary but do not by themselves answer:

  ```rae
  let a: Person = createPerson()
  let b: Person = a
  ```

  This could mean move, deep copy, shallow alias, or compile
  error. Rae needs a clear and ergonomic rule, but not
  necessarily one that statically guarantees full memory
  safety. Custom-drop types may add stricter rules on top of
  whatever the general rule turns out to be. See the section
  on copy / move policy below.
* Whether user-defined custom drop becomes a first-class feature
  at all, or whether external resources are released through
  some other mechanism (handles + explicit close, capability
  objects, FFI shims).
* Whether explicit clone / copy needs to be a language concept
  or can stay in user-land helper functions.
* Drop order across fields and across local bindings (reverse
  declaration order, declaration order, or something else
  entirely).
* Whether custom drop runs before, after, or instead of the
  automatic structural pass over a struct's fields.
* Whether panic unwinds (running drops along the way) or aborts.
* What custom drop is allowed to do (allocate, fail, panic,
  await, return values, …).
* The syntax for explicit early cleanup (`drop value`,
  `value.drop()`, a stdlib function, …).
* The order in which the above stages get implemented.

The rest of this document expands each of these.

## Structural drop (likely-required)

The case for this is **leak prevention and convenience**, not
soundness. If `String` and `List(T)` already know how to clean
themselves up at scope exit, hiding one of them inside a struct
should not silently break that. The simplest model is fully
compiler-generated structural cleanup.

For every type, the compiler determines its drop behavior from its
fields:

```rae
type Point {
  x: Float
  y: Float
}
```

`Point` needs no drop.

```rae
type Label {
  text: String
  position: Point
}
```

`Label` needs drop because `String` needs drop.

```rae
type Screen {
  title: Label
  widgets: List(Widget)
}
```

`Screen` recursively needs drop because both fields may contain
owned resources.

The compiler can synthesize something conceptually like:

```rae
func raeDropLabel(this: mod Label) {
  raeDropString(this.text)
}

func raeDropScreen(this: mod Screen) {
  raeDropLabel(this.title)
  raeDropList(this.widgets)
}
```

This is not really optional if Rae wants automatic memory management
for nested owned values.

Otherwise this works:

```rae
let names: List(String) = createList()
```

but this leaks:

```rae
type State {
  names: List(String)
}

let state: State = createState()
```

That would make ownership depend accidentally on whether the
container is directly bound or hidden inside another type.

This is the part of the design that is hardest to argue against:
Rae already has owned heap data behind `String` and `List(T)`, and
already needs it to participate in struct ownership for the
language to fit together.

## User-defined cleanup as an extension of that model (likely)

Now imagine:

```rae
type File {
  handle: Int
}
```

The compiler cannot infer from `Int` that the handle must be closed.
Structurally, `File` looks trivial.

A user-defined drop operation could supply the missing semantic
rule:

```rae
func drop(this: mod File) {
  closeFile(this.handle)
}
```

Then the compiler-generated drop for a larger type naturally
includes it:

```rae
type AssetLoader {
  configFile: File
  names: List(String)
}
```

Conceptually:

```rae
func raeDropAssetLoader(this: mod AssetLoader) {
  drop(this.configFile)
  drop(this.names)
}
```

This is why user-defined cleanup can fit naturally. It does not need
to be an entirely separate mechanism. It can be one more source of
a type’s drop behavior.

The compiler already asks whether each field needs dropping. A
user-defined `drop` would simply make the answer “yes” for types
whose resource ownership is not visible from their field types.

Whether Rae actually adopts user-defined drop is itself open: an
alternative is to leave external resources as plain handles plus
manual close calls (with `defer` for short-lived scopes), and only
ship structural drop for owned data. That is a real option, not a
strawman — for some kinds of resources, the "do nothing automatic,
make the user write `close(x)`" rule is honest and easy to teach.

## The copy / move / assignment model — the real gating question (open)

This is the question that gates most of the others, and it
exists with or without custom drop:

```rae
let a: Person = createPerson("Alice", ...)
let b: Person = a       // move? deep copy? alias? error?
```

Rae has to answer this for the existing built-ins — `String`,
`List(T)`, structs containing them — even before custom drop
enters the picture. Custom-drop types may add stricter rules on
top, but they do not introduce a brand-new problem; they push on
one Rae already has.

The aim, per the framing at the top of this document, is **a clear
and ergonomic rule**, not a statically-sound proof. Different
categories of type can plausibly follow different rules:

**1. Plain value types.** Fields are primitives, or other plain
value types, recursively. Structural drop is a no-op. They behave
like values everywhere — `let b = a` produces an independent
equal value.

```rae
type Color { r: Int  g: Int  b: Int  a: Int }
type Vec2  { x: Float  y: Float }
```

**2. Owned-data types.** Fields contain or transitively contain
owned heap data (`String`, `List`, buffers). Structural drop
recurses through them. `let b = a` could be deep-copy by default
(predictable, but a perf footgun for large lists), move (a
matches the underlying heap-ownership story), or aliased-with-a-
warning (most permissive, easiest to misuse). `own` / `view` /
`mod` are the vocabulary that lets the programmer be explicit
when the default isn't what they want.

**3. Resource-wrapper types.** A struct that uses a custom `drop`
to release an external resource (file handle, GPU texture, OS
socket). The handle inside is often a plain `Int`, so a naive
field-by-field copy would silently duplicate ownership of the
underlying resource:

```rae
type Texture { id: Int }
func drop(this: mod Texture) { unloadTexture(this.id) }

let a: Texture = loadTexture(path)
let b: Texture = a          // both later try to unload id
```

For category-3 types, accidental copy is the obvious mistake that
"sensible defaults" should prevent. The category-1 case has no
such hazard. The category-2 case is mostly a performance and
clarity question rather than a correctness one (the program won't
double-free, but it might silently allocate when the programmer
expected a move).

Some design knobs that could appear, separately or in
combination:

* **A. One rule for everything.** Pick a single answer (e.g. move
  by default) and apply it uniformly. Simple to teach. Cost: every
  `Color` and `Vec2` loses implicit-copy ergonomics for no
  benefit.
* **B. Per-category defaults, all implicit.** Plain value types
  copy; owned-data types do whatever the chosen general rule
  says (move, deep-copy, alias-with-warning); custom-drop types
  default to "no implicit copy." Most ergonomic; relies on the
  compiler inferring the category.
* **C. Explicit category marker.** A type-level annotation like
  `resource type Texture { … }` or `noCopy` makes the
  classification visible at the type declaration. Lets the user
  say "this looks like an `Int`, but it is not."
* **D. Lint, don't forbid.** Allow implicit copy of a custom-drop
  type but emit a warning, and provide a syntactic opt-in to
  silence it. Preserves an FFI escape hatch at the cost of
  letting accidental copies compile.
* **E. Compromise: implicit-copy of custom-drop types is an
  error, but Rae provides an unambiguous escape hatch.** For
  example, an explicit `Texture.unsafeAlias()` or `unsafe { let b
  = a }` block. "Hard to do by accident, possible to do on
  purpose" — matches the working principle at the top.

There isn't a single right answer in advance of trying a few of
these against real Rae code. None of them require Rae to
guarantee static soundness; even the strictest option above only
prevents the accidental case. Truly adversarial misuse remains
the user's responsibility, as it would be in C.

A related question: regardless of which rule wins, does Rae need
a language-wide `clone` concept, or can each resource type expose
a domain-specific operation (`Texture.duplicate()`,
`File.reopen()`) that allocates a fresh handle? The latter avoids
naming a language-wide trait and is consistent with Rae's
preference for explicit functions over generic machinery.

## Why this is different from `defer`

`defer` attaches cleanup to a control-flow scope:

```rae
let file = openFile(path)
defer closeFile(file)
```

A destructor attaches cleanup to the value:

```rae
let file: File = openFile(path)
```

When ownership of `file` moves, the cleanup responsibility moves
with it.

That distinction becomes important immediately:

```rae
func openLog() -> own File {
  let file = openFile("log.txt")
  return file
}
```

With value-based destruction, this is natural. The returned file
remains alive and is dropped by its eventual owner.

With `defer`, this would be wrong:

```rae
func openLog() -> File {
  let file = openFile("log.txt")
  defer closeFile(file)
  return file
}
```

The file would be closed when `openLog` returns.

You can manually avoid that defer, but then the resource-management
rule exists only in programmer discipline rather than in the type.

## Ownership composition

User-defined drop becomes especially powerful when resources are
nested or moved through multiple layers:

```rae
type Texture {
  id: Int
}

type Font {
  texture: Texture
  glyphs: List(Glyph)
}

type UiAssets {
  font: Font
  icons: List(Texture)
}
```

If these types own their resources, cleanup composes automatically.

Dropping `UiAssets` drops:

1. `font`
2. `font.texture`
3. `font.glyphs`
4. every texture in `icons`
5. the icons list buffer

The caller does not need to remember every cleanup operation.

A manual shutdown function tends to duplicate the object structure:

```rae
func shutdownUiAssets(this: mod UiAssets) {
  unloadFontTexture(this.font.texture)
  dropList(this.font.glyphs)

  for texture in this.icons {
    unloadTexture(texture)
  }

  dropList(this.icons)
}
```

The more deeply nested the program becomes, the more brittle this
manual symmetry becomes.

## The wrapper struct is not necessarily a tax

A wrapper struct created solely to trigger cleanup may feel
ceremonial.

But the wrapper also carries crucial information:

```rae
let textureId: Int
```

does not communicate:

* whether this is a texture
* whether it owns the texture
* whether zero is valid
* whether it may be copied
* whether it must be unloaded
* whether it is still alive

Whereas:

```rae
let texture: own Texture
```

can encode all of that.

So the wrapper is often the actual resource type, not merely
boilerplate around an integer.

The type system should ideally distinguish:

```rae
Texture
TextureId
view Texture
own Texture
```

rather than treating every external resource as an `Int`. Whether
this should be enforced or only encouraged is open.

## Drop order (open)

If Rae adopts user-visible drop semantics, it must specify an
order. There are two main conventions and at least one hybrid:

**Reverse declaration / reverse binding order.** Mirrors stack
unwinding. Useful when later fields depend on earlier ones — a
texture created from a device should be released before the
device. Familiar to anyone coming from C++ or Rust.

```rae
type Renderer {
  device: Device
  pipeline: Pipeline
  texture: Texture
}
```

Reverse order drops `texture`, then `pipeline`, then `device`.

**Declaration order.** Symmetric with construction. Easier to
reason about visually but can release dependencies before
dependents.

**Compiler-determined dependency order.** Inspect the type graph
and drop in a topological order. More automatic, harder to teach,
needs more ceremony in the type system to be sound.

The reverse-declaration convention is probably the safest default
to start from, but this is not yet decided. Whichever wins, it
must be **deterministic and documented** — silent compiler
freedom here is hostile to debugging.

## Custom drop relative to automatic field drop (open)

If a user provides a custom `drop` for a struct that also has
ordinary owned fields:

```rae
type Font {
  handle: FontHandle
  glyphs: List(Glyph)
}

func drop(this: mod Font) {
  unloadFont(this.handle)
}
```

…how does the compiler combine the two cleanups?

**Option A: custom drop runs, then structural drop visits owned
fields.** The user only writes the part that the compiler cannot
infer. Ergonomic — no manual recursion over fields. Risk: the
custom body might invalidate fields that structural drop then
tries to use, causing double-free or undefined behaviour.

**Option B: custom drop replaces the structural pass entirely.**
The user is responsible for releasing every owned field too. More
control, much easier to get wrong as the type grows.

**Option C: structural drop visits owned fields first, then
custom drop runs.** Inverts A. The custom body sees fields that
have already been cleared. Easier to reason about for some
resource types, but counterintuitive when the custom body needs
to read field values to release the external resource.

**Option D: a marker per field opts out of automatic dropping.**
Most flexible, most syntax to learn.

The most ergonomic default is probably A, but it has a real
failure mode (custom body frees memory that structural drop then
re-frees) that may push toward C or D. Open.

## Partial initialization

Structural drop is also a constructor-failure problem, not just a
scope-exit problem.

Consider:

```rae
func createRenderer() -> Renderer {
  let device = createDevice()
  let pipeline = createPipeline(device)
  let texture = createTexture(device)
  return Renderer(
    device: device,
    pipeline: pipeline,
    texture: texture
  )
}
```

What happens if `createTexture` fails?

Already-created values must be dropped:

```text
drop pipeline
drop device
```

If Rae uses `Result`, early returns also need cleanup:

```rae
let device = try createDevice()
let pipeline = try createPipeline(device)
let texture = try createTexture(device)
```

Every `try` must drop all live owned locals before returning the
error.

This is another reason structural drop is fundamental. It cannot
only happen at the closing brace in simple functions. It must
integrate with every exit path:

* normal return
* early return
* propagated error
* loop break
* loop continue where scopes end
* possibly panic or recoverable failure

## Panic (open)

This connects directly to Rae’s broader panic design, which has
not been settled.

### Unwinding

A panic walks back through scopes and drops live owned values.

Advantages:

* files close
* locks release
* temporary memory is freed
* invariants are more likely to be restored

Disadvantages:

* runtime and compiler complexity
* potential latency spikes
* destructors may execute arbitrary code
* unsafe during some low-level failures

### Abort

A panic terminates the process immediately.

Advantages:

* simple and predictable
* no destructor execution during corrupted state
* good fit for catastrophic failure

Disadvantages:

* no cleanup
* unsuitable if Rae wants local recovery or subsystem restart

Rae could plausibly support both — ordinary `Result` propagation
performing full cleanup, fatal panic aborting without cleanup — but
even that is a design choice rather than a fact. For games and
real-time systems, avoiding arbitrary destructor work in an audio
callback or hard real-time section may matter greatly, which is an
argument toward making "no unwind, no surprise work in destructors"
the path the runtime is best at.

## Should custom drop be constrained? (proposals)

If user-defined `drop` ships, the question of what it is allowed to
do is itself open. Possible restrictions, all of which are
proposals rather than commitments:

* `drop` cannot return a value.
* `drop` cannot fail with `Result`.
* `drop` cannot panic, or panic inside drop becomes fatal.
* `drop` cannot move out ordinary fields except through special
  operations.
* `drop` executes synchronously.
* `drop` should not allocate, perhaps as guidance rather than
  enforcement.
* `drop` cannot be `async`.
* `drop` is called exactly once for each fully initialized owned
  value.

Each of these has real trade-offs. Allowing `drop` to fail with
`Result` would model many real situations (closing a database
transaction, flushing a buffered writer) but creates the question
of what to do with errors raised during stack unwinding. Allowing
allocation is the difference between a clean RAII abstraction and
one that cannot be used in low-memory cleanup paths.

The leaning is toward "drop is small, total, synchronous, and
infallible" because that keeps destruction predictable, but Rae
has not committed to any of these yet.

## Explicit early destruction (open syntax)

Automatic scope-end drop should not prevent early cleanup. There
are real reasons to end ownership before scope exit:

* releasing large buffers before function exit
* unlocking before performing expensive work
* closing a file before renaming it
* unloading GPU memory before loading replacements

What this looks like syntactically is open. Three possibilities:

```rae
drop texture           // keyword statement
texture.drop()         // method call on the value
sysDropOwned(texture)  // plain stdlib function
```

A keyword form mirrors `ret` / `let` and signals "the compiler
treats this specially." A method form is cheaper to parse but
overloads the method-call slot. A function form keeps the surface
syntax small but loses the move-out signalling that the compiler
needs to enforce "you cannot use `texture` after this."

Whichever syntax wins, after explicit drop, the compiler must mark
the value as consumed and reject further use:

```rae
drop texture
use(texture) // compile error
```

That part — "explicit drop must invalidate the binding" — is
likely. The spelling is open.

## `defer` is still useful

Even with user-defined drop, `defer` has independent value.

Some cleanup is tied to an operation, not naturally to a value
type:

```rae
beginDebugGroup("shadow pass")
defer endDebugGroup()
```

Or:

```rae
pushUiClip(rect)
defer popUiClip()
```

Or:

```rae
temporarilyDisableNotifications()
defer restoreNotifications()
```

These are paired control-flow operations. Inventing a move-only
wrapper type for each might be excessive.

A good division is:

* **drop** for owned resources
* **defer** for lexical paired actions

They overlap, but neither fully replaces the other.

## A possible staging (one ordering, not the ordering)

If Rae does add this, an incremental rollout could look like:

### Stage 1: compiler-generated recursive drop

No user syntax yet.

The compiler recursively drops:

* `String`
* `List(T)`
* buffers and other built-in owning containers
* structs containing droppable fields
* generic specializations containing droppable types

This is already needed to fix nested memory ownership correctly,
which is why "structural drop" sits under "decided / likely."

### Stage 2: explicit early `drop`

Allow some form of:

```rae
drop value
```

(Syntax open per the section above.) This invokes the generated
cleanup and consumes the value.

### Stage 3: a resolved copy / move story

Settle the question raised under "does custom drop require
move-only types?". Until that lands, custom drop should not ship,
because the soundness of user-defined cleanup depends on it.

### Stage 4: user-defined `drop`

For external resources:

```rae
func drop(this: mod Texture) {
  unloadTexture(this.id)
}
```

The compiler combines that with automatic recursive field cleanup
according to whichever ordering rule wins (the section above on
"custom drop relative to automatic field drop").

### Stage 5: `defer`

Add lexical cleanup for paired operations and cases that do not
deserve a dedicated owned type.

This is **one** possible ordering. Other orderings make sense too —
`defer` is the easiest of these to implement in isolation and
could land first as a small win, even though structural drop is
more foundational to Rae's existing ownership claims.

## The font example revisited

For the global Rae UI font, a destructor is not automatically the
right solution.

A global font slot has unusual lifetime:

* initialized globally or in UI startup
* expected to live for the entire process
* possibly intentionally left alive until OS teardown
* macOS termination may use `_exit(0)`
* no stack unwinding occurs through `_exit`

A `Font` destructor would not run under `_exit`, just as `defer`
would not run.

So the font case does not prove Rae needs destructors.

But mid-program resources do:

```rae
func renderPreview(path: String) {
  let image: own Image = loadImage(path)
  let texture: own Texture = uploadTexture(image)

  // both clean themselves up correctly
}
```

That is where value-based cleanup becomes meaningful.

## Preferred Rae direction (working hypothesis)

Three concepts, kept separate:

```text
Structural drop:
    Compiler-generated recursion through owned fields.

Custom drop:
    A type-specific release hook for external resources.

Defer:
    A lexical control-flow cleanup action.
```

Rae definitely needs the first (decided).

Rae will probably eventually benefit from the second, especially
for graphics, audio, files, sockets, native handles, and C interop
(likely).

Rae may also benefit from the third, but it should not be used as
a substitute for proper owned resource types (likely).

Two principles, in order:

> Rae should automatically handle ordinary ownership cleanup, warn
> or restrict obvious hazards, and allow explicit unsafe or manual
> patterns when needed.

and

> Cleanup should follow ownership whenever ownership exists. When
> cleanup instead belongs to a temporary control-flow action, use
> `defer`.

The first sets the safety bar — Rae is helpful and ergonomic, not
proof-carrying. The second gives the model its shape: ownership
goes with the value, paired control-flow goes with the scope.

Together they let Rae avoid the false binary of "destructors
everywhere" versus "manual shutdown functions everywhere," without
adopting Rust's full borrow-checking apparatus. Whether each piece
of that model actually ships, and in what shape, is still open —
see the at-a-glance summary at the top.
