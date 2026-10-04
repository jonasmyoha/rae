/* Box3D oracle driver: broad-phase pair finding (docs/physics-rae-port-design.md
 * §4, P3b): a world of static, kinematic and dynamic bodies packed close
 * enough to overlap, b3UpdateBroadPhasePairs called directly (workerCount 1,
 * no solver step), bodies moved with b3Body_SetTransform between updates.
 *
 * The world, body and shape operations use the same line formats as
 * world.c (see there). Added here:
 *
 *   pairs.update ->  count key(u64)*count   the pair keys of the contacts
 *                    the update created, in creation order
 *   pairs.state  ->  the pair set (capacity count (key(u64) hash)*capacity)
 *                    then the static, kinematic and dynamic trees (as in
 *                    world.c's tree dump)
 *
 * A second world then has static compound bodies (P5d) among the moving
 * crowd: a compound pair becomes one pair per child the other shape's box
 * overlaps (b3EmitCompoundPairs), dropped as a whole when b3ShouldCreatePair
 * refuses. Its line, in compoundscenes.c's format:
 *
 *   shape.compound body def <compound.c's create arguments> -> shape
 *
 * No shape or body is destroyed: in Box3D that destroys contacts, which is
 * P4b. The filters vary (categories, masks, groups) so b3ShouldCreatePair
 * rejects pairs; shapes share bodies so same-body pairs are rejected too.
 *
 * Compiled against the cached Box3D checkout by tools/box3d-oracle/oracle.sh,
 * never by a Rae build. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "body.h"
#include "broad_phase.h"
#include "contact.h"
#include "island.h"
#include "physics_world.h"
#include "shape.h"
#include "solver_set.h"

#include "box3d/box3d.h"
#include "box3d/collision.h"
#include "box3d/math_functions.h"

static uint32_t g_state = 0x510e527fu;

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

static b3Quat next_quat(void) {
  b3Vec3 axis = b3Normalize(next_nonzero_vec(4.0f));
  return b3MakeQuatFromAxisAngle(axis, next_float(3.0f));
}

static void f(float x) { printf(" %a", (double)x); }
static void i(long long x) { printf(" %lld", x); }
static void u64(uint64_t x) { printf(" %u %u", (unsigned)(x >> 32), (unsigned)(x & 0xffffffffu)); }
static void v3(b3Vec3 v) { f(v.x); f(v.y); f(v.z); }
static void q4(b3Quat q) { v3(q.v); f(q.s); }
static void m3(b3Matrix3 m) { v3(m.cx); v3(m.cy); v3(m.cz); }
static void tf(b3Transform t) { v3(t.p); q4(t.q); }
static void box(b3AABB a) { v3(a.lowerBound); v3(a.upperBound); }

static void name(const char* n) { printf("%s", n); }
static void arrow(void) { printf(" ->"); }
static void end(void) { printf("\n"); }

static b3World* g_world;
static b3WorldId g_worldId;

static void pool(const b3IdPool* p) {
  i(p->nextIndex); i(p->freeArray.count);
  for (int k = 0; k < p->freeArray.count; k++) i(p->freeArray.data[k]);
}

static void tree(const b3DynamicTree* t) {
  i(t->nodeEnd); i(t->pairFreeList); i(t->proxyCount); i(t->proxyCapacity); i(t->proxyFreeList); i(t->dfsOrdered);
  for (int k = 0; k < t->nodeEnd; k++) { box(t->nodes[k].aabb); i(t->nodes[k].flagIndex); i(t->nodes[k].height); }
  for (int k = 0; k < t->nodeEnd; k++) i(t->parents[k]);
  for (int k = 0; k < t->proxyCapacity; k++) {
    i(t->proxies[k].node); i(t->proxies[k].next); u64(t->proxies[k].categoryBits); u64(t->proxies[k].userData);
  }
}

static void material(b3SurfaceMaterial m) {
  f(m.friction); f(m.restitution); f(m.rollingResistance); v3(m.tangentVelocity); u64(m.userMaterialId); i(m.customColor);
}

/* The world's state: the id pools (body, shape, solver set, island), every
   body slot, every solver set (body sims, body states, island sims, contact
   count), every island, every shape slot with its fat AABB, then the three
   broad-phase trees. */
