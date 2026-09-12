# Compiler versioning, toolchain pinning, and the road to a package manager

**Status:** design, agreed direction. §1 (#930), §2 (#931), §3 (#932) and the
resolution half of §4 (#933) are implemented: `compiler/VERSION`,
`rae --version`/`-v`/`--json`, the bump discipline, the `v0.1.0` tag; the
`rae: { version }` pack requirement, the run/build/watch toolchain check
(before the format preflight), `--check-toolchain`, `RAE_TOOLCHAIN_CHECK=off`,
the `rae init` requirement; `rae toolchain status`/`list`/`use`; and the pack's
`dependencies: { dep x: { path } | { git, rev } }` resolved into the module
search (a dep named `x` owns the `import x/...` prefix), git deps cloned into
`.rae/deps/<name>` and pinned by the generated `rae.lock` (`toolchain` block +
`dep` records with commit and `git-tree` content hash — the lock's commit wins
over the pack's `rev`, so a build is reproducible from the lockfile alone).
The stdlib is the lock's first `dep lib` record, resolved through the
toolchain rather than a project-local `lib` symlink. Only the §4 CLI
(#934: `rae add`/`fetch`/`update`/`tree`) remains. Extends
`docs/raepack-v2-and-packages.md` §12 (dependencies + `rae.lock`), which
already covers *package* versions and dependency pins but says nothing about
the **compiler's own version** — the gap this doc closes.

## 0. The problem

Rae has no version. `rae --version` is an unknown command, there is no
`VERSION` file and no release tag. A downstream project pins Rae by **git
commit** in a hand-rolled `rae.lock` written by its own Makefile, and on a
second machine the only diagnostic is "the pinned commit differs from your
checkout". That worked for one author on one machine. It breaks the moment:

- Rae's `main` moves 50 commits in a day with several signature changes (the
  shared-renderer slices #869–#925 did exactly this), so "HEAD" is not a
  version anyone can name;
- a second person clones both repos and has no way to know WHICH Rae the
  project builds against except by reading a commit hash out of a text file
  and checking it out by hand;
- the compiler is co-developed with the project, so a strict pin must be
  overridable for the person doing both.

Rust solves this with `rustc --version`, `rust-toolchain.toml` (what the
project wants) and `rustup` (get it). Rae wants the same three pieces, sized
for a language whose toolchain is a sibling git checkout, not a download.

## 1. The compiler has a version (#930)

- **Semantic versioning, pre-1.0.** `MAJOR.MINOR.PATCH`, starting at `0.1.0`.
  While `0.x`: a **MINOR** bump means a breaking change to the language, the
  compiler CLI or `lib/` (a function signature moved, a keyword changed, a
  module renamed); a **PATCH** bump is compatible (fixes, additions, new
  diagnostics). `1.0.0` is deferred until the language stops moving.
- **One source of truth: `compiler/VERSION`** — a single line, `0.1.0`. The
  build bakes it into the binary together with `git describe`, so:
  - on the tagged commit: `rae --version` → `rae 0.1.0 (a1b2c3d 2026-09-13)`
  - between tags: `rae 0.2.0-dev.14 (a1b2c3d 2026-09-13)` — the NEXT version
    with `-dev.N`, N commits since the last tag, like `git describe`;
  - a dirty tree appends `+dirty`.
  `rae --version --json` prints `{ version, tag, commit, date, dirty }` for
  tooling.
- **A release is a tag.** `git tag -a v0.1.0` on the commit that bumps
  `compiler/VERSION`; nothing else. No branches, no artifacts yet: the toolchain
  is a source checkout, so a tag IS the release.
- **Bump discipline** goes into `AGENTS.md`: a commit that changes a `lib/`
  signature or CLI behaviour bumps MINOR in the same commit and says so in the
  message; the queue's `[compiler]`/`[lib]` tasks state which bump they carry.
  The first tag is cut once the current in-flight renderer slices land.

## 2. The project says which Rae it needs (#931)

The `.raepack` (v2 `meta`, per the packages doc) gains a **toolchain
requirement**, the analogue of `rust-version` in `Cargo.toml` and of
`rust-toolchain.toml`:

```rae
pack Studio {
  format: "raepack"
  version: 2
  meta: { name: "Studio", version: "0.4.0" }
  rae: { version: "0.3" }     // "0.3" = >=0.3.0 <0.4.0 (caret, the cargo default)
}
```

- **Requirement grammar:** a bare version is a caret requirement (`"0.3"` and
  `"0.3.1"` both mean `>=0.3.1 <0.4.0` on the patch given, since MINOR is the
  breaking axis pre-1.0); explicit `">=0.3.0 <0.5.0"` for anything else. No
  wildcards beyond that; keep it small.
- **Enforced by the compiler, at the start of `rae run` / `build` / `watch`**,
  BEFORE the format preflight: it reads the project's pack, compares its own
  version, and on mismatch fails with both numbers and the repair:

  ```
  error: this project requires Rae 0.3 (from studio.raepack); this compiler is
         0.2.0-dev.14 (a1b2c3d). Run:  rae toolchain use 0.3
         or set RAE_TOOLCHAIN_CHECK=off to build against this compiler anyway.
  ```
- **`-dev` builds satisfy the NEXT version** (`0.3.0-dev.7` satisfies `"0.3"`)
  so that developing Rae and the project together on `main` does not trip the
  check every commit; `RAE_TOOLCHAIN_CHECK=off` is the explicit override for
  everything else, and `--check-toolchain` the strict form for CI-style runs.
- **Zero-config projects (no pack) skip the check** — the packages doc's
  principle 6 stands. `rae init` writes the requirement at the current version.
- The check is a **version** comparison, not a commit comparison. A commit
  hash in a project's `rae.lock` stays useful as the exact record of what was
  last verified (#933 makes the lockfile carry it officially), but the
  human-facing contract is the version.

## 3. Getting that Rae onto a machine (#932)

`rustup` is the piece that makes the requirement actionable. For Rae the
toolchain is a sibling git checkout, so the tool is small:

- **`rae toolchain status`** — prints this compiler's version, the checkout it
  came from, and (inside a project) the pack's requirement with a
  satisfied/unsatisfied verdict. This is what a project's `make doctor` should
  call rather than re-deriving the answer with `awk`.
- **`rae toolchain use <req>`** — resolves the requirement to the newest
  matching tag (`git fetch --tags` in the checkout it was built from, or
  `$RAE_ROOT`), checks it out, and rebuilds the compiler (`make -C compiler
  build`). Refuses when the checkout is dirty, so nobody's in-flight work is
  stashed silently. `rae toolchain use main` returns to the development head.
- **`rae toolchain list`** — the tags, marking the current one.

Multiple side-by-side toolchains (rustup's real trick) are explicitly out of
scope: one checkout, moved between tags, is enough while every user is also
a Rae developer. If that stops being true, the next step is a per-version
`~/.rae/toolchains/<ver>` layout, not a rewrite.

**On the second machine** the loop becomes: `git pull` in the project →
`make doctor` says *requires 0.3, have 0.2.0* → `rae toolchain use 0.3` (or
the project's `make rae-use` wrapper) → build. The person maintaining both
repos keeps `$RAE_ROOT` on `main` and relies on the `-dev` rule plus the
override.

## 4. Dependencies and the lockfile — the package manager core (#933, #934)

Everything above is about the *compiler*. The packages doc §12 already
designs the *package* side; this section only sequences it and fixes the
lockfile's shape so #931's toolchain record and #933's dependency records
live in one file.

- **`rae.lock` v2** — generated by the compiler, committed by the project:

  ```
  # generated by rae 0.3.0 — do not edit
  toolchain { version: "0.3.0" commit: "a1b2c3d…" }
  dep ui    { source: "path:../lib-ui" }
  dep codec { source: "git:https://…/rae-codec" rev: "v1.2.0" commit: "…" hash: "sha256:…" }
  ```

  The project's existing hand-written `rae.lock` (commit + subject) is the
  `toolchain` block by another name; #931 teaches `rae` to write that block and
  the Makefile wrapper retires.
- **#933 — resolution.** `dependencies: { dep x: { path | git, rev } }` in the
  pack, resolved into the compiler's module search (`resolve_stdlib_root` /
  `try_resolve_lib_module` is the hook, as §12 says), pinned into `rae.lock`
  with commit + content hash, and reproducible from the lockfile alone. The
  standard library becomes the first dependency: a project stops needing a
  `lib` symlink into the Rae checkout because `lib/` resolves through the
  toolchain the lockfile names.
- **#934 — the CLI.** `rae add <name> --path|--git` (edits the pack), `rae
  fetch` (populate from the lockfile, offline-safe), `rae update [<name>]`
  (re-resolve within the pack's requirement, rewrite the lock), `rae tree`.
  Git deps are cloned into `.rae/deps/<name>` (already gitignored).
- **A registry is deliberately later** (§15 of the packages doc). Path + git
  cover every current consumer — the sibling game projects, `rae_ui`,
  `game-proto1` — and a registry adds hosting, auth and yanking, none of
  which anyone needs yet.

## 5. What this replaces in a downstream project

Today a project carries: `scripts/rae-env.sh` (find the checkout, read a
commit out of `rae.lock`, compare, print), `make rae-lock` (rewrite that
file), a `lib` symlink, and prose in its agent guide about all three. After
#930–#933 the project carries a `.raepack` with `rae: { version }` and a
generated `rae.lock`; `make doctor` becomes `rae toolchain status`, `make
rae-lock` becomes `rae update`, and the symlink goes. The Makefile keeps only
`RAE_ROOT` discovery until `rae toolchain` owns that too.
