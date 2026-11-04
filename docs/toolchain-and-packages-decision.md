# Decision record: toolchain pins, one store, and the package manager

**Status: proposed. Not agreed. Nothing here is built, and no project has moved.**
Section 5 lists the answers that are the owner's to give. The migration in section 6 does not
start until they are given.

This record answers the queue task "toolchain: pins, one shared store, and the package manager".
It builds on docs/versioning-and-toolchain.md and docs/raepack-v2-and-packages.md.

The three games outside this repo are called by neutral names here, because their real names must
not appear in this repository: game proto1 builds with `rae-pinned`, game proto2 has `src` and `server`
packs, and game proto3's lock names a commit that no longer exists.

## 1. What was measured (2026-11-04)

| question | measured |
|---|---|
| Does a normal build change a pin? | Yes. A project pinned to `930bf77d` was built by main's compiler. Its `rae.lock` then said `0.2.339-dev.668+dirty`, `47ba936a`. That is how `+dirty` got into the committed locks of game proto2 and game proto3. |
| When does a build write the lock? | Only when the pack parses and declares a `rae` requirement or dependencies. A pack that does not parse is skipped without a message, so it has no lock check at all. |
| Is the commit in the lock always right? | No. It is baked in when the compiler is built, so it can be older than the checkout: `47ba936a` above, with HEAD at `55a8ac7e`. |
| Can an old compiler be used as it is? | Yes. The real `930bf77d` compiler builds the same project and writes its own commit back. |
| Size of a pin today | 286-369 MB for a full checkout, 428 MB for `rae-pinned`. |
| Size with only what a build needs | 55 MB checked out, 64 MB after the compiler build, using a sparse checkout of `compiler lib tools third_party`. 143 and 106 both emit C from it. |
| Time to make a pin | 14 s from nothing, with the clone from a local repository and the compiler build. |
| Can the git metadata be dropped (`git archive`)? | No. That compiler reports `unknown unknown`, and would write it into locks. The checkout must stay a git checkout. |
| The clone every pin shares | 389 MB, once. |
| Do the pinned commits still exist? | **Not all.** game proto3's `a6c0ae07` is gone from Rae, removed by a history rewrite. The current pins of the new SUMU, game proto1 and game proto2 are on main and pushed. |

How each project picks its Rae today:

| project | env script | Rae it builds with | packs and locks |
|---|---|---|---|
| new SUMU | 362 lines, plus 73 in `rae-pin.sh` | committed pin `a34534a1` | 3 locks in one repo, moved together with `sed` |
| game proto1 | 196 lines | `rae-pinned` at `930bf77d`, named in the uncommitted `.env` | 1 |
| game proto2 | 143 lines | main checkout; the lock says `5898190c` | 2 (`src`, `server`) |
| game proto3 | 142 lines | main checkout; the lock says `a6c0ae07`, which no longer exists | 1 |

None of these has a real git dependency. Every `dep` line is a path inside the project's own repo.

## 2. The six questions

### (a) Inside the compiler, or a separate tool written in Rae?

**Recommendation: inside the compiler, in C, extending what is there.**

- It is already there: `rae_deps.c` (405 lines), the package commands in `main.c` (about 200) and
  `rae toolchain` (about 260). It has no bootstrapping problem.
- Resolution has to run inside every build anyway. Imports are resolved through it, and `sema.c`
  uses it to enforce package boundaries. A separate tool would have to hand its result back to
  the compiler on each build, or the compiler would still have to read the lock itself.
- A build must work with no second program present and no network.

Against it: Rae's rule is "written in Rae whenever it can be". That rule is about the runtime and
the libraries, and AGENTS.md says it does not apply to the compiler's own C. The compiler is C, so
this code joins the language it already depends on. If the compiler is ever written in Rae, this
code moves with it, and nothing here blocks that.

### (b) How much does it need from the parser and semantics?

**Very little, in one direction only.** The manager needs no knowledge of Rae syntax:

