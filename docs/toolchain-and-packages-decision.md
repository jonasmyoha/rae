# Decision record: toolchain pins, one store, and the package manager

**Status: agreed on 2026-11-04 (section 7). Not built yet, and no project has moved. The steps are in
QUEUE.md.**

The three games outside this repo have neutral names here, because their real names must not appear
in this repository: game proto1 builds with `rae-pinned`, game proto2 has `src` and `server` packs,
and game proto3's lock names a commit that no longer exists.

## 1. Decisions

1. **No `rae` on PATH.** Each project has its own small `./rae` (section 3).
2. **The package manager is in the C compiler, for now.** It extends what exists.
3. **One file.** The `.raepack` holds what `rae.lock` holds today, and `rae.lock` goes (section 4).
4. **A pin is a version number, with an optional commit**, for the compiler and for packages (section 5).
5. **A pin is a name, a source (git URL or local path) and a version.**
6. **One git repo can hold several packages.**
7. **No registry yet.** Git and local paths are enough for now.
8. **Build real example packages**, to prove the design (section 6).

## 2. What was measured (2026-11-04)

| question | measured |
|---|---|
| Does a normal build change a pin? | Yes. Main's compiler overwrote a pin to `930bf77d` with `0.2.339-dev.668+dirty`, `47ba936a`. That is how `+dirty` got into committed locks. |
| When does a build write the lock? | Only when the pack parses and declares a `rae` requirement or dependencies. A pack that does not parse skips the check with no message. |
| Can an old compiler be used as it is? | Yes. The real `930bf77d` compiler built the same project. |
| Size of a pin | 286-428 MB as a full checkout. 64 MB as a sparse checkout of `compiler lib tools third_party`, with the compiler built. 143 and 106 emit C from it. |
| Time to make a pin | 14 s, including a clone from a local repository. |
| Can the git metadata be dropped? | Not today. A compiler built from `git archive` reports `unknown unknown`. Its version text would need another source. |
| The one clone that pins share | 389 MB. |
| Do commit pins survive? | **No.** Game proto3's `a6c0ae07` is gone from Rae, removed by a history rewrite. |
| Rae's tags | 3 tags, in a history where `compiler/VERSION` changed in 659 commits. |

## 3. `./rae`, instead of PATH

