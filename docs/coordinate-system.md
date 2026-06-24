# Rae 3D coordinate system

Rae uses a single, project-wide 3D convention: **right-handed, Z-up, matching
Blender**. The goal is maximum interoperability with Blender (scenes, cameras,
rotations, exported data) so there is exactly one standard to reason about.

> If Blender and another tool disagree, Rae follows **Blender**.

## Axes

```
        +Z  up
         |
         |
         |________ +X  right
        /
       /
     +Y  forward (into the scene)
```

- **+X → right**
- **+Y → forward** (the direction the default camera looks — Blender "front view")
- **+Z → up**
- **Right-handed**: `X × Y = Z`.

This is exactly Blender's world convention. A point `(1, 2, 3)` is 1 right,
2 forward, 3 up.

## Camera

- The **default camera** sits on the `-Y` side looking toward `+Y`, with
  world-up `+Z` — i.e. Blender's front view (Numpad 1).
- A camera is specified unambiguously by **position + look-at target +
  world-up (`+Z`)**. The basis is derived as:
  - `forward = normalize(target - position)`
  - `right   = normalize(cross(forward, worldUp))`
  - `up      = cross(right, forward)`
  (right-handed; `worldUp = (0, 0, 1)`).
- A degenerate look (camera pointing straight up/down `±Z`, parallel to
  `worldUp`) must be handled by choosing an alternate up reference.

## Rotations

Rotations are **right-handed** (counter-clockwise positive when looking from
the positive end of the axis toward the origin), matching Blender:

- **Yaw** — rotation about **+Z**. Positive yaw turns from `+X` toward `+Y`
  (counter-clockwise seen from above).
- **Pitch** — rotation about **+X**. Positive pitch tilts `+Y` toward `+Z`
  (look up).
- **Roll** — rotation about **+Y** (the forward axis).

Euler order follows Blender's default **XYZ**.

## Field of view

Vertical FOV in radians (Blender's camera "Field of View" is also definable
vertically). The viewport half-height at focus distance `d` is
`d * tan(fovY / 2)`; half-width is `aspect * half-height`, where
`aspect = width / height`.

## Image / screen mapping

The world convention above is independent of how the finished image is shown.
Rae's raster/window layer (e.g. raylib textures) uses the usual screen
convention: **origin top-left, +x right, +y down**, so **row 0 is the top of
the image**. When generating primary rays, map pixel `(px, py)`:

- horizontal `u = px / (width - 1)` spans the camera's **right** axis,
- vertical `v = (height - 1 - py) / (height - 1)` spans the camera's **up**
  axis (the `height - 1 - py` flips raster top-down rows into world bottom-up
  `+Z`, so the top image row is the highest point in the scene).

A primary ray is then:

```
dir = forward * focal
    + right   * (u * 2 - 1) * halfWidth
    + up      * (v * 2 - 1) * halfHeight
```

This keeps the **world** Z-up while the **image** is written top row first, with
no contradiction between the two.

## Why Z-up (and not the graphics-default Y-up)

Most real-time graphics stacks (OpenGL, glTF, raylib's own `Camera3D`) default
to **Y-up**. Rae deliberately does **not** follow that for world space: CAD,
DCC, and simulation tooling — Blender above all — are Z-up, and Rae targets
that ecosystem. Because Rae's renderers (the CPU raytracer, etc.) own their ray
math and only hand the rasterizer a finished 2D image, the host graphics API's
up-axis preference never leaks into Rae world space. The reference C++ raytracer
(`rae_ui`) made the same call — its camera defaults to `CoordinatesUp::Z`.

## Scope

This convention applies to all Rae code that reasons about 3D: the raytracer
examples (`examples/40_raytracer_*` onward), any future scene/camera/ECS APIs,
and exported/imported geometry. New 3D code MUST use Z-up Blender conventions
rather than introducing a local one.
