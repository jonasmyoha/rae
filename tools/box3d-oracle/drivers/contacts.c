/* Box3D oracle driver: contacts, the constraint graph and contact islands
 * (docs/physics-rae-port-design.md §4, P4b): a world of static, kinematic and
 * dynamic bodies packed so their shapes touch, stepped with a ZERO time step
 * (b3World_Step then runs pair finding and the narrow phase and skips the
 * solver: the step stops after collide). Between steps bodies are moved by
 * small amounts (contact recycling) and large ones (contacts stop touching,
 * fat boxes separate), woken by velocity changes, put to sleep with
 * b3TrySleepIsland, re-massed, and shapes and bodies are destroyed.
 *
 * The world, body and shape operations use world.c's line formats. Added:
 *
 *   world.collide ->             b3World_Step( world, 0, 4 )
 *   island.trySleep islandId ->  b3TrySleepIsland
 *   contacts.state ->            see dumpContacts below
 *
 * Compiled against the cached Box3D checkout by tools/box3d-oracle/oracle.sh,
 * never by a Rae build. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "body.h"
#include "broad_phase.h"
#include "contact.h"
#include "constraint_graph.h"
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




static void point(const b3ManifoldPoint* mp) {
  v3(mp->anchorA); v3(mp->anchorB); f(mp->separation); f(mp->normalImpulse); f(mp->totalNormalImpulse);
  f(mp->normalVelocity); f(mp->baseSeparation); i(mp->featureId); i(mp->triangleIndex); i(mp->persisted);
}

static void edge(b3ContactEdge e) { i(e.bodyId); i(e.prevKey); i(e.nextKey); }

/* contacts.state: the contact id pool; every contact slot (a free one: its
 * id and generation); the 24 graph colours (body bit set blocks, convex
 * contact ids, contact specs); every solver set (body ids, contact ids,
 * island ids); every island (bodies, contact links); every body's contact
 * and island fields; the pair set's count. */
static void dumpContacts(void) {
  b3World* w = g_world;
  name("contacts.state"); arrow();
  pool(&w->contactIdPool);
  i(w->contacts.count);
  for (int k = 0; k < w->contacts.count; k++) {
    const b3Contact* c = w->contacts.data + k;
    i(c->contactId);
    if (c->contactId == B3_NULL_INDEX) { i(c->generation); continue; }
    i(c->setIndex); i(c->colorIndex); i(c->localIndex); edge(c->edges[0]); edge(c->edges[1]);
    i(c->shapeIdA); i(c->shapeIdB); i(c->childIndex); i(c->islandId); i(c->islandIndex);
    i(c->encodedBodySimA); i(c->encodedBodySimB); i(c->flags); i(c->manifoldCount);
    for (int m = 0; m < c->manifoldCount; m++) {
      const b3Manifold* mf = c->manifolds + m;
      v3(mf->normal); f(mf->twistImpulse); v3(mf->frictionImpulse); v3(mf->rollingImpulse); i(mf->pointCount);
      for (int p = 0; p < mf->pointCount; p++) point(mf->points + p);
    }
    q4(c->cachedRotationA); q4(c->cachedRotationB); tf(c->cachedRelativePose);
    f(c->friction); f(c->restitution); f(c->rollingResistance); v3(c->tangentVelocity); i(c->generation);
  }
  for (int k = 0; k < B3_GRAPH_COLOR_COUNT; k++) {
    const b3GraphColor* color = w->constraintGraph.colors + k;
    i(color->bodySet.blockCount);
    for (uint32_t b = 0; b < color->bodySet.blockCount; b++) u64(color->bodySet.bits[b]);
    i(color->convexContacts.count);
    for (int n = 0; n < color->convexContacts.count; n++) i(color->convexContacts.data[n]);
    i(color->contacts.count);
    for (int n = 0; n < color->contacts.count; n++) {
      i(color->contacts.data[n].contactId); i(color->contacts.data[n].manifoldStart); i(color->contacts.data[n].manifoldCount);
    }
  }
  i(w->solverSets.count);
  for (int k = 0; k < w->solverSets.count; k++) {
    const b3SolverSet* s = w->solverSets.data + k;
    i(s->setIndex);
    i(s->bodySims.count);
    for (int n = 0; n < s->bodySims.count; n++) i(s->bodySims.data[n].bodyId);
    i(s->contactIndices.count);
    for (int n = 0; n < s->contactIndices.count; n++) i(s->contactIndices.data[n]);
    i(s->islandSims.count);
    for (int n = 0; n < s->islandSims.count; n++) i(s->islandSims.data[n].islandId);
  }
  i(w->islands.count);
  for (int k = 0; k < w->islands.count; k++) {
    const b3Island* is = w->islands.data + k;
    i(is->setIndex); i(is->localIndex); i(is->islandId); i(is->constraintRemoveCount);
    i(is->bodies.count);
    for (int n = 0; n < is->bodies.count; n++) i(is->bodies.data[n]);
    i(is->contacts.count);
    for (int n = 0; n < is->contacts.count; n++) {
      i(is->contacts.data[n].contactId); i(is->contacts.data[n].bodyIdA); i(is->contacts.data[n].bodyIdB);
    }
  }
  i(w->bodies.count);
  for (int k = 0; k < w->bodies.count; k++) {
    const b3Body* b = w->bodies.data + k;
    i(b->setIndex); i(b->localIndex); i(b->headContactKey); i(b->contactCount); i(b->islandId); i(b->islandIndex);
    f(b->sleepTime); i(b->flags);
  }
  i(w->broadPhase.pairSet.count);
  end();
}

