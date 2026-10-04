/* Box3D oracle driver: the world queries (docs/physics-rae-port-design.md §7
 * P7a: b3World_CastRayClosest, b3World_CastRay, b3World_CastShape,
 * b3World_OverlapAABB, b3World_OverlapShape, b3World_CollideMover and
 * b3World_CastMover of physics_world.c, and b3RayCastShape,
 * b3ShapeCastShape, b3OverlapShape and b3CollideMover of shape.c). A world of
 * static, kinematic and dynamic spheres, capsules and boxes (some in a second
 * filter category, one rotated) is stepped (1/60 s, 4 sub-steps, workerCount
 * 1, sleep on); before the first step and after 30 and 90 steps the same
 * battery of queries runs. The line formats are meshscenes.c's for the world
 * and bodies, plus
 *
 *   query.rayClosest origin(3) translation(3) filter -> hit shape point(3)
 *                    normal(3) fraction userMaterialId triangle child
 *                    nodeVisits leafVisits
 *   query.ray origin(3) translation(3) filter -> count (hit)*count nodeVisits
 *             leafVisits
 *   query.shape origin(3) proxy translation(3) filter -> count (hit)*count
 *               nodeVisits leafVisits
 *   query.shapeClosest origin(3) proxy translation(3) filter -> found hit
 *   query.overlapAabb lower(3) upper(3) filter -> count (shape)*count
 *                     nodeVisits leafVisits
 *   query.overlapShape origin(3) proxy filter -> count (shape)*count
 *                      nodeVisits leafVisits
 *   query.collideMover origin(3) capsule(7) filter -> count (shape normal(3)
 *                      offset point(3) triangle child material)*count
 *   query.castMover origin(3) capsule(7) translation(3) filter ignoreCount
 *                   (shape)*ignoreCount -> fraction
 *
 * where a filter is categoryBits maskBits id (each 64-bit, as two 32-bit
 * halves), a proxy is count points(3)*count radius, a shape is its index and
 * generation, and a hit is shape point(3) normal(3) fraction userMaterialId
 * triangle child. `query.ray` and `query.shape` collect every hit (the
 * callback returns 1); `query.shapeClosest` keeps the last hit, the callback
 * returning its fraction.
 *
 * A second world runs a battery against the surface shapes (P7b: the
 * mesh.c, height_field.c and compound.c queries): a grid mesh, a mirrored
 * and non-uniformly scaled grid mesh, a wave height field with holes, a
 * flat height field and a compound of capsules, boxes, a box mesh and
 * spheres with distinct materials, with a few dynamic bodies resting on
 * them. The shape lines are meshscenes.c's and compoundscenes.c's
 * (shape.mesh, shape.heightField, shape.compound).
 *
 * Compiled against the cached Box3D checkout by tools/box3d-oracle/oracle.sh,
 * never by a Rae build. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "body.h"
#include "physics_world.h"

#include "box3d/box3d.h"
#include "box3d/collision.h"
#include "box3d/math_functions.h"

#ifndef DUMP_EVERY
#define DUMP_EVERY 20
#endif

static void f(float x) { printf(" %a", (double)x); }
static void i(long long x) { printf(" %lld", x); }
static void u64(uint64_t x) { printf(" %u %u", (unsigned)(x >> 32), (unsigned)(x & 0xffffffffu)); }
static void v3(b3Vec3 v) { f(v.x); f(v.y); f(v.z); }
static void q4(b3Quat q) { v3(q.v); f(q.s); }
static void tf(b3Transform t) { v3(t.p); q4(t.q); }

static void name(const char* n) { printf("%s", n); }
static void arrow(void) { printf(" ->"); }
static void end(void) { printf("\n"); }

static b3World* g_world;
static b3WorldId g_worldId;
static int g_stepIndex;

static void material(b3SurfaceMaterial m) {
  f(m.friction); f(m.restitution); f(m.rollingResistance); v3(m.tangentVelocity); u64(m.userMaterialId); i(m.customColor);
}

static b3BodyId g_bodies[128];
static int g_bodyCount;

static void bodyRef(b3BodyId id) { i(id.index1 - 1); i(id.generation); }
static void shapeRef(b3ShapeId id) { i(id.index1 - 1); i(id.generation); }

static b3BodyId makeMovingBody(b3BodyType type, b3Vec3 position, b3Quat rotation, b3Vec3 linear, b3Vec3 angular) {
  b3BodyDef def = b3DefaultBodyDef();
  def.type = type;
  def.position = position;
  def.rotation = rotation;
  def.linearVelocity = linear;
  def.angularVelocity = angular;
  b3BodyId id = b3CreateBody(g_worldId, &def);
  name("body.create"); i(def.type); v3(def.position); q4(def.rotation); v3(def.linearVelocity); v3(def.angularVelocity);
  f(def.linearDamping); f(def.angularDamping); f(def.gravityScale); f(def.sleepThreshold); f(def.safetyFactor); i(0);
  i(def.enableSleep); i(def.isAwake); i(def.isBullet); i(def.isEnabled); i(def.allowFastRotation);
  i(def.enableContactRecycling); arrow(); bodyRef(id); end();
  g_bodies[g_bodyCount++] = id;
  return id;
}

static b3BodyId makeBody(b3BodyType type, b3Vec3 position) {
  return makeMovingBody(type, position, b3Quat_identity, b3Vec3_zero, b3Vec3_zero);
}

static void defOut(const b3ShapeDef* def) {
  material(def->baseMaterial); f(def->density); f(def->explosionScale); u64(def->filter.categoryBits);
  u64(def->filter.maskBits); i(def->filter.groupIndex); i(def->enableSensorEvents); i(def->enableContactEvents);
  i(def->enableHitEvents); i(def->enablePreSolveEvents); i(def->enableCustomFiltering); i(def->enableSpeculativeContact);
  i(def->invokeContactCreation); i(def->updateBodyMass);
}


static b3ShapeDef shapeDefIn(uint64_t category) {
  b3ShapeDef def = b3DefaultShapeDef();
  def.filter.categoryBits = category;
  return def;
}

static void boxShapeAt(b3BodyId body, b3ShapeDef def, float hx, float hy, float hz, b3Transform transform) {
  b3BoxHull boxHull = b3MakeTransformedBoxHull(hx, hy, hz, transform);
  b3ShapeId id = b3CreateHullShape(body, &def, &boxHull.base);
  name("shape.hull"); bodyRef(body); defOut(&def); i(1); f(hx); f(hy); f(hz); tf(transform); arrow(); shapeRef(id); end();
}

static void sphereShapeIn(b3BodyId body, b3ShapeDef def, float radius) {
  b3Sphere s = { b3Vec3_zero, radius };
  b3ShapeId id = b3CreateSphereShape(body, &def, &s);
  name("shape.sphere"); bodyRef(body); defOut(&def); v3(s.center); f(s.radius); arrow(); shapeRef(id); end();
}

static void capsuleShapeIn(b3BodyId body, b3ShapeDef def, float halfLength, float radius) {
  b3Capsule c = { { -halfLength, 0.0f, 0.0f }, { halfLength, 0.0f, 0.0f }, radius };
  b3ShapeId id = b3CreateCapsuleShape(body, &def, &c);
  name("shape.capsule"); bodyRef(body); defOut(&def); v3(c.center1); v3(c.center2); f(c.radius); arrow(); shapeRef(id); end();
}

static b3Transform identityAt(float x, float y, float z) {
  b3Transform t = { { x, y, z }, b3Quat_identity };
  return t;
}

static void filterOut(b3QueryFilter filter) { u64(filter.categoryBits); u64(filter.maskBits); u64(filter.id); }

static void proxyOut(const b3ShapeProxy* proxy) {
  i(proxy->count);
  for (int k = 0; k < proxy->count; k++) v3(proxy->points[k]);
  f(proxy->radius);
}

static void capsuleOut(const b3Capsule* c) { v3(c->center1); v3(c->center2); f(c->radius); }

/* --- Collecting callbacks --- */

