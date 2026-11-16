# Modules: a PascalCase file or a PascalCase folder

Status: **accepted** (2026-11-16, after a second review). Implemented in the compiler: rule 8 in
Rae 0.3.173, rules 1-7 and 9 in 0.3.174 (compiler/src/module_folder.c; cases 1503-1508, 721,
729). The library and tools follow in the queued tasks [modules 3/5 ... 5/5].

## The proposal

A module is **either a PascalCase file or a PascalCase folder**, and the code that uses it cannot
tell which.

- `import Gpu2d` finds `lib/Gpu2d.rae` **or** the folder `lib/Gpu2d/`. A folder module is every
  `.rae` file inside it, one module.
- A module that outgrows the 1,000-line cap becomes a folder. Nothing that uses it changes: the
  import, the qualifier (`Gpu2d.drawRect`) and every call stay as they were.
- **A module is one thing on disk**: one PascalCase file, or one PascalCase folder. So it can be
  moved, deleted and searched as one thing.
- **camelCase means a plain folder**, part of a path only (`ui/`, `ecs/`, `collections/`), as
  today.
- **A file is its own module only when no folder above it is PascalCase.** Inside a PascalCase
  folder, a file is storage for that folder's module, whatever its name.
- Calling code reads exactly as today: a module is always written in PascalCase (`Vec3.`, `Gpu2d.`,
  `RenderSystem.`), so it never looks like a variable.

### Rules

1. **File or folder, never both.** A file and a folder with the same name in the same place
   (`Gpu2d.rae` beside `Gpu2d/`) is an error that names both. This holds inside a module folder
   too (`Gpu2d/Images.rae` beside `Gpu2d/Images/`): otherwise creating the folder would move the
   file's functions from `Gpu2d.` to `Images.`, and adding a folder must never rename existing
   functions. A module that outgrows its file moves into its folder
   (`git mv Gpu2d.rae Gpu2d/Gpu2d.rae`); no user of it changes.
2. **One scope.** The files of a folder module share one scope, as one file does: a function in
   `Images.rae` calls a function in `Window.rae` with no import. Declaring the same name twice
   (the same parameters) is an error; overloads (other labels or parameter types) are fine.
3. **Imports per file.** Each file keeps its own `import` / `open` lines: a file says what it uses.
4. **File names inside a module do not matter.** Rename, split and merge them freely; they are
   storage. (They stay PascalCase, like every `.rae` file.) A file named after the module
   (`RenderSystem/RenderSystem.rae`) is allowed and is just one of its files. Importing a file of
   a module folder (`import Gpu2d/Images` for `Gpu2d/Images.rae`) is an error that says
   "import Gpu2d".
5. **Subfolders follow the casing rule too.** Inside `Gpu2d/`, a camelCase subfolder
   (`Gpu2d/platform/Web.rae`) only organizes files: they are still part of `Gpu2d`. A PascalCase
   subfolder (`Gpu2d/Images/`) is a module of its own, imported as `import Gpu2d/Images` and
   written `Images.loadImageKey(...)`.
6. **Platform files.** A file whose whole body sits in `when Target.isWeb { ... }` is part of the
   module only on that target. The per-platform code of a module can then live in its own files
   instead of `when` branches in one file.
7. **Diagnostics** name the file and line where a declaration really is.
8. **Names are checked against the disk exactly** (implemented in Rae 0.3.173, before the rest). Casing carries meaning here, and macOS and
   Windows ignore case in file names (today `import vec3` loads `lib/Vec3.rae` on a Mac). The
   compiler compares every import path segment with the exact name on disk and reports a
   mismatch as an error that gives the right spelling.
9. **A PascalCase folder in a program's own tree is an error for now** ("module folders are for
   lib/ and packages"), until the program rule below is decided.

### Before and after

The 2D renderer, split into files because it reached the cap:

```text
lib/Gpu2d.rae              →    lib/Gpu2d/Window.rae
                                lib/Gpu2d/Frame.rae
                                lib/Gpu2d/Shapes.rae
                                lib/Gpu2d/Images.rae
                                lib/Gpu2d/Text.rae
                                lib/Gpu2d/platform/Web.rae     (its body: when Target.isWeb { ... })
```

```rae
import Gpu2d                                               # unchanged

Gpu2d.beginFrame(canvas: canvas, r: 0.0, g: 0.0, b: 0.0, a: 1.0)
Gpu2d.drawRect(canvas: canvas, x: 10.0, y: 10.0, w: 100.0, h: 50.0, color: 0xFF00FF00)
let logo: Int = Gpu2d.loadImageKey(canvas: canvas, key: "logo", path: "logo.png")
Gpu2d.endFrame(canvas: canvas)
```

