/* Box3D oracle driver: compound shapes (docs/physics-rae-port-design.md §7
 * P5d): b3CreateCompound (material sharing, the child tree built with
 * b3DynamicTree_CreateProxy and a full rebuild), b3GetCompoundChild,
 * b3ComputeCompoundAABB and b3QueryCompound.
 *
 * A compound is described by its children, each line
 *
 *   b3CreateCompound capsuleCount (capsule(7) material)* hullCount
 *     (box(3) transform(7) material)* meshCount (boxMesh center(3) extent(3)
 *     transform(7) scale(3) materialCount material*)* sphereCount
 *     (sphere(4) material)* -> materialCount material* nodeEnd
 *     (aabb(6) flagIndex height)* proxyCount (node next category userData)*
 *     childCount (type transform(7) materialCount materialIndex* geometry)*
 *
 * (hulls are boxes made with b3MakeTransformedBoxHull at the identity,
 * meshes b3CreateBoxMesh with edge identification; a child's geometry is its
 * capsule(7) or sphere(4), and nothing for hulls and meshes), then
 * `compound.aabb transform(7) -> box(6)` and `compound.query box(6) ->
 * count childIndex*` (the query's visiting order).
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

static uint32_t g_state = 0x3c6ef372u;

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
static void u64(uint64_t x) { printf(" %u %u", (unsigned)(x >> 32), (unsigned)(x & 0xffffffffu)); }
static void v3(b3Vec3 v) { f(v.x); f(v.y); f(v.z); }
static void q4(b3Quat q) { v3(q.v); f(q.s); }
static void tf(b3Transform t) { v3(t.p); q4(t.q); }
static void box(b3AABB a) { v3(a.lowerBound); v3(a.upperBound); }
static void name(const char* n) { printf("%s", n); }
static void arrow(void) { printf(" ->"); }
static void end(void) { printf("\n"); }

static void material(b3SurfaceMaterial m) {
  f(m.friction); f(m.restitution); f(m.rollingResistance); v3(m.tangentVelocity); u64(m.userMaterialId); i(m.customColor);
}

/* A few materials, so children share some of them. */
static b3SurfaceMaterial pickMaterial(void) {
  b3SurfaceMaterial m = b3DefaultSurfaceMaterial();
  uint32_t r = next_u32() % 4u;
  m.friction = 0.2f * (float)(r + 1);
  if (r == 3) m.restitution = 0.5f;
  if (r == 2) m.userMaterialId = 7;
  return m;
}

static b3Transform pickTransform(void) {
  b3Vec3 axis = b3Normalize((b3Vec3){ next_float(1.0f), 1.0f, next_float(1.0f) });
  b3Transform t = { { next_float(4.0f), next_float(2.0f), next_float(4.0f) }, b3MakeQuatFromAxisAngle(axis, next_float(2.0f)) };
  return t;
}

typedef struct QueryResult {
  int indices[256];
  int count;
} QueryResult;

static bool collectChild(const b3CompoundData* compound, int childIndex, void* context) {
  (void)compound;
  QueryResult* result = context;
  if (result->count < 256) result->indices[result->count++] = childIndex;
  return true;
}

static void probe(const b3CompoundData* compound) {
  for (int k = 0; k < 2; k++) {
    b3Transform t = pickTransform();
    name("compound.aabb"); tf(t); arrow(); box(b3ComputeCompoundAABB(compound, t)); end();
  }
  b3AABB root = b3ComputeCompoundAABB(compound, b3Transform_identity);
  for (int k = 0; k < 6; k++) {
    b3Vec3 center = { next_float(5.0f), next_float(3.0f), next_float(5.0f) };
    b3Vec3 half = { 0.2f + 1.5f * (next_float(1.0f) + 1.0f), 0.2f + (next_float(1.0f) + 1.0f),
                    0.2f + 1.5f * (next_float(1.0f) + 1.0f) };
    b3AABB bounds = { b3Sub(center, half), b3Add(center, half) };
    if (k == 5) bounds = root;
    QueryResult result = { .count = 0 };
    b3QueryCompound(compound, bounds, collectChild, &result);
    name("compound.query"); box(bounds); arrow(); i(result.count);
    for (int n = 0; n < result.count; n++) i(result.indices[n]);
    end();
  }
}

static void compoundOut(const b3CompoundData* compound) {
  int materialCount = compound->materialCount;
  i(materialCount);
  const b3SurfaceMaterial* materials = b3GetCompoundMaterials(compound);
  for (int k = 0; k < materialCount; k++) material(materials[k]);
  const b3DynamicTree* t = &compound->tree;
  i(t->nodeEnd);
  for (int k = 0; k < t->nodeEnd; k++) { box(t->nodes[k].aabb); i(t->nodes[k].flagIndex); i(t->nodes[k].height); }
  i(t->proxyCount);
  for (int k = 0; k < t->proxyCount; k++) {
    i(t->proxies[k].node); i(t->proxies[k].next); u64(t->proxies[k].categoryBits); u64(t->proxies[k].userData);
  }
  int childCount = compound->capsuleCount + compound->hullCount + compound->meshCount + compound->sphereCount;
  i(childCount);
  for (int k = 0; k < childCount; k++) {
    b3ChildShape child = b3GetCompoundChild(compound, k);
    i(child.type); tf(child.transform); i(child.materialCount);
    for (int n = 0; n < child.materialCount; n++) i(child.materialIndices[n]);
    if (child.type == b3_capsuleShape) { v3(child.capsule.center1); v3(child.capsule.center2); f(child.capsule.radius); }
    if (child.type == b3_sphereShape) { v3(child.sphere.center); f(child.sphere.radius); }
  }
}

