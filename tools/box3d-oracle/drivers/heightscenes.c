/* Box3D oracle driver: simulated scenes on height fields
 * (docs/physics-rae-port-design.md §7 P5c: height_field.c, the height shape,
 * the height-field cases of contact.c and mesh_contact.c), stepped with
 * b3World_Step (1/60 s, 4 sub-steps, workerCount 1), 600 steps each:
 *
 *   wave   spheres, capsules and boxes rolling, sliding and tumbling over a
 *          wave height field
 *   grid   a flat grid and a tilted wave field, boxes and capsules sliding
 *          down it across the cell diagonals; sleep enabled
 *
 * No body is fast (continuous collision against height fields is not part
 * of this step). The line formats are meshscenes.c's, plus
 *
 *   shape.heightField body def kind args -> shape
 *
 * kind 0 a wave (rowCount columnCount scale(3) rowFrequency
 * columnFrequency makeHoles), 1 a grid (rowCount columnCount scale(3)
 * makeHoles). Every 20 steps the bodies and every live height-field contact
 * (world.meshContacts, the same dump as meshscenes.c) are written.
 *
 * Compiled against the cached Box3D checkout by tools/box3d-oracle/oracle.sh,
 * never by a Rae build. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "body.h"
#include "contact.h"
#include "physics_world.h"
#include "shape.h"

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

static void material(b3SurfaceMaterial m) {
  f(m.friction); f(m.restitution); f(m.rollingResistance); v3(m.tangentVelocity); u64(m.userMaterialId); i(m.customColor);
}

static b3BodyId g_bodies[128];
static int g_bodyCount;

/* The meshes of the current scene, destroyed with its world. */
static b3HeightFieldData* g_fields[16];
static int g_fieldCount;

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