(In 0.3.172 only, the image functions were a separate module and the call read
`Gpu2dImages.loadImageKey`; since 0.3.175 lib/Gpu2d/ is a module folder holding them again.)

Vectors stay exactly as they are:

```rae
import Vec3

let forward: Vec3 = Vec3.vnorm(a: Vec3.vsub(a: target, b: position))
```

A UI system becomes one module folder instead of several modules a caller must tell apart:

```text
lib/ui/renderSystem/RenderSystem.rae     →    lib/ui/RenderSystem/Paint.rae
lib/ui/renderSystem/RenderFills.rae            lib/ui/RenderSystem/Fills.rae
lib/ui/renderSystem/RenderSprite.rae           lib/ui/RenderSystem/Sprite.rae
```

```rae
import ui/RenderSystem                     # was: import ui/renderSystem/RenderSystem
                                           #      import ui/renderSystem/RenderFills
RenderSystem.paintWorld(world: uiWorld, systems: uiSystems, canvas: canvas)
RenderSystem.paintGradient(canvas: canvas, entityId: entityId, fill: fill)   # was RenderFills.paintGradient
```

The ECS can stay a plain folder of small modules (today) or become one module folder:

```text
lib/ecs/Entity.rae, lib/ecs/Query.rae, ...     →  either unchanged, or
                                                  lib/Ecs/Entity.rae, lib/Ecs/Query.rae, ...
```

```rae
open ecs/Entity          # unchanged layout          open Ecs                  # one module folder
open ecs/Query                                       let entityId: EntityId = createEntity(world: world)
let entityId: EntityId = createEntity(world: world)
```

### What affects what

| Question | Affects | Answer |
|---|---|---|
| Is a module a file, or a file or a folder? | Whether splitting a big file changes the code that uses it | A file or a folder; using code cannot tell, so a split never changes it |
| How is a module written in code? | Whether `Name.call()` reads as a module or a variable | Always PascalCase, as today; a camelCase folder is only a path |

### Not part of this proposal

- A program's own folders keep today's rule (every project file sees every other; a folder name
  breaks a tie: `enemies.tick()`). Whether that should follow the same casing rule is a separate
  question.
- The same file-and-folder pair that Rust allows (`foo.rs` beside `foo/`) was considered and
  refused (rule 1): it saves one file move, and the module would live in two places.

### What follows from it (second review, 2026-11-16)

- **Merged files share all their names.** Rae has no file-private names, so when several modules
  become one folder, two helpers with the same name and parameters become a duplicate error and
  one is renamed. `lib/ui/renderSystem` (18 files, 3,805 lines) has no such pair.
- **The system rule in AGENTS.md changes.** Today a system is a camelCase folder with a
  PascalCase file (`relaySystem/RelaySystem.rae`); with this it is a PascalCase folder
  (`RelaySystem/`) with freely named files, imported as `import net/RelaySystem`.
- **"Package" means a `.raepack` package only.** A camelCase folder in an import path is a
  folder, not a package; AGENTS.md's naming section says "package FOLDERS" and is reworded.
- **A nested module is qualified by its last name** (`Images.loadImageKey` for `Gpu2d/Images`),
  so general names (`Images`, `Text`) meet more often. The existing error for two imports with
  one last name covers it (import one with `as`).
- **Tools that work per file work per module** (0.3.176): a build reads every file of the
  program each time (no source cache), so a change to any file of a module folder, or a file
  added to it, is in the next build; `run_cases.sh --grep lib/Gpu2d/Images.rae --list` searches
  for the file's module (`Gpu2d`); the format tree check and the example-copy gate walk module
  folders. `compiler/tools/module-folder-tools-check.sh` holds a fixture for each, one pre-suite
  case of every full run.
- **Rae keeps two folder rules for now:** module folders in `lib/` and packages, and a program's
  own folders (next point).

- The other layouts below (one shared math module, calling with the dot form, opening a whole
  package) are style choices that work with this proposal or without it, one folder at a time.

---

# Other layouts considered

The examples below show the same code in a few other folder layouts, where every folder is a
namespace and its camelCase name is the qualifier. They were the step before the proposal above:
they showed that lowercase qualifiers read like variables (`hierarchySystem.updateHierarchy(...)`)
and that `vec3` next to the type `Vec3` reads badly, which led to keeping modules PascalCase.