static b3BodyId g_bodies[64];
static int g_bodyCount;
static b3ShapeId g_shapes[128];
static int g_shapeCount;

static void bodyRef(b3BodyId id) { i(id.index1 - 1); i(id.generation); }
static void shapeRef(b3ShapeId id) { i(id.index1 - 1); i(id.generation); }

static b3BodyId createBody(b3BodyType type, b3Vec3 position, b3Quat rotation, bool isAwake, bool isEnabled, int locks) {
  b3BodyDef def = b3DefaultBodyDef();
  def.type = type;
  def.position = position;
  def.rotation = rotation;
  def.linearVelocity = next_vec(2.0f);
  def.angularVelocity = next_vec(1.0f);
  def.linearDamping = (float)next_int(4) * 0.125f;
  def.angularDamping = (float)next_int(4) * 0.0625f;
  def.gravityScale = next_int(4) == 0 ? 0.5f : 1.0f;
  def.isAwake = isAwake;
  def.isEnabled = isEnabled;
  def.enableSleep = next_int(5) != 0;
  def.isBullet = next_int(6) == 0;
  def.allowFastRotation = next_int(6) == 0;
  def.enableContactRecycling = next_int(6) != 0;
  def.motionLocks.linearX = (locks & 1) != 0;
  def.motionLocks.linearY = (locks & 2) != 0;
  def.motionLocks.linearZ = (locks & 4) != 0;
  def.motionLocks.angularX = (locks & 8) != 0;
  def.motionLocks.angularY = (locks & 16) != 0;
  def.motionLocks.angularZ = (locks & 32) != 0;
  b3BodyId id = b3CreateBody(g_worldId, &def);
  name("body.create"); i(def.type); v3(def.position); q4(def.rotation); v3(def.linearVelocity); v3(def.angularVelocity);
  f(def.linearDamping); f(def.angularDamping); f(def.gravityScale); f(def.sleepThreshold); f(def.safetyFactor); i(locks);
  i(def.enableSleep); i(def.isAwake); i(def.isBullet); i(def.isEnabled); i(def.allowFastRotation);
  i(def.enableContactRecycling); arrow(); bodyRef(id); end();
  g_bodies[g_bodyCount++] = id;
  return id;
}

static b3ShapeDef shapeDef(bool updateMass) {
  b3ShapeDef def = b3DefaultShapeDef();
  def.density = next_int(5) == 0 ? 0.0f : 250.0f + (float)next_int(8) * 125.0f;
  def.baseMaterial.friction = (float)next_int(8) * 0.125f;
  def.baseMaterial.restitution = (float)next_int(4) * 0.25f;
  def.baseMaterial.rollingResistance = (float)next_int(4) * 0.0625f;
  def.baseMaterial.tangentVelocity = next_int(4) == 0 ? next_vec(1.0f) : b3Vec3_zero;
  def.baseMaterial.userMaterialId = next_int(3) == 0 ? ((uint64_t)next_u32() << 32) | next_u32() : 0;
  def.baseMaterial.customColor = next_u32();
  def.explosionScale = next_int(3) == 0 ? 2.0f : 1.0f;
  if (next_int(3) == 0) {
    def.filter.categoryBits = (uint64_t)1 << next_int(64);
    def.filter.maskBits = ~((uint64_t)1 << next_int(64));
    def.filter.groupIndex = next_int(5) - 2;
  }
  def.enableSensorEvents = next_int(2) == 1;
  def.enableContactEvents = next_int(3) != 0;
  def.enableHitEvents = next_int(3) == 0;
  def.enablePreSolveEvents = next_int(4) == 0;
  def.enableCustomFiltering = next_int(4) == 0;
  def.enableSpeculativeContact = next_int(5) != 0;
  def.invokeContactCreation = next_int(3) != 0;
  def.updateBodyMass = updateMass;
  return def;
}

