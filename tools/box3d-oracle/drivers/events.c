/* Box3D oracle driver: sleep, island splitting, sensors and events
 * (docs/physics-rae-port-design.md §4, P4d): worlds stepped with b3World_Step
 * (1/60 s, 4 sub-steps, workerCount 1) with sleeping ENABLED:
 *
 *   sleep      spheres and a box stack fall asleep; a sphere is woken by a
 *              velocity, the stack by a box dropped on it
 *   split      a stack of four boxes knocked apart by an impulse: the island
 *              must split before its pieces can sleep
 *   sensor     no gravity: spheres cross a static sensor box, a fast bullet
 *              crosses a thin sensor (a continuous sensor hit), and the
 *              sensor shape is destroyed part way through
 *   events     spheres with contact and hit events bounce on the ground; one
 *              is destroyed while touching
 *
 * The world, body and shape operations use world.c's line formats, except
 * that shape lines carry isSensor after the shape definition. Each step is
 *
 *   world.step timeStep subSteps -> hash(u64) awakeBodyCount splitIslandId
 *       islandCount contactBegin contactEnd contactHit sensorBegin sensorEnd
 *       bodyMove
 *
 * (each list a count, then its events: shape ids, contact ids, points and
 * normals, approach speeds, material ids, body ids, fellAsleep and
 * transforms), and every 20 steps world.bodies dumps every body (zeros for a
 * destroyed one), as in scenes.c.
 *
 * Compiled against the cached Box3D checkout by tools/box3d-oracle/oracle.sh,
 * never by a Rae build. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "body.h"
#include "island.h"
#include "solver_set.h"
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

static void material(b3SurfaceMaterial m) {
  f(m.friction); f(m.restitution); f(m.rollingResistance); v3(m.tangentVelocity); u64(m.userMaterialId); i(m.customColor);
}

/* The world's state: the id pools (body, shape, solver set, island), every
   body slot, every solver set (body sims, body states, island sims, contact
   count), every island, every shape slot with its fat AABB, then the three
   broad-phase trees. */

static b3BodyId g_bodies[128];
static int g_bodyCount;

static void bodyRef(b3BodyId id) { i(id.index1 - 1); i(id.generation); }
static void shapeRef(b3ShapeId id) { i(id.index1 - 1); i(id.generation); }

static bool g_nextBullet;

static b3BodyId makeBody(b3BodyType type, b3Vec3 position, b3Quat rotation) {
  b3BodyDef def = b3DefaultBodyDef();
  def.isBullet = g_nextBullet;
  g_nextBullet = false;
  def.type = type;
  def.position = position;
  def.rotation = rotation;
  b3BodyId id = b3CreateBody(g_worldId, &def);
  name("body.create"); i(def.type); v3(def.position); q4(def.rotation); v3(def.linearVelocity); v3(def.angularVelocity);
  f(def.linearDamping); f(def.angularDamping); f(def.gravityScale); f(def.sleepThreshold); f(def.safetyFactor); i(0);
  i(def.enableSleep); i(def.isAwake); i(def.isBullet); i(def.isEnabled); i(def.allowFastRotation);
  i(def.enableContactRecycling); arrow(); bodyRef(id); end();
  g_bodies[g_bodyCount++] = id;
  return id;
}

static void defOut(const b3ShapeDef* def) {
  material(def->baseMaterial); f(def->density); f(def->explosionScale); u64(def->filter.categoryBits);
  u64(def->filter.maskBits); i(def->filter.groupIndex); i(def->enableSensorEvents); i(def->enableContactEvents);
  i(def->enableHitEvents); i(def->enablePreSolveEvents); i(def->enableCustomFiltering); i(def->enableSpeculativeContact);
  i(def->invokeContactCreation); i(def->updateBodyMass);
}

static void boxShape(b3BodyId body, b3ShapeDef def, float hx, float hy, float hz) {
  b3Transform identity = { b3Vec3_zero, b3Quat_identity };
  b3BoxHull boxHull = b3MakeTransformedBoxHull(hx, hy, hz, identity);
  b3ShapeId id = b3CreateHullShape(body, &def, &boxHull.base);
  name("shape.hull"); bodyRef(body); defOut(&def); i(def.isSensor); i(1); f(hx); f(hy); f(hz); tf(identity); arrow(); shapeRef(id); end();
}

static void sphereShape(b3BodyId body, b3ShapeDef def, float radius) {
  b3Sphere s = { b3Vec3_zero, radius };
  b3ShapeId id = b3CreateSphereShape(body, &def, &s);
  name("shape.sphere"); bodyRef(body); defOut(&def); i(def.isSensor); v3(s.center); f(s.radius); arrow(); shapeRef(id); end();
}

static uint64_t hashFloat(uint64_t hash, float value) {
  uint32_t bits;
  memcpy(&bits, &value, sizeof(bits));
  hash ^= bits;
  hash *= 0x100000001b3ull;
  return hash;
}

