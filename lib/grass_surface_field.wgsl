// Rae's grass surface field (lib/GrassBlockField.rae sets these constants and
// fills the field): where grass grows and how high its ground is, for an app's
// grass surface part of the grass compute shader (ahead of lib/grass_compute.wgsl,
// in place of lib/world_biome.wgsl). The field is one value per cell, the
// ground's height where the cell grows grass and at or below
// GRASS_FIELD_NONE_BELOW where it does not: a base field (a level's surface map,
// app3d/SurfaceMap surfaceGrassField) and after it a block per tile of a grid
// (a streamed tile's cells, given by the app as the tile loads). A blade grows
// only where all four nearest cell centers are grass, so none stands on a road
// or rock edge, at the height between them.
override RAE_SURFACE_FIELD_ORIGIN_X: f32 = 0.0;
override RAE_SURFACE_FIELD_ORIGIN_Y: f32 = 0.0;
override RAE_SURFACE_FIELD_CELL: f32 = 2.0;
override RAE_SURFACE_FIELD_COLUMNS: f32 = 0.0;
override RAE_SURFACE_FIELD_ROWS: f32 = 0.0;
override RAE_BLOCK_FIELD_OFFSET: f32 = 0.0;
override RAE_BLOCK_FIELD_ORIGIN_X: f32 = 0.0;
override RAE_BLOCK_FIELD_ORIGIN_Y: f32 = 0.0;
override RAE_BLOCK_FIELD_CELL: f32 = 2.0;
override RAE_BLOCK_FIELD_TILE_CELLS: f32 = 1.0;
override RAE_BLOCK_FIELD_TILE_COLUMNS: f32 = 0.0;
override RAE_BLOCK_FIELD_TILE_ROWS: f32 = 0.0;

// Rae's grass compute binds the app's surface field here (grassSetSurfaceField)
@group(0) @binding(4) var<storage, read> grassSurfaceField: array<f32>;

// A field value at or below this is "no grass" (app3d/SurfaceMap grassFieldNone)
const GRASS_FIELD_NONE_BELOW: f32 = -1000.0;

// The base field at `p`, between the four nearest cell centers: x is 1 where
// all four are grass, y the ground's height there
fn surfaceFieldAt(p: vec2<f32>) -> vec2<f32> {
  let g = (p - vec2<f32>(RAE_SURFACE_FIELD_ORIGIN_X, RAE_SURFACE_FIELD_ORIGIN_Y)) / RAE_SURFACE_FIELD_CELL - vec2<f32>(0.5, 0.5);
  let corner = floor(g);
  if (corner.x < 0.0 || corner.y < 0.0 || corner.x + 1.0 >= RAE_SURFACE_FIELD_COLUMNS || corner.y + 1.0 >= RAE_SURFACE_FIELD_ROWS) {
    return vec2<f32>(0.0, 0.0);
  }
  let t = g - corner;
  let columns = u32(RAE_SURFACE_FIELD_COLUMNS);
  let cell = u32(corner.y) * columns + u32(corner.x);
  let h00 = grassSurfaceField[cell];
  let h10 = grassSurfaceField[cell + 1u];
  let h01 = grassSurfaceField[cell + columns];
  let h11 = grassSurfaceField[cell + columns + 1u];
  if (min(min(h00, h10), min(h01, h11)) <= GRASS_FIELD_NONE_BELOW) {
    return vec2<f32>(0.0, 0.0);
  }
  let height = mix(mix(h00, h10, t.x), mix(h01, h11, t.x), t.y);
  return vec2<f32>(1.0, height);
}

// The blocks' value at cell (x, y), counted from the grid's south-west
// corner: in that cell's tile's block (none past the grid)
fn blockFieldValue(x: i32, y: i32) -> f32 {
  let tileCells = i32(RAE_BLOCK_FIELD_TILE_CELLS);
  let columns = i32(RAE_BLOCK_FIELD_TILE_COLUMNS) * tileCells;
  let rows = i32(RAE_BLOCK_FIELD_TILE_ROWS) * tileCells;
  if (x < 0 || y < 0 || x >= columns || y >= rows) {
    return GRASS_FIELD_NONE_BELOW - 1.0;
  }
  let block = (y / tileCells) * i32(RAE_BLOCK_FIELD_TILE_COLUMNS) + x / tileCells;
  let cell = (y % tileCells) * tileCells + x % tileCells;
  return grassSurfaceField[u32(RAE_BLOCK_FIELD_OFFSET) + u32(block * tileCells * tileCells + cell)];
}

// The blocks' grass at `p`, by the same rule as the base field's
fn blockFieldAt(p: vec2<f32>) -> vec2<f32> {
  let g = (p - vec2<f32>(RAE_BLOCK_FIELD_ORIGIN_X, RAE_BLOCK_FIELD_ORIGIN_Y)) / RAE_BLOCK_FIELD_CELL - vec2<f32>(0.5, 0.5);
  let corner = floor(g);
  let t = g - corner;
  let x = i32(corner.x);
  let y = i32(corner.y);
  let h00 = blockFieldValue(x, y);
  let h10 = blockFieldValue(x + 1, y);
  let h01 = blockFieldValue(x, y + 1);
  let h11 = blockFieldValue(x + 1, y + 1);
  if (min(min(h00, h10), min(h01, h11)) <= GRASS_FIELD_NONE_BELOW) {
    return vec2<f32>(0.0, 0.0);
  }
  let height = mix(mix(h00, h10, t.x), mix(h01, h11, t.x), t.y);
  return vec2<f32>(1.0, height);
}

// Is `p` in the block grid?
fn inBlockField(p: vec2<f32>) -> bool {
  let size = vec2<f32>(RAE_BLOCK_FIELD_TILE_COLUMNS, RAE_BLOCK_FIELD_TILE_ROWS) * RAE_BLOCK_FIELD_TILE_CELLS * RAE_BLOCK_FIELD_CELL;
  let d = p - vec2<f32>(RAE_BLOCK_FIELD_ORIGIN_X, RAE_BLOCK_FIELD_ORIGIN_Y);
  return d.x >= 0.0 && d.y >= 0.0 && d.x < size.x && d.y < size.y;
}

// The grass where `p` is: the blocks' there, else the base field's
fn groundFieldAt(p: vec2<f32>) -> vec2<f32> {
  if (inBlockField(p)) {
    return blockFieldAt(p);
  }
  return surfaceFieldAt(p);
}