static void collideStep(void) {
  b3World_Step(g_worldId, 0.0f, 4);
  name("world.collide"); arrow(); end();
}

static void addShapes(b3BodyId body, int count) {
  for (int n = 0; n < count; n++) {
    switch (next_int(4)) {
      case 0: sphereShape(body); break;
      case 1: capsuleShape(body, false); break;
      default: hullShape(body, next_int(3) == 0); break;
    }
  }
}

static void moveBody(b3BodyId body, float range) {
  b3WorldTransform t = b3Body_GetTransform(body);
  b3Vec3 p = b3Add(t.p, next_vec(range));
  b3Quat q = next_int(4) == 0 ? next_quat() : t.q;
  b3Body_SetTransform(body, p, q);
  name("body.setTransform"); bodyRef(body); v3(p); q4(q); arrow(); end();
}

static bool liveBody(b3BodyId id) { return b3Body_IsValid(id); }

/* A wide flat box with the default shape def: one dynamic body touching
 * more bodies than there are dynamic colours, so contacts overflow. */
static void bigBox(b3BodyId body) {
  b3ShapeDef def = b3DefaultShapeDef();
  def.updateBodyMass = true;
  float hx = 3.0f, hy = 0.25f, hz = 3.0f;
  b3Transform boxTransform = { b3Vec3_zero, b3Quat_identity };
  b3BoxHull boxHull = b3MakeTransformedBoxHull(hx, hy, hz, boxTransform);
  b3ShapeId id = b3CreateHullShape(body, &def, &boxHull.base);
  name("shape.hull"); bodyRef(body); defOut(&def); i(1); f(hx); f(hy); f(hz); tf(boxTransform);
  arrow(); shapeRef(id); end();
  g_shapes[g_shapeCount++] = id;
}

static void sleepIslands(int count) {
  for (int n = 0; n < count; n++) {
    b3SolverSet* awake = g_world->solverSets.data + b3_awakeSet;
    if (awake->islandSims.count == 0) return;
    int islandId = awake->islandSims.data[next_int(awake->islandSims.count)].islandId;
    b3TrySleepIsland(g_world, islandId);
    name("island.trySleep"); i(islandId); arrow(); end();
  }
}

