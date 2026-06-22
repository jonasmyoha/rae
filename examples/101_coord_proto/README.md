# 101 — Coordinate-unit prototype

A focused experiment for the frames architecture
(`rae/docs/ui-coordinate-and-responsive-layout.md`). It exists to answer the
**one open coordinate question** before we lock the design unit: are high-res
design-unit numbers sane when the *same* `200 du` button and a fixed
`1080×1600 du` board appear in both a phone frame (`1080×2280 du`) and a
desktop frame (`5760×3240 du`)?

This deliberately does **not** use the ECS/frames runtime — it isolates the
unit feel only.

## Run

```bash
# from the rae/ checkout
make -C examples/101_coord_proto run
```

## Controls

- **SPACE** — toggle phone ⇄ desktop reference frame
- **UP / DOWN** — raise / lower `unitScale` (design units per logical point) live

## What to judge

- Watch the **`200du = N pt`** readout — that is the button's physical size.
  At `unitScale = 3`, `200 du = 66 pt` (a chunky primary button). Tweak
  `unitScale` with UP/DOWN and decide what physical size feels right.
- Note that the **same `200 du` button is one consistent size** conceptually,
  but occupies **18.5 % of the phone frame width vs 3.5 % of the desktop
  frame** — the design's core claim ("one unit, area grows, desktop is not a
  big phone").
- `unitScale` is intentionally **soft** (a `let`, not hard-coded `3`). The
  on-screen frame is a fit-to-window *editor preview*; that preview scale is
  not the runtime `renderScale`.

Decision this feeds: **lock `unitScale` (recommended `3`) or keep it
per-scene** — see the open decisions in the architecture doc.