int main(void) {
  printf("# box3d oracle: compounds, commit %s, scalar, -ffp-contract=off\n", BOX3D_ORACLE_COMMIT);
  b3MeshData* meshes[64];
  int meshTotal = 0;

  for (int n = 0; n < 10; n++) {
    b3CompoundCapsuleDef capsules[8];
    b3CompoundHullDef hulls[8];
    b3BoxHull boxes[8];
    b3CompoundMeshDef meshDefs[4];
    b3SurfaceMaterial meshMaterials[4][3];
    b3CompoundSphereDef spheres[8];
    int capsuleCount = (int)(next_u32() % 4u);
    int hullCount = (int)(next_u32() % 5u);
    int meshCount = (n % 3 == 1) ? 1 + (int)(next_u32() % 2u) : 0;
    int sphereCount = (int)(next_u32() % 4u);
    if (capsuleCount + hullCount + meshCount + sphereCount == 0) sphereCount = 1;

    name("b3CreateCompound");
    i(capsuleCount);
    for (int k = 0; k < capsuleCount; k++) {
      b3Vec3 c = { next_float(4.0f), next_float(2.0f), next_float(4.0f) };
      b3Vec3 d = { next_float(1.0f), next_float(1.0f), next_float(1.0f) };
      capsules[k].capsule = (b3Capsule){ b3Sub(c, d), b3Add(c, d), 0.1f + 0.4f * (next_float(1.0f) + 1.0f) * 0.5f };
      capsules[k].material = pickMaterial();
      v3(capsules[k].capsule.center1); v3(capsules[k].capsule.center2); f(capsules[k].capsule.radius);
      material(capsules[k].material);
    }
    i(hullCount);
    for (int k = 0; k < hullCount; k++) {
      float hx = 0.2f + 0.25f * (next_float(1.0f) + 1.0f);
      float hy = 0.2f + 0.25f * (next_float(1.0f) + 1.0f);
      float hz = 0.2f + 0.25f * (next_float(1.0f) + 1.0f);
      boxes[k] = b3MakeTransformedBoxHull(hx, hy, hz, b3Transform_identity);
      hulls[k].hull = &boxes[k].base;
      hulls[k].transform = pickTransform();
      hulls[k].material = pickMaterial();
      f(hx); f(hy); f(hz); tf(hulls[k].transform); material(hulls[k].material);
    }
    i(meshCount);
    for (int k = 0; k < meshCount; k++) {
      b3Vec3 center = { next_float(1.0f), next_float(1.0f), next_float(1.0f) };
      b3Vec3 extent = { 0.5f + (next_float(1.0f) + 1.0f), 0.3f + 0.5f * (next_float(1.0f) + 1.0f), 0.5f + (next_float(1.0f) + 1.0f) };
      b3MeshData* mesh = b3CreateBoxMesh(center, extent, true);
      meshes[meshTotal++] = mesh;
      meshDefs[k].meshData = mesh;
      meshDefs[k].transform = pickTransform();
      meshDefs[k].scale = (b3Vec3){ 1.0f, (k == 1) ? -1.0f : 1.0f, 1.5f };
      meshDefs[k].materialCount = mesh->materialCount;
      for (int m = 0; m < mesh->materialCount && m < 3; m++) meshMaterials[k][m] = pickMaterial();
      meshDefs[k].materials = meshMaterials[k];
      v3(center); v3(extent); tf(meshDefs[k].transform); v3(meshDefs[k].scale); i(meshDefs[k].materialCount);
      for (int m = 0; m < meshDefs[k].materialCount; m++) material(meshMaterials[k][m]);
    }
    i(sphereCount);
    for (int k = 0; k < sphereCount; k++) {
      spheres[k].sphere = (b3Sphere){ { next_float(4.0f), next_float(2.0f), next_float(4.0f) }, 0.1f + 0.5f * (next_float(1.0f) + 1.0f) * 0.5f };
      spheres[k].material = pickMaterial();
      v3(spheres[k].sphere.center); f(spheres[k].sphere.radius); material(spheres[k].material);
    }

    b3CompoundDef def = { 0 };
    def.capsules = capsules; def.capsuleCount = capsuleCount;
    def.hulls = hulls; def.hullCount = hullCount;
    def.meshes = meshDefs; def.meshCount = meshCount;
    def.spheres = spheres; def.sphereCount = sphereCount;
    b3CompoundData* compound = b3CreateCompound(&def);
    arrow(); compoundOut(compound); end();
    probe(compound);
    b3DestroyCompound(compound);
  }
  for (int k = 0; k < meshTotal; k++) b3DestroyMesh(meshes[k]);
  return 0;
}
