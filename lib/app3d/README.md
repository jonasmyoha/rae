# `lib/app3d` — shared building blocks for 3D apps

Everything here was extracted from an example that already had a working
version, rather than designed up front. The test of each module is that
the example it came from still passes its gate afterwards, and that a
second example uses it without copying anything.

| module | what it owns |
| --- | --- |
| `camera_rig.rae` | orbit + WASD-fly camera state, input, clamps, snapshot |
| `ui_shell.rae` | window extent, fonts + theme, world/scene bootstrap, named-node access |
| `control_panel.rae` | a panel an app fills with its own buttons; radio groups |
| `camera_bar.rae` + `scenes/camera_bar.raescene` | the camera readout bar |

## Using a shared component

A component owns its CHROME (a `.raescene` under `scenes/`) and the app
owns WHERE it goes. The app authors a host node in its own scene, then
mounts:

```
mountCameraBar(world: uiWorld, res: gpuUi, hostNodeId: "CameraBarSlot")
```

The host keeps its own placement components (`Rect` / `Size` / `Align` /
`Offset`); everything else comes from the shared scene. Examples 110 and
113 mount the same bar into differently-placed slots.

## The theme contract

Shared components never name a colour. They paint through palette SLOTS
and text STYLE IDs that the app's theme must define:

* palette slots — `surface`, `surfaceAlt`, `accent`
* text styles — `panelButton`, `muted`

The palette vocabulary is CLOSED. `lib/ui/theme.rae` resolves ten fixed
names (`background`, `surface`, `surfaceAlt`, `textPrimary`, `textMuted`,
`outline`, `accent`, `accentText`, `accentTextMuted`, `imageTint`) and
paints anything else magenta. A theme may re-colour a slot; it may not
invent one.

Load the theme through `loadUiAssets`, which activates BOTH the token
tables and the palette. `setActiveThemeTokens` alone covers space, radius,
padding and container tokens only — a theme loaded without the palette
bridge is honoured for four of its five sections and silently renders in
the built-in light palette.

## Asset paths

`scenes/` resolves relative to the working directory, which for an
in-tree example is the repository root. An app that relocates these files
sets `RAE_APP3D_SCENES` instead of forking the module.

## Adding a component

1. Author `scenes/<name>.raescene` with a `Layout`, so it flows its own
   children instead of needing per-app coordinates.
2. Paint from palette slots and style ids, never literals.
3. Expose `mount…`, `sync…` and `handle…` — mount once, push state per
   frame, and return whether an action fired.
4. Migrate the example it came from. Until that happens it is a copy,
   not a shared component.