static void defOut(const b3ShapeDef* def) {
  material(def->baseMaterial); f(def->density); f(def->explosionScale); u64(def->filter.categoryBits);
  u64(def->filter.maskBits); i(def->filter.groupIndex); i(def->enableSensorEvents); i(def->enableContactEvents);
  i(def->enableHitEvents); i(def->enablePreSolveEvents); i(def->enableCustomFiltering); i(def->enableSpeculativeContact);
  i(def->invokeContactCreation); i(def->updateBodyMass);
}

static void sphereShape(b3BodyId body) {
  b3ShapeDef def = shapeDef(next_int(5) != 0);
  b3Sphere s = { next_vec(0.5f), 0.125f + (float)next_int(8) * 0.125f };
  b3ShapeId id = b3CreateSphereShape(body, &def, &s);
  name("shape.sphere"); bodyRef(body); defOut(&def); v3(s.center); f(s.radius); arrow(); shapeRef(id); end();
  g_shapes[g_shapeCount++] = id;
}

static void capsuleShape(b3BodyId body, bool degenerate) {
  b3ShapeDef def = shapeDef(next_int(5) != 0);
  b3Capsule c = { next_vec(0.5f), next_vec(0.5f), 0.125f + (float)next_int(4) * 0.125f };
  if (degenerate) c.center2 = b3Add(c.center1, (b3Vec3){ 0.001953125f, 0.0f, 0.0f });
  b3ShapeId id = b3CreateCapsuleShape(body, &def, &c);
  name("shape.capsule"); bodyRef(body); defOut(&def); v3(c.center1); v3(c.center2); f(c.radius); arrow(); shapeRef(id);
  end();
  g_shapes[g_shapeCount++] = id;
}

static void hullShape(b3BodyId body, bool transformed) {
  b3ShapeDef def = shapeDef(next_int(5) != 0);
  int kind = next_int(2);
  b3Vec3 points[24];
  int count = 0;
  float hx = 0.25f + (float)next_int(6) * 0.125f, hy = 0.25f + (float)next_int(6) * 0.125f, hz = 0.25f + (float)next_int(6) * 0.125f;
  b3Transform boxTransform = { next_vec(0.5f), next_int(2) ? b3Quat_identity : next_quat() };
  b3BoxHull boxHull;
  b3HullData* hull;
  if (kind == 0) {
    count = 8 + next_int(16);
    for (int k = 0; k < count; k++) points[k] = next_vec(0.75f);
    hull = b3CreateHull(points, count, 128);
  } else {
    boxHull = b3MakeTransformedBoxHull(hx, hy, hz, boxTransform);
    hull = &boxHull.base;
  }
  b3ShapeId id;
  b3Transform shapeTransform = { next_vec(0.5f), next_quat() };
  b3Vec3 scale = { 0.5f + (float)next_int(4) * 0.25f, 0.5f + (float)next_int(4) * 0.25f, next_int(3) == 0 ? -1.0f : 1.0f };
  if (transformed) id = b3CreateTransformedHullShape(body, &def, hull, shapeTransform, scale);
  else id = b3CreateHullShape(body, &def, hull);
  name(transformed ? "shape.transformedHull" : "shape.hull"); bodyRef(body); defOut(&def); i(kind);
  if (kind == 0) { i(count); for (int k = 0; k < count; k++) v3(points[k]); }
  else { f(hx); f(hy); f(hz); tf(boxTransform); }
  if (transformed) { tf(shapeTransform); v3(scale); }
  arrow(); shapeRef(id); end();
  g_shapes[g_shapeCount++] = id;
  if (kind == 0) b3DestroyHull(hull);
}