static void bodyValues(b3BodyId id, float* out) {
  b3WorldTransform t = b3Body_GetTransform(id);
  b3Vec3 v = b3Body_GetLinearVelocity(id);
  b3Vec3 w = b3Body_GetAngularVelocity(id);
  float values[13] = { t.p.x, t.p.y, t.p.z, t.q.v.x, t.q.v.y, t.q.v.z, t.q.s, v.x, v.y, v.z, w.x, w.y, w.z };
  memcpy(out, values, sizeof(values));
}

static void shapeIdOut(b3ShapeId id) { i(id.index1 - 1); i(id.generation); }
static void contactIdOut(b3ContactId id) { i(id.index1 - 1); i(id.generation); }

static void step(int stepIndex) {
  float timeStep = 1.0f / 60.0f;
  int subSteps = 4;
  b3World_Step(g_worldId, timeStep, subSteps);
  uint64_t hash = 0xcbf29ce484222325ull;
  for (int k = 0; k < g_bodyCount; k++) {
    if (b3Body_IsValid(g_bodies[k]) == false) continue;
    float values[13];
    bodyValues(g_bodies[k], values);
    for (int n = 0; n < 13; n++) hash = hashFloat(hash, values[n]);
  }
  name("world.step"); f(timeStep); i(subSteps); arrow(); u64(hash);
  i(g_world->solverSets.data[b3_awakeSet].bodySims.count); i(g_world->splitIslandId); i(b3GetIdCount(&g_world->islandIdPool));
  b3ContactEvents contacts = b3World_GetContactEvents(g_worldId);
  i(contacts.beginCount);
  for (int k = 0; k < contacts.beginCount; k++) {
    shapeIdOut(contacts.beginEvents[k].shapeIdA); shapeIdOut(contacts.beginEvents[k].shapeIdB); contactIdOut(contacts.beginEvents[k].contactId);
  }
  i(contacts.endCount);
  for (int k = 0; k < contacts.endCount; k++) {
    shapeIdOut(contacts.endEvents[k].shapeIdA); shapeIdOut(contacts.endEvents[k].shapeIdB); contactIdOut(contacts.endEvents[k].contactId);
  }
  i(contacts.hitCount);
  for (int k = 0; k < contacts.hitCount; k++) {
    const b3ContactHitEvent* e = contacts.hitEvents + k;
    shapeIdOut(e->shapeIdA); shapeIdOut(e->shapeIdB); contactIdOut(e->contactId); v3(e->point); v3(e->normal);
    f(e->approachSpeed); u64(e->userMaterialIdA); u64(e->userMaterialIdB);
  }
  b3SensorEvents sensors = b3World_GetSensorEvents(g_worldId);
  i(sensors.beginCount);
  for (int k = 0; k < sensors.beginCount; k++) { shapeIdOut(sensors.beginEvents[k].sensorShapeId); shapeIdOut(sensors.beginEvents[k].visitorShapeId); }
  i(sensors.endCount);
  for (int k = 0; k < sensors.endCount; k++) { shapeIdOut(sensors.endEvents[k].sensorShapeId); shapeIdOut(sensors.endEvents[k].visitorShapeId); }
  b3BodyEvents bodies = b3World_GetBodyEvents(g_worldId);
  i(bodies.moveCount);
  for (int k = 0; k < bodies.moveCount; k++) {
    bodyRef(bodies.moveEvents[k].bodyId); i(bodies.moveEvents[k].fellAsleep); tf(bodies.moveEvents[k].transform);
  }
  end();
  if (stepIndex % 20 == 19) {
    name("world.bodies"); arrow(); i(g_bodyCount);
    for (int k = 0; k < g_bodyCount; k++) {
      float values[13] = { 0 };
      if (b3Body_IsValid(g_bodies[k])) bodyValues(g_bodies[k], values);
      for (int n = 0; n < 13; n++) f(values[n]);
    }
    end();
  }
}

static void beginScene(float gravity) {
  b3WorldDef worldDef = b3DefaultWorldDef();
  worldDef.gravity = (b3Vec3){ 0.0f, gravity, 0.0f };
  worldDef.enableSleep = true;
  worldDef.workerCount = 1;
  g_worldId = b3CreateWorld(&worldDef);
  g_world = b3GetWorldFromId(g_worldId);
  g_bodyCount = 0;
  name("world.scene"); v3(worldDef.gravity); i(worldDef.enableSleep); arrow(); end();
}

static void setVelocity(b3BodyId body, b3Vec3 v) {
  b3Body_SetLinearVelocity(body, v);
  name("body.setLinearVelocity"); bodyRef(body); v3(v); arrow(); end();
}

static void impulse(b3BodyId body, b3Vec3 p) {
  b3Body_ApplyLinearImpulseToCenter(body, p, true);
  name("body.applyLinearImpulseToCenter"); bodyRef(body); v3(p); i(1); arrow(); end();
}

static b3ShapeDef eventDef(void) {
  b3ShapeDef def = b3DefaultShapeDef();
  def.enableContactEvents = true;
  def.enableHitEvents = true;
  def.enableSensorEvents = true;
  return def;
}

static void ground(float hx, float hy, float hz) {
  b3BodyId body = makeBody(b3_staticBody, (b3Vec3){ 0.0f, -hy, 0.0f }, b3Quat_identity);
  b3ShapeDef def = b3DefaultShapeDef();
  boxShape(body, def, hx, hy, hz);
}


