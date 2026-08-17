# Rae monorepo

Rae uses one repository for the language, runtime, standard library, examples,
benchmarks, and development tools.

## Layout

```text
compiler/                 C compiler and runtime
lib/                      Rae standard library and engine libraries
spec/                     language specification
docs/                     design and implementation documentation
examples/                 runnable Rae examples
benchmarks/               reproducible performance suites
tools/devtools-web/       Bun/TypeScript development dashboard
tools/                     other repository tooling
QUEUE.md                  single development queue
```

Run common commands from the repository root:

```sh
./setup.sh                # install Devtools dependencies and build Rae
make build                # build compiler/bin/rae
make test                 # compiler test suite
make devtools-lint        # type-check Devtools Web
make devtools-test        # Devtools runner tests
make dev                  # start Devtools Web
```

## Imported history

The monorepo keeps the original `rae` history as the main line. The complete
histories of `rae-lang-dev` and `rae-devtools-web` were rewritten only to place
their files under collision-free paths, then merged with explicit merge commits.
No source repository was squashed.

Historical branches are retained under:

```text
history/rae/*
history/rae-lang-dev/*
history/rae-devtools-web/*
```

The old meta-repository tag is retained as `VERY_GOOD_FOR_DEMO`. The
old-to-new hashes produced by
`git filter-repo` are recorded in:

- `docs/history/rae-lang-dev-commit-map.txt`
- `docs/history/rae-devtools-web-commit-map.txt`

For example, inspect a rewritten historical commit with:

```sh
git show history/rae-lang-dev/main
git log history/rae-devtools-web/main -- tools/devtools-web
```

The imported meta repository initially lived under `legacy/rae-lang-dev/` so
its complete snapshots could merge without colliding with Rae. Its active
queue, agent configuration, Roundtable state, and orchestration scripts were
then promoted to the monorepo root. Removing the temporary current-tree prefix
does not remove those historical commits from the DAG.

## No submodules

The monorepo has no `.gitmodules` file or gitlink entries. A normal clone is
complete; contributors do not need `git submodule update` or coordinated
submodule-pointer commits.
