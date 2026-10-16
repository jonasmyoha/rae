# 142 — Shadow cascades

The debugging ground for the sun's cascaded shadow maps (`lib/Shadow3d.rae`
fits them, `lib/ShadowMaps.rae` renders them, `lib/deferred_shade.wgsl`
samples them). Use it to see what a cascade fit does before you change one in
an app.

## What you see

With **Cascades** on (the top bar), each cascade has its own colour: red,
green, blue, yellow from the camera out.

- The ground and objects a cascade shadows are tinted in its colour.
- A line in its colour is drawn where the cascade ends. Over the last 12% of
  each cascade's range the shader mixes its shadow with the next cascade's,
  and the tint blends into the next colour the same way, so the change in
  shadow sharpness is a gradient ending at the line, not a step. The last
  cascade fades out to no shadow across its band; past its line nothing is
  tinted and nothing casts.
- Each shadow map's square is outlined in its colour. Shadows outside the
  square are cut off even before the split line.

The orange marker is a 1.8 m character at the camera target. The camera
starts where a game camera follows its player (12.5 m behind, as in 136),
with the walker's fit (24 m, lambda 0.75, 3 cascades, 2048 px, back extent
12 m). Posts stand every metre in rings out to 16 m, every 10 m along the
avenue out to 400 m (a tall red mast every 50 m), and cubes on a 20 m grid.

## Controls

The standard editor panel (`lib/app3d/EditorPanel.rae`):

- drag to orbit, wheel to zoom, the camera bar's **Orbit** button for the free
  WASD camera;
- **Cascades** in the top bar: the debug colours on and off;
- the menu button opens the side panel. Its first section is the fit: each
  button steps one setting (shadow distance 24 / 50 / 100 / 200 / 400 m,
  split lambda 0.5 / 0.75 / 0.9 / 0.95, cascades 2 / 3 / 4, map 1024 / 2048 /
  4096 px, back extent 12 / 50 / 200 m), and the readout shows the split
  distances. Below it are the shared settings: the sun's time of day and date,
  the sky, shadows on and off.

## For agents: headless runs

Every setting has an environment override, and every fit is logged:

```sh
RAE_SHADOW_FAR=200 RAE_SHADOW_COUNT=4 RAE_SHADOW_LAMBDA=0.9 RAE_SHADOW_VIEW=high \
  RAE_SDL_HEADLESS_MS=3000 RAE_GPU2D_SCREENSHOT=/tmp/cascades.bmp \
  compiler/bin/rae run examples/142_shadow_cascades/Main.rae
# [shadow] distance 200 m, lambda 0.9, 4 cascades at 2048 px, back extent 200 m:
#   splits 5.6 / 14.0 / 41.9 / 200.0 m, texel 0.8 / 1.6 / 5.0 / 24.8 cm, casters 770
```

| variable | meaning | default |
|---|---|---|
| `RAE_SHADOW_FAR` | shadow distance (m), the last split | 24 |
| `RAE_SHADOW_LAMBDA` | split scheme: 0 uniform, 1 logarithmic | 0.75 |
| `RAE_SHADOW_COUNT` | cascades, 1-4 | 3 |
| `RAE_SHADOW_RES` | shadow map size (px) | 2048 |
| `RAE_SHADOW_BACK` | how far behind the view the light's box reaches (m) | 12 |
| `RAE_SHADOW_DEBUG` | `0` turns the debug colours off | on |
| `RAE_SHADOW_HOUR` | the sun's hour | 15.5 |
| `RAE_SHADOW_VIEW` | `follow` (game camera), `high` (down the avenue), `low` (eye height) | follow |
| `RAE_SHADOW_PANEL` | `1` opens the side panel | closed |

Any app can show the same colours: `setShadowCascadeDebug(renderer:, on: true)`
(`lib/RendererDeferred.rae`), on the deferred and the forward path.
