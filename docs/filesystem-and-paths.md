# Filesystem & paths (Rae standard library)

Status: **decision record / design**, 2026-06-26. Companion to
`tech-stack-and-dependencies.md` and `module-namespacing.md`.

The immediate trigger: the raytracer PNG save writes a fixed `rae_render.png` to
the working directory, overwriting every time and landing wherever the app was
launched from. The fix needs cross-platform path discovery (Desktop folder) and
directory listing — which is also a general stdlib gap.

---

## Backend decision: wrap SDL3, own only the path-string helpers

Use **SDL3's filesystem API** (`SDL_filesystem.h`, already linked for windowed
apps) behind a Rae-owned seam. SDL3 has the full surface, cross-platform incl.
Android/iOS:

- Known folders: `SDL_GetUserFolder(SDL_Folder)` — `DESKTOP`, `PICTURES`,
  `SCREENSHOTS`, `DOCUMENTS`, `HOME`, … (NULL where the platform has none, e.g.
  Desktop on mobile). Thread-safe, **no `SDL_Init` required** (verified in header).
- `SDL_GetBasePath` (app/asset dir), `SDL_GetPrefPath(org, app)` (writable,
  sandbox-correct per-app dir).
- Listing/detection: `SDL_EnumerateDirectory`, `SDL_GlobDirectory`,
  `SDL_GetPathInfo` (type none/file/dir/other, size, mod-time).
- Mutations: `SDL_CreateDirectory` (mkdir-p), `SDL_RemovePath`, `SDL_RenamePath`,
  `SDL_CopyFile`.

Rationale (per the dependency philosophy): known-folders + mobile-sandbox storage
are a genuine per-platform minefield — textbook "buy, don't build." SDL3 is
already our platform layer and solves it everywhere. We **own only** the pure
path-string helpers (`join`, `baseName`, `dirName`, `ext`) — small, bounded, no
platform calls, good dogfooding. The seam keeps it reversible: a POSIX+Win32
runtime fallback for non-SDL CLI tools can slot behind the same API later if needed.

Implemented in an `RAE_HAS_SDL3` runtime block, so `import filesystem` requires an
SDL3-linked app (fine for the raytracers; the watch/`.deps` plumbing already links
SDL3 on import).

---

## API: `lib/filesystem.rae` (namespaced — see module-namespacing.md)

Functions are defined **unprefixed**; callers use the `filesystem.` qualifier.

```rae
# Detection / info
func exists(path: view String) ret Bool
func isDir(path: view String) ret Bool
func isFile(path: view String) ret Bool
func size(path: view String) ret Int          # bytes, -1 if missing
func modTime(path: view String) ret Int        # unix seconds, -1 if missing

# Listing (names only, non-recursive)
func list(path: view String) ret List(String)
func glob(path: view String, pattern: view String) ret List(String)

# Mutations
func makeDir(path: view String) ret Bool       # mkdir -p
func remove(path: view String) ret Bool
func rename(from: view String, to: view String) ret Bool

# Known folders ("" when the platform has none)
func desktopDir() ret String
func picturesDir() ret String
func documentsDir() ret String
func homeDir() ret String
func prefDir(org: view String, app: view String) ret String   # writable, sandbox-correct
func appDir() ret String                                       # base/asset path

# Pure path helpers (implemented in Rae, no platform dep)
func join(a: view String, b: view String) ret String
func baseName(path: view String) ret String
func dirName(path: view String) ret String
func ext(path: view String) ret String
```

Usage: `filesystem.desktopDir()`, `filesystem.exists(path)`, `path.filesystem.exists()`
once namespace-qualified UFCS lands.

---

## Locked decisions (PNG save policy)

1. **Desktop, directly** (no subfolder) — this is a demo capture.
2. **Fallback** when there is no Desktop (mobile/headless): the writable sandbox
   dir, `filesystem.prefDir("Rae", "render")` + `/render` — *not* cwd (which can
   be read-only on mobile).
3. **Filename:** ISO date + per-day sequential — `rae_render_2026-06-26_0001.png`.
   The sequential index is `1 + max` of the existing `rae_render_<today>_*` files
   in the target dir (found via `filesystem.glob`), so F2 never overwrites and the
   scan dogfoods the listing API. Needs a small runtime date helper (`strftime`).

A helper (in `lib/filesystem.rae` or `lib/image.rae`) packages the policy:

```
renderTarget(stem):
  dir = filesystem.desktopDir()
  if dir == "": dir = filesystem.join(filesystem.prefDir("Rae","render"), "render")
  filesystem.makeDir(dir)
  n = 1 + maxExistingIndex(dir, stem, today)
  ret filesystem.join(dir, stem + "_" + today + "_" + pad4(n) + ".png")
```

---

## Ordering

`lib/filesystem.rae` is namespaced from day one, so it lands **after** module
namespacing (Option A) is in place — see `module-namespacing.md`. Then rewire the
raytracer F2 save (currently `imageSavePng(path: "rae_render.png", …)`) to
`imageSavePng(path: filesystem-renderTarget…, …)`.

## Verify before committing

- `SDL_GetUserFolder(SDL_FOLDER_DESKTOP)` returns a sane path on macOS in our
  setup (headers say no init needed; smoke-test the C call).
- `SDL_GlobDirectory` pattern semantics + UTF-8 on each platform.