- It reads `.raepack`, with `raepack.c` (1,033 lines, using the compiler's lexer), and `rae.lock`.
- It never reads imports or source. It does not need to know what a program uses.
- The compiler asks it two questions: where does the import prefix `x/` live, and which package owns
  this file? Those are the only calls from the compiler into it, apart from the commands and the
  format preflight, which skips dependency files.

So deep syntax knowledge is not the reason to keep it inside. The reason is the first question: the
compiler needs its answers on every build, and it needs no bootstrapping.

### (c) If it were written in Rae: what builds it?

This is the case against a Rae tool, so it is answered in full:

- **Fresh machine:** it needs a Rae compiler first, so the C compiler still has to be built by hand
  or by a script. The tool saves none of that step.
- **Which Rae builds it:** either the project's pin, so the tool differs per project, and old pins
  cannot build a tool newer than they are. Or a fixed Rae, which is a second pin to manage, for the
  thing that manages pins.
- **Shipping:** a binary per platform, or compiled on first use. A small Rae program takes about
  0.2 s with the runtime cached, so speed is not the problem. The version tangle is.

Nothing here needs a tool in Rae today. **Recommendation: do not write one.**

### (d) One system, one lockfile, one store, one resolver?

**One lockfile and one store root: yes. One resolver: no, because they answer different things.**

- `rae.lock` keeps both, as it does now. A separate pin file would solve only the case of several
  packs in one repo, which section 3.3 solves another way.
- A toolchain is chosen before any compiler runs, by exact commit. Dependencies are resolved by the
  chosen compiler. A shared resolver would have the launcher depending on the format of whichever
  compiler is pinned.
- So the launcher reads and writes the `toolchain` line only. Everything else in the file belongs to
  the pinned compiler.

### (e) What happens to what exists?

**Extend it. Replace nothing except `rae toolchain use`.**

- Keep `rae add`, `fetch`, `update`, `tree`, path and git dependencies, and `.rae/deps/<name>`.
- Keep the pack's `rae: { version }`. It says what a library needs, which a pin cannot say. A pin is
  the exact commit for one project.
- **Remove `rae toolchain use`.** It moves the one shared checkout, which is what this task ends.
  Its error message currently tells people to run it, so that text changes too.
- **Do not build a shared store for git dependencies yet.** No project has one. Add it when one does.

### (f) Is a registry still out of scope?

**Yes, still out.** Several projects can share a package by git URL and commit today, and none does.
A registry adds hosting, accounts, name ownership and withdrawing bad releases.

Reconsider it when any of these becomes true: people outside these repositories publish packages;
one project needs two packages that need different versions of a third; or finding a package by
name is a real problem. The dependency rule stays as it is until then: one version per name in a
build, and a conflict is an error.

## 3. The toolchain design

### 3.1 The launcher

The proposal does not say which `rae` a person types. Today none is on PATH, and each project finds
its own path, which is why each project has a script.

- `~/.rae/bin/rae` is on PATH. It is an ordinary compiler binary, taken from a store entry.
- At the start of a command it finds the nearest `rae.lock` going up from the project. If that lock
  names a different commit, it makes that toolchain ready if needed and runs the same command with
  that compiler, passing every argument on.
- `rae toolchain ...` always runs in the launcher. Everything else runs in the pinned compiler.
- A pinned compiler can be old and know nothing of this, and that is fine. It only receives a command
  it already understands. The launcher sets an environment variable so a newer pinned compiler does
  not dispatch again.
- It points at a store entry, not at the Rae checkout, so work in progress on Rae cannot break every
  project. `rae toolchain self-update` moves it.
- A project with no pin uses the launcher's own compiler, as zero-config projects do now.

### 3.2 The store

```
~/.rae/                       (RAE_HOME to move it)
  bin/rae                     the launcher
  repo/                       one clone, with no worktree in the Rae checkout
  toolchains/<commit>/        a sparse checkout, compiler built, a ready marker
  deps/<name>/<pin>/          iOS frameworks, Android, SQLite, Box3D, WAMR
  projects                    each project that used a pin: its path and last use
```

- **Where:** `~/.rae`, as the older doc named it, not `~/.cache`. The launcher lives here, and a
  cache may be deleted at any time.
- **Size:** sparse checkouts, so a pin is about 64 MB rather than 370 MB. With five pins: 389 + 320 MB
  against about 1.8 GB today.
- **Two runs at once:** the first one prepares, the other waits on a lock, as SUMU's script does.
- **A failed build leaves nothing:** a commit whose compiler does not build gets no ready marker.

### 3.3 The lock

- **A build never changes the `toolchain` line.** It may change `dep` lines. Only `rae toolchain pin`
  and `update` move the pin. This is the fix for rule (5).
- **The line holds the full commit hash**, and the version without `+dirty`. Short hashes are still
  read, by prefix.
- **Several packs in one repo:** `pin` and `update` change every tracked `rae.lock` in the git repo,
  as SUMU's `sed` does now, so they cannot drift apart. Building with locks that disagree is an error
  that names them.
- **An old pinned compiler still rewrites the lock on build.** It writes its own commit, which is the
  pin, so the pin does not move. The file's only change is the longer or shorter hash until that
  project moves past this change. This was tested with `930bf77d`.

### 3.4 The commands

| command | what it does |
|---|---|
| `rae toolchain pin <commit\|main>` | Makes the toolchain ready, or refuses and leaves the pin. Writes the locks. Warns when the commit is not on the remote, because another machine could not get it. |
| `rae toolchain update` | Tries main in a copy of the check below. Moves the pin only when it passes, and otherwise prints what failed. |
| `rae toolchain gc` | Removes toolchains that no project in `projects` pins now. `--dry-run` lists them. A removed toolchain comes back on demand, so an old branch is slower but never broken. |
| `rae toolchain status` | The pin, the compiler in use, and why. It replaces each project's `make doctor` logic. |

"Still builds" for `update` means: `rae build --emit-c` and the C compile of every target in the
pack, or the command in the pack's optional `check` line when it has one (`make check` for SUMU).

### 3.5 Working on Rae and a project together

`RAE_ROOT=<checkout> rae build ...` uses that checkout's compiler for one command. It prints one line
saying so, and it does not change the pin, since no build does. game proto1's and game proto2's `.env`
lines go away.

### 3.6 Heavy shared builds

The proposal shares the iOS frameworks and SQLite. The same applies to Android's SDL and wgpu, Box3D
and WAMR, which it does not list.

They must be shared **per version, not one per machine**. Today `~/.cache/rae/box3d` holds one Box3D,
and gen-ios fails when it is not at the checkout's pin, so two pins with different Box3D commits
cannot both build. SUMU's shared `frameworks/` has no version at all, so a Rae that moves SDL would
silently link the old one. The folder name comes from the pin file, or from a hash of the build
script for those with no pin file.

Old toolchains read the old paths. The launcher sets `RAE_BOX3D`, `RAE_SQLITE_CACHE` and `RAE_WAMR`
before running them, which all exist already. The iOS and Android folders are symlinked into the
toolchain's `tools/`, as SUMU does.

## 4. What changed from the proposal

1. **Added:** which `rae` runs (3.1). The proposal needs this and does not name it.
2. **Added:** a pin is only as safe as Rae's history (question 3 below). This has already happened once.
3. **Changed:** a pin is 64 MB rather than 370 MB, using a sparse checkout.
4. **Changed:** shared builds are stored per version, and include Android, Box3D and WAMR.
5. **Added:** several packs in one repo (3.3). SUMU has 3, game proto2 has 2.
6. **Changed:** `gc` needs a record of projects, because a store cannot otherwise know what is used.
7. **Removed:** the idea of keeping the newest three pins, which can delete a pin in use.

## 5. Questions for the owner

Each has a recommended answer. "Agreed" to all of them is enough to start.

1. **(a) Inside the compiler, in C?** The current thinking allowed either. Recommended: inside, per 2(a).
2. **(f) Registry stays out?** Recommended: yes, until a trigger in 2(f) happens. Is one of those
   already planned, such as people outside these repositories using Rae?
3. **History rewrites and pins.** A commit pin dies when Rae's history is rewritten, as game proto3's did.
   Three options:
   - **Keep a ref:** `pin` pushes `refs/pins/<commit>` to the remote. A rewrite must then keep those
     refs, though a rewrite done to remove a term from history would have to delete them. Then a pin
     breaks with a clear error, and its project has to move.
   - **Accept it:** no extra ref; after a rewrite, the store clone still has the old commits on this
     machine, and other machines get an error.
   - **Recommended: keep a ref**, because it needs no action except during a rewrite.
4. **Pins on unpushed commits.** Rae's rule is not to push without being told, and main is 2 commits
   ahead now. A pin to an unpushed commit works on this machine only. Recommended: warn, and allow it.
5. **The launcher on PATH.** This adds an install step and an entry on PATH on each machine. Recommended:
   yes. `make -C compiler install` makes the first store entry and prints the PATH line.
6. **Source of a Rae commit.** Recommended: the remote, plus the local checkout that the launcher was
   installed from, so new local commits can be pinned, as SUMU can do now.
7. **game proto3's pin.** Its commit is gone. Its catch-up commit is dated 2026-09-30 10:09, and the last
   commit on main before that is `3937b4d5`, at version 0.1.91 as its lock says. It has not been built. Recommended:
   try that one, and if it does not build, pin game proto3 to main and fix it. Or is it OK to bring
   game proto3 to current main directly?

## 6. Plan, after agreement

Each step is a queue item of its own, added once this is agreed, with the full suite on the last one.

1. **Stop a build from moving a pin.** The `toolchain` line is left alone, and it holds the full hash.
   This is worth doing first, because it stops the damage today.
2. **The store and `pin`, `status`, `gc`.** Sparse checkouts, the ready marker, the lock on preparing,
   and every tracked lock in the repo.
3. **The launcher and `RAE_ROOT`.** Dispatch, the install step and `self-update`. Remove
   `rae toolchain use`.
4. **`update`,** with the pack's optional `check` line.
5. **Shared builds per version** (3.6).

Then the migration, in the order the task gives. Each project's `scripts/rae-env.sh` shrinks to what is
truly its own, or goes:

1. the new SUMU, at its `a34534a1` or whatever it is then;
2. game proto1 at `930bf77d`, then delete `~/personal/rae/rae-pinned` and its `make rae-pin`;
3. game proto2 at `5898190c`, or newer if it has moved;
4. game proto3, per question 7.

**Risk found for the migration:** the `RAE_ROOT` in game proto1's and game proto2's `.env` has to go at
the same step as its pin arrives, or that `.env` line keeps overriding the pin without anyone noticing.
