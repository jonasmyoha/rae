/* Box3D oracle driver: the first simulated scenes (docs/physics-rae-port-design.md
 * §4, P4c): stepped with b3World_Step (1/60 s, 4 sub-steps, workerCount 1),
 * 600 steps each, with sleeping disabled (sleep and island splitting are
 * P4d):
 *
 *   sphere     a sphere falling on a static box
 *   bounce     five spheres of restitution 0 to 1 dropped on the box
 *   slope      four boxes of friction 0 to 0.6 on a 20-degree slope
 *   pyramid    a 10-level pyramid of 55 boxes
 *
 * The world, body and shape operations use world.c's line formats; a scene
 * starts with
 *
 *   world.scene gravity(3) enableSleep ->
 *
 * and every step is
 *
 *   world.step timeStep subSteps -> hash(u64)
 *
 * the FNV-1a hash (64 bits, over 32-bit words) of the float bits of every
 * body's position(3), rotation(4), linear velocity(3) and angular
 * velocity(3), bodies in creation order. Every 20 steps
 *
 *   world.bodies -> count (position rotation linear angular)*count
 *
 * dumps the same values, to locate a difference.
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

static b3BodyId makeBody(b3BodyType type, b3Vec3 position, b3Quat rotation) {
  b3BodyDef def = b3DefaultBodyDef();
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
  name("shape.hull"); bodyRef(body); defOut(&def); i(1); f(hx); f(hy); f(hz); tf(identity); arrow(); shapeRef(id); end();
}

static void sphereShape(b3BodyId body, b3ShapeDef def, float radius) {
  b3Sphere s = { b3Vec3_zero, radius };
  b3ShapeId id = b3CreateSphereShape(body, &def, &s);
  name("shape.sphere"); bodyRef(body); defOut(&def); v3(s.center); f(s.radius); arrow(); shapeRef(id); end();
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

static void step(int stepIndex) {
  float timeStep = 1.0f / 60.0f;
  int subSteps = 4;
  b3World_Step(g_worldId, timeStep, subSteps);
  uint64_t hash = 0xcbf29ce484222325ull;
  for (int k = 0; k < g_bodyCount; k++) {
    float values[13];
    bodyValues(g_bodies[k], values);
    for (int n = 0; n < 13; n++) hash = hashFloat(hash, values[n]);
  }
  name("world.step"); f(timeStep); i(subSteps); arrow(); u64(hash); end();
  if (stepIndex % 20 == 19) {
    name("world.bodies"); arrow(); i(g_bodyCount);
    for (int k = 0; k < g_bodyCount; k++) {
      float values[13];
      bodyValues(g_bodies[k], values);
      for (int n = 0; n < 13; n++) f(values[n]);
    }
    end();
  }
}

static void beginScene(void) {
  b3WorldDef worldDef = b3DefaultWorldDef();
  worldDef.gravity = (b3Vec3){ 0.0f, -10.0f, 0.0f };
  worldDef.enableSleep = false;
  worldDef.workerCount = 1;
  g_worldId = b3CreateWorld(&worldDef);
  g_world = b3GetWorldFromId(g_worldId);
  g_bodyCount = 0;
  name("world.scene"); v3(worldDef.gravity); i(worldDef.enableSleep); arrow(); end();
}

static void endScene(void) {
  for (int k = 0; k < 600; k++) step(k);
  b3DestroyWorld(g_worldId);
}

static void ground(float hx, float hy, float hz) {
  b3BodyId body = makeBody(b3_staticBody, (b3Vec3){ 0.0f, -hy, 0.0f }, b3Quat_identity);
  b3ShapeDef def = b3DefaultShapeDef();
  boxShape(body, def, hx, hy, hz);
}

int main(void) {
  printf("# box3d oracle: the first scenes, commit %s, scalar, -ffp-contract=off\n", BOX3D_ORACLE_COMMIT);

  /* A sphere falling on a box */
  printf("# scene sphere\n");
  beginScene();
  ground(5.0f, 0.5f, 5.0f);
  {
    b3BodyId ball = makeBody(b3_dynamicBody, (b3Vec3){ 0.0f, 3.0f, 0.0f }, b3Quat_identity);
    sphereShape(ball, b3DefaultShapeDef(), 0.5f);
  }
  endScene();

  /* Restitution: five spheres from 0 to 1 */
  printf("# scene bounce\n");
  beginScene();
  ground(6.0f, 0.5f, 3.0f);
  for (int k = 0; k < 5; k++) {
    b3BodyId ball = makeBody(b3_dynamicBody, (b3Vec3){ -4.0f + 2.0f * (float)k, 4.0f, 0.0f }, b3Quat_identity);
    b3ShapeDef def = b3DefaultShapeDef();
    def.baseMaterial.restitution = 0.25f * (float)k;
    sphereShape(ball, def, 0.5f);
  }
  endScene();

  /* Friction on a 20-degree slope */
  printf("# scene slope\n");
  beginScene();
  {
    float angle = 20.0f * B3_PI / 180.0f;
    b3Quat tilt = b3MakeQuatFromAxisAngle((b3Vec3){ 0.0f, 0.0f, 1.0f }, angle);
    b3BodyId slope = makeBody(b3_staticBody, b3Vec3_zero, tilt);
    boxShape(slope, b3DefaultShapeDef(), 6.0f, 0.25f, 3.0f);
    b3Vec3 normal = b3RotateVector(tilt, (b3Vec3){ 0.0f, 1.0f, 0.0f });
    b3Vec3 along = b3RotateVector(tilt, (b3Vec3){ 1.0f, 0.0f, 0.0f });
    for (int k = 0; k < 4; k++) {
      b3Vec3 p = b3MulAdd(b3MulSV(2.0f, along), 0.25f + 0.25f + 0.0625f, normal);
      p.z = -1.5f + 1.0f * (float)k;
      b3BodyId box = makeBody(b3_dynamicBody, p, tilt);
      b3ShapeDef def = b3DefaultShapeDef();
      def.baseMaterial.friction = 0.2f * (float)k;
      boxShape(box, def, 0.25f, 0.25f, 0.25f);
    }
  }
  endScene();

  /* A 10-level pyramid */
  printf("# scene pyramid\n");
  beginScene();
  ground(20.0f, 0.5f, 20.0f);
  for (int row = 0; row < 10; row++) {
    int count = 10 - row;
    for (int k = 0; k < count; k++) {
      float x = ( (float)k - 0.5f * (float)( count - 1 ) ) * 1.0f;
      float y = 0.5f + 1.0f * (float)row;
      b3BodyId box = makeBody(b3_dynamicBody, (b3Vec3){ x, y, 0.0f }, b3Quat_identity);
      boxShape(box, b3DefaultShapeDef(), 0.5f, 0.5f, 0.5f);
    }
  }
  endScene();

  return 0;
}