#define MAX_HITS 64

typedef struct Hit {
  b3ShapeId shapeId;
  b3Pos point;
  b3Vec3 normal;
  float fraction;
  uint64_t userMaterialId;
  int triangleIndex;
  int childIndex;
} Hit;

typedef struct Hits {
  Hit hits[MAX_HITS];
  int count;
  bool closest;
} Hits;

static float collectFcn(b3ShapeId shapeId, b3Pos point, b3Vec3 normal, float fraction, uint64_t userMaterialId,
                        int triangleIndex, int childIndex, void* context) {
  Hits* hits = (Hits*)context;
  if (hits->count < MAX_HITS) {
    hits->hits[hits->count++] = (Hit){ shapeId, point, normal, fraction, userMaterialId, triangleIndex, childIndex };
  }
  return hits->closest ? fraction : 1.0f;
}

static void hitOut(const Hit* hit) {
  shapeRef(hit->shapeId); v3(hit->point); v3(hit->normal); f(hit->fraction); u64(hit->userMaterialId);
  i(hit->triangleIndex); i(hit->childIndex);
}

typedef struct Overlaps {
  b3ShapeId ids[MAX_HITS];
  int count;
} Overlaps;

static bool overlapFcn(b3ShapeId shapeId, void* context) {
  Overlaps* overlaps = (Overlaps*)context;
  if (overlaps->count < MAX_HITS) overlaps->ids[overlaps->count++] = shapeId;
  return true;
}

typedef struct Planes {
  b3ShapeId ids[MAX_HITS];
  b3PlaneResult planes[MAX_HITS];
  int count;
} Planes;

static bool planeFcn(b3ShapeId shapeId, const b3PlaneResult* results, int count, void* context) {
  Planes* planes = (Planes*)context;
  for (int k = 0; k < count && planes->count < MAX_HITS; k++) {
    planes->ids[planes->count] = shapeId;
    planes->planes[planes->count] = results[k];
    planes->count += 1;
  }
  return true;
}

