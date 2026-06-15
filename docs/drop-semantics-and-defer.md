# Drop semantics, destructors, and defer in Rae

> Design notes on how cleanup should work in Rae — written before any
> of this is implemented. Captures the reasoning so future
> implementation has a coherent target rather than ad-hoc cases.

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

## Structural drop

The simplest model is fully compiler-generated structural cleanup.

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

## User-defined cleanup is an extension of that model

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
user-defined `drop` simply makes the answer “yes” for types whose
resource ownership is not visible from their field types.

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

rather than treating every external resource as an `Int`.

## The dangerous part: copying

User-defined destruction cannot be added safely unless Rae’s copy
and move rules are clear.

Consider:

```rae
let a: Texture = loadTexture(...)
let b = a
```

If this copies the raw handle, both `a` and `b` may later call
`unloadTexture` on the same GPU object.

That causes double-free-like behavior.

So resource-owning types must usually be one of:

* move-only
* explicitly cloneable
* reference-counted
* non-owning handle types
* backed by some shared ownership mechanism

Rae already has `own`, `view`, and `mod`, so it has useful
vocabulary for this.

A likely rule could be:

> A type with custom drop is not implicitly copyable unless it also
> defines an explicit clone operation.

That is close to Rust’s logic, but Rae does not need to copy Rust’s
syntax or complexity.

For example:

```rae
let texture: own Texture = loadTexture(path)
let moved: own Texture = texture
```

After the move, `texture` is no longer usable.

But:

```rae
let reference: view Texture = moved
```

does not create another owner and therefore does not create another
cleanup obligation.

## Drop order matters

Rae would need a precise drop order.

Usually, fields should be dropped in reverse declaration order,
mirroring stack unwinding:

```rae
type Renderer {
  device: Device
  pipeline: Pipeline
  texture: Texture
}
```

If later fields depend on earlier ones, reverse order means:

1. `texture`
2. `pipeline`
3. `device`

That is often desirable because resources are commonly created in
declaration order and should be destroyed in reverse creation
order.

But Rae must make this deterministic and documented.

Local variables need an order too:

```rae
let device = createDevice()
let texture = createTexture(device)
```

At scope exit:

```text
drop texture
drop device
```

Again, reverse binding order is the natural rule.

## Custom drop plus automatic field drop

A crucial language-design choice is whether a custom drop function
replaces or supplements automatic field dropping.

Suppose:

```rae
type Font {
  handle: FontHandle
  glyphs: List(Glyph)
}

func drop(this: mod Font) {
  unloadFont(this.handle)
}
```

Should Rae also automatically drop `glyphs`?

The safer ergonomic design is usually:

1. run the user-defined cleanup body
2. automatically drop owned fields afterward

That prevents users from needing to manually recurse through every
field.

Conceptually:

```rae
func generatedDropFont(this: mod Font) {
  userDropFont(this)
  drop(this.glyphs)
}
```

However, there are complications.

What if `unloadFont` also frees memory represented by `glyphs`?
Then automatic field cleanup would double-free.

That suggests Rae may need a distinction between:

* ordinary owned fields, which are always recursively dropped
* raw or externally managed fields, which are not
* fields explicitly extracted or invalidated during custom drop

A strong default should be that normal Rae-owned fields remain
automatically managed. Custom drop should primarily release external
resources not represented by ordinary Rae ownership.

## Partial initialization

This gets difficult during construction.

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

## What happens during panic?

This connects directly to Rae’s broader panic design.

There are two broad models.

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

Rae could support both:

* ordinary `Result` propagation performs full cleanup
* fatal panic aborts without cleanup
* a recoverable panic mechanism, if added, would unwind

For games and real-time systems, avoiding arbitrary destructor work
in an audio callback or hard real-time section may matter greatly.

## Destructors should probably not be general arbitrary magic

There is a spectrum.

At one extreme:

```rae
func drop(this: mod Thing) {
  sendNetworkMessage()
  saveDatabase()
  spawnThread()
  throwError()
}
```

That creates unpredictable hidden behavior.

At the other extreme, custom drop is constrained to simple,
non-failing release operations.

Rae could define strong rules:

* `drop` cannot return a value
* `drop` cannot fail with `Result`
* `drop` cannot panic, or panic inside drop becomes fatal
* `drop` cannot move out ordinary fields except through special
  operations
* `drop` executes synchronously
* `drop` should not allocate, perhaps as guidance rather than
  enforcement
* `drop` cannot be `async`
* `drop` is called exactly once for each fully initialized owned
  value

These restrictions keep destruction understandable.

## Explicit destruction may still be needed

Automatic scope-end drop should not prevent early cleanup.

For example:

```rae
let texture = loadTexture(path)

// use texture

drop texture

// texture is now unusable
```

An explicit `drop texture` operation could end ownership early.

This is useful for:

* releasing large buffers before function exit
* unlocking before performing expensive work
* closing a file before renaming it
* unloading GPU memory before loading replacements

After explicit drop, the compiler must mark the value as consumed.

That aligns well with an ownership model:

```rae
drop texture
use(texture) // compile error
```

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

## How this might fit Rae specifically

A conservative Rae design could proceed in stages.

### Stage 1: compiler-generated recursive drop

No user syntax yet.

The compiler recursively drops:

* `String`
* `List(T)`
* buffers and other built-in owning containers
* structs containing droppable fields
* generic specializations containing droppable types

This is already needed to fix nested memory ownership correctly.

### Stage 2: explicit early `drop`

Allow:

```rae
drop value
```

This invokes the generated cleanup and consumes the value.

### Stage 3: non-copyable owned resource types

Introduce or infer a rule that types with resource ownership cannot
be implicitly copied.

This may already follow naturally from owned fields.

### Stage 4: user-defined `drop`

For external resources:

```rae
func drop(this: mod Texture) {
  unloadTexture(this.id)
}
```

The compiler combines that with automatic recursive field cleanup.

### Stage 5: `defer`

Add lexical cleanup for paired operations and cases that do not
deserve a dedicated owned type.

This ordering matters. `defer` is easier to implement in isolation,
but recursive drop is more foundational to Rae’s existing ownership
claims.

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

## Preferred Rae direction

Three concepts, kept separate:

```text
Structural drop:
    Compiler-generated recursion through owned fields.

Custom drop:
    A type-specific release hook for external resources.

Defer:
    A lexical control-flow cleanup action.
```

Rae definitely needs the first.

Rae will probably eventually benefit from the second, especially
for graphics, audio, files, sockets, native handles, and C interop.

Rae may also benefit from the third, but it should not be used as a
substitute for proper owned resource types.

The deepest principle is:

> Cleanup should follow ownership whenever ownership exists.

When cleanup instead belongs to a temporary control-flow action,
use `defer`.

That gives Rae a coherent model rather than choosing between
“destructors everywhere” and “manual shutdown functions
everywhere.”