int main(void) {
  printf("# box3d oracle: contacts, graph colours and contact islands, commit %s, scalar, -ffp-contract=off\n",
         BOX3D_ORACLE_COMMIT);

  b3WorldDef worldDef = b3DefaultWorldDef();
  worldDef.gravity = (b3Vec3){ 0.0f, -10.0f, 0.0f };
  worldDef.workerCount = 1;
  b3WorldId worldId = b3CreateWorld(&worldDef);
  g_world = b3GetWorldFromId(worldId);
  g_worldId = worldId;
  name("world.create"); v3(worldDef.gravity); arrow(); end();

  /* An empty world collides nothing. */
  collideStep();

  /* Ground: two static bodies; a few kinematic movers. */
  for (int k = 0; k < 2; k++) {
    b3BodyId ground = createBody(b3_staticBody, (b3Vec3){ next_float(3.0f), -0.75f, next_float(3.0f) }, b3Quat_identity, true, true, 0);
    addShapes(ground, 2 + next_int(2));
  }
  for (int k = 0; k < 3; k++) {
    b3BodyId mover = createBody(b3_kinematicBody, next_vec(3.0f), next_quat(), true, true, 0);
    addShapes(mover, 1);
  }
  /* A packed crowd of dynamic bodies, some asleep, one disabled. */
  for (int k = 0; k < 20; k++) {
    bool isAwake = k % 3 != 1;
    bool isEnabled = k != 11;
    b3Vec3 p = { next_float(3.0f), next_float(1.0f), next_float(3.0f) };
    b3BodyId body = createBody(b3_dynamicBody, p, next_quat(), isAwake, isEnabled, 0);
    addShapes(body, 1 + next_int(2));
  }
  {
    b3BodyId big = createBody(b3_dynamicBody, (b3Vec3){ 0.0f, 0.0f, 0.0f }, b3Quat_identity, true, true, 0);
    bigBox(big);
  }
  collideStep();
  dumpContacts();
  /* Nothing moved: the contacts update again (recycling where enabled). */
  collideStep();
  dumpContacts();

  for (int round = 0; round < 30; round++) {
    int actions = 1 + next_int(5);
    for (int a = 0; a < actions; a++) {
      b3BodyId body = g_bodies[next_int(g_bodyCount)];
      if (liveBody(body) == false || b3Body_IsEnabled(body) == false) continue;
      int kind = next_int(10);
      if (kind <= 3) {
        moveBody(body, 0.015625f);
      } else if (kind <= 5) {
        moveBody(body, 1.0f);
      } else if (kind == 6) {
        b3Vec3 v = next_vec(1.0f);
        b3Body_SetLinearVelocity(body, v);
        name("body.setLinearVelocity"); bodyRef(body); v3(v); arrow(); end();
      } else if (kind == 7) {
        b3SolverSet* awake = g_world->solverSets.data + b3_awakeSet;
        if (awake->islandSims.count > 0) {
          int islandId = awake->islandSims.data[next_int(awake->islandSims.count)].islandId;
          b3TrySleepIsland(g_world, islandId);
          name("island.trySleep"); i(islandId); arrow(); end();
        }
      } else if (kind == 8) {
        b3Body_ApplyMassFromShapes(body);
        name("body.applyMassFromShapes"); bodyRef(body); arrow(); end();
      } else if (round % 7 == 5) {
        b3DestroyBody(body);
        name("body.destroy"); bodyRef(body); arrow(); end();
      } else if (b3Body_GetShapeCount(body) > 1) {
        b3ShapeId shapes[8];
        int count = b3Body_GetShapes(body, shapes, 8);
        b3ShapeId shape = shapes[next_int(count)];
        bool updateMass = next_int(2) == 0;
        b3DestroyShape(shape, updateMass);
        name("shape.destroy"); shapeRef(shape); i(updateMass); arrow(); end();
      }
    }
    if (round % 4 == 1) {
      sleepIslands(3);
      dumpContacts();
    }
    if (round % 10 == 9) {
      b3BodyId body = createBody(b3_dynamicBody, next_vec(3.0f), next_quat(), round % 20 == 9, true, 0);
      addShapes(body, 1 + next_int(2));
    }
    collideStep();
    if (round % 3 == 2) dumpContacts();
  }

  b3DestroyWorld(worldId);
  return 0;
}