typedef struct Ignore {
  b3ShapeId ids[4];
  int count;
} Ignore;

static bool moverFilterFcn(b3ShapeId shapeId, void* context) {
  Ignore* ignore = (Ignore*)context;
  for (int k = 0; k < ignore->count; k++) {
    if (B3_ID_EQUALS(shapeId, ignore->ids[k])) return false;
  }
  return true;
}

/* --- The queries --- */

static void rayClosest(b3Vec3 origin, b3Vec3 translation, b3QueryFilter filter) {
  b3RayResult r = b3World_CastRayClosest(g_worldId, origin, translation, filter);
  name("query.rayClosest"); v3(origin); v3(translation); filterOut(filter); arrow(); i(r.hit); shapeRef(r.shapeId);
  v3(r.point); v3(r.normal); f(r.fraction); u64(r.userMaterialId); i(r.triangleIndex); i(r.childIndex);
  i(r.nodeVisits); i(r.leafVisits); end();
}

static void rayAll(b3Vec3 origin, b3Vec3 translation, b3QueryFilter filter) {
  Hits hits = { 0 };
  b3TreeStats stats = b3World_CastRay(g_worldId, origin, translation, filter, collectFcn, &hits);
  name("query.ray"); v3(origin); v3(translation); filterOut(filter); arrow(); i(hits.count);
  for (int k = 0; k < hits.count; k++) hitOut(hits.hits + k);
  i(stats.nodeVisits); i(stats.leafVisits); end();
}

static void shapeCast(b3Vec3 origin, const b3ShapeProxy* proxy, b3Vec3 translation, b3QueryFilter filter, bool closest) {
  Hits hits = { 0 };
  hits.closest = closest;
  b3TreeStats stats = b3World_CastShape(g_worldId, origin, proxy, translation, filter, collectFcn, &hits);
  name(closest ? "query.shapeClosest" : "query.shape"); v3(origin); proxyOut(proxy); v3(translation); filterOut(filter);
  arrow();
  if (closest) {
    i(hits.count > 0);
    if (hits.count > 0) hitOut(hits.hits + hits.count - 1);
  } else {
    i(hits.count);
    for (int k = 0; k < hits.count; k++) hitOut(hits.hits + k);
    i(stats.nodeVisits); i(stats.leafVisits);
  }
  end();
}

static void overlapAabb(b3AABB box, b3QueryFilter filter) {
  Overlaps overlaps = { 0 };
  b3TreeStats stats = b3World_OverlapAABB(g_worldId, box, filter, overlapFcn, &overlaps);
  name("query.overlapAabb"); v3(box.lowerBound); v3(box.upperBound); filterOut(filter); arrow(); i(overlaps.count);
  for (int k = 0; k < overlaps.count; k++) shapeRef(overlaps.ids[k]);
  i(stats.nodeVisits); i(stats.leafVisits); end();
}

static void overlapShape(b3Vec3 origin, const b3ShapeProxy* proxy, b3QueryFilter filter) {
  Overlaps overlaps = { 0 };
  b3TreeStats stats = b3World_OverlapShape(g_worldId, origin, proxy, filter, overlapFcn, &overlaps);
  name("query.overlapShape"); v3(origin); proxyOut(proxy); filterOut(filter); arrow(); i(overlaps.count);
  for (int k = 0; k < overlaps.count; k++) shapeRef(overlaps.ids[k]);
  i(stats.nodeVisits); i(stats.leafVisits); end();
}

static void collideMover(b3Vec3 origin, const b3Capsule* mover, b3QueryFilter filter) {
  Planes planes = { 0 };
  b3World_CollideMover(g_worldId, origin, mover, filter, planeFcn, &planes);
  name("query.collideMover"); v3(origin); capsuleOut(mover); filterOut(filter); arrow(); i(planes.count);
  for (int k = 0; k < planes.count; k++) {
    b3PlaneResult* p = planes.planes + k;
    shapeRef(planes.ids[k]); v3(p->plane.normal); f(p->plane.offset); v3(p->point); i(p->triangleIndex);
    i(p->childIndex); i(p->materialIndex);
  }
  end();
}

static void castMover(b3Vec3 origin, const b3Capsule* mover, b3Vec3 translation, b3QueryFilter filter, Ignore ignore) {
  float fraction = b3World_CastMover(g_worldId, origin, mover, translation, filter, moverFilterFcn, &ignore);
  name("query.castMover"); v3(origin); capsuleOut(mover); v3(translation); filterOut(filter); i(ignore.count);
  for (int k = 0; k < ignore.count; k++) shapeRef(ignore.ids[k]);
  arrow(); f(fraction); end();
}

/* --- The surface shapes --- */