static void tf7(b3Transform t) { v3(t.p); q4(t.q); }

static b3MeshData* g_meshes[16];
static int g_meshCount;
static b3CompoundData* g_compounds[8];
static int g_compoundCount;

/* A random compound of capsules, box hulls, box meshes and spheres around
   the origin of its body, written in compound.c's create format. */
static void compoundShape(b3BodyId body) {
  b3CompoundCapsuleDef capsules[4];
  b3CompoundHullDef hulls[6];
  b3BoxHull boxes[6];
  float halfs[6][3];
  b3CompoundMeshDef meshes[1];
  b3SurfaceMaterial meshMaterials[1];
  b3Vec3 meshCenter = b3Vec3_zero;
  b3Vec3 meshExtent = b3Vec3_zero;
  b3CompoundSphereDef spheres[4];
  int capsuleCount = next_int(4);
  int hullCount = 1 + next_int(6);
  int meshCount = next_int(2);
  int sphereCount = next_int(4);
  for (int k = 0; k < capsuleCount; k++) {
    b3Vec3 c = next_vec(2.5f);
    b3Vec3 d = next_vec(0.75f);
    capsules[k].capsule = (b3Capsule){ b3Sub(c, d), b3Add(c, d), 0.1f + 0.25f * (float)next_int(3) };
    capsules[k].material = b3DefaultSurfaceMaterial();
    capsules[k].material.friction = 0.25f * (float)next_int(3);
  }
  for (int k = 0; k < hullCount; k++) {
    halfs[k][0] = 0.25f + 0.25f * (float)next_int(3);
    halfs[k][1] = 0.25f + 0.25f * (float)next_int(3);
    halfs[k][2] = 0.25f + 0.25f * (float)next_int(3);
    boxes[k] = b3MakeTransformedBoxHull(halfs[k][0], halfs[k][1], halfs[k][2], b3Transform_identity);
    hulls[k].hull = &boxes[k].base;
    hulls[k].transform = (b3Transform){ next_vec(2.5f), next_int(2) ? b3Quat_identity : next_quat() };
    hulls[k].material = b3DefaultSurfaceMaterial();
    hulls[k].material.friction = 0.25f * (float)next_int(3);
  }
  for (int k = 0; k < meshCount; k++) {
    meshCenter = next_vec(0.5f);
    meshExtent = (b3Vec3){ 0.5f + 0.25f * (float)next_int(3), 0.25f, 0.5f + 0.25f * (float)next_int(3) };
    b3MeshData* mesh = b3CreateBoxMesh(meshCenter, meshExtent, true);
    g_meshes[g_meshCount++] = mesh;
    meshes[k].meshData = mesh;
    meshes[k].transform = (b3Transform){ next_vec(2.0f), next_quat() };
    meshes[k].scale = (b3Vec3){ 1.0f, 1.0f, 1.0f };
    meshMaterials[0] = b3DefaultSurfaceMaterial();
    meshes[k].materials = meshMaterials;
    meshes[k].materialCount = mesh->materialCount;
  }
  for (int k = 0; k < sphereCount; k++) {
    spheres[k].sphere = (b3Sphere){ next_vec(2.5f), 0.2f + 0.2f * (float)next_int(3) };
    spheres[k].material = b3DefaultSurfaceMaterial();
  }
  b3CompoundDef compoundDef = { 0 };
  compoundDef.capsules = capsules; compoundDef.capsuleCount = capsuleCount;
  compoundDef.hulls = hulls; compoundDef.hullCount = hullCount;
  compoundDef.meshes = meshes; compoundDef.meshCount = meshCount;
  compoundDef.spheres = spheres; compoundDef.sphereCount = sphereCount;
  b3CompoundData* compound = b3CreateCompound(&compoundDef);
  g_compounds[g_compoundCount++] = compound;

  b3ShapeDef def = shapeDef(false);
  def.enableSensorEvents = false;
  /* Every other compound accepts only the low 16 categories: its pairs with
     shapes of a higher category fail b3ShouldCreatePair and the whole child
     batch is dropped. */
  if (g_compoundCount % 2 == 1) def.filter.maskBits = 0xffffull;
  name("shape.compound"); bodyRef(body); defOut(&def);
  i(capsuleCount);
  for (int k = 0; k < capsuleCount; k++) {
    v3(capsules[k].capsule.center1); v3(capsules[k].capsule.center2); f(capsules[k].capsule.radius);
    material(capsules[k].material);
  }
  i(hullCount);
  for (int k = 0; k < hullCount; k++) {
    f(halfs[k][0]); f(halfs[k][1]); f(halfs[k][2]); tf7(hulls[k].transform); material(hulls[k].material);
  }
  i(meshCount);
  for (int k = 0; k < meshCount; k++) {
    v3(meshCenter); v3(meshExtent); tf7(meshes[k].transform); v3(meshes[k].scale); i(meshes[k].materialCount);
    for (int n = 0; n < meshes[k].materialCount; n++) material(meshMaterials[0]);
  }
  i(sphereCount);
  for (int k = 0; k < sphereCount; k++) {
    v3(spheres[k].sphere.center); f(spheres[k].sphere.radius); material(spheres[k].material);
  }
  arrow();
  b3ShapeId id = b3CreateBakedCompoundShape(body, &def, compound);
  shapeRef(id); end();
  g_shapes[g_shapeCount++] = id;
}

