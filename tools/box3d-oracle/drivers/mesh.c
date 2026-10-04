/* Box3D oracle driver: triangle meshes (docs/physics-rae-port-design.md §4,
 * P5b): b3CreateMesh (welding, the BVH with the binned SAH, median and half
 * splits, the depth-first triangle order, the edge flags), the generated
 * meshes, b3GetHeight, b3QueryMesh and b3ComputeMeshAABB.
 *
 * A mesh-building line dumps the whole mesh after `->`:
 *
 *   mesh   valid; if 1: bounds(6) surfaceArea treeHeight degenerateCount
 *          materialCount nodeCount, per node lower(3) upper(3) kind count
 *          triangleOffset (kind 0-2 the split axis and count the right child
 *          offset, kind 3 a leaf and count its triangles), vertexCount
 *          vertices(3 each), triangleCount, per triangle index1 index2
 *          index3 material flags, then b3GetHeight
 *   def    vertexCount vertices, triangleCount indices(3 each), hasMaterials
 *          [materials], weldTolerance weldVertices useMedianSplit
 *          identifyEdges clockWiseWinding
 *
 * b3QueryMesh and b3ComputeMeshAABB lines apply to the mesh built on the
 * line before them: `b3QueryMesh scale(3) bounds(6) -> count, per triangle
 * a(3) b(3) c(3) triangleIndex`, `b3ComputeMeshAABB transform scale ->
 * bounds(6)`.
 *
 * Compiled against the cached Box3D checkout by tools/box3d-oracle/oracle.sh,
 * never by a Rae build. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "box3d/collision.h"
#include "box3d/math_functions.h"

static uint32_t g_state = 0x9b05688cu;

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

static int next_int(int count) { return (int)(next_u32() % (uint32_t)count); }

static b3Vec3 next_vec(float range) {
  b3Vec3 v = { next_float(range), next_float(range), next_float(range) };
  return v;
}

static b3Vec3 next_nonzero_vec(float range) {
  for (;;) {
    b3Vec3 v = next_vec(range);
    if (b3LengthSquared(v) > 0.01f) return v;
  }
}

static b3Quat next_quat(float maxAngle) {
  b3Vec3 axis = b3Normalize(next_nonzero_vec(4.0f));
  return b3MakeQuatFromAxisAngle(axis, next_float(maxAngle));
}

static void f(float x) { printf(" %a", (double)x); }
static void i(long long x) { printf(" %lld", x); }
static void v3(b3Vec3 v) { f(v.x); f(v.y); f(v.z); }
static void q4(b3Quat q) { v3(q.v); f(q.s); }
static void tf(b3Transform t) { v3(t.p); q4(t.q); }
static void box(b3AABB a) { v3(a.lowerBound); v3(a.upperBound); }
static void name(const char* n) { printf("%s", n); }
static void arrow(void) { printf(" ->"); }
static void end(void) { printf("\n"); }

static void meshOut(const b3MeshData* m) {
  if (m == NULL) { i(0); return; }
  i(1); box(m->bounds); f(m->surfaceArea); i(m->treeHeight); i(m->degenerateCount); i(m->materialCount); i(m->nodeCount);
  const b3MeshNode* nodes = b3GetMeshNodes(m);
  for (int k = 0; k < m->nodeCount; k++) {
    v3(nodes[k].lowerBound); v3(nodes[k].upperBound);
    i(nodes[k].data.asNode.axis); i(nodes[k].data.asNode.childOffset); i(nodes[k].triangleOffset);
  }
  const b3Vec3* vertices = b3GetMeshVertices(m);
  i(m->vertexCount);
  for (int k = 0; k < m->vertexCount; k++) v3(vertices[k]);
  const b3MeshTriangle* triangles = b3GetMeshTriangles(m);
  const uint8_t* materials = b3GetMeshMaterialIndices(m);
  const uint8_t* flags = b3GetMeshFlags(m);
  i(m->triangleCount);
  for (int k = 0; k < m->triangleCount; k++) {
    i(triangles[k].index1); i(triangles[k].index2); i(triangles[k].index3); i(materials[k]); i(flags[k]);
  }
  i(b3GetHeight(m));
}

typedef struct QueryContext { int count; } QueryContext;

static bool queryOut(b3Vec3 a, b3Vec3 b, b3Vec3 c, int triangleIndex, void* context) {
  ((QueryContext*)context)->count += 1;
  v3(a); v3(b); v3(c); i(triangleIndex);
  return true;
}

static int countQuery(b3Vec3 a, b3Vec3 b, b3Vec3 c, int triangleIndex, void* context) {
  (void)a; (void)b; (void)c; (void)triangleIndex;
  ((QueryContext*)context)->count += 1;
  return 1;
}

static bool countCallback(b3Vec3 a, b3Vec3 b, b3Vec3 c, int triangleIndex, void* context) {
  return countQuery(a, b, c, triangleIndex, context) != 0;
}

/* Queries and AABBs of the mesh just built. */
static void probe(const b3MeshData* data) {
  if (data == NULL) return;
  b3Vec3 scales[4] = { { 1.0f, 1.0f, 1.0f }, { 2.0f, 0.5f, 1.5f }, { -1.0f, 1.0f, 1.0f }, { 1.0f, -0.75f, -1.25f } };
  for (int k = 0; k < 6; k++) {
    b3Mesh mesh = { data, scales[next_int(4)] };
    b3Vec3 lower = b3Mul(mesh.scale, data->bounds.lowerBound);
    b3Vec3 upper = b3Mul(mesh.scale, data->bounds.upperBound);
    b3Vec3 lo = b3Min(lower, upper), hi = b3Max(lower, upper);
    b3Vec3 c = b3Lerp(lo, hi, 0.5f + next_float(0.5f));
    b3Vec3 e = { 0.125f + (float)next_int(8) * 0.125f, 0.125f + (float)next_int(8) * 0.125f, 0.125f + (float)next_int(8) * 0.125f };
    b3AABB bounds = { b3Sub(c, e), b3Add(c, e) };
    /* The count first, so a fixture knows how many triangles follow. */
    QueryContext counter = { 0 };
    b3QueryMesh(&mesh, bounds, countCallback, &counter);
    name("b3QueryMesh"); v3(mesh.scale); box(bounds); arrow(); i(counter.count);
    QueryContext context = { 0 };
    b3QueryMesh(&mesh, bounds, queryOut, &context);
    end();
  }
  for (int k = 0; k < 2; k++) {
    b3Transform t = { next_vec(2.0f), next_quat(3.0f) };
    b3Vec3 scale = scales[next_int(4)];
    name("b3ComputeMeshAABB"); tf(t); v3(scale); arrow(); box(b3ComputeMeshAABB(data, t, scale)); end();
  }
}