static b3MeshData* g_meshes[8];
static int g_meshCount;
static b3HeightFieldData* g_fields[8];
static int g_fieldCount;
static b3CompoundData* g_compounds[4];
static int g_compoundCount;

static void tf7(b3Transform t) { v3(t.p); q4(t.q); }

static void gridMeshShape(b3BodyId body, b3ShapeDef def, int xCount, int zCount, float cellWidth, b3Vec3 scale) {
  b3MeshData* mesh = b3CreateGridMesh(xCount, zCount, cellWidth, 1, true);
  g_meshes[g_meshCount++] = mesh;
  name("shape.mesh"); bodyRef(body); defOut(&def); i(0); i(xCount); i(zCount); f(cellWidth); i(1); i(1); v3(scale); arrow();
  b3ShapeId id = b3CreateMeshShape(body, &def, mesh, scale);
  shapeRef(id); end();
}

static void waveShape(b3BodyId body, b3ShapeDef def, int rows, int columns, b3Vec3 scale, float rowFrequency,
                      float columnFrequency, bool makeHoles) {
  b3HeightFieldData* field = b3CreateWave(rows, columns, scale, rowFrequency, columnFrequency, makeHoles);
  g_fields[g_fieldCount++] = field;
  name("shape.heightField"); bodyRef(body); defOut(&def); i(0); i(rows); i(columns); v3(scale); f(rowFrequency);
  f(columnFrequency); i(makeHoles); arrow();
  b3ShapeId id = b3CreateHeightFieldShape(body, &def, field);
  shapeRef(id); end();
}

static void flatFieldShape(b3BodyId body, b3ShapeDef def, int rows, int columns, b3Vec3 scale, bool makeHoles) {
  b3HeightFieldData* field = b3CreateGrid(rows, columns, scale, makeHoles);
  g_fields[g_fieldCount++] = field;
  name("shape.heightField"); bodyRef(body); defOut(&def); i(1); i(rows); i(columns); v3(scale); i(makeHoles); arrow();
  b3ShapeId id = b3CreateHeightFieldShape(body, &def, field);
  shapeRef(id); end();
}

static b3SurfaceMaterial idMaterial(uint64_t id, float friction) {
  b3SurfaceMaterial m = b3DefaultSurfaceMaterial();
  m.userMaterialId = id;
  m.friction = friction;
  return m;
}

/* A compound of two capsule rails, four boxes (one turned), a box-mesh
 * ramp and two sphere bumps, each with its own material id. */
static void compoundShape(b3BodyId body, b3ShapeDef def) {
  b3CompoundCapsuleDef capsules[2];
  b3CompoundHullDef hulls[4];
  b3BoxHull boxes[4];
  float halfs[4][3];
  b3CompoundMeshDef meshes[1];
  b3SurfaceMaterial meshMaterials[1];
  b3Vec3 meshCenter = { 0.0f, 0.0f, 0.0f };
  b3Vec3 meshExtent = { 1.5f, 0.2f, 1.0f };
  b3CompoundSphereDef spheres[2];

  for (int k = 0; k < 2; k++) {
    float z = k == 0 ? -2.5f : 2.5f;
    capsules[k].capsule = (b3Capsule){ { -3.0f, 0.3f, z }, { 3.0f, 0.3f, z }, 0.3f };
    capsules[k].material = idMaterial(100 + (uint64_t)k, 0.6f);
  }
  for (int k = 0; k < 4; k++) {
    halfs[k][0] = 1.0f; halfs[k][1] = 0.25f; halfs[k][2] = 1.0f;
    boxes[k] = b3MakeTransformedBoxHull(1.0f, 0.25f, 1.0f, b3Transform_identity);
    hulls[k].hull = &boxes[k].base;
    b3Quat turn = k == 3 ? b3MakeQuatFromAxisAngle((b3Vec3){ 0.0f, 1.0f, 0.0f }, 0.4f) : b3Quat_identity;
    hulls[k].transform = (b3Transform){ { -3.0f + 2.0f * (float)k, -0.25f, 0.0f }, turn };
    hulls[k].material = idMaterial(200 + (uint64_t)k, 0.5f);
  }
  b3MeshData* mesh = b3CreateBoxMesh(meshCenter, meshExtent, true);
  g_meshes[g_meshCount++] = mesh;
  meshes[0].meshData = mesh;
  meshes[0].transform = (b3Transform){ { 1.0f, 0.4f, -1.2f }, b3MakeQuatFromAxisAngle((b3Vec3){ 0.0f, 0.0f, 1.0f }, 0.2f) };
  meshes[0].scale = (b3Vec3){ 1.0f, 1.0f, 1.0f };
  meshMaterials[0] = idMaterial(300, 0.4f);
  meshes[0].materials = meshMaterials;
  meshes[0].materialCount = mesh->materialCount;
  for (int k = 0; k < 2; k++) {
    spheres[k].sphere = (b3Sphere){ { -1.5f + 3.0f * (float)k, 0.1f, 1.3f }, 0.35f };
    spheres[k].material = idMaterial(400 + (uint64_t)k, 0.6f);
  }
  b3CompoundDef compoundDef = { 0 };
  compoundDef.capsules = capsules; compoundDef.capsuleCount = 2;
  compoundDef.hulls = hulls; compoundDef.hullCount = 4;
  compoundDef.meshes = meshes; compoundDef.meshCount = 1;
  compoundDef.spheres = spheres; compoundDef.sphereCount = 2;

  b3CompoundData* compound = b3CreateCompound(&compoundDef);
  g_compounds[g_compoundCount++] = compound;
  name("shape.compound"); bodyRef(body); defOut(&def);
  i(compoundDef.capsuleCount);
  for (int k = 0; k < compoundDef.capsuleCount; k++) {
    const b3CompoundCapsuleDef* c = compoundDef.capsules + k;
    v3(c->capsule.center1); v3(c->capsule.center2); f(c->capsule.radius); material(c->material);
  }
  i(compoundDef.hullCount);
  for (int k = 0; k < compoundDef.hullCount; k++) {
    const b3CompoundHullDef* h = compoundDef.hulls + k;
    f(halfs[k][0]); f(halfs[k][1]); f(halfs[k][2]); tf7(h->transform); material(h->material);
  }
  i(compoundDef.meshCount);
  for (int k = 0; k < compoundDef.meshCount; k++) {
    const b3CompoundMeshDef* m = compoundDef.meshes + k;
    v3(meshCenter); v3(meshExtent); tf7(m->transform); v3(m->scale); i(m->materialCount);
    for (int n = 0; n < m->materialCount; n++) material(m->materials[n]);
  }
  i(compoundDef.sphereCount);
  for (int k = 0; k < compoundDef.sphereCount; k++) {
    const b3CompoundSphereDef* sp = compoundDef.spheres + k;
    v3(sp->sphere.center); f(sp->sphere.radius); material(sp->material);
  }
  arrow();
  b3ShapeId id = b3CreateBakedCompoundShape(body, &def, compound);
  shapeRef(id); end();
}