"The dot form" below means calling a function with its first argument before the dot:
`forward.normalize()` is `normalize(this: forward)`.

## 1. Vectors

### Today

```text
lib/Vec3.rae          type Vec3, create, vadd, vsub, vscale, vdot, vcross, vlen, vnorm
lib/Math.rae          sqrt, sin, clamp, ...
```

```rae
import Vec3

let worldUp: Vec3 = Vec3.create(x: 0.0, y: 0.0, z: 1.0)
let forward: Vec3 = Vec3.vnorm(a: Vec3.vsub(a: target, b: position))
let right: Vec3 = Vec3.vnorm(a: Vec3.vcross(a: forward, b: worldUp))
let up: Vec3 = Vec3.vcross(a: right, b: forward)
let length: Float = Math.sqrt(x: Vec3.vdot(a: forward, b: forward))
```

### Folder per type

```text
lib/vec3/Vec3.rae
lib/vec3/Swizzle.rae
lib/math/Scalar.rae
```

```rae
import vec3
import math

let worldUp: Vec3 = Vec3.create(x: 0.0, y: 0.0, z: 1.0)
let forward: Vec3 = vec3.vnorm(a: vec3.vsub(a: target, b: position))
let right: Vec3 = vec3.vnorm(a: vec3.vcross(a: forward, b: worldUp))
let up: Vec3 = vec3.vcross(a: right, b: forward)
let length: Float = math.sqrt(x: vec3.vdot(a: forward, b: forward))
```

### One math folder, names overloaded by type, called with the dot form

```text
lib/math/Vec2.rae      normalize(this: Vec2), dot(this: Vec2, other: Vec2), ...
lib/math/Vec3.rae      normalize(this: Vec3), dot(this: Vec3, other: Vec3), cross, ...
lib/math/Quat.rae
lib/math/Scalar.rae    sqrt, sin, clamp, ...
```

```rae
import math

let worldUp: Vec3 = Vec3.create(x: 0.0, y: 0.0, z: 1.0)
let forward: Vec3 = target.sub(other: position).normalize()
let right: Vec3 = forward.cross(other: worldUp).normalize()
let up: Vec3 = right.cross(other: forward)
let length: Float = math.sqrt(x: forward.dot(other: forward))
```

### The same, opened

```rae
open math

let forward: Vec3 = normalize(this: sub(this: target, other: position))
let length: Float = sqrt(x: dot(this: forward, other: forward))
```

---

## 2. The 2D renderer

### Today

```text
lib/Gpu2d.rae
lib/Gpu2dImages.rae
lib/Gpu2dCanvas.rae
lib/Gpu2dText.rae
```

```rae
import Gpu2d
import Gpu2dImages
import Gpu2dText

Gpu2d.initWindow(width: 1280, height: 800, title: "Demo")
var canvas: Gpu2dCanvas = createGpu2dCanvas()
let logo: Int = Gpu2dImages.loadImageKey(canvas: canvas, key: "logo", path: "logo.png")
Gpu2d.beginFrame(canvas: canvas, r: 0.0, g: 0.0, b: 0.0, a: 1.0)
Gpu2d.drawRect(canvas: canvas, x: 10.0, y: 10.0, w: 100.0, h: 50.0, color: 0xFF00FF00)
Gpu2dImages.drawImageKey(canvas: canvas, key: "logo", x: 20.0, y: 80.0, w: 64.0, h: 64.0)
Gpu2dText.drawText(canvas: canvas, font: font, text: "Hello", x: 20.0, y: 200.0, sizePx: 32.0, color: -1)
Gpu2d.endFrame(canvas: canvas)
```

### One folder

```text
lib/gpu2d/Window.rae
lib/gpu2d/Frame.rae
lib/gpu2d/Shapes.rae
lib/gpu2d/Images.rae
lib/gpu2d/Text.rae
lib/gpu2d/Canvas.rae
lib/gpu2d/Web.rae         its body: when Target.isWeb { ... }
```