static b3BodyId makeBody(b3BodyType type, b3Vec3 position, b3Quat rotation) {
  return makeMovingBody(type, position, rotation, b3Vec3_zero, b3Vec3_zero);
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

static void capsuleShape(b3BodyId body, b3ShapeDef def, float halfLength, float radius) {
  b3Capsule c = { { -halfLength, 0.0f, 0.0f }, { halfLength, 0.0f, 0.0f }, radius };
  b3ShapeId id = b3CreateCapsuleShape(body, &def, &c);
  name("shape.capsule"); bodyRef(body); defOut(&def); v3(c.center1); v3(c.center2); f(c.radius); arrow(); shapeRef(id); end();
}

static void fieldShape(b3BodyId body, b3ShapeDef def, b3HeightFieldData* field) {
  g_fields[g_fieldCount++] = field;
  b3ShapeId id = b3CreateHeightFieldShape(body, &def, field);
  shapeRef(id); end();
}

static void waveShape(b3BodyId body, b3ShapeDef def, int rows, int columns, b3Vec3 scale, float rowFrequency,
                      float columnFrequency) {
  b3HeightFieldData* field = b3CreateWave(rows, columns, scale, rowFrequency, columnFrequency, false);
  name("shape.heightField"); bodyRef(body); defOut(&def); i(0); i(rows); i(columns); v3(scale); f(rowFrequency);
  f(columnFrequency); i(0); arrow();
  fieldShape(body, def, field);
}

static void gridShape(b3BodyId body, b3ShapeDef def, int rows, int columns, b3Vec3 scale) {
  b3HeightFieldData* field = b3CreateGrid(rows, columns, scale, false);
  name("shape.heightField"); bodyRef(body); defOut(&def); i(1); i(rows); i(columns); v3(scale); i(0); arrow();
  fieldShape(body, def, field);
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

static void meshContacts(void) {
  int count = 0;
  for (int k = 0; k < g_world->contacts.count; k++) {
    b3Contact* c = g_world->contacts.data + k;
    if (c->contactId != B3_NULL_INDEX && (c->flags & b3_simMeshContact)) count += 1;
  }
  name("world.meshContacts"); arrow(); i(count);
  for (int k = 0; k < g_world->contacts.count; k++) {
    b3Contact* c = g_world->contacts.data + k;
    if (c->contactId == B3_NULL_INDEX || (c->flags & b3_simMeshContact) == 0) continue;
    i(c->contactId); i(c->flags); i(c->colorIndex); i(c->localIndex); i(c->manifoldCount);
    for (int m = 0; m < c->manifoldCount; m++) {
      b3Manifold* manifold = c->manifolds + m;
      v3(manifold->normal); f(manifold->twistImpulse); v3(manifold->frictionImpulse); v3(manifold->rollingImpulse);
      i(manifold->pointCount);
      for (int p = 0; p < manifold->pointCount; p++) {
        b3ManifoldPoint* mp = manifold->points + p;
        v3(mp->anchorA); v3(mp->anchorB); f(mp->separation); f(mp->normalImpulse); f(mp->totalNormalImpulse);
        f(mp->normalVelocity); f(mp->baseSeparation); i((int)mp->featureId); i(mp->triangleIndex); i(mp->persisted);
      }
    }
    f(c->friction); f(c->restitution); f(c->rollingResistance); v3(c->tangentVelocity);
    i(c->meshContact.triangleCache.count);
  }
  end();
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
  if (stepIndex % DUMP_EVERY == DUMP_EVERY - 1) {
    name("world.bodies"); arrow(); i(g_bodyCount);
    for (int k = 0; k < g_bodyCount; k++) {
      float values[13];
      bodyValues(g_bodies[k], values);
      for (int n = 0; n < 13; n++) f(values[n]);
    }
    end();
    meshContacts();
  }
}

static void beginScene(bool enableSleep) {
  b3WorldDef worldDef = b3DefaultWorldDef();
  worldDef.gravity = (b3Vec3){ 0.0f, -10.0f, 0.0f };
  worldDef.enableSleep = enableSleep;
  worldDef.workerCount = 1;
  g_worldId = b3CreateWorld(&worldDef);
  g_world = b3GetWorldFromId(g_worldId);
  g_bodyCount = 0;
  g_fieldCount = 0;
  name("world.scene"); v3(worldDef.gravity); i(worldDef.enableSleep); arrow(); end();
}

static void endScene(void) {
  for (int k = 0; k < 600; k++) step(k);
  b3DestroyWorld(g_worldId);
  for (int k = 0; k < g_fieldCount; k++) b3DestroyHeightField(g_fields[k]);
}

static b3Quat axisAngle(float x, float y, float z, float angle) {
  return b3MakeQuatFromAxisAngle(b3Normalize((b3Vec3){ x, y, z }), angle);
}

int main(void) {
  printf("# box3d oracle: height-field scenes, commit %s, scalar, -ffp-contract=off\n", BOX3D_ORACLE_COMMIT);

  /* Rolling, sliding and tumbling over a wave field */
  printf("# scene wave\n");
  beginScene(false);
  {
    b3BodyId ground = makeBody(b3_staticBody, (b3Vec3){ -12.0f, 0.0f, -12.0f }, b3Quat_identity);
    waveShape(ground, b3DefaultShapeDef(), 49, 49, (b3Vec3){ 0.5f, 0.6f, 0.5f }, 0.04f, 0.05f);

    b3ShapeDef rolling = b3DefaultShapeDef();
    rolling.baseMaterial.rollingResistance = 0.2f;
    b3BodyId ball = makeMovingBody(b3_dynamicBody, (b3Vec3){ -5.0f, 1.8f, -2.0f }, b3Quat_identity,
                                   (b3Vec3){ 2.5f, 0.0f, 1.0f }, b3Vec3_zero);
    sphereShape(ball, rolling, 0.5f);

    b3BodyId pill = makeMovingBody(b3_dynamicBody, (b3Vec3){ 2.0f, 2.0f, 3.0f }, axisAngle(0.0f, 1.0f, 0.0f, 0.6f),
                                   (b3Vec3){ -1.5f, 0.0f, -1.0f }, b3Vec3_zero);
    capsuleShape(pill, rolling, 0.5f, 0.3f);

    b3BodyId slider = makeMovingBody(b3_dynamicBody, (b3Vec3){ -3.0f, 1.6f, 5.0f }, b3Quat_identity,
                                     (b3Vec3){ 3.0f, 0.0f, 0.0f }, b3Vec3_zero);
    b3ShapeDef slick = b3DefaultShapeDef();
    slick.baseMaterial.friction = 0.2f;
    boxShape(slider, slick, 0.5f, 0.5f, 0.5f);

    b3BodyId tumbler = makeMovingBody(b3_dynamicBody, (b3Vec3){ 4.0f, 2.2f, -4.0f }, axisAngle(1.0f, 0.5f, 0.25f, 0.7f),
                                      (b3Vec3){ -1.0f, 0.0f, 1.0f }, (b3Vec3){ 1.0f, 1.5f, 0.0f });
    boxShape(tumbler, b3DefaultShapeDef(), 0.6f, 0.4f, 0.5f);

    b3BodyId rest = makeBody(b3_dynamicBody, (b3Vec3){ 0.0f, 1.6f, 0.0f }, b3Quat_identity);
    sphereShape(rest, b3DefaultShapeDef(), 0.75f);
  }
  endScene();

  /* A flat grid and a tilted wave, sleep on */
  printf("# scene grid\n");
  beginScene(true);
  {
    b3BodyId floor = makeBody(b3_staticBody, (b3Vec3){ -15.0f, -2.0f, -15.0f }, b3Quat_identity);
    gridShape(floor, b3DefaultShapeDef(), 31, 31, (b3Vec3){ 1.0f, 1.0f, 1.0f });
    b3BodyId ramp = makeBody(b3_staticBody, (b3Vec3){ -4.0f, 0.0f, -3.0f }, axisAngle(0.0f, 0.0f, 1.0f, -0.2f));
    waveShape(ramp, b3DefaultShapeDef(), 13, 17, (b3Vec3){ 0.5f, 0.15f, 0.5f }, 0.1f, 0.12f);

    for (int k = 0; k < 3; k++) {
      b3BodyId box = makeBody(b3_dynamicBody, (b3Vec3){ -3.0f + 0.4f * (float)k, 0.6f, -2.0f + 1.6f * (float)k },
                              axisAngle(0.0f, 0.0f, 1.0f, -0.2f));
      b3ShapeDef def = b3DefaultShapeDef();
      def.baseMaterial.friction = 0.05f + 0.1f * (float)k;
      boxShape(box, def, 0.35f, 0.35f, 0.35f);
    }
    b3BodyId pill = makeBody(b3_dynamicBody, (b3Vec3){ -2.0f, 1.0f, 2.5f }, axisAngle(0.0f, 1.0f, 0.0f, 1.5707963f));
    b3ShapeDef rolling = b3DefaultShapeDef();
    rolling.baseMaterial.rollingResistance = 0.3f;
    capsuleShape(pill, rolling, 0.4f, 0.25f);

    b3BodyId sitter = makeBody(b3_dynamicBody, (b3Vec3){ 6.0f, -1.2f, 6.0f }, axisAngle(0.0f, 1.0f, 0.0f, 0.5f));
    boxShape(sitter, b3DefaultShapeDef(), 0.8f, 0.8f, 0.8f);
    b3BodyId ball = makeMovingBody(b3_dynamicBody, (b3Vec3){ 3.0f, -1.4f, -6.0f }, b3Quat_identity,
                                   (b3Vec3){ 1.5f, 0.0f, 2.5f }, b3Vec3_zero);
    sphereShape(ball, rolling, 0.5f);
  }
  endScene();

  return 0;
}
