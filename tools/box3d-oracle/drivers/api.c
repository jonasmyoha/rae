/* Box3D oracle driver: the body, shape and world API an engine layer uses
 * between steps (docs/physics-rae-port-design.md §4, P4e): worlds stepped as
 * in events.c (1/60 s, 4 sub-steps, workerCount 1, sleeping on), with these
 * calls made between steps:
 *
 *   types    b3Body_SetType on touching dynamic, static and kinematic bodies
 *   enable   b3Body_Disable / b3Body_Enable on a sleeping body and on the
 *            static ground, b3Body_SetAwake, b3Body_EnableSleep,
 *            b3Body_SetSleepThreshold, a type change while disabled
 *   shapes   b3Shape_SetFriction / SetRestitution / SetSurfaceMaterial /
 *            SetFilter (with and without invokeContacts) / SetDensity /
 *            Enable*Events, b3Shape_GetContactData, b3Shape_GetSensorData
 *   world    b3World_EnableSleeping, SetGravity, SetContactTuning,
 *            SetRestitutionThreshold, SetHitEventThreshold,
 *            SetMaximumLinearSpeed, EnableWarmStarting, EnableContinuous,
 *            b3Body_SetBullet, b3Body_SetMotionLocks
 *
 * Every call is one line (`name args ->` and its result, if any); the step
 * and body-dump lines are events.c's. b3World_GetAwakeBodyCount is dumped
 * after each change, and the contact-data lines carry every manifold field.
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

static void awakeCount(void) {
  name("world.awakeBodyCount"); arrow(); i(b3World_GetAwakeBodyCount(g_worldId)); end();
}

static void manifoldsOut(const b3Manifold* manifolds, int count) {
  i(count);
  for (int m = 0; m < count; m++) {
    const b3Manifold* manifold = manifolds + m;
    v3(manifold->normal); f(manifold->twistImpulse); v3(manifold->frictionImpulse); v3(manifold->rollingImpulse);
    i(manifold->pointCount);
    for (int p = 0; p < manifold->pointCount; p++) {
      const b3ManifoldPoint* point = manifold->points + p;
      v3(point->anchorA); v3(point->anchorB); f(point->separation); f(point->normalImpulse);
      f(point->totalNormalImpulse); f(point->normalVelocity); f(point->baseSeparation);
      i(point->featureId); i(point->triangleIndex); i(point->persisted);
    }
  }
}

static void contactDataOut(const b3ContactData* data, int count) {
  i(count);
  for (int k = 0; k < count; k++) {
    contactIdOut(data[k].contactId); shapeIdOut(data[k].shapeIdA); shapeIdOut(data[k].shapeIdB);
    manifoldsOut(data[k].manifolds, data[k].manifoldCount);
  }
}

static void bodyContactData(b3BodyId body) {
  b3ContactData data[32];
  int count = b3Body_GetContactData(body, data, 32);
  name("body.contactData"); bodyRef(body); i(32); arrow(); contactDataOut(data, count); end();
}

static b3ShapeId firstShape(b3BodyId body) {
  b3ShapeId shapes[4];
  b3Body_GetShapes(body, shapes, 4);
  return shapes[0];
}

static void shapeContactData(b3ShapeId shape) {
  b3ContactData data[32];
  int count = b3Shape_GetContactData(shape, data, 32);
  name("shape.contactData"); shapeRef(shape); i(32); arrow(); contactDataOut(data, count); end();
}

static void shapeSensorData(b3ShapeId shape) {
  b3ShapeId visitors[32];
  int count = b3Shape_GetSensorData(shape, visitors, 32);
  name("shape.sensorData"); shapeRef(shape); i(32); arrow(); i(count);
  for (int k = 0; k < count; k++) shapeIdOut(visitors[k]);
  end();
}

static void setType(b3BodyId body, b3BodyType type) {
  b3Body_SetType(body, type);
  name("body.setType"); bodyRef(body); i(type); arrow(); end();
  awakeCount();
}

static void disableBody(b3BodyId body) {
  b3Body_Disable(body);
  name("body.disable"); bodyRef(body); arrow(); end();
  awakeCount();
}

static void enableBody(b3BodyId body) {
  b3Body_Enable(body);
  name("body.enable"); bodyRef(body); arrow(); end();
  awakeCount();
}

static void setAwake(b3BodyId body, bool flag) {
  b3Body_SetAwake(body, flag);
  name("body.setAwake"); bodyRef(body); i(flag); arrow(); end();
  awakeCount();
}

static void enableSleep(b3BodyId body, bool flag) {
  b3Body_EnableSleep(body, flag);
  name("body.enableSleep"); bodyRef(body); i(flag); arrow(); end();
  awakeCount();
}

static void sleepThreshold(b3BodyId body, float value) {
  b3Body_SetSleepThreshold(body, value);
  name("body.setSleepThreshold"); bodyRef(body); f(value); arrow(); end();
}

static void bullet(b3BodyId body, bool flag) {
  b3Body_SetBullet(body, flag);
  name("body.setBullet"); bodyRef(body); i(flag); arrow(); end();
}

static void motionLocks(b3BodyId body, bool lx, bool ly, bool lz, bool ax, bool ay, bool az) {
  b3MotionLocks locks = { lx, ly, lz, ax, ay, az };
  b3Body_SetMotionLocks(body, locks);
  name("body.setMotionLocks"); bodyRef(body); i(lx); i(ly); i(lz); i(ax); i(ay); i(az); arrow(); end();
}

static void friction(b3ShapeId shape, float value) {
  b3Shape_SetFriction(shape, value);
  name("shape.setFriction"); shapeRef(shape); f(value); arrow(); end();
}

static void restitution(b3ShapeId shape, float value) {
  b3Shape_SetRestitution(shape, value);
  name("shape.setRestitution"); shapeRef(shape); f(value); arrow(); end();
}

static void surfaceMaterial(b3ShapeId shape, b3SurfaceMaterial m) {
  b3Shape_SetSurfaceMaterial(shape, m);
  name("shape.setSurfaceMaterial"); shapeRef(shape); material(m); arrow(); end();
}

static void density(b3ShapeId shape, float value, bool updateBodyMass) {
  b3Shape_SetDensity(shape, value, updateBodyMass);
  name("shape.setDensity"); shapeRef(shape); f(value); i(updateBodyMass); arrow(); end();
}

static void filter(b3ShapeId shape, uint64_t category, uint64_t mask, int group, bool invoke) {
  b3Filter value = { category, mask, group };
  b3Shape_SetFilter(shape, value, invoke);
  name("shape.setFilter"); shapeRef(shape); u64(category); u64(mask); i(group); i(invoke); arrow(); end();
  awakeCount();
}

static void shapeEvents(const char* which, b3ShapeId shape, bool flag) {
  if (which[0] == 's') b3Shape_EnableSensorEvents(shape, flag);
  else if (which[0] == 'c') b3Shape_EnableContactEvents(shape, flag);
  else b3Shape_EnableHitEvents(shape, flag);
  printf("shape.enable%sEvents", which[0] == 's' ? "Sensor" : which[0] == 'c' ? "Contact" : "Hit");
  shapeRef(shape); i(flag); arrow(); end();
}

static b3BodyId dynamicBox(b3Vec3 position, b3ShapeDef def) {
  b3BodyId body = makeBody(b3_dynamicBody, position, b3Quat_identity);
  boxShape(body, def, 0.5f, 0.5f, 0.5f);
  return body;
}

static b3BodyId dynamicSphere(b3Vec3 position, b3ShapeDef def, float radius) {
  b3BodyId body = makeBody(b3_dynamicBody, position, b3Quat_identity);
  sphereShape(body, def, radius);
  return body;
}

int main(void) {
  printf("# box3d oracle: the body, shape and world API between steps, commit %s, scalar, -ffp-contract=off\n", BOX3D_ORACLE_COMMIT);

  printf("# scene types\n");
  beginScene(-10.0f);
  b3BodyId groundA = makeBody(b3_staticBody, (b3Vec3){ 0.0f, -0.5f, 0.0f }, b3Quat_identity);
  boxShape(groundA, eventDef(), 8.0f, 0.5f, 4.0f);
  b3BodyId lower = dynamicBox((b3Vec3){ 0.0f, 0.5f, 0.0f }, eventDef());
  b3BodyId upper = dynamicBox((b3Vec3){ 0.0f, 1.5f, 0.0f }, eventDef());
  b3BodyId rolling = dynamicSphere((b3Vec3){ 3.0f, 1.0f, 0.0f }, eventDef(), 0.5f);
  b3BodyId platform = makeBody(b3_kinematicBody, (b3Vec3){ -3.0f, 1.0f, 0.0f }, b3Quat_identity);
  boxShape(platform, eventDef(), 1.0f, 0.25f, 1.0f);
  runSteps(0, 60);
  bodyContactData(lower);
  setType(upper, b3_staticBody);
  runSteps(60, 90);
  bodyContactData(lower);
  setType(upper, b3_dynamicBody);
  runSteps(90, 120);
  setType(platform, b3_dynamicBody);
  setType(groundA, b3_kinematicBody);
  runSteps(120, 150);
  setType(groundA, b3_staticBody);
  runSteps(150, 250);
  setType(rolling, b3_kinematicBody);
  setVelocity(rolling, (b3Vec3){ 1.0f, 0.0f, 0.0f });
  runSteps(250, 300);
  setType(rolling, b3_dynamicBody);
  runSteps(300, 360);
  b3DestroyWorld(g_worldId);

  printf("# scene enable\n");
  beginScene(-10.0f);
  b3BodyId groundB = makeBody(b3_staticBody, (b3Vec3){ 0.0f, -0.5f, 0.0f }, b3Quat_identity);
  boxShape(groundB, eventDef(), 8.0f, 0.5f, 4.0f);
  b3BodyId tower[3];
  for (int k = 0; k < 3; k++) tower[k] = dynamicBox((b3Vec3){ 0.0f, 0.5f + 1.0f * (float)k, 0.0f }, eventDef());
  b3BodyId ball = dynamicSphere((b3Vec3){ -4.0f, 0.5f, 0.0f }, eventDef(), 0.5f);
  runSteps(0, 200);
  awakeCount();
  disableBody(tower[1]);
  runSteps(200, 260);
  enableBody(tower[1]);
  runSteps(260, 320);
  disableBody(groundB);
  runSteps(320, 340);
  enableBody(groundB);
  runSteps(340, 500);
  setAwake(ball, false);
  setAwake(ball, true);
  setAwake(tower[0], false);
  enableSleep(tower[0], false);
  sleepThreshold(tower[2], 0.5f);
  runSteps(500, 700);
  disableBody(ball);
  setType(ball, b3_staticBody);
  enableBody(ball);
  runSteps(700, 760);
  b3DestroyWorld(g_worldId);

  printf("# scene shapes\n");
  beginScene(-10.0f);
  b3BodyId groundC = makeBody(b3_staticBody, (b3Vec3){ 0.0f, -0.5f, 0.0f }, b3Quat_identity);
  boxShape(groundC, eventDef(), 8.0f, 0.5f, 4.0f);
  b3BodyId sensorBody = makeBody(b3_staticBody, (b3Vec3){ 4.0f, 0.5f, 0.0f }, b3Quat_identity);
  b3ShapeDef sensorDef = b3DefaultShapeDef();
  sensorDef.isSensor = true;
  sensorDef.enableSensorEvents = true;
  boxShape(sensorBody, sensorDef, 1.0f, 1.0f, 1.0f);
  b3BodyId balls[3];
  for (int k = 0; k < 3; k++) balls[k] = dynamicSphere((b3Vec3){ -2.0f + 3.0f * (float)k, 1.0f, 0.0f }, eventDef(), 0.5f);
  runSteps(0, 60);
  b3ShapeId groundShape = firstShape(groundC);
  b3ShapeId sensorShape = firstShape(sensorBody);
  b3ShapeId ballShapes[3];
  for (int k = 0; k < 3; k++) ballShapes[k] = firstShape(balls[k]);
  shapeContactData(groundShape);
  shapeSensorData(sensorShape);
  shapeContactData(sensorShape);
  friction(ballShapes[0], 0.125f);
  restitution(ballShapes[1], 0.75f);
  {
    b3SurfaceMaterial m = b3DefaultSurfaceMaterial();
    m.friction = 0.25f;
    m.restitution = 0.5f;
    m.rollingResistance = 0.125f;
    m.userMaterialId = 42;
    surfaceMaterial(ballShapes[2], m);
  }
  setVelocity(balls[0], (b3Vec3){ 3.0f, 0.0f, 0.0f });
  impulse(balls[1], (b3Vec3){ 0.0f, 3.0f, 0.0f });
  runSteps(60, 120);
  filter(ballShapes[1], 2, UINT64_MAX, 0, true);
  filter(ballShapes[0], 1, UINT64_MAX - 1, 0, true);
  density(ballShapes[2], 5.0f, true);
  density(ballShapes[2], 5.0f, true);
  shapeEvents("contact", ballShapes[2], false);
  shapeEvents("hit", ballShapes[1], false);
  shapeEvents("sensor", ballShapes[2], false);
  impulse(balls[2], (b3Vec3){ 20.0f, 0.0f, 0.0f });
  runSteps(120, 240);
  shapeSensorData(sensorShape);
  shapeContactData(ballShapes[2]);
  bodyContactData(groundC);
  filter(ballShapes[1], 4, UINT64_MAX, 0, false);
  shapeEvents("contact", ballShapes[2], true);
  shapeEvents("sensor", ballShapes[2], true);
  impulse(balls[2], (b3Vec3){ -30.0f, 0.0f, 0.0f });
  runSteps(240, 320);
  shapeSensorData(sensorShape);
  b3DestroyWorld(g_worldId);

  printf("# scene world\n");
  beginScene(-10.0f);
  b3BodyId groundD = makeBody(b3_staticBody, (b3Vec3){ 0.0f, -0.5f, 0.0f }, b3Quat_identity);
  boxShape(groundD, eventDef(), 8.0f, 0.5f, 4.0f);
  b3BodyId crates[3];
  for (int k = 0; k < 3; k++) crates[k] = dynamicBox((b3Vec3){ -3.0f + 3.0f * (float)k, 0.5f, 0.0f }, eventDef());
  b3BodyId dart = dynamicSphere((b3Vec3){ -6.0f, 3.0f, 0.0f }, eventDef(), 0.125f);
  runSteps(0, 150);
  b3World_EnableSleeping(g_worldId, false);
  name("world.enableSleeping"); i(0); arrow(); end();
  awakeCount();
  b3World_SetGravity(g_worldId, (b3Vec3){ 0.0f, -5.0f, 1.0f });
  name("world.setGravity"); v3((b3Vec3){ 0.0f, -5.0f, 1.0f }); arrow(); end();
  b3World_SetContactTuning(g_worldId, 20.0f, 5.0f, 2.0f);
  name("world.setContactTuning"); f(20.0f); f(5.0f); f(2.0f); arrow(); end();
  b3World_SetRestitutionThreshold(g_worldId, 0.5f);
  name("world.setRestitutionThreshold"); f(0.5f); arrow(); end();
  b3World_SetHitEventThreshold(g_worldId, 0.125f);
  name("world.setHitEventThreshold"); f(0.125f); arrow(); end();
  b3World_SetMaximumLinearSpeed(g_worldId, 50.0f);
  name("world.setMaximumLinearSpeed"); f(50.0f); arrow(); end();
  b3World_EnableWarmStarting(g_worldId, false);
  name("world.enableWarmStarting"); i(0); arrow(); end();
  b3World_EnableContinuous(g_worldId, false);
  name("world.enableContinuous"); i(0); arrow(); end();
  bullet(dart, true);
  motionLocks(crates[1], true, false, false, false, true, false);
  setVelocity(dart, (b3Vec3){ 80.0f, 0.0f, 0.0f });
  impulse(crates[1], (b3Vec3){ 5.0f, 5.0f, 0.0f });
  runSteps(150, 250);
  b3World_EnableWarmStarting(g_worldId, true);
  name("world.enableWarmStarting"); i(1); arrow(); end();
  b3World_EnableContinuous(g_worldId, true);
  name("world.enableContinuous"); i(1); arrow(); end();
  b3World_EnableSleeping(g_worldId, true);
  name("world.enableSleeping"); i(1); arrow(); end();
  motionLocks(crates[1], false, false, false, true, true, true);
  bullet(dart, false);
  runSteps(250, 450);
  awakeCount();
  b3DestroyWorld(g_worldId);
  return 0;
}