```rae
import gpu2d

gpu2d.initWindow(width: 1280, height: 800, title: "Demo")
var canvas: Gpu2dCanvas = gpu2d.createGpu2dCanvas()
let logo: Int = gpu2d.loadImageKey(canvas: canvas, key: "logo", path: "logo.png")
gpu2d.beginFrame(canvas: canvas, r: 0.0, g: 0.0, b: 0.0, a: 1.0)
gpu2d.drawRect(canvas: canvas, x: 10.0, y: 10.0, w: 100.0, h: 50.0, color: 0xFF00FF00)
gpu2d.drawImageKey(canvas: canvas, key: "logo", x: 20.0, y: 80.0, w: 64.0, h: 64.0)
gpu2d.drawText(canvas: canvas, font: font, text: "Hello", x: 20.0, y: 200.0, sizePx: 32.0, color: -1)
gpu2d.endFrame(canvas: canvas)
```

### One folder, called with the dot form on the canvas

```rae
import gpu2d

gpu2d.initWindow(width: 1280, height: 800, title: "Demo")
var canvas: Gpu2dCanvas = gpu2d.createGpu2dCanvas()
let logo: Int = canvas.loadImageKey(key: "logo", path: "logo.png")
canvas.beginFrame(r: 0.0, g: 0.0, b: 0.0, a: 1.0)
canvas.drawRect(x: 10.0, y: 10.0, w: 100.0, h: 50.0, color: 0xFF00FF00)
canvas.drawImageKey(key: "logo", x: 20.0, y: 80.0, w: 64.0, h: 64.0)
canvas.drawText(font: font, text: "Hello", x: 20.0, y: 200.0, sizePx: 32.0, color: -1)
canvas.endFrame()
```

### A folder with sub-namespaces

```text
lib/gpu2d/Window.rae
lib/gpu2d/Frame.rae
lib/gpu2d/images/Load.rae
lib/gpu2d/images/Draw.rae
lib/gpu2d/text/Text.rae
```

```rae
import gpu2d
import gpu2d/images
import gpu2d/text

gpu2d.beginFrame(canvas: canvas, r: 0.0, g: 0.0, b: 0.0, a: 1.0)
let logo: Int = images.loadImageKey(canvas: canvas, key: "logo", path: "logo.png")
images.drawImageKey(canvas: canvas, key: "logo", x: 20.0, y: 80.0, w: 64.0, h: 64.0)
text.drawText(canvas: canvas, font: font, text: "Hello", x: 20.0, y: 200.0, sizePx: 32.0, color: -1)
gpu2d.endFrame(canvas: canvas)
```

---

## 3. The ECS

### Today

```text
lib/ecs/Entity.rae
lib/ecs/World.rae
lib/ecs/Query.rae
lib/ecs/Event.rae
lib/ecs/hierarchySystem/HierarchySystem.rae
lib/ecs/transformSystem/TransformSystem.rae
```

```rae
open ecs/Entity
open ecs/World
open ecs/Event
import ecs/Query
import ecs/hierarchySystem/HierarchySystem
import ecs/transformSystem/TransformSystem

let entityId: EntityId = createEntity(world: world)
addComponent(world: world, entityId: entityId, value: Position { x: 0.0, y: 0.0 })
loop let row: QueryRow2 in Query.query2(Position, Velocity, world: world) {
  row.a.x = row.a.x + row.b.x
}
HierarchySystem.updateHierarchy(world: world)
TransformSystem.updateTransforms(world: world)
sendEvent(world: world, event: PlayerJoined { entityId: entityId })
```

### One ecs folder, systems in subfolders

```text
lib/ecs/Entity.rae
lib/ecs/World.rae
lib/ecs/Query.rae
lib/ecs/Event.rae
lib/ecs/hierarchySystem/HierarchySystem.rae
lib/ecs/transformSystem/TransformSystem.rae
```

```rae
open ecs
import ecs/hierarchySystem
import ecs/transformSystem

let entityId: EntityId = createEntity(world: world)
addComponent(world: world, entityId: entityId, value: Position { x: 0.0, y: 0.0 })
loop let row: QueryRow2 in query2(Position, Velocity, world: world) {
  row.a.x = row.a.x + row.b.x
}
hierarchySystem.updateHierarchy(world: world)
transformSystem.updateTransforms(world: world)
sendEvent(world: world, event: PlayerJoined { entityId: entityId })
```

### The same, imported (qualified)

```rae
import ecs
import ecs/hierarchySystem
import ecs/transformSystem

let entityId: EntityId = ecs.createEntity(world: world)
ecs.addComponent(world: world, entityId: entityId, value: Position { x: 0.0, y: 0.0 })
loop let row: QueryRow2 in ecs.query2(Position, Velocity, world: world) {
  row.a.x = row.a.x + row.b.x
}
hierarchySystem.updateHierarchy(world: world)
transformSystem.updateTransforms(world: world)
ecs.sendEvent(world: world, event: PlayerJoined { entityId: entityId })
```

