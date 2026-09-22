# macOS app icons — should the compiler generate one? (#40180143, question only)

**Question.** A text-mode CLI needs no icon. But if the app opens a UI window, the
compiler should give it a decent default icon: look in the project's assets folder,
and if there is no icon there, generate one (macOS only) showing the app's name,
with macOS corner roundness and good antialiasing. Opt out with a parameter.
`.png` or `.jpg`?

**Short answer.** Yes, and it is cheaper than it sounds — the needed system
frameworks are *already linked*. But there is a prerequisite the idea trips over:
**Rae does not build a macOS `.app` bundle today**, so right now there is nowhere
for an icon to live. That fork has to be decided first. Everything below was
verified by prototype on this machine (2026-09-22), not assumed.

---

## 1. The blocker: there is no macOS bundle

`rae build` / `rae run` produce a **bare Unix executable** (`gcc -o app out.c …`).
Only iOS has bundle machinery (`tools/ios/gen-ios.sh` + `tools/ios/Info.plist.in`,
#520) — there is no macOS equivalent, no `Info.plist`, no `Contents/Resources`.

A bare executable has **no icon slot at all**. So there are two different features
hiding behind one question:

| | how the icon is set | works without a bundle? | icon visible where |
|---|---|---|---|
| **(a) Runtime Dock icon** | `NSApp.applicationIconImage = <NSImage>` at startup | **yes** | Dock + ⌘-Tab, while running |
| **(b) Real app icon** | `.app` bundle: `CFBundleIconFile` → `.icns` | no — needs a bundle | Finder, Dock, everywhere, always |

**(a) is small and lands today.** One ObjC call in the SDL3 startup path, fed a
generated PNG. It gives exactly what the question asks for — a windowed Rae app
stops showing the generic terminal-executable icon in the Dock.

**(b) is the real feature** (a double-clickable, shippable `.app`), and it is a
bigger piece of work than the icon: bundle layout, `Info.plist`, asset copying,
rpath fixups for `libSDL3`/`libwgpu_native`, and eventually codesigning. The icon
is then a five-line detail of it.

**Recommendation: do (a) now, and treat (b) as its own task.** Do not let "nice
icon" smuggle in a bundle builder.

## 2. The trigger — "only if a window is created"

The compiler already computes this at build time: `main.c` sets `uses_sdl3` by
scanning the module graph for `Sdl3`, `Gpu2d`, `Gpu3d` … **but also `Filesystem`**,
because both live in the same `RAE_HAS_SDL3` runtime block.

So `uses_sdl3` is *not* the signal wanted here — a text-mode CLI that merely reads
a file would get an app icon, which is precisely what the question rules out. The
icon trigger must be the window owners only (`Gpu2d` / `Gpu3d` / `Sdl3`), a
separate flag from the link flag. Cheap, but it is a real distinction and easy to
get wrong by reusing `uses_sdl3`.

## 3. Format: `.icns`, or `.png` — never `.jpg`

**`.jpg` is out.** JPEG has no alpha channel, and the corners outside the squircle
must be transparent. A JPEG icon is a coloured square with visible corner
triangles. "Temporary" does not change that.

Measured, 1024×1024, drawn with CoreGraphics:

| artefact | size | note |
|---|---|---|
| `.icns`, full 16→1024 ladder | **19 KB** | the actual macOS icon format |
| `.png`, flat fill | **31 KB** | for the runtime `NSImage` path |
| `.png`, vertical gradient | 180 KB | the gradient is what costs |

So "can we compress it small" — yes, comfortably. The `.icns` with *every* size is
smaller than one flat PNG.

**Gotcha, verified:** ImageIO writes `.icns` directly
(`CGImageDestinationCreateWithURL(url, CFSTR("com.apple.icns"), n, NULL)`) — no
`iconutil` shell-out — **but `Finalize` fails with a single image**. It succeeds
only when the whole size ladder (16/32/64/128/256/512/1024) is added. Render each
size natively rather than downsampling one master; that is also what makes the
small sizes crisp.

## 4. Dependencies: already paid for

`compiler/Makefile:8` already links `-framework Foundation -framework ImageIO
-framework CoreGraphics`, and every built app links the same. So:

- **drawing + antialiasing** — CoreGraphics: already there.
- **PNG / ICNS encoding** — ImageIO: already there.
- **text** — CoreText: **the one addition**, `-framework CoreText`.

No third-party code, no bundled font, no new asset. This is why the feature is
cheap on macOS and would be a different proposition on Linux/Windows (where it
would need a rasteriser, a font, and a PNG encoder — recommend not doing it there).

## 5. Shape and antialiasing

Apple's Big Sur+ grid: **1024 canvas, 824 body** (100 px margin), corner radius
**≈ 0.225 × body ≈ 185 px**. Getting these proportions right is most of what makes
an icon look native.

One honest caveat: `CGPathCreateWithRoundedRect` draws a **circular-arc** rounded
rect, not Apple's **continuous** squircle. At icon sizes the difference is very
hard to see and most third-party icon tools ship the arc version — but it is not
pixel-identical to a system icon. A true continuous corner needs a hand-rolled
superellipse path (AppKit does not expose `.continuous`; SwiftUI/UIKit do).

Antialiasing is free (`CGContextSetShouldAntialias`) and looked clean at every size.

## 6. The text: initials, not the app name

This is the finding that changes the design. Rendering the full name works at large
sizes and **fails completely at small ones** — "Rae UI Editor" at 32 px (the Dock's
small size, Finder list view) is an unreadable smudge. A short name is fine.

So the default should draw **initials or the first word — roughly ≤4 characters**
(`Rae UI Editor` → `RUE`), auto-fitted, not the full `name:` string. A long name
should not silently shrink to mush.

## 7. Where the file goes ("no images in stupid places")

- **Look for a user icon first**: `assets/icon.png` (and `assets/icon.icns`) in the
  project dir. If present, use it and generate nothing, ever.
- **Generated art goes in the build dir** — `.rae/apps/<app>/` (already gitignored),
  never into the user's source tree. A compiler that writes a PNG into someone's
  repo on every build is the "stupid place" failure mode; it would show up in
  `git status`, get committed by accident, and drift from the app name.
- Regenerate when the name changes; it is ~10 ms and the output is deterministic.

## 8. Opt-out

`.raepack` already carries `name:` and a `display: { width height }` block, so the
icon setting belongs there, consistent with the existing schema:

```
display: {
  width: 1280
  height: 800
  icon: none              # never generate, never set (the opt-out)
  # icon: "assets/icon.png"   # or point at your own
}
```

Default (key absent) = generate for windowed apps only. An env override
(`RAE_APP_ICON=off`) is worth having for CI/screenshot runs, matching
`RAE_FORMAT` / `RAE_SHADER_VALIDATE`.

## 9. Prototype

Verified end to end on macOS: CoreGraphics + CoreText + ImageIO, ~120 lines of C,
producing the squircle, the gradient, auto-fitted centred text, and both a PNG and
a valid multi-size `.icns` (`file` reports `Mac OS X icon, 18997 bytes`). The two
renders below were checked by eye at 256 px (clean) and 32 px (where the long name
failed, per §6).

## Suggested split, if this is taken up

1. **Icon generator** — the C above, behind the precise windowed-app trigger (§2),
   writing `.png` + `.icns` into the build dir, `icon: none` opt-out. Lands alone
   and is testable headlessly (generate, assert the files exist and `.icns` parses).
2. **Runtime Dock icon** — set `NSApp.applicationIconImage` from the generated PNG
   in SDL3 startup. Small, and gives the visible win.
3. **macOS `.app` bundle** — the separate, larger feature; the icon then becomes
   `CFBundleIconFile`.
