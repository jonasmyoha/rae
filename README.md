# Rae Programming Language

[![XKCD 927: Standards](https://imgs.xkcd.com/comics/standards.png)](https://xkcd.com/927/)

A minimalistic language for code that humans and AI agents both have to read.

Few concepts, few special cases, and nothing implicit. What a function does to
your data is in its signature; what a call means is at the call site. That suits
a person skimming at midnight and a model editing at scale for the same reason:
neither has to hold the missing half in their head.

## A taste

```rae
import core

type Track {
  title: String
  seconds: Int
}

# Parameter modes are always written out. `view` reads, `mod` may write back,
# `own` takes ownership — so a signature tells you what a call does to your data.
func totalSeconds(tracks: view List(Track)) ret Int {
  var total: Int = 0
  var i: Int = 0
  loop i < tracks.length {
    total = total + tracks.at(index: i).seconds
    i = i + 1
  }
  ret total
}

func formatDuration(seconds: view Int) ret String {
  ret "{seconds / 60}m {seconds % 60}s"
}

func main() {
  var tracks: List(Track) = createList(Track, cap: 3)
  tracks.add(value: Track { title: "Intro", seconds: 95 })
  tracks.add(value: Track { title: "Middle Eight", seconds: 212 })
  tracks.add(value: Track { title: "Outro", seconds: 143 })

  # Arguments are named at the call site, and "{}" is how a value becomes text.
  log("{tracks.length} tracks, {formatDuration(seconds: totalSeconds(tracks: tracks))}")

  var i: Int = 0
  loop i < tracks.length {
    let track: Track = tracks.at(index: i)
    log("  {track.title} — {formatDuration(seconds: track.seconds)}")
    i = i + 1
  }
}
```

```
3 tracks, 7m 30s
  Intro — 1m 35s
  Middle Eight — 3m 32s
  Outro — 2m 23s
```

## Absence, and references

`opt T` says a value may not be there — in the type, not in a convention. There
is no null and no sentinel, so a caller cannot forget the empty case.

```rae
type Library {
  tracks: List(Track)
  current: Track
  playing: Bool
}

# An optional REFERENCE: a view of a track the library owns, or nothing at all.
func nowPlaying(lib: view Library) ret opt view Track {
  if lib.playing is false {
    ret none
  }
  ret view lib.current
}

# `if let` narrows it to a reference for the branch. `=>` binds — required
# here because the optional holds a reference, and `=` never produces a
# shared handle.
if let track: view Track => nowPlaying(lib: view lib) {
  log("playing {track.title} — {formatDuration(seconds: track.seconds)}")
} else {
  log("nothing playing")
}
```

```
playing Middle Eight — 3m 32s
nothing playing
```

An optional reference costs a nullable pointer — the same eight bytes as a plain
one, with no box and no allocation — so `nowPlaying` hands back a window onto the
library's own track rather than a copy of it.

## What the syntax is doing

- **Named arguments, always.** `formatDuration(seconds: total)` rather than
  `formatDuration(total)`. Two arguments of the same type cannot be swapped
  silently, and a call reads without jumping to the declaration.
- **Parameter modes, always.** Every parameter is `view`, `copy`, `mod` or
  `own`; a bare `seconds: Int` is a compile error. "Does this function change my
  value" should never be a thing you have to guess or go and check.
- **`{}` is how a value becomes text.** Interpolation evaluates expressions and
  calls, so printing does not need a formatting mini-language.
- **`=` copies, `=>` binds.** One character tells you whether a line produces an
  independent value or a second name for an existing one — without knowing how
  big the type is or what it holds. `=>` is required wherever the type contains
  `view` or `mod`, and refused everywhere else.

## Targets

**Compiled** is the default: the compiler emits C and links a native binary.

**Browser WASM** builds the same source into an SDL3 + WebGPU bundle with
Emscripten, or a headless WASI module for programs that only print. 52 of the 67
examples run in a browser.

Built on top, all in Rae except the raw GPU and platform calls:

- **2D renderer** — WebGPU through wgpu-native and SDL3: shapes, MSDF text,
  images, clipping, a design-resolution coordinate system.
- **3D renderer** — forward and deferred paths over the same scenes, with PBR
  materials, cascaded shadows, SSAO, TAA, SDF metaballs, glTF loading and
  skinned animation, and a physical sky (Preetham and Hosek-Wilkie).
- **UI** — an ECS whose widgets are entities and whose layout is a `.raescene`
  document, with a theme of palette slots, spacing and text styles.
- **Standard library** — 107 modules: containers, strings, JSON, files, maths,
  time, a calendar, crypto, PNG and DEFLATE written in Rae rather than bound.

## Getting started

```sh
make -C compiler                                     # builds compiler/bin/rae
./compiler/bin/rae run examples/01_hello/main.rae    # from the repo root
```

Run examples from the repo root: the ones that load assets resolve their paths
from there.

`rae run` builds and runs; `rae build --target wasm --out app.html …` produces a
browser bundle; `rae watch` rebuilds and restarts on save, and an app using
`lib/hot_reload` keeps its state across the restart. `rae init` scaffolds a
project. `rae format` pretty-prints.

Run the test suite with `make -C compiler test` — 318 cases.

## Devtools: the best way to see this

[**rae-devtools-web**](https://github.com/jonaskivi/rae-devtools-web) is a local
dashboard that browses every example, runs each one natively or in the browser
with a click, and shows build health, the test tree and history.

```sh
git clone git@github.com:jonaskivi/rae-devtools-web.git   # next to this repo
cd rae-devtools-web && bun install && make dev            # http://localhost:3000
```

Start on the **Featured** tab — a cross-section of what the language can do:

| | |
|---|---|
| **Metaballs (deferred rendering)** | the 3D renderer, with a settings panel and a sky that moves with the time of day |
| **3D Renderer — Walker Character** | a skinned, animated glTF character |
| **Mobile UI — GPU2D** | a phone-shaped application at real app scale |
| **2D Renderer — Vector shapes** | the 2D renderer's shape pipeline |
| **Raytracer — GPU + MTSDF text** | a path tracer on the GPU with a crisp text overlay |
| **Pong** and **Tetris 2D** | complete little games |
| **Functions and interpolation** | one step past hello world, no graphics |

The two UI examples are worth opening next: `104_ui_hello` is a value and three
buttons — the smallest thing that is still an application — and `105_ui_counter`
is the same idea with a `.raescene` layout, a theme and a list rebuilt from data.

The dashboard runs examples Compiled by default, and the ones that support it
also run in an embedded WebGPU canvas.

## Where to look next

| | |
|---|---|
| `spec/rae.md` | the language spec |
| `docs/` | 72 design notes, including the ownership model and the renderer architecture |
| `examples/` | 67 programs, from hello world up |

## License

MIT — see `LICENSE`.