static b3ShapeId g_shapes[64];
static int g_shapeCount;

static void battery(void) {
  b3QueryFilter all = b3DefaultQueryFilter();
  b3QueryFilter skipTwo = { 1, ~(uint64_t)2, 7, NULL };
  b3QueryFilter onlyTwo = { 1, 2, 9, NULL };
  b3QueryFilter filters[3] = { all, skipTwo, onlyTwo };

  /* A fan of rays from three origins */
  b3Vec3 origins[3] = { { -6.0f, 4.0f, 0.5f }, { 0.2f, 6.0f, -0.3f }, { 5.0f, 1.0f, 2.0f } };
  for (int o = 0; o < 3; o++) {
    for (int k = 0; k < 8; k++) {
      float angle = 0.785398f * (float)k;
      b3Vec3 translation = { 12.0f * cosf(angle), -6.0f + 1.5f * (float)k, 12.0f * sinf(angle) };
      b3QueryFilter filter = filters[(o + k) % 3];
      rayClosest(origins[o], translation, filter);
      rayAll(origins[o], translation, filter);
    }
  }
  /* Rays starting inside shapes */
  rayClosest((b3Vec3){ 0.0f, -0.25f, 0.0f }, (b3Vec3){ 0.0f, 5.0f, 0.0f }, all);
  rayAll((b3Vec3){ 0.0f, -0.25f, 0.0f }, (b3Vec3){ 0.0f, 5.0f, 0.0f }, all);
  rayClosest((b3Vec3){ -4.0f, 1.0f, 0.0f }, (b3Vec3){ 8.0f, 0.0f, 0.0f }, all);

  /* Shape casts: a sphere, a capsule and a box */
  b3Vec3 spherePoint = { 0.0f, 0.0f, 0.0f };
  b3ShapeProxy sphere = { &spherePoint, 1, 0.3f };
  b3Vec3 capsulePoints[2] = { { -0.4f, 0.0f, 0.0f }, { 0.4f, 0.0f, 0.0f } };
  b3ShapeProxy capsule = { capsulePoints, 2, 0.2f };
  b3Vec3 boxPoints[8];
  for (int k = 0; k < 8; k++) {
    boxPoints[k] = (b3Vec3){ (k & 1) ? 0.3f : -0.3f, (k & 2) ? 0.2f : -0.2f, (k & 4) ? 0.25f : -0.25f };
  }
  b3ShapeProxy box = { boxPoints, 8, 0.0f };
  b3ShapeProxy proxies[3] = { sphere, capsule, box };
  b3Vec3 castOrigins[4] = { { -7.0f, 3.0f, 0.0f }, { 3.0f, 5.0f, 1.0f }, { 0.0f, 0.6f, -5.0f }, { 2.0f, 2.0f, 2.0f } };
  b3Vec3 castTranslations[4] = { { 14.0f, -2.5f, 0.3f }, { -2.0f, -6.0f, -1.0f }, { 0.5f, 0.0f, 10.0f }, { -6.0f, 0.0f, -3.0f } };
  for (int p = 0; p < 3; p++) {
    for (int k = 0; k < 4; k++) {
      b3QueryFilter filter = filters[(p + k) % 3];
      shapeCast(castOrigins[k], proxies + p, castTranslations[k], filter, false);
      shapeCast(castOrigins[k], proxies + p, castTranslations[k], filter, true);
    }
  }

  /* Overlaps */
  b3AABB boxes[3] = { { { -1.0f, -1.0f, -1.0f }, { 1.0f, 1.0f, 1.0f } },
                      { { -8.0f, 0.0f, -3.0f }, { -2.0f, 3.0f, 3.0f } },
                      { { 2.0f, 0.5f, -1.0f }, { 7.0f, 5.0f, 4.0f } } };
  for (int k = 0; k < 3; k++) {
    for (int m = 0; m < 3; m++) overlapAabb(boxes[k], filters[m]);
  }
  b3Vec3 overlapOrigins[4] = { { -3.0f, 1.0f, 0.0f }, { 3.0f, 0.4f, 0.0f }, { 0.0f, 2.2f, 0.0f }, { 5.0f, 1.2f, 1.5f } };
  for (int p = 0; p < 3; p++) {
    for (int k = 0; k < 4; k++) overlapShape(overlapOrigins[k], proxies + p, filters[(p + k) % 3]);
  }

  /* The mover */
  b3Capsule mover = { { 0.0f, -0.5f, 0.0f }, { 0.0f, 0.5f, 0.0f }, 0.3f };
  b3Vec3 moverOrigins[5] = { { -3.0f, 0.75f, 0.0f }, { -3.0f, 0.82f, 0.4f }, { 3.0f, 1.0f, 0.2f },
                             { 0.0f, 1.9f, 0.0f }, { 5.0f, 0.9f, 1.6f } };
  for (int k = 0; k < 5; k++) {
    collideMover(moverOrigins[k], &mover, filters[k % 3]);
  }
  Ignore none = { 0 };
  Ignore ignoreFirst = { 0 };
  ignoreFirst.ids[0] = g_shapes[2];
  ignoreFirst.ids[1] = g_shapes[4];
  ignoreFirst.count = 2;
  b3Vec3 moverMoves[5] = { { 6.0f, 0.0f, 0.0f }, { 0.0f, -2.0f, 0.0f }, { -4.0f, 0.5f, -0.3f }, { 0.0f, -3.0f, 0.0f },
                           { -3.0f, 0.0f, -3.0f } };
  for (int k = 0; k < 5; k++) {
    castMover(moverOrigins[k], &mover, moverMoves[k], filters[k % 3], none);
    castMover(moverOrigins[k], &mover, moverMoves[k], filters[(k + 1) % 3], ignoreFirst);
  }
}