static void pairsUpdate(void) {
  int before = g_world->contacts.count;
  b3UpdateBroadPhasePairs(g_world);
  int after = g_world->contacts.count;
  name("pairs.update"); arrow(); i(after - before);
  for (int k = before; k < after; k++) {
    const b3Contact* contact = g_world->contacts.data + k;
    u64(b3ShapePairKey(contact->shapeIdA, contact->shapeIdB, contact->childIndex));
  }
  end();
}

static void pairsState(void) {
  const b3HashSet* s = &g_world->broadPhase.pairSet;
  name("pairs.state"); arrow(); i(s->capacity); i(s->count);
  for (uint32_t k = 0; k < s->capacity; k++) { u64(s->items[k].key); i(s->items[k].hash); }
  for (int t = 0; t < 3; t++) tree(g_world->broadPhase.trees + t);
  end();
}

static void addShapes(b3BodyId body, int count) {
  for (int n = 0; n < count; n++) {
    switch (next_int(4)) {
      case 0: sphereShape(body); break;
      case 1: capsuleShape(body, false); break;
      case 2: hullShape(body, false); break;
      default: hullShape(body, true); break;
    }
  }
}

static void moveBody(b3BodyId body, float range) {
  b3WorldTransform t = b3Body_GetTransform(body);
  b3Vec3 p = b3Add(t.p, next_vec(range));
  b3Quat q = next_int(3) == 0 ? next_quat() : t.q;
  b3Body_SetTransform(body, p, q);
  name("body.setTransform"); bodyRef(body); v3(p); q4(q); arrow(); end();
}

