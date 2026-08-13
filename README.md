# Rae Programming Language

A language for code that humans and AI agents both have to read.

Rae compiles to C. It is explicit where most languages are implicit — what a
function does to your data is in its signature, what a call means is at the call
site — because both a person skimming at midnight and a model editing at scale
do better with a language that says what it is doing.

**Early days.** The compiler, the standard library, the 2D and 3D renderers and
the UI stack all work and are exercised by 318 test cases and 67 examples. The
language is not stable, there is no package ecosystem, and the sharp edges are
real. Read this as an account of what runs today, not a promise about tomorrow.

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

## What the syntax is doing

- **Named arguments, always.** `formatDuration(seconds: total)` rather than
  `formatDuration(total)`. Two arguments of the same type cannot be swapped
  silently, and a call reads without jumping to the declaration.
- **Parameter modes, always.** Every parameter is `view`, `copy`, `mod` or
  `own`; a bare `seconds: Int` is a compile error. "Does this function change my
  value" should never be a thing you have to guess or go and check.
- **`{}` is how a value becomes text.** Interpolation evaluates expressions and
  calls, so printing does not need a formatting mini-language.
- **No visibility keyword.** Everything is visible across files, so there is no
  `pub` to sprinkle and no false signal that something nearby is private.
- **`camelCase` functions, `PascalCase` types.** Normative, and followed
  throughout the tree — though the compiler does not yet reject a violation.

## What runs today

**Compiled (C backend)** is the default and the only target that matters for new
work. `rae build` emits C and links a native binary.

**Browser WASM** builds the same source into an SDL3 + WebGPU bundle with
Emscripten, or a headless WASI module for programs that only print. Around fifty
of the examples run in a browser.

**Live (bytecode VM)** is preserved but unsupported. It is still invocable with
`--target live`, and it is not a compatibility target for new language work; see
`docs/live-vm-status.md`. **Hybrid** packages a compiled host with Live bundles
and is in the same category.

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

Run the test suite with `make -C compiler test`.

## Where to look next

| | |
|---|---|
| `examples/104_ui_hello` | the smallest real application: a value and three buttons |
| `examples/105_ui_counter` | the same idea with a scene, a theme and a list rebuilt from data |
| `examples/112_metaballs_deferred` | the 3D renderer, with a settings panel and a live sky |
| `spec/rae.md` | the language spec |
| `docs/` | 72 design notes, including the ownership model and the renderer architecture |
| `examples/` | 67 programs, from hello world up |

## License

MIT — see `LICENSE`.
