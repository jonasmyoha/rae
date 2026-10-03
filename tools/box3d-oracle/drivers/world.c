/* Box3D oracle driver: the world, bodies and shapes (docs/physics-rae-port-design.md
 * §4, P4a): a scripted sequence of public API calls (create world, bodies of
 * every type and state, sphere/capsule/hull shapes, destroy, set transform,
 * velocities, forces, impulses, mass data, damping) with the whole internal
 * world state dumped at checkpoints. No stepping.
 *
 * One line per operation, `<op> <inputs> -> <outputs>`, replayed in order by
 * fixture 954: floats as C hex floats (`%a`), integers as decimals, a uint64
 * as two decimals (high, low). A body or shape is named by its index (id - 1)
 * and generation.
 *
 *   transform  position(3) rotation(4)
 *   spec       a hull: `0 count point(3)*count` (b3CreateHull, 128 max) or
 *              `1 hx hy hz transform` (b3MakeTransformedBoxHull)
 *   material   friction restitution rollingResistance tangentVelocity(3)
 *              userMaterialId(u64) customColor
 *   filter     categoryBits(u64) maskBits(u64) groupIndex
 *   state      see dumpWorld below
 *
 * Compiled against the cached Box3D checkout by tools/box3d-oracle/oracle.sh,
 * never by a Rae build. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "body.h"
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
static void dumpWorld(void) {
  b3World* w = g_world;
  name("world.state"); arrow();
  pool(&w->bodyIdPool); pool(&w->shapeIdPool); pool(&w->solverSetIdPool); pool(&w->islandIdPool);
  i(w->bodies.count);
  for (int k = 0; k < w->bodies.count; k++) {
    b3Body* b = w->bodies.data + k;
    i(b->setIndex); i(b->localIndex); i(b->headContactKey); i(b->contactCount); i(b->headShapeId); i(b->shapeCount);
    i(b->headChainId); i(b->headJointKey); i(b->jointCount); i(b->islandId); i(b->islandIndex); f(b->sleepThreshold);
    f(b->sleepTime); f(b->sleepVelocity); f(b->safetyFactor); f(b->mass); m3(b->inertia); i(b->bodyMoveIndex); i(b->id);
    i(b->flags); i(b->type); i(b->generation);
  }
  i(w->solverSets.count);
  for (int k = 0; k < w->solverSets.count; k++) {
    b3SolverSet* s = w->solverSets.data + k;
    i(s->setIndex); i(s->bodySims.count);
    for (int n = 0; n < s->bodySims.count; n++) {
      b3BodySim* m = s->bodySims.data + n;
      tf(m->transform); v3(m->center); i(m->flags); f(m->minExtent); v3(m->maxExtent); q4(m->rotation0); v3(m->center0);
      v3(m->localCenter); v3(m->force); v3(m->torque); f(m->invMass); m3(m->invInertiaLocal); m3(m->invInertiaWorld);
      f(m->linearDamping); f(m->angularDamping); f(m->gravityScale); i(m->bodyId);
    }
    i(s->bodyStates.count);
    for (int n = 0; n < s->bodyStates.count; n++) {
      b3BodyState* st = s->bodyStates.data + n;
      v3(st->linearVelocity); v3(st->angularVelocity); v3(st->deltaPosition); i(st->flags); q4(st->deltaRotation);
    }
    i(s->islandSims.count);
    for (int n = 0; n < s->islandSims.count; n++) i(s->islandSims.data[n].islandId);
    i(s->contactIndices.count);
  }
  i(w->islands.count);
  for (int k = 0; k < w->islands.count; k++) {
    b3Island* is = w->islands.data + k;
    i(is->setIndex); i(is->localIndex); i(is->islandId); i(is->constraintRemoveCount); i(is->bodies.count);
    for (int n = 0; n < is->bodies.count; n++) i(is->bodies.data[n]);
  }
  i(w->shapes.count);
  for (int k = 0; k < w->shapes.count; k++) {
    b3Shape* s = w->shapes.data + k;
    i(s->id); i(s->bodyId); i(s->prevShapeId); i(s->nextShapeId); i(s->sensorIndex); i(s->proxyKey); i(s->type);
    f(s->density); f(s->explosionScale); f(s->aabbMargin); box(s->aabb); v3(s->localCentroid); i(s->materialCount);
    material(s->material); u64(s->filter.categoryBits); u64(s->filter.maskBits); i(s->filter.groupIndex); i(s->flags);
    i(s->generation); box(w->fatAABBs.data[k]);
    if (s->id < 0) continue;
    if (s->type == b3_sphereShape) { v3(s->sphere.center); f(s->sphere.radius); }
    else if (s->type == b3_capsuleShape) { v3(s->capsule.center1); v3(s->capsule.center2); f(s->capsule.radius); }
    else if (s->type == b3_hullShape) { i(s->hull->vertexCount); v3(s->hull->center); f(s->hull->volume); }
  }
  for (int t = 0; t < b3_bodyTypeCount; t++) tree(w->broadPhase.trees + t);
  end();
}

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

static void bodyQuery(b3BodyId id) {
  b3Vec3 point = next_vec(2.0f);
  name("body.query"); bodyRef(id); v3(point); arrow();
  b3WorldTransform t = b3Body_GetTransform(id);
  tf(t); v3(b3Body_GetLocalPoint(id, point)); v3(b3Body_GetWorldPoint(id, point)); v3(b3Body_GetLocalVector(id, point));
  v3(b3Body_GetWorldVector(id, point)); v3(b3Body_GetLinearVelocity(id)); v3(b3Body_GetAngularVelocity(id));
  v3(b3Body_GetLocalPointVelocity(id, point)); v3(b3Body_GetWorldPointVelocity(id, point)); f(b3Body_GetMass(id));
  f(b3Body_GetInverseMass(id)); v3(b3Body_GetLocalCenter(id)); v3(b3Body_GetWorldCenter(id));
  m3(b3Body_GetLocalRotationalInertia(id)); m3(b3Body_GetWorldInverseRotationalInertia(id)); i(b3Body_GetType(id));
  i(b3Body_IsAwake(id)); i(b3Body_IsEnabled(id)); f(b3Body_GetLinearDamping(id)); f(b3Body_GetAngularDamping(id));
  f(b3Body_GetGravityScale(id)); i(b3Body_GetShapeCount(id));
  end();
}

static b3BodyId pickBody(void) {
  for (;;) {
    b3BodyId id = g_bodies[next_int(g_bodyCount)];
    if (b3Body_IsValid(id)) return id;
  }
}

int main(void) {
  printf("# box3d oracle: world, bodies and shapes, commit %s, scalar, -ffp-contract=off\n", BOX3D_ORACLE_COMMIT);

  b3WorldDef worldDef = b3DefaultWorldDef();
  worldDef.gravity = (b3Vec3){ 0.0f, -9.75f, 0.25f };
  b3WorldId worldId = b3CreateWorld(&worldDef);
  g_world = b3GetWorldFromId(worldId);
  g_worldId = worldId;
  name("world.create"); v3(worldDef.gravity); arrow(); end();
  dumpWorld();

  /* The ground, a mix of bodies, every shape kind. */
  b3BodyId ground = createBody(b3_staticBody, (b3Vec3){ 0.0f, -1.0f, 0.0f }, b3Quat_identity, true, true, 0);
  hullShape(ground, false);
  sphereShape(ground);
  for (int k = 0; k < 14; k++) {
    b3BodyType type = k % 7 == 3 ? b3_kinematicBody : k % 7 == 5 ? b3_staticBody : b3_dynamicBody;
    bool isAwake = k % 4 != 1;
    bool isEnabled = k % 6 != 4;
    int locks = k % 5 == 2 ? (next_int(2) ? 56 : next_int(64)) : 0;
    b3BodyId body = createBody(type, next_vec(6.0f), next_quat(), isAwake, isEnabled, locks);
    int shapes = 1 + next_int(3);
    for (int n = 0; n < shapes; n++) {
      switch (next_int(5)) {
        case 0: sphereShape(body); break;
        case 1: capsuleShape(body, next_int(4) == 0); break;
        case 2: hullShape(body, false); break;
        case 3: hullShape(body, true); break;
        default: capsuleShape(body, false); break;
      }
    }
    if (k % 3 == 0) bodyQuery(body);
  }
  dumpWorld();

  /* Setters, forces, impulses (some on sleeping bodies, which wake). */
  for (int k = 0; k < 40; k++) {
    b3BodyId body = pickBody();
    bool wake = next_int(2) == 1;
    switch (next_int(14)) {
      case 0: {
        b3Vec3 p = next_vec(6.0f); b3Quat q = next_quat();
        b3Body_SetTransform(body, p, q);
        name("body.setTransform"); bodyRef(body); v3(p); q4(q); arrow(); end();
      } break;
      case 1: {
        b3Vec3 v = next_int(4) == 0 ? b3Vec3_zero : next_vec(3.0f);
        b3Body_SetLinearVelocity(body, v);
        name("body.setLinearVelocity"); bodyRef(body); v3(v); arrow(); end();
      } break;
      case 2: {
        b3Vec3 v = next_vec(3.0f);
        b3Body_SetAngularVelocity(body, v);
        name("body.setAngularVelocity"); bodyRef(body); v3(v); arrow(); end();
      } break;
      case 3: {
        b3WorldTransform target = { next_vec(6.0f), next_quat() };
        float dt = next_int(4) == 0 ? 0.0f : 1.0f / 60.0f;
        b3Body_SetTargetTransform(body, target, dt, wake);
        name("body.setTargetTransform"); bodyRef(body); tf(target); f(dt); i(wake); arrow(); end();
      } break;
      case 4: {
        b3Vec3 force = next_vec(50.0f), point = next_vec(6.0f);
        b3Body_ApplyForce(body, force, point, wake);
        name("body.applyForce"); bodyRef(body); v3(force); v3(point); i(wake); arrow(); end();
      } break;
      case 5: {
        b3Vec3 force = next_vec(50.0f);
        b3Body_ApplyForceToCenter(body, force, wake);
        name("body.applyForceToCenter"); bodyRef(body); v3(force); i(wake); arrow(); end();
      } break;
      case 6: {
        b3Vec3 torque = next_vec(20.0f);
        b3Body_ApplyTorque(body, torque, wake);
        name("body.applyTorque"); bodyRef(body); v3(torque); i(wake); arrow(); end();
      } break;
      case 7: {
        b3Vec3 impulse = next_vec(5.0f), point = next_vec(6.0f);
        if (next_int(6) == 0) impulse = b3MulSV(1.0e6f, impulse); /* over the speed cap */
        b3Body_ApplyLinearImpulse(body, impulse, point, wake);
        name("body.applyLinearImpulse"); bodyRef(body); v3(impulse); v3(point); i(wake); arrow(); end();
      } break;
      case 8: {
        b3Vec3 impulse = next_vec(5.0f);
        b3Body_ApplyLinearImpulseToCenter(body, impulse, wake);
        name("body.applyLinearImpulseToCenter"); bodyRef(body); v3(impulse); i(wake); arrow(); end();
      } break;
      case 9: {
        b3Vec3 impulse = next_vec(5.0f);
        b3Body_ApplyAngularImpulse(body, impulse, wake);
        name("body.applyAngularImpulse"); bodyRef(body); v3(impulse); i(wake); arrow(); end();
      } break;
      case 10: {
        b3MassData m = { 1.0f + (float)next_int(8), next_vec(0.5f),
                         b3MakeDiagonalMatrix(0.5f + (float)next_int(4), 0.5f + (float)next_int(4), 0.5f + (float)next_int(4)) };
        if (next_int(4) == 0) m.inertia = b3Mat3_zero;
        b3Body_SetMassData(body, m);
        name("body.setMassData"); bodyRef(body); f(m.mass); v3(m.center); m3(m.inertia); arrow(); end();
      } break;
      case 11: {
        b3Body_ApplyMassFromShapes(body);
        name("body.applyMassFromShapes"); bodyRef(body); arrow(); end();
      } break;
      case 12: {
        float a = (float)next_int(8) * 0.125f, b = (float)next_int(8) * 0.0625f, g = (float)next_int(5) * 0.5f;
        b3Body_SetLinearDamping(body, a);
        b3Body_SetAngularDamping(body, b);
        b3Body_SetGravityScale(body, g);
        name("body.setDamping"); bodyRef(body); f(a); f(b); f(g); arrow(); end();
      } break;
      default:
        bodyQuery(body);
        break;
    }
  }
  dumpWorld();

  /* Destroy shapes and bodies, then create more (ids and slots reused). */
  for (int k = 0; k < 6; k++) {
    b3ShapeId shape = g_shapes[next_int(g_shapeCount)];
    if (b3Shape_IsValid(shape) == false) continue;
    bool updateMass = next_int(3) != 0;
    b3DestroyShape(shape, updateMass);
    name("shape.destroy"); shapeRef(shape); i(updateMass); arrow(); end();
  }
  for (int k = 0; k < 4; k++) {
    b3BodyId body = pickBody();
    b3DestroyBody(body);
    name("body.destroy"); bodyRef(body); arrow(); end();
  }
  dumpWorld();
  for (int k = 0; k < 6; k++) {
    b3BodyId body = createBody(k % 3 == 0 ? b3_staticBody : b3_dynamicBody, next_vec(6.0f), next_quat(), k % 2 == 0, true, 0);
    sphereShape(body);
    if (k % 2 == 1) hullShape(body, k == 3);
    bodyQuery(body);
  }
  dumpWorld();

  b3DestroyWorld(worldId);
  return 0;
}