/* The battery against each surface: rays down, slanted and level; sphere,
 * capsule and box casts down and sideways; overlaps at the surface; mover
 * planes at three heights; mover casts down and sideways. */
static void surfaceBattery(const b3Vec3* centers, int centerCount) {
  b3QueryFilter all = b3DefaultQueryFilter();
  b3Vec3 spherePoint = { 0.0f, 0.0f, 0.0f };
  b3ShapeProxy sphere = { &spherePoint, 1, 0.3f };
  b3Vec3 capsulePoints[2] = { { -0.4f, 0.0f, 0.0f }, { 0.4f, 0.0f, 0.0f } };
  b3ShapeProxy capsule = { capsulePoints, 2, 0.2f };
  b3Vec3 boxPoints[8];
  for (int k = 0; k < 8; k++) {
    boxPoints[k] = (b3Vec3){ (k & 1) ? 0.3f : -0.3f, (k & 2) ? 0.2f : -0.2f, (k & 4) ? 0.25f : -0.25f };
  }
  b3ShapeProxy box = { boxPoints, 8, 0.0f };
  b3ShapeProxy proxies[3] = { sphere, capsule, box };
  b3Capsule mover = { { 0.0f, -0.5f, 0.0f }, { 0.0f, 0.5f, 0.0f }, 0.3f };
  b3Vec3 offsets[5] = { { 0.3f, 0.0f, 0.2f }, { -1.7f, 0.0f, 0.9f }, { 2.1f, 0.0f, -1.4f }, { -0.6f, 0.0f, -2.3f },
                        { 1.2f, 0.0f, 2.6f } };

  for (int c = 0; c < centerCount; c++) {
    b3Vec3 center = centers[c];
    for (int k = 0; k < 5; k++) {
      b3Vec3 above = b3Add(center, b3Add(offsets[k], (b3Vec3){ 0.0f, 4.0f, 0.0f }));
      rayClosest(above, (b3Vec3){ 0.0f, -8.0f, 0.0f }, all);
      rayAll(above, (b3Vec3){ 0.3f * (float)k - 0.6f, -8.0f, 0.5f }, all);
      b3Vec3 side = b3Add(center, b3Add(offsets[k], (b3Vec3){ -6.0f, 0.4f, 0.0f }));
      rayClosest(side, (b3Vec3){ 12.0f, -0.3f, 0.1f * (float)k }, all);

      b3ShapeProxy* proxy = proxies + (k % 3);
      shapeCast(above, proxy, (b3Vec3){ 0.2f, -7.0f, -0.1f }, all, false);
      shapeCast(above, proxy, (b3Vec3){ 0.2f, -7.0f, -0.1f }, all, true);
      shapeCast(side, proxy, (b3Vec3){ 12.0f, -0.5f, 0.3f }, all, true);

      b3Vec3 near = b3Add(center, b3Add(offsets[k], (b3Vec3){ 0.0f, 0.3f, 0.0f }));
      overlapShape(near, proxy, all);
      b3AABB region = { b3Sub(near, (b3Vec3){ 0.5f, 0.5f, 0.5f }), b3Add(near, (b3Vec3){ 0.5f, 0.5f, 0.5f }) };
      overlapAabb(region, all);

      for (int h = 0; h < 3; h++) {
        b3Vec3 standing = b3Add(center, b3Add(offsets[k], (b3Vec3){ 0.0f, 0.55f + 0.25f * (float)h, 0.0f }));
        collideMover(standing, &mover, all);
      }
      b3Vec3 high = b3Add(center, b3Add(offsets[k], (b3Vec3){ 0.0f, 2.5f, 0.0f }));
      Ignore none = { 0 };
      castMover(high, &mover, (b3Vec3){ 0.1f, -4.0f, 0.2f }, all, none);
      castMover(b3Add(high, (b3Vec3){ 0.0f, -1.5f, 0.0f }), &mover, (b3Vec3){ 3.0f, -0.5f, -1.0f }, all, none);
    }
  }
}

