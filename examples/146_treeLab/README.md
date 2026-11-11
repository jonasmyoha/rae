# 146 — Tree lab

The lab where trees are made and judged, on the shared editor chrome of the 3D examples
(`lib/app3d/EditorPanel.rae`, as 138 and 142). Its first job was lib/treeGen's T02 tree: the
hand-drawn trace of reference crop T02 turned into a trunk, branches and leaf masses filled with
camera-facing leaf cards (docs/reference-tree-plan.md). It now also shows the tree sets of the
walkers' forests at every distance, through the year, with their grass and weather.

Run it from Rae devtools' Examples, or:

```sh
compiler/bin/rae run examples/146_treeLab/Main.rae
```

## Scenes

The side panel's scene chooser (`LabScenes.rae`). A scene is built the first time it is shown and
kept; a control that changes what it is built from builds it again.

- **Forest** (the default): a forest of the stylized, low-poly or reference tree set at every
  distance (near, middle and impostor levels, out to 1900 m), grass round the camera, rain and
  snow, falling leaves.
- **One tree**: one tree on the lab ground, seen from 20 m. It is drawn as a forest kind, so it
  turns in autumn, loses its leaves and carries snow in winter.
- **Reference T02**: the reference-comparison scene: the traced T02 tree before placeholder trees,
  bushes and clouds, in the lab's look. Its fixed framings are the **T02 near ... T02 bush** camera
  buttons. In an interactive run its leaves follow the date too.
- **Studies**: the meadow flower studies (`LabStudies.rae`).

## Controls

The camera bar at the top: drag to orbit, the wheel to zoom, **Orbit** switches to the free WASD
camera. The "..." menu has the debug readout. The edge tab opens the side panel. Its readout line
shows the tree (in One tree and Reference), the nearest city, the temperature and the season. Its
buttons, each group showing its current choice:

| Group | Choices | Key |
|---|---|---|
| Scene | Forest, One tree, Reference T02, Studies | |
| T02 framing | near, mid, far, bark, row, fork, tracedFork, small, bush | |
| Weather | Auto (the weather model's), Clear, Cloudy, Rain, Snow, Mist | U |
| Wind | Auto, Calm, Breeze, Strong wind, Gale | I |
| Year's speed | still, 1x, 7x, 30x (days a day) | N |
| Latitude | 0, 30, 45, 60 | |
| Look | Lab look, Plain look | |
| Forest set | Stylized, Low-poly, Reference | T |
| Tree kind (One tree) | Previous, Next: the T02 tree, then the stylized set's kinds | |
| Seed (Reference) | Previous, Next | |
| Level drawn | All levels, Near, Middle, Impostor (the T02 kinds honor it) | |
| Forest | on, off; reach 400, 1000, 1900 m; Sparse, Normal, Dense | |
| Grass | on, off; Sparse, Half, Full; blades Short, Medium, Tall and Thin, Medium, Wide; to 20, 45, 80 m; wind none, normal, strong | |
| Grass kind | Meadow grass (the walkers'), Painted grass (game proto1's, in rings to 20, 120 and 300 m); the painted rings on or off. The blade, reach and wind controls are the meadow's; the painted grass keeps its recipe and takes the density | |

Below them are the shared settings of every 3D example: the date and time of day (the lab's
season runs on this date: `,` and `.` step it a day, held they slide), the sky, shadows, TAA and
quality. Y shows the lab's own season slider at the bottom.

## For agents: headless runs

A run that makes pictures (`RAE_SDL_HEADLESS_MS`, `RAE_HEADLESS_FRAMES`, `RAE_GPU2D_SCREENSHOT` or
the dolly) has no editor and starts in the Reference scene unless told otherwise, so its pictures
stay the same from version to version. `RAE_TREELAB_EDITOR=1` keeps the editor in one (and
`RAE_SETTINGS_OPEN=1` opens its panel). `RAE_TREELAB_CONTROLS=d:400,s:1` clicks panel controls (the
ids in `LabEditor.rae` labControlButtons) at frame 20.

| Variable | What it does |
|---|---|
| `RAE_TREELAB_SCENE` | `forest` or `onetree` to start there |
| `RAE_TREELAB_CAMERA` | the Reference framing (`near` ... `bush`), or `high` in the forest |
| `RAE_TREELAB_STUDY` | a study view (`flowers`, `bellflowers`, ...) |
| `RAE_TREELAB_TREE` | `levels` (the T02 tree as a forest kind), `placeholder` |
| `RAE_TREELAB_SEED`, `RAE_TREELAB_ROW` | the traced tree's seed; the grown trees beside it |
| `RAE_TREELAB_SPECIES`, `RAE_TREELAB_SCALE` | One tree's kind and size |
| `RAE_TREE_SET`, `RAE_TREE_LEVEL` | the forest's tree set; one level at every distance |
| `RAE_TREELAB_FOREST`, `_FOREST_FAR`, `_FOREST_SPACING` | the forest off, its reach and spacing (m) |
| `RAE_DAY_OF_YEAR`, `RAE_LATITUDE`, `RAE_YEAR_SPEED` | the date, the place, the year's speed |
| `RAE_TREELAB_WEATHER`, `RAE_WEATHER_WIND` | the weather and the wind at the start |
| `RAE_TREELAB_GRASS` | `meadow` (the default) or `painted` |
| `RAE_TREELAB_LEAVES=off`, `RAE_TREELAB_CLUMPS=off` | the plain leaf masses; the bare wood |
| `RAE_TREELAB_DOLLY=<folder>` | a fixed dolly from 20 to 170 m, one picture a frame |
| `RAE_TREELAB_STATS=1` | a line of what the forest drew and how long frames took |

Scripts:

- `compare.sh <camera>` renders the Reference scene beside T02.
- `seasonCheck.sh` checks that the One tree picture changes from summer to autumn to winter.
- The visual example gate (`RAE_EXAMPLE_FILTER=146_treeLab make -C compiler test-examples`)
  renders the Reference scene, the studies and the forest with each tree set. It also renders the
  editor with its panel open over the Forest and the Reference scene, each built again by a panel
  control.