static void defOut(const b3MeshDef* def) {
  i(def->vertexCount);
  for (int k = 0; k < def->vertexCount; k++) v3(def->vertices[k]);
  i(def->triangleCount);
  for (int k = 0; k < 3 * def->triangleCount; k++) i(def->indices[k]);
  i(def->materialIndices != NULL);
  if (def->materialIndices != NULL) {
    for (int k = 0; k < def->triangleCount; k++) i(def->materialIndices[k]);
  }
  f(def->weldTolerance); i(def->weldVertices); i(def->useMedianSplit); i(def->identifyEdges); i(def->clockWiseWinding);
}

#define MAX_VERTICES 64
#define MAX_TRIANGLES 96

int main(void) {
  printf("# box3d oracle: triangle meshes, commit %s, scalar, -ffp-contract=off\n", BOX3D_ORACLE_COMMIT);

  /* The generated meshes. */
  for (int k = 0; k < 6; k++) {
    int xCount = 1 + next_int(8), zCount = 1 + next_int(8);
    float cellWidth = 0.5f + (float)next_int(4) * 0.25f;
    int materialCount = next_int(4);
    bool identify = next_int(4) != 0;
    b3MeshData* m = b3CreateGridMesh(xCount, zCount, cellWidth, materialCount, identify);
    name("b3CreateGridMesh"); i(xCount); i(zCount); f(cellWidth); i(materialCount); i(identify); arrow(); meshOut(m); end();
    probe(m);
    if (m) b3DestroyMesh(m);
  }
  for (int k = 0; k < 4; k++) {
    b3Vec3 center = next_vec(2.0f);
    b3Vec3 extent = { 0.25f + (float)next_int(8) * 0.25f, 0.25f + (float)next_int(8) * 0.25f, 0.25f + (float)next_int(8) * 0.25f };
    bool identify = next_int(2) != 0;
    b3MeshData* m = b3CreateBoxMesh(center, extent, identify);
    name("b3CreateBoxMesh"); v3(center); v3(extent); i(identify); arrow(); meshOut(m); end();
    probe(m);
    if (m) b3DestroyMesh(m);
    m = b3CreateHollowBoxMesh(center, extent);
    name("b3CreateHollowBoxMesh"); v3(center); v3(extent); arrow(); meshOut(m); end();
    probe(m);
    if (m) b3DestroyMesh(m);
    float height = 0.5f + (float)next_int(4) * 0.25f, top = 1.0f + (float)next_int(4) * 0.5f, bottom = top + 1.0f;
    m = b3CreatePlatformMesh(center, height, top, bottom);
    name("b3CreatePlatformMesh"); v3(center); f(height); f(top); f(bottom); arrow(); meshOut(m); end();
    probe(m);
    if (m) b3DestroyMesh(m);
  }

  /* Triangle soups: random vertices and triangles, some degenerate, some
     duplicated vertices (for welding), materials, either winding, SAH or
     median splits. A large one goes deep enough for the half split. */
  for (int k = 0; k < 40; k++) {
    b3Vec3 vertices[MAX_VERTICES];
    int indices[3 * MAX_TRIANGLES];
    uint8_t materials[MAX_TRIANGLES];
    int vertexCount = 12 + next_int(MAX_VERTICES - 12 - 8);
    for (int v = 0; v < vertexCount; v++) vertices[v] = next_vec(2.0f + (float)(k % 4));
    if (k % 5 == 0) {
      /* points on a plane, so many triangles are coplanar (flat edges) */
      for (int v = 0; v < vertexCount; v++) vertices[v].y = 0.0f;
    }
    /* duplicates within the weld tolerance */
    int duplicates = next_int(8);
    for (int d = 0; d < duplicates; d++) {
      vertices[vertexCount] = b3Add(vertices[next_int(vertexCount)], next_vec(0.00390625f));
      vertexCount += 1;
    }
    int triangleCount = 8 + next_int(MAX_TRIANGLES - 8);
    if (k % 13 == 7) triangleCount = MAX_TRIANGLES;
    for (int t = 0; t < triangleCount; t++) {
      indices[3 * t + 0] = next_int(vertexCount);
      indices[3 * t + 1] = next_int(vertexCount);
      indices[3 * t + 2] = next_int(vertexCount);
      if (t % 17 == 3) indices[3 * t + 2] = indices[3 * t + 1]; /* degenerate */
      materials[t] = (uint8_t)next_int(4);
    }
    b3MeshDef def = { 0 };
    def.vertices = vertices;
    def.vertexCount = vertexCount;
    def.indices = indices;
    def.triangleCount = triangleCount;
    def.materialIndices = next_int(2) ? materials : NULL;
    def.weldVertices = next_int(2) != 0;
    def.weldTolerance = def.weldVertices ? 0.0078125f : 0.0f;
    def.useMedianSplit = next_int(3) == 0;
    def.identifyEdges = next_int(4) != 0;
    def.clockWiseWinding = next_int(4) == 0;
    b3MeshData* m = b3CreateMesh(&def, NULL, 0);
    name("b3CreateMesh"); defOut(&def); arrow(); meshOut(m); end();
    probe(m);
    if (m) b3DestroyMesh(m);
  }
  return 0;
}