### Systems as plain files in the ecs folder

```text
lib/ecs/Entity.rae
lib/ecs/World.rae
lib/ecs/Query.rae
lib/ecs/HierarchySystem.rae
lib/ecs/TransformSystem.rae
```

```rae
open ecs

let entityId: EntityId = createEntity(world: world)
updateHierarchy(world: world)
updateTransforms(world: world)
```

---

## 4. A UI system

### Today

```text
lib/ui/renderSystem/RenderSystem.rae
lib/ui/renderSystem/RenderFills.rae
lib/ui/renderSystem/RenderSprite.rae
lib/ui/layoutSystem/LayoutSystem.rae
```

```rae
import ui/renderSystem/RenderSystem
import ui/renderSystem/RenderFills
import ui/layoutSystem/LayoutSystem

LayoutSystem.layoutWorld(world: uiWorld, systems: uiSystems)
RenderSystem.paintWorld(world: uiWorld, systems: uiSystems, canvas: canvas)
RenderFills.paintGradient(canvas: canvas, entityId: entityId, fill: fill)
```

### A folder per system

```rae
import ui/renderSystem
import ui/layoutSystem

layoutSystem.layoutWorld(world: uiWorld, systems: uiSystems)
renderSystem.paintWorld(world: uiWorld, systems: uiSystems, canvas: canvas)
renderSystem.paintGradient(canvas: canvas, entityId: entityId, fill: fill)
```

### The same, called with the dot form on the world

```rae
import ui/renderSystem
import ui/layoutSystem

uiWorld.layoutWorld(systems: uiSystems)
uiWorld.paintWorld(systems: uiSystems, canvas: canvas)
```

---

## 5. A program's own files

### Today

```text
game/Main.rae
game/Player.rae
game/enemies/Tick.rae      func tick()
game/enemies/Spawn.rae     func spawnWave()
game/particles/Tick.rae    func tick()
```

```rae
func main() {
  spawnWave()
  enemies.tick()
  particles.tick()
  movePlayer(player: player)
}
```

### Folders as namespaces

The same code, the same files: a program already works this way.

---

## 6. A dependency package

### Today

```text
packages/audio/Audio.rae
packages/audio/Sound.rae
packages/audio/WavWriter.rae
packages/audio/AudioGraph.rae
```

```rae
open audio/Audio
open audio/Sound
open audio/WavWriter

var audio: AudioEngine = startAudio()
let data: SoundData = readSoundFile(path: "hit.wav")
var writer: WavWriter = openWavWriter(path: "mix.wav", channels: 2, sampleRate: 48000, format: WavFormat.pcm24, dither: false)
```

### Flat package

```text
packages/audio/Audio.rae
packages/audio/Sound.rae
packages/audio/WavWriter.rae
packages/audio/AudioGraph.rae
```

```rae
open audio

var audio: AudioEngine = startAudio()
let data: SoundData = readSoundFile(path: "hit.wav")
var writer: WavWriter = openWavWriter(path: "mix.wav", channels: 2, sampleRate: 48000, format: WavFormat.pcm24, dither: false)
```

### Package with sub-namespaces

```text
packages/audio/engine/Audio.rae
packages/audio/files/Sound.rae
packages/audio/files/WavWriter.rae
packages/audio/graph/AudioGraph.rae
```

```rae
import audio/engine
import audio/files

var audio: AudioEngine = engine.startAudio()
let data: SoundData = files.readSoundFile(path: "hit.wav")
var writer: WavWriter = files.openWavWriter(path: "mix.wav", channels: 2, sampleRate: 48000, format: WavFormat.pcm24, dither: false)
```

---

## 7. Per-platform code

### Today

```rae
# lib/Gpu2d.rae
when Target.isWeb {
  func presentFrame(...) { ... }
} else {
  func presentFrame(...) { ... }
}
```

### Folders as namespaces

```text
lib/gpu2d/Present.rae
lib/gpu2d/PresentWeb.rae
```

```rae
# lib/gpu2d/Present.rae
when not Target.isWeb {
  func presentFrame(...) { ... }
}
```

```rae
# lib/gpu2d/PresentWeb.rae
when Target.isWeb {
  func presentFrame(...) { ... }
}
```

The calling code is the same in every case: `gpu2d.presentFrame(...)`.