static void step(void) {
  b3World_Step(g_worldId, 1.0f / 60.0f, 4);
  name("world.step"); f(1.0f / 60.0f); i(4); arrow(); end();
}

static void bodies(void) {
  name("world.bodies"); arrow(); i(g_bodyCount);
  for (int k = 0; k < g_bodyCount; k++) {
    b3WorldTransform t = b3Body_GetTransform(g_bodies[k]);
    v3(t.p); q4(t.q);
  }
  end();
}

static b3ShapeId lastShape(b3BodyId body) {
  b3ShapeId ids[4];
  int count = b3Body_GetShapes(body, ids, 4);
  return ids[count - 1];
}

int main(void) {
  printf("# box3d oracle: world queries, commit %s, scalar, -ffp-contract=off\n", BOX3D_ORACLE_COMMIT);
  b3WorldDef worldDef = b3DefaultWorldDef();
  worldDef.gravity = (b3Vec3){ 0.0f, -10.0f, 0.0f };
  worldDef.workerCount = 1;
  g_worldId = b3CreateWorld(&worldDef);
  g_world = b3GetWorldFromId(g_worldId);
  name("world.scene"); v3(worldDef.gravity); i(worldDef.enableSleep); arrow(); end();

  b3ShapeDef plain = b3DefaultShapeDef();
  b3ShapeDef team = shapeDefIn(2);
  team.baseMaterial.userMaterialId = 77;

  b3BodyId ground = makeBody(b3_staticBody, (b3Vec3){ 0.0f, -0.5f, 0.0f });
  boxShapeAt(ground, plain, 10.0f, 0.5f, 10.0f, identityAt(0.0f, 0.0f, 0.0f));
  g_shapes[g_shapeCount++] = lastShape(ground);

  b3BodyId pillar = makeBody(b3_staticBody, (b3Vec3){ -3.0f, 1.0f, 0.0f });
  b3Transform tilted = { { 0.0f, 0.0f, 0.0f }, b3MakeQuatFromAxisAngle((b3Vec3){ 0.0f, 1.0f, 0.0f }, 0.5f) };
  boxShapeAt(pillar, plain, 0.4f, 1.0f, 0.4f, tilted);
  g_shapes[g_shapeCount++] = lastShape(pillar);

  b3BodyId ball = makeBody(b3_staticBody, (b3Vec3){ 3.0f, 1.0f, 0.0f });
  sphereShapeIn(ball, team, 0.6f);
  g_shapes[g_shapeCount++] = lastShape(ball);

  b3BodyId rod = makeMovingBody(b3_staticBody, (b3Vec3){ 0.0f, 2.0f, 0.0f },
                                b3MakeQuatFromAxisAngle((b3Vec3){ 0.0f, 0.0f, 1.0f }, 0.3f), b3Vec3_zero, b3Vec3_zero);
  capsuleShapeIn(rod, plain, 1.0f, 0.2f);
  g_shapes[g_shapeCount++] = lastShape(rod);

  b3BodyId platform = makeMovingBody(b3_kinematicBody, (b3Vec3){ 5.0f, 0.5f, 2.0f }, b3Quat_identity,
                                     (b3Vec3){ -0.5f, 0.0f, 0.0f }, (b3Vec3){ 0.0f, 0.4f, 0.0f });
  boxShapeAt(platform, team, 0.8f, 0.1f, 0.8f, identityAt(0.0f, 0.0f, 0.0f));
  g_shapes[g_shapeCount++] = lastShape(platform);

  for (int k = 0; k < 4; k++) {
    b3BodyId body = makeMovingBody(b3_dynamicBody, (b3Vec3){ -1.0f + 1.2f * (float)k, 3.0f + 0.7f * (float)k, 0.5f },
                                   b3MakeQuatFromAxisAngle((b3Vec3){ 1.0f, 0.0f, 0.0f }, 0.3f * (float)k),
                                   (b3Vec3){ 0.3f, 0.0f, -0.2f }, b3Vec3_zero);
    if (k % 2 == 0) {
      boxShapeAt(body, k == 2 ? team : plain, 0.3f, 0.3f, 0.3f, identityAt(0.0f, 0.0f, 0.0f));
    } else {
      sphereShapeIn(body, plain, 0.35f);
    }
    g_shapes[g_shapeCount++] = lastShape(body);
  }

  battery();
  for (int k = 0; k < 30; k++) step();
  bodies();
  battery();
  for (int k = 0; k < 60; k++) step();
  bodies();
  battery();

  b3DestroyWorld(g_worldId);

  /* The surfaces */
  printf("# scene surfaces\n");
  g_worldId = b3CreateWorld(&worldDef);
  g_world = b3GetWorldFromId(g_worldId);
  g_bodyCount = 0;
  name("world.scene"); v3(worldDef.gravity); i(worldDef.enableSleep); arrow(); end();
  b3Vec3 centers[5] = { { 0.0f, 0.0f, 0.0f }, { 14.0f, 0.0f, 0.0f }, { -14.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 14.0f },
                        { 0.0f, 0.0f, -14.0f } };
  {
    b3ShapeDef def = b3DefaultShapeDef();
    def.baseMaterial.userMaterialId = 11;
    b3BodyId grid = makeBody(b3_staticBody, (b3Vec3){ -6.0f, 0.0f, -6.0f });
    gridMeshShape(grid, def, 12, 12, 1.0f, (b3Vec3){ 1.0f, 1.0f, 1.0f });

    def.baseMaterial.userMaterialId = 12;
    b3BodyId mirrored = makeMovingBody(b3_staticBody, (b3Vec3){ 14.0f + 6.0f, 0.0f, 0.0f - 4.0f },
                                       b3MakeQuatFromAxisAngle((b3Vec3){ 0.0f, 0.0f, 1.0f }, 0.15f), b3Vec3_zero,
                                       b3Vec3_zero);
    gridMeshShape(mirrored, def, 8, 8, 1.0f, (b3Vec3){ -1.5f, 1.0f, 1.0f });

    def.baseMaterial.userMaterialId = 13;
    b3BodyId wave = makeBody(b3_staticBody, (b3Vec3){ -14.0f - 5.0f, -0.5f, -5.0f });
    waveShape(wave, def, 11, 11, (b3Vec3){ 1.0f, 0.6f, 1.0f }, 0.15f, 0.1f, true);

    def.baseMaterial.userMaterialId = 14;
    b3BodyId flat = makeBody(b3_staticBody, (b3Vec3){ -5.0f, 0.0f, 14.0f - 5.0f });
    flatFieldShape(flat, def, 11, 11, (b3Vec3){ 1.0f, 1.0f, 1.0f }, true);

    b3BodyId course = makeBody(b3_staticBody, (b3Vec3){ 0.0f, 0.0f, -14.0f });
    compoundShape(course, b3DefaultShapeDef());

    for (int k = 0; k < 5; k++) {
      b3BodyId body = makeMovingBody(b3_dynamicBody, b3Add(centers[k], (b3Vec3){ 0.5f, 1.5f, -0.5f }),
                                     b3Quat_identity, b3Vec3_zero, b3Vec3_zero);
      if (k % 2 == 0) {
        boxShapeAt(body, b3DefaultShapeDef(), 0.3f, 0.3f, 0.3f, identityAt(0.0f, 0.0f, 0.0f));
      } else {
        sphereShapeIn(body, b3DefaultShapeDef(), 0.3f);
      }
    }
  }
  surfaceBattery(centers, 5);
  for (int k = 0; k < 60; k++) step();
  bodies();
  surfaceBattery(centers, 5);
  b3DestroyWorld(g_worldId);

  for (int k = 0; k < g_meshCount; k++) b3DestroyMesh(g_meshes[k]);
  for (int k = 0; k < g_fieldCount; k++) b3DestroyHeightField(g_fields[k]);
  for (int k = 0; k < g_compoundCount; k++) b3DestroyCompound(g_compounds[k]);
  return 0;
}
