/* Box3D oracle driver: height fields (docs/physics-rae-port-design.md §7
 * P5c): b3CreateHeightField (quantized heights, convexity flags),
 * b3GetHeightFieldTriangle, b3GetHeightFieldMaterial,
 * b3ComputeHeightFieldAABB, b3QueryHeightField, b3CreateGrid and
 * b3CreateWave.
 *
 * Every created field is dumped as
 *
 *   ... -> aabb(6) minHeight maxHeight heightScale scale(3) columnCount
 *          rowCount clockwise heights* materials* flags*
 *
 * then each of its triangles (`heightField.triangle index -> vertices(9) i1
 * i2 i3 flags material`), its AABB under a transform and the triangles
 * queries of a few boxes find (`heightField.query box(6) -> count indices*`;
 * the collector keeps at most 256, as b3RefreshCache's does).
 *
 * Compiled against the cached Box3D checkout by tools/box3d-oracle/oracle.sh,
 * never by a Rae build. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "shape.h"

#include "box3d/box3d.h"
#include "box3d/collision.h"
#include "box3d/math_functions.h"

static uint32_t g_state = 0x6a09e667u;

static uint32_t next_u32(void) {
  uint32_t x = g_state;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  g_state = x;
  return x;
}

static float next_float(float range) {
  int steps = (int)(range * 256.0f);
  int k = (int)(next_u32() % (uint32_t)(2 * steps + 1)) - steps;
  return (float)k / 256.0f;
}

static void f(float x) { printf(" %a", (double)x); }
static void i(long long x) { printf(" %lld", x); }
static void v3(b3Vec3 v) { f(v.x); f(v.y); f(v.z); }
static void q4(b3Quat q) { v3(q.v); f(q.s); }
static void box(b3AABB a) { v3(a.lowerBound); v3(a.upperBound); }
static void name(const char* n) { printf("%s", n); }
static void arrow(void) { printf(" ->"); }
static void end(void) { printf("\n"); }

static void dataOut(const b3HeightFieldData* hf) {
  box(hf->aabb); f(hf->minHeight); f(hf->maxHeight); f(hf->heightScale); v3(hf->scale);
  i(hf->columnCount); i(hf->rowCount); i(hf->clockwise);
  int heightCount = hf->columnCount * hf->rowCount;
  int cellCount = (hf->columnCount - 1) * (hf->rowCount - 1);
  const uint16_t* heights = b3GetHeightFieldCompressedHeights(hf);
  const uint8_t* materials = b3GetHeightFieldMaterialIndices(hf);
  const uint8_t* flags = b3GetHeightFieldFlags(hf);
  for (int k = 0; k < heightCount; k++) i(heights[k]);
  for (int k = 0; k < cellCount; k++) i(materials[k]);
  for (int k = 0; k < 2 * cellCount; k++) i(flags[k]);
}

typedef struct Collector {
  int indices[256];
  int count;
} Collector;

static bool collect(b3Vec3 a, b3Vec3 b, b3Vec3 c, int triangleIndex, void* context) {
  (void)a; (void)b; (void)c;
  Collector* collector = context;
  if (collector->count == 256) return false;
  collector->indices[collector->count++] = triangleIndex;
  return collector->count < 256;
}

static void probe(const b3HeightFieldData* hf) {
  int cellCount = (hf->columnCount - 1) * (hf->rowCount - 1);
  const uint8_t* materials = b3GetHeightFieldMaterialIndices(hf);
  for (int t = 0; t < 2 * cellCount; t++) {
    if (materials[t >> 1] == B3_HEIGHT_FIELD_HOLE) continue;
    b3Triangle tri = b3GetHeightFieldTriangle(hf, t);
    name("heightField.triangle"); i(t); arrow();
    v3(tri.vertices[0]); v3(tri.vertices[1]); v3(tri.vertices[2]);
    i(tri.i1); i(tri.i2); i(tri.i3); i(tri.flags); i(b3GetHeightFieldMaterial(hf, t)); end();
  }
  b3Vec3 axis = b3Normalize((b3Vec3){ next_float(1.0f), 1.0f, next_float(1.0f) });
  b3Transform xf = { { next_float(4.0f), next_float(4.0f), next_float(4.0f) },
                     b3MakeQuatFromAxisAngle(axis, next_float(2.0f)) };
  name("heightField.aabb"); v3(xf.p); q4(xf.q); arrow(); box(b3ComputeHeightFieldAABB(hf, xf)); end();
  b3Vec3 extent = { hf->aabb.upperBound.x, 0.0f, hf->aabb.upperBound.z };
  for (int k = 0; k < 6; k++) {
    b3Vec3 center = { extent.x * (0.5f + next_float(0.6f)), next_float(2.0f), extent.z * (0.5f + next_float(0.6f)) };
    b3Vec3 half = { 0.1f + 0.5f * extent.x * (next_float(0.5f) + 0.5f), 0.2f + next_float(2.0f) * next_float(2.0f),
                    0.1f + 0.5f * extent.z * (next_float(0.5f) + 0.5f) };
    if (half.y < 0.0f) half.y = -half.y;
    b3AABB bounds = { b3Sub(center, half), b3Add(center, half) };
    Collector collector = { .count = 0 };
    b3QueryHeightField(hf, bounds, collect, &collector);
    name("heightField.query"); box(bounds); arrow(); i(collector.count);
    for (int n = 0; n < collector.count; n++) i(collector.indices[n]);
    end();
  }
}

int main(void) {
  printf("# box3d oracle: height fields, commit %s, scalar, -ffp-contract=off\n", BOX3D_ORACLE_COMMIT);

  /* Random fields: heights, materials with holes, both windings */
  for (int n = 0; n < 8; n++) {
    int columns = 3 + (int)(next_u32() % 6u);
    int rows = 3 + (int)(next_u32() % 6u);
    float heights[64];
    uint8_t materials[64];
    for (int k = 0; k < columns * rows; k++) heights[k] = next_float(3.0f);
    for (int k = 0; k < (columns - 1) * (rows - 1); k++) {
      uint32_t r = next_u32() % 10u;
      materials[k] = r == 0 ? B3_HEIGHT_FIELD_HOLE : (uint8_t)(r % 3u);
    }
    b3HeightFieldDef def = { 0 };
    def.heights = heights;
    def.materialIndices = (n % 3 == 2) ? NULL : materials;
    def.scale = (b3Vec3){ 0.5f + (float)(next_u32() % 4u) * 0.25f, 0.5f + (float)(next_u32() % 3u) * 0.5f,
                          0.75f + (float)(next_u32() % 3u) * 0.25f };
    def.countX = columns;
    def.countZ = rows;
    def.globalMinimumHeight = (n == 5) ? -1.0f : -4.0f;
    def.globalMaximumHeight = (n == 5) ? 1.5f : 4.0f;
    def.clockwiseWinding = (n % 2) == 1;
    b3HeightFieldData* hf = b3CreateHeightField(&def);
    name("b3CreateHeightField"); i(columns); i(rows);
    for (int k = 0; k < columns * rows; k++) f(heights[k]);
    i(def.materialIndices != NULL);
    if (def.materialIndices) for (int k = 0; k < (columns - 1) * (rows - 1); k++) i(materials[k]);
    v3(def.scale); f(def.globalMinimumHeight); f(def.globalMaximumHeight); i(def.clockwiseWinding);
    arrow(); dataOut(hf); end();
    probe(hf);
    b3DestroyHeightField(hf);
  }

  /* The generated fields */
  {
    b3Vec3 scale = { 1.0f, 1.0f, 1.0f };
    b3HeightFieldData* hf = b3CreateGrid(6, 7, scale, false);
    name("b3CreateGrid"); i(6); i(7); v3(scale); i(0); arrow(); dataOut(hf); end();
    probe(hf);
    b3DestroyHeightField(hf);
  }
  {
    b3Vec3 scale = { 0.5f, 2.0f, 0.75f };
    b3HeightFieldData* hf = b3CreateGrid(9, 8, scale, true);
    name("b3CreateGrid"); i(9); i(8); v3(scale); i(1); arrow(); dataOut(hf); end();
    probe(hf);
    b3DestroyHeightField(hf);
  }
  {
    b3Vec3 scale = { 1.0f, 0.5f, 1.0f };
    b3HeightFieldData* hf = b3CreateWave(10, 12, scale, 0.1f, 0.15f, false);
    name("b3CreateWave"); i(10); i(12); v3(scale); f(0.1f); f(0.15f); i(0); arrow(); dataOut(hf); end();
    probe(hf);
    b3DestroyHeightField(hf);
  }
  {
    b3Vec3 scale = { 0.75f, 1.5f, 0.5f };
    b3HeightFieldData* hf = b3CreateWave(17, 9, scale, 0.2f, 0.05f, true);
    name("b3CreateWave"); i(17); i(9); v3(scale); f(0.2f); f(0.05f); i(1); arrow(); dataOut(hf); end();
    probe(hf);
    b3DestroyHeightField(hf);
  }
  return 0;
}