int main(void) {
  printf("# box3d oracle: broad-phase pair finding, commit %s, scalar, -ffp-contract=off\n", BOX3D_ORACLE_COMMIT);

  b3WorldDef worldDef = b3DefaultWorldDef();
  worldDef.gravity = (b3Vec3){ 0.0f, -10.0f, 0.0f };
  worldDef.workerCount = 1;
  b3WorldId worldId = b3CreateWorld(&worldDef);
  g_world = b3GetWorldFromId(worldId);
  g_worldId = worldId;
  name("world.create"); v3(worldDef.gravity); arrow(); end();

  /* An empty world finds nothing. */
  pairsUpdate();

  /* Ground: three static bodies; a few kinematic movers. */
  for (int k = 0; k < 3; k++) {
    b3BodyId ground = createBody(b3_staticBody, next_vec(3.0f), next_int(2) ? b3Quat_identity : next_quat(), true, true, 0);
    addShapes(ground, 1 + next_int(3));
  }
  for (int k = 0; k < 3; k++) {
    b3BodyId mover = createBody(b3_kinematicBody, next_vec(3.0f), next_quat(), true, true, 0);
    addShapes(mover, 1 + next_int(2));
  }
  /* A crowd of dynamic bodies, some asleep, a few disabled. */
  for (int k = 0; k < 40; k++) {
    bool isAwake = k % 5 != 2;
    bool isEnabled = k % 13 != 7;
    b3BodyId body = createBody(b3_dynamicBody, next_vec(3.0f), next_quat(), isAwake, isEnabled, 0);
    addShapes(body, 1 + next_int(2));
  }
  pairsUpdate();
  pairsState();
  /* Nothing moved since: no new pairs, nothing rebuilt. */
  pairsUpdate();

  for (int round = 0; round < 24; round++) {
    int moves = 1 + next_int(8);
    for (int m = 0; m < moves; m++) {
      b3BodyId body = g_bodies[next_int(g_bodyCount)];
      if (b3Body_IsEnabled(body) == false) continue;
      moveBody(body, next_int(4) == 0 ? 3.0f : 0.5f);
    }
    if (round % 6 == 3) {
      b3BodyType type = round % 12 == 3 ? b3_staticBody : b3_dynamicBody;
      b3BodyId body = createBody(type, next_vec(3.0f), next_quat(), true, true, 0);
      addShapes(body, 1 + next_int(3));
    }
    pairsUpdate();
    if (round % 8 == 7) pairsState();
  }
  pairsState();

  b3DestroyWorld(worldId);

  /* Static compounds among a moving crowd. */
  worldId = b3CreateWorld(&worldDef);
  g_world = b3GetWorldFromId(worldId);
  g_worldId = worldId;
  g_bodyCount = 0;
  g_shapeCount = 0;
  name("world.create"); v3(worldDef.gravity); arrow(); end();
  for (int k = 0; k < 3; k++) {
    b3BodyId ground = createBody(b3_staticBody, next_vec(3.0f), next_int(2) ? b3Quat_identity : next_quat(), true, true, 0);
    compoundShape(ground);
    if (k == 1) addShapes(ground, 1);
  }
  for (int k = 0; k < 30; k++) {
    bool isAwake = k % 5 != 2;
    bool isEnabled = k % 13 != 7;
    b3BodyId body = createBody(b3_dynamicBody, next_vec(4.0f), next_quat(), isAwake, isEnabled, 0);
    addShapes(body, 1 + next_int(2));
  }
  pairsUpdate();
  pairsState();
  pairsUpdate();
  for (int round = 0; round < 24; round++) {
    int moves = 1 + next_int(8);
    for (int m = 0; m < moves; m++) {
      b3BodyId body = g_bodies[next_int(g_bodyCount)];
      if (b3Body_IsEnabled(body) == false) continue;
      if (b3Body_GetType(body) == b3_staticBody) continue;
      moveBody(body, next_int(4) == 0 ? 3.0f : 0.75f);
    }
    if (round % 8 == 5) {
      b3BodyId ground = createBody(b3_staticBody, next_vec(3.0f), next_quat(), true, true, 0);
      compoundShape(ground);
    }
    pairsUpdate();
    if (round % 8 == 7) pairsState();
  }
  pairsState();
  b3DestroyWorld(worldId);
  for (int k = 0; k < g_compoundCount; k++) b3DestroyCompound(g_compounds[k]);
  for (int k = 0; k < g_meshCount; k++) b3DestroyMesh(g_meshes[k]);
  return 0;
}