static void runSteps(int from, int to) {
  for (int k = from; k < to; k++) step(k);
}

int main(void) {
  printf("# box3d oracle: sleep, splitting, sensors and events, commit %s, scalar, -ffp-contract=off\n", BOX3D_ORACLE_COMMIT);

  printf("# scene sleep\n");
  beginScene(-10.0f);
  ground(8.0f, 0.5f, 4.0f);
  b3BodyId spheres[3];
  for (int k = 0; k < 3; k++) {
    spheres[k] = makeBody(b3_dynamicBody, (b3Vec3){ -5.0f + 1.5f * (float)k, 1.0f + 0.5f * (float)k, 0.0f }, b3Quat_identity);
    sphereShape(spheres[k], eventDef(), 0.5f);
  }
  for (int k = 0; k < 3; k++) {
    b3BodyId box = makeBody(b3_dynamicBody, (b3Vec3){ 3.0f, 0.5f + 1.0f * (float)k, 0.0f }, b3Quat_identity);
    boxShape(box, b3DefaultShapeDef(), 0.5f, 0.5f, 0.5f);
  }
  runSteps(0, 200);
  setVelocity(spheres[1], (b3Vec3){ 2.0f, 0.0f, 0.0f });
  runSteps(200, 320);
  {
    b3BodyId dropped = makeBody(b3_dynamicBody, (b3Vec3){ 3.0f, 4.5f, 0.0f }, b3Quat_identity);
    boxShape(dropped, b3DefaultShapeDef(), 0.5f, 0.5f, 0.5f);
  }
  runSteps(320, 600);
  b3DestroyWorld(g_worldId);

  printf("# scene split\n");
  beginScene(-10.0f);
  ground(8.0f, 0.5f, 4.0f);
  b3BodyId stack[4];
  for (int k = 0; k < 4; k++) {
    stack[k] = makeBody(b3_dynamicBody, (b3Vec3){ 0.0f, 0.5f + 1.0f * (float)k, 0.0f }, b3Quat_identity);
    boxShape(stack[k], eventDef(), 0.5f, 0.5f, 0.5f);
  }
  runSteps(0, 120);
  impulse(stack[2], (b3Vec3){ 6000.0f, 0.0f, 0.0f });
  runSteps(120, 600);
  b3DestroyWorld(g_worldId);

  printf("# scene sensor\n");
  beginScene(0.0f);
  b3BodyId sensorBody = makeBody(b3_staticBody, (b3Vec3){ 0.0f, 0.0f, 0.0f }, b3Quat_identity);
  b3ShapeDef sensorDef = b3DefaultShapeDef();
  sensorDef.isSensor = true;
  sensorDef.enableSensorEvents = true;
  boxShape(sensorBody, sensorDef, 1.0f, 1.0f, 1.0f);
  b3BodyId wall = makeBody(b3_staticBody, (b3Vec3){ 0.0f, 0.0f, 4.0f }, b3Quat_identity);
  boxShape(wall, sensorDef, 2.0f, 2.0f, 0.03125f);
  for (int k = 0; k < 3; k++) {
    b3BodyId ball = makeBody(b3_dynamicBody, (b3Vec3){ -4.0f - 1.5f * (float)k, 0.25f * (float)k, 0.0f }, b3Quat_identity);
    sphereShape(ball, eventDef(), 0.25f);
    setVelocity(ball, (b3Vec3){ 3.0f, 0.0f, 0.0f });
  }
  g_nextBullet = true;
  b3BodyId bullet = makeBody(b3_dynamicBody, (b3Vec3){ 0.0f, 0.0f, -2.0f }, b3Quat_identity);
  sphereShape(bullet, eventDef(), 0.0625f);
  setVelocity(bullet, (b3Vec3){ 0.0f, 0.0f, 150.0f });
  runSteps(0, 120);
  {
    b3ShapeId shapes[2];
    b3Body_GetShapes(sensorBody, shapes, 2);
    b3DestroyShape(shapes[0], false);
    name("shape.destroy"); shapeRef(shapes[0]); i(0); arrow(); end();
  }
  runSteps(120, 600);
  b3DestroyWorld(g_worldId);

  printf("# scene events\n");
  beginScene(-10.0f);
  ground(8.0f, 0.5f, 4.0f);
  b3BodyId balls[4];
  for (int k = 0; k < 4; k++) {
    balls[k] = makeBody(b3_dynamicBody, (b3Vec3){ -3.0f + 2.0f * (float)k, 2.0f + 1.0f * (float)k, 0.0f }, b3Quat_identity);
    b3ShapeDef def = eventDef();
    def.baseMaterial.restitution = 0.5f;
    def.baseMaterial.userMaterialId = 7 + (uint64_t)k;
    sphereShape(balls[k], def, 0.5f);
  }
  runSteps(0, 150);
  b3DestroyBody(balls[0]);
  name("body.destroy"); bodyRef(balls[0]); arrow(); end();
  runSteps(150, 600);
  b3DestroyWorld(g_worldId);
  return 0;
}