A short script committed in each project, the way Gradle's `gradlew` works. A person types `./rae
build`, and a Makefile calls `./rae` too.

- It reads the version from the project's `.raepack`. It can read it with a few lines of shell,
  because `rae format` owns the file's layout, so the line is always in the same form.
- It runs `~/.rae/toolchains/<version>/compiler/bin/rae` with the same arguments. If that toolchain
  is missing, it fetches and builds it first.
- `RAE_ROOT=<checkout> ./rae build` uses that checkout for one command, and prints one line saying so.
- `./rae toolchain ...` needs commands that old compilers do not have, so it runs the newer of the
  compiler that wrote this `./rae` and the pin (the script's `tool=` line).
- When a toolchain is missing, the script only gets the source and takes `tools/toolchain-prepare.sh`
  from that version of Rae itself. The script does not grow when preparing does.
- The source is `RAE_SOURCE`, else the checkout saved in `~/.rae/source` when a compiler built in a
  checkout pins, else the GitHub repo. Local commits can so be pinned before a push, and `pin` warns
  that another machine cannot get them yet.

Why this is better than PATH, and not only different:

- A fresh clone builds with one command and no install step. It needs only git (with git-lfs, as
  the fonts and images are in Git LFS since 0.3.123), make and a C compiler.
- Nothing global chooses a version. The project chooses.

**Criticism of this choice:** it puts a script back in each project, which is the problem this task
started from. The difference is that it is generated and the same everywhere, has no project logic,
and holds about 40 lines against 142-362 today. `./rae toolchain status` says when it is out of date,
and `rae init` writes it. A `~/.rae` store still exists, but it holds only copies that can be made
again, and nothing in it decides anything.

## 4. One file: `.raepack`

```
pack Game {
  format: "raepack"
  version: 2
  meta: { name: "Game", version: "0.1.6" }
  rae: { version: "0.2.340" }           # or { version: "0.2.340", commit: "55a8ac7e…" }
  dependencies: {
    dep sdk: { path: "../sdk" }
    dep qr: {
      git: "https://github.com/jonasmyoha/rae"
      subdir: "packages/qr"
      version: "1.2.0"
      hash: "git-tree:…"
    }
  }
}
```

**This works, under one condition, and it is better than two files if that condition holds.** Every
other package manager has two files because the manifest holds a wish (`^1.2`) and the lock holds the
answer (`1.2.7`). With a wish there has to be an answer somewhere. If the pin is always one exact
version, the wish is the answer, and the second file has nothing left to say.

So the condition is: **no version ranges.** `"0.2"` stops being accepted as a pin. `./rae update` looks
for newer versions and writes the exact one.

What this gains:

- **A build never writes any file.** That ends the bug in section 2 completely. Only `./rae add`,
  `update` and `toolchain pin` change the pack.
- One place to read, and no generated `rae.lock` to merge.

What has to stay, or it gets worse:

- **The hash stays, written by the tool, in the human's file.** A version is only a name. A git tag can
  be moved, and a server can return something else. Without the hash, "version 1.2.0" does not promise
  the same files. Go has the same design and keeps `go.sum` for this one reason. The hash can stay in
  the pack, because a dependency's own pack holds the hashes of its own dependencies, so the chain is
  checked without listing them all at the top.
- **Two dependencies wanting different versions of a third is an error**, unless the project declares
  that third package itself, and then its version wins. There is no solver.

`rae` means one thing on a project, and another on a dependency: the project's is the exact compiler it
uses; a dependency's is the lowest compiler it needs, checked against the project's.

## 5. Version pins, and what they require from Rae

**Agreed: this is right, and proto3 shows why.** A history rewrite killed a commit pin, while a tag name
survives one (`git filter-repo` moves tags to the new commits). And a version does not tie the design
to git: `git:` is one source, and a later `url:` or a registry is another, with the same version.

**The catch: today a Rae version number does not name one thing.** `0.2.339` covers every commit until
the next bump, and there are only 3 tags. A version pin only means something if each version is one
fixed set of files. That requires:

- **Every `compiler/VERSION` bump gets the tag `v<version>`, on that commit, automatically.** AGENTS.md
  already says a release is a tag on the bump commit, so this only applies the rule every time.
- **Old versions get tags too**, one on each past bump commit, so the projects have versions to pin to.
- **Before 1.0, a version says nothing about compatibility**, since PATCH can break things. A pin still
  gives the same build every time, which is what it is for.

**An optional commit covers what a tag cannot.** The version is required. A `commit` may be added:

```
rae: { version: "0.2.340" }                       # the tag v0.2.340
rae: { version: "0.2.340", commit: "55a8ac7e…" }  # that exact commit, which must be at version 0.2.340
```

- **With no commit,** the version means its tag, and a missing tag is an error.
- **With a commit,** that commit is used, from the remote or from the local Rae checkout, so a fix can be
  pinned at once, with no bump, even before it is pushed. Its `compiler/VERSION` must equal the version
  in the pin, or it is an error, so the two cannot disagree without anyone noticing.
- **The commit is what can die.** A commit may be unpushed, so it exists on one machine only, or a history
  rewrite may remove it. Then the build stops and says so, and names the version as the way back:
  `./rae toolchain pin 0.2.340`. `pin` warns when a commit is not on the remote. The version is what
  lasts, so the commit is the short-term form and the tag is the long-term one.
- **The store folder** is `toolchains/<version>`, or `toolchains/<version>-<commit>` with a commit.
- **`./rae toolchain pin <version>`** writes the version alone, and **`pin <commit>`** writes both, reading
  the version from that commit. A checkout with uncommitted changes cannot be pinned: that is what
  `RAE_ROOT` is for.
- **A git dependency works the same way:** a required `version`, an optional `commit`. A `path` dependency
  needs no version, since it is files in place. If it gives one, that is checked against the dependency's
  `meta.version`.

Old versions may not build the same way. Each migrating project is built at its tag first, and moves
to the next tag if it does not build.

## 6. Packages, and what to do with the stdlib

**Several packages in one git repo: yes.** A `subdir` field and a tag per package (`qr/v1.2.0`), the way
Go does it. Small to add.

**Splitting the stdlib into versioned packages: not yet, and I think `core` never.**

- `core` is what the compiler itself understands (`List`, the prelude). Its version is the compiler's
  version by nature.
- Much of the rest calls C in `compiler/runtime/` (`rae_ext_...`). `lib/AppLinks` needs
  `runtime_applinks.c`, for one. A `lib` module at a different version from the compiler would need
  the C that it expects, and today that C is part of the compiler.
- Today every `lib` module is tested against every other at each commit. Separate versions replace
  that with a matrix of combinations, for one user with a few projects.

So the compiler and the stdlib stay one version until a package gives a real reason to split.

**The 3D engine is the right goal and the wrong first package.** It is large (`lib/app3d` alone is 138
files and 36,000 lines), and two things it needs do not exist yet: a package cannot carry C code, and
its shaders are found by paths like `lib/foo.wgsl`, which assume the stdlib's folder. `lib/stylizedTrees`,
`lib/treeSets` and `packages/physicsScenes` also import `app3d` today.

**Basic 3D in the base stdlib: leave it there for now.** It already is. Moving it out is a cost with no
user asking for it. Decide after a second package has shown what a split costs.

**The order:**

1. **`physicsScenes` first** (`packages/physicsScenes`, no C). Done in 0.2.347: tag `physicsScenes/v1.0.0`,
   used by examples 122-125 and cases 1002, 1005 and 1205 with `git`, `subdir`, `version` and `hash`. It proves
   the whole path. `qr` was the first plan, and cannot go first: `lib/ui/qrCodeSystem` and the walker kit import
   it, and a stdlib module cannot import a package. Check who imports a module, not only what it imports.
2. **Then a package with its own C**, which forces the design for C and shaders (below). `box3d` was the
   candidate, and cannot be first; `sql` did it in 0.3.4, tag `sql/v1.0.0`.
3. **Then the engine**, starting from the top of the dependency tree (`app3d`).
4. **`trees`:** `treeGen`, `stylizedTrees`, `lowPolyTrees` and `treeSets`, one package. `app3d` imports them, so
   it goes with or after the engine, and it has shaders, so it needs the design from step 2.
5. **`qr`:** done in 0.3.8, tag `qr/v1.0.0`, once the walker kit had a hook that paints a package's nodes
   (`walkerGamePaint`). `QrCodeSystem` and `WalkerQrScan` moved in with it, so `RAE_WALKER_QR` works only in a game
   that uses the package.

### How a package carries C, shaders and files (all built in 0.3.6; shaders and assets have no real package yet)

Today each of these assumes the stdlib's folder or names one library by hand:

| what | today | where it is hard-coded |
|---|---|---|
| C library, native | `box3d_link_flags`, `sqlite_link_flags`, `wamr_link_flags`: found by a header in the emitted C, then `RAE_<NAME>` | `compiler/src/main.c` |
| C library, browser | the same three again | `compiler/src/main.c`, `compiler/tools/wasm_build.sh` |
| C library, iOS and Android | compiled from sources, per library | `tools/ios/gen-ios.sh`, `tools/android/gen-android.sh` |
| shaders | `shader(files:)`, 62 uses over 84 `.wgsl` files, embedded at build; a path is tried in the project, then `lib/...` in the stdlib | `compiler/src/shader_compose.c` |
| files read at run time | `Sys.readAsset("lib/...")` with `RAE_STDLIB`, staged for a device by `tools/stage-app-assets.sh`, preloaded for the browser by a list in `main.c` | three places, all naming `lib/` |

**C: the package's pack says it, and no compiler code names a library.**

```
pack Sql {
  meta: { version: "1.0.0" }
  native: {
    build: "native/build.sh"
    header: "rae_sql.h"
    include: "include"
    link: "lib/libsqlite3.a"
    linkBrowser: "lib/emscripten/libsqlite3.a"
    linkLinux: "-lpthread -lm"
    sourcesDevice: "src/sqlite3.c src/rae_sql_glue.c"
    definesDevice: "DEVICE_DEFINES"
  }
}
```

- A package's build goes in `~/.rae/packages/<name>/<pin>/install`, and `<pin>` is the package's own `git-tree` hash, or a hash
  of its files for a `path` dep, so two versions with different C both build. Its script gets that folder as
  `RAE_PACKAGE_INSTALL` and the folder above it as `RAE_PACKAGE_CACHE`. `gc` removes those no recorded project's packs pin, and
  they come back on demand.
- A build that needs it and does not find it builds it, once, under a lock, the way it clones a dep it does not have, so a
  fresh clone needs one command. `rae fetch` builds them all ahead of time. The pack is never written.
- A program links it only when its emitted C includes `header`, the rule before this, so importing a package you do not reach
  costs nothing.
- `rae tree --native` lists each package's device sources and options, and `gen-ios.sh` reads that instead of naming a library.
  `gen-android.sh` reads `rae tree --native-android`: the same, with the pack's `sourcesAndroid` / `definesAndroid` where it
  gives them. `packages/audio` does: its iOS glue is Objective-C (`.m`), the same text that Android compiles as C.
  A `.java` among them is the package's Android Java (`packages/audio`'s session: the audio focus and a MediaSession):
  `gen-android.sh` compiles it with SDL's and the app's Java, and the package's C reaches it through JNI. So a package
  carries its own Android half, and no toolchain file names it. A package may also install Android parts,
  `android/parts/<name>/manifest.xml` (lines put inside the app's `<application>`) and `android/parts/<name>/res/`
  (compiled and linked): `gen-android.sh` adds one only when the build names it in `RAE_ANDROID_PARTS`, so a part
  meant for tests (`packages/audio`'s `midiLoopback`, a MIDI device other apps would see) is in no normal app.
  Files in the install folder, not a pack field: the build script already copies them, and the pack stays the
  same for every package. `RAE_BOX3D` and `RAE_WAMR` stay for the stdlib's own until those libraries move.
- A library that needs `cmake` or a network says so in its script's error. A fresh clone of a project with C needs those too.

**Shaders: a path in a package's module is tried next to the file, then in that package's folder, then `lib/...` in the stdlib.**
`shader(files: ["shaders/spray.wgsl", "lib/noise.wgsl"])` is embedded at build and validated by naga, so it needs no run-time path
on any platform. A package never looks in the project above it, even for `lib/...`, and `..` is refused.

**Files read at run time: embed small text, declare the rest.**

- A compile-time `embedText(file: "scenes/Hud.raescene")`, built like `shader(files:)`, gives a `String` built into the program.
  It works in the browser and on a phone with nothing to stage. Done in 0.3.5: `physicsScenes` 1.1.0 is back on real `.raescene`
  files with it. A path is tried next to the file. In a package's module it is then tried in the package's folder and nowhere
  else, so a package cannot reach the project above it, and `..` is refused there. Elsewhere it is tried against the project and
  the stdlib, as for a shader. The file is a build input, so `rae watch` rebuilds when it changes. The price: a running app does not
  see an edit until it is rebuilt.
- Pictures, fonts and models, too large to embed, are declared: `assets: "assets maps"` in the pack, space separated, as the
  pack format has no lists. `Sys.readPackageAsset(package:, path:)` reads a text file, and `Sys.packageAssetPath(package:, path:)`
  gives the path for a loader that takes one. `rae run` exports each package's folder and declared folders, so **a file the pack
  does not declare is refused on the Mac too**, and not only found missing on a phone. `stage-app-assets.sh`, for iOS and Android,
  and the browser's preload copy declared folders to `packages/<name>/...`, read by the same calls.

**What this design refuses:** a build system in the pack (the script is the package's own), version ranges, and a package's C
reaching into the compiler's `runtime/` folder.

### Which package proves it: `box3d` cannot, yet

The plan named `box3d`, as a candidate. Checked on 2026-11-05, as the `qr` lesson says to: **24 `lib/app3d` modules import
`box3d`**, the walker kit among them, and a stdlib module cannot import a package. It can only move with `app3d` or after it.

| package | C | importers in `lib/` | real users | verdict |
|---|---|---|---|---|
| `sql` | SQLite and a glue header: native, browser, iOS and Android | 0 | 2 servers, case 1288 | **the proof**, done in 0.3.4 |
| `wasm` | WAMR, but its header is in `compiler/runtime/` | 0 | cases only | second; its header must move first |
| `box3d` | Box3D and a glue header | 24 | 3 examples, 19 cases, `physicsScenes` | with the engine |

No small package can prove shaders and assets on real code, checked on 2026-11-05: the only unit with a shader and no importer in
`lib/` is `Gpu`, a general compute API with no `.wgsl` of its own, and `waterSpray` and `sql` have no shader. So cases prove them,
and `trees` is their first real use.

So the order changed, agreed on 2026-11-05: `sql` proves C; `physicsScenes` 1.1.0 proves `embedText`; `trees`, with its shaders and pictures,
proves the rest once `app3d` moves; `box3d` goes with `app3d`.

## 7. Agreed on 2026-11-04

1. **`./rae` in each project.**
2. **A version is required, and a commit is optional** (section 5).
3. **No version ranges. Exact versions only.**

The tool-written `hash` on git dependencies stays, as recommended in section 4. It was asked together
with point 3 and was not rejected. Dropping it later is one field.

## 8. Unchanged from the earlier recommendations

- The manager reads `.raepack` only, and never imports or source. The compiler asks it where a prefix
  `x/` lives and which package owns a file.
- `rae add`, `fetch`, `update`, `tree`, and path and git dependencies stay. `rae toolchain use` is gone.
- `~/.rae` holds `repo/` (one clone), `toolchains/<version>/` or `<version>-<commit>/` (sparse checkouts), `deps/<name>/<pin>/`
  and `projects` (who uses what). `RAE_HOME` moves it.
- `./rae toolchain pin <version|commit>`, `update` (moves only if the project still builds: every target, or
  the pack's optional command, `rae: { check: "..." }`) and `gc` (removes what no recorded project uses; it comes back
  on demand).
- Shared builds are stored **per version**: iOS frameworks, Android, SQLite, Box3D and WAMR, in
  `deps/<name>/<pin>`. The pin is a hash of the files that decide that build, read from the toolchain's own
  commit: the `tools/box3d`, `tools/sqlite` and `tools/wamr` folders (WAMR also its runtime header), and the
  iOS and Android build scripts. So two versions with another Box3D use two folders, and two with the same
  one share a folder. Each toolchain has a `.deps` file with `RAE_BOX3D`, `RAE_SQLITE`, `RAE_WAMR` and their
  `_CACHE` forms, which `./rae` reads, so old compilers and old build scripts use the store too. A variable
  someone already set wins. `tools/ios/frameworks` and `tools/android/deps` are links into the store.
- `./rae toolchain deps box3d` builds one, because these builds need more than a C compiler. Alone, it says
  what is built. The compiler's "is missing" error names the command. `gc` removes a build no kept toolchain
  names. Without `./rae`, a checkout keeps using `~/.cache/rae` and `make setup`.
- Several packs in one repo must name the same compiler version. `pin` changes them all, and a
  disagreement is an error. SUMU has 3 packs and game proto2 has 2. This means project packs, the
  ones with a `rae` block and targets. A library's `rae` is the lowest compiler it needs, and stays.

## 9. Plan

Each step is its own queue item, with the full suite on each step that touches the compiler.

1. A build stops writing `rae.lock`. Done in 0.2.340: `rae fetch` and `rae update` still write it and keep
   its toolchain line, and `rae fetch` now pins a dependency added after the lock was made.
2. Version tags: automatic on each bump, and the old ones added. Done in 0.2.341. `v0.2.321` is exactly
   `930bf77d`, game proto1's commit, so it needs no `commit` in its pin. `v0.2.325` is one commit before
   SUMU's `a34534a1`, `v0.2.330` is just after game proto2's `5898190c`, and `v0.1.91` is three before
   `3937b4d5`. Each still has to build. None of these tags is on the remote until the next
   `git push --follow-tags`.
3. The `.raepack` as the one file: exact versions, the optional `commit`, `hash`, `subdir`, and `rae.lock`
   read once and retired. Done in 0.2.342. A compiler at another version than the pin stops the build,
   unless `RAE_ROOT` is set, which also keeps the projects still building on main working until they
   move. `rev` is an error with the repair.
4. The store, `./rae`, and `toolchain pin`, `status`, `gc`, then `update`. `rae toolchain use` is gone.
   Done in 0.2.344. `make -C compiler toolchain-store-test` runs it from a fresh clone with an empty
   `RAE_HOME`. It takes about two minutes, so it is not part of the full run.
5. Shared builds per version. Done in 0.2.346.
6. The first package: `physicsScenes`, moved to `packages/physicsScenes` (section 6). Done in 0.2.347.

Steps 1-6 together are **Rae 0.3.0**, the start of the package manager. The numbers 0.3.0 to 0.10.0 were used once before, in
September 2026, and abandoned by the reset to 0.1.7. They were never released, so their tags were removed before the first
push, and `tools/version-tags.sh` does not make them again.

The new SUMU moved first (pin 0.2.326). Its `rae-env.sh` went from 362 lines to 77 and `rae-pin.sh` went. What a
real project needed: `rae toolchain` run from a repo root with packs only in subfolders, `rae toolchain path` for
scripts that call `tools/ios` (a script may not export `RAE_ROOT`, which means an override to `./rae`), the UI editor in
the sparse checkout, and a `status` verdict that knows `./rae` builds with the pin. A pack made from a template holds no
pin of its own: the generator copies the block from a tracked pack. A pin before 0.2.340 still writes `rae.lock` on a
build, which is why SUMU moved on to 0.3.0 the same day. A pin before 0.2.349 cannot build its iOS frameworks on Xcode 27.

Game proto1 moved second (pin 0.2.321, the tag on the commit its worktree `rae-pinned` held, now deleted). The C the
compiler emits for it stayed identical but for the toolchain's path, so the move cannot have changed the game. A pin
before 0.2.340 writes an ignored `rae.lock`, and its iOS frameworks must be built on an Xcode before 27 or copied into the
store, until the pin moves.

Game proto2 moved third (pin 0.2.340, both packs). Its old locks named 0.2.329, but its `.env` pointed at the live Rae
checkout, so its code had moved on: it imports a module added in 0.2.337. So a lock that follows a live checkout is not
evidence of what a project builds on: build at the tag before pinning. 0.2.340 is the first version that never writes
`rae.lock`, which is the lowest pin worth choosing when a project can use it.

Both active games then moved to 0.3.0, game proto1 and game proto2: **pin an active project to the current version.** An old
pin only suits a project nobody is working on, which keeps it on old tools and an old `rae.lock`. Game proto1 needed two
small changes, and is the first project outside this repo to use a package: `physicsScenes` by `git`, `subdir`, `version` and
`hash` from GitHub, where a fresh clone fetched it with no local Rae. A project that builds its tests in isolated folders
gives each a pack declaring the copy it fetched.

Game proto3 moved last (pin 0.1.91, for now, as nobody is working on it), which finished the migration: every project now
builds through `./rae`. The commit
in its lock no longer exists after a history rewrite, but the tag `v0.1.91` built it with no change. Its lockstep hash
over 1200 ticks stayed the one its history records, the best evidence a move changed nothing. At this pin `make ios`
needs Xcode 16 or older, because `tools/ios` at 0.1.91 targets iOS 13.

This was the plan:

Then the migration, one project at a time: the new SUMU; game proto1, then delete the `rae-pinned` worktree
and its `make rae-pin`; game proto2; game proto3, starting from `v0.1.91`. Each `scripts/rae-env.sh` shrinks
to what is truly the project's own, or goes. Each project's `.env` line that sets `RAE_ROOT` must go in
the same step, or it keeps overriding the pin without anyone noticing.
