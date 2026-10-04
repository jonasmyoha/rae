/* Box3D oracle driver: simulated scenes on a compound shape
 * (docs/physics-rae-port-design.md §7 P5d: compound.c, the compound shape,
 * the compound cases of contact.c and the compound pair emission of
 * broad_phase.c), stepped with b3World_Step (1/60 s, 4 sub-steps,
 * workerCount 1), 600 steps each:
 *
 *   course  a static compound of box-hull tiles, a box-mesh ramp, capsule
 *           rails and sphere bumps (shared and per-child materials), with
 *           spheres, capsules and boxes rolling and sliding over it
 *   sleep   the same compound with sleep on and bodies coming to rest
 *   settype b3Body_SetType on a compound's body and on a height field's
 *           body, which refuse to leave the static type (no steps)
 *
 * No body is fast. The line formats are meshscenes.c's, plus
 *
 *   shape.compound body def <compound.c's create arguments> -> shape
 *   shape.heightField body def 0 rows columns scale(3) rowFrequency
 *     columnFrequency makeHoles -> shape
 *   body.setType body type -> type after
 *
 * Every 20 steps the bodies and every live mesh contact (world.meshContacts,
 * a compound's mesh child included) are written.
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
static b3MeshData* g_meshes[16];
static int g_meshCount;
static b3CompoundData* g_compounds[8];
static int g_compoundCount;
static b3HeightFieldData* g_fields[8];
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

static void tf7(b3Transform t) { v3(t.p); q4(t.q); }

/* The compound of the scene, written in compound.c's create format. */
static void compoundShape(b3BodyId body, b3ShapeDef def, const b3CompoundDef* compoundDef, const float boxHalfs[][3],
                          const b3Vec3 meshCenters[], const b3Vec3 meshExtents[]) {
  b3CompoundData* compound = b3CreateCompound(compoundDef);
  g_compounds[g_compoundCount++] = compound;
  name("shape.compound"); bodyRef(body); defOut(&def);
  i(compoundDef->capsuleCount);
  for (int k = 0; k < compoundDef->capsuleCount; k++) {
    const b3CompoundCapsuleDef* c = compoundDef->capsules + k;
    v3(c->capsule.center1); v3(c->capsule.center2); f(c->capsule.radius); material(c->material);
  }
  i(compoundDef->hullCount);
  for (int k = 0; k < compoundDef->hullCount; k++) {
    const b3CompoundHullDef* h = compoundDef->hulls + k;
    f(boxHalfs[k][0]); f(boxHalfs[k][1]); f(boxHalfs[k][2]); tf7(h->transform); material(h->material);
  }
  i(compoundDef->meshCount);
  for (int k = 0; k < compoundDef->meshCount; k++) {
    const b3CompoundMeshDef* m = compoundDef->meshes + k;
    v3(meshCenters[k]); v3(meshExtents[k]); tf7(m->transform); v3(m->scale); i(m->materialCount);
    for (int n = 0; n < m->materialCount; n++) material(m->materials[n]);
  }
  i(compoundDef->sphereCount);
  for (int k = 0; k < compoundDef->sphereCount; k++) {
    const b3CompoundSphereDef* sp = compoundDef->spheres + k;
    v3(sp->sphere.center); f(sp->sphere.radius); material(sp->material);
  }
  arrow();
  b3ShapeId id = b3CreateBakedCompoundShape(body, &def, compound);
  shapeRef(id); end();
}

static void waveShape(b3BodyId body, b3ShapeDef def, int rows, int columns, b3Vec3 scale, float rowFrequency,
                      float columnFrequency) {
  b3HeightFieldData* field = b3CreateWave(rows, columns, scale, rowFrequency, columnFrequency, false);
  g_fields[g_fieldCount++] = field;
  name("shape.heightField"); bodyRef(body); defOut(&def); i(0); i(rows); i(columns); v3(scale); f(rowFrequency);
  f(columnFrequency); i(0); arrow();
  b3ShapeId id = b3CreateHeightFieldShape(body, &def, field);
  shapeRef(id); end();
}

static void setType(b3BodyId body, b3BodyType type) {
  b3Body_SetType(body, type);
  name("body.setType"); bodyRef(body); i(type); arrow(); i(b3Body_GetType(body)); end();
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
  g_meshCount = 0;
  g_compoundCount = 0;
  g_fieldCount = 0;
  name("world.scene"); v3(worldDef.gravity); i(worldDef.enableSleep); arrow(); end();
}

static void endSceneData(void);

static void endScene(void) {
  for (int k = 0; k < 600; k++) step(k);
  b3DestroyWorld(g_worldId);
  endSceneData();
}

static void endSceneData(void) {
  for (int k = 0; k < g_meshCount; k++) b3DestroyMesh(g_meshes[k]);
  for (int k = 0; k < g_compoundCount; k++) b3DestroyCompound(g_compounds[k]);
  for (int k = 0; k < g_fieldCount; k++) b3DestroyHeightField(g_fields[k]);
}

static b3Quat axisAngle(float x, float y, float z, float angle) {
  return b3MakeQuatFromAxisAngle(b3Normalize((b3Vec3){ x, y, z }), angle);
}

static b3SurfaceMaterial frictionMaterial(float friction) {
  b3SurfaceMaterial m = b3DefaultSurfaceMaterial();
  m.friction = friction;
  return m;
}

/* The course: a 6 x 6 floor of box tiles, a box-mesh ramp, two capsule
   rails and three sphere bumps. */
static void buildCourse(b3BodyId ground) {
  b3CompoundCapsuleDef capsules[2];
  b3CompoundHullDef hulls[36];
  b3BoxHull boxes[36];
  float halfs[36][3];
  b3CompoundMeshDef meshes[1];
  b3SurfaceMaterial meshMaterials[1];
  b3Vec3 meshCenters[1];
  b3Vec3 meshExtents[1];
  b3CompoundSphereDef spheres[3];

  for (int k = 0; k < 2; k++) {
    float z = k == 0 ? -4.2f : 4.2f;
    capsules[k].capsule = (b3Capsule){ { -4.0f, 0.3f, z }, { 4.0f, 0.3f, z }, 0.3f };
    capsules[k].material = frictionMaterial(0.6f);
  }
  for (int row = 0; row < 6; row++) {
    for (int column = 0; column < 6; column++) {
      int k = row * 6 + column;
      halfs[k][0] = 1.0f; halfs[k][1] = 0.25f; halfs[k][2] = 1.0f;
      boxes[k] = b3MakeTransformedBoxHull(1.0f, 0.25f, 1.0f, b3Transform_identity);
      hulls[k].hull = &boxes[k].base;
      hulls[k].transform = (b3Transform){ { -5.0f + 2.0f * (float)column, -0.25f, -5.0f + 2.0f * (float)row }, b3Quat_identity };
      hulls[k].material = frictionMaterial((k % 3 == 0) ? 0.3f : 0.6f);
    }
  }
  meshCenters[0] = (b3Vec3){ 0.0f, 0.0f, 0.0f };
  meshExtents[0] = (b3Vec3){ 1.5f, 0.2f, 1.0f };
  b3MeshData* mesh = b3CreateBoxMesh(meshCenters[0], meshExtents[0], true);
  g_meshes[g_meshCount++] = mesh;
  meshes[0].meshData = mesh;
  meshes[0].transform = (b3Transform){ { 2.0f, 0.35f, 0.0f }, axisAngle(0.0f, 0.0f, 1.0f, 0.2f) };
  meshes[0].scale = (b3Vec3){ 1.0f, 1.0f, 1.0f };
  meshMaterials[0] = frictionMaterial(0.4f);
  meshes[0].materials = meshMaterials;
  meshes[0].materialCount = mesh->materialCount;
  for (int k = 0; k < 3; k++) {
    spheres[k].sphere = (b3Sphere){ { -2.5f + 1.5f * (float)k, 0.0f, -2.0f }, 0.35f };
    spheres[k].material = frictionMaterial(0.6f);
  }
  b3CompoundDef def = { 0 };
  def.capsules = capsules; def.capsuleCount = 2;
  def.hulls = hulls; def.hullCount = 36;
  def.meshes = meshes; def.meshCount = 1;
  def.spheres = spheres; def.sphereCount = 3;
  compoundShape(ground, b3DefaultShapeDef(), &def, halfs, meshCenters, meshExtents);
}

int main(void) {
  printf("# box3d oracle: compound scenes, commit %s, scalar, -ffp-contract=off\n", BOX3D_ORACLE_COMMIT);

  printf("# scene course\n");
  beginScene(false);
  {
    b3BodyId ground = makeBody(b3_staticBody, (b3Vec3){ 0.0f, 0.0f, 0.0f }, axisAngle(0.0f, 1.0f, 0.0f, 0.1f));
    buildCourse(ground);

    b3ShapeDef rolling = b3DefaultShapeDef();
    rolling.baseMaterial.rollingResistance = 0.2f;
    b3BodyId ball = makeMovingBody(b3_dynamicBody, (b3Vec3){ -3.0f, 1.0f, -1.0f }, b3Quat_identity,
                                   (b3Vec3){ 2.0f, 0.0f, 0.5f }, b3Vec3_zero);
    sphereShape(ball, rolling, 0.4f);
    b3BodyId pill = makeMovingBody(b3_dynamicBody, (b3Vec3){ -1.0f, 1.2f, 2.0f }, axisAngle(0.0f, 1.0f, 0.0f, 0.5f),
                                   (b3Vec3){ 1.5f, 0.0f, -0.5f }, b3Vec3_zero);
    capsuleShape(pill, rolling, 0.4f, 0.25f);
    b3BodyId slider = makeMovingBody(b3_dynamicBody, (b3Vec3){ -2.5f, 0.45f, 1.0f }, b3Quat_identity,
                                     (b3Vec3){ 3.0f, 0.0f, 0.0f }, b3Vec3_zero);
    boxShape(slider, b3DefaultShapeDef(), 0.4f, 0.4f, 0.4f);
    b3BodyId ramp = makeBody(b3_dynamicBody, (b3Vec3){ 2.0f, 1.3f, 0.2f }, axisAngle(0.0f, 0.0f, 1.0f, 0.2f));
    boxShape(ramp, b3DefaultShapeDef(), 0.3f, 0.3f, 0.3f);
    b3BodyId bump = makeBody(b3_dynamicBody, (b3Vec3){ -1.0f, 1.5f, -2.0f }, b3Quat_identity);
    sphereShape(bump, rolling, 0.3f);
    b3BodyId rail = makeBody(b3_dynamicBody, (b3Vec3){ 0.5f, 1.2f, -4.0f }, axisAngle(1.0f, 0.2f, 0.0f, 0.4f));
    boxShape(rail, b3DefaultShapeDef(), 0.35f, 0.35f, 0.35f);
  }
  endScene();

  printf("# scene sleep\n");
  beginScene(true);
  {
    b3BodyId ground = makeBody(b3_staticBody, (b3Vec3){ 0.0f, 0.0f, 0.0f }, b3Quat_identity);
    buildCourse(ground);

    for (int k = 0; k < 4; k++) {
      b3BodyId box = makeBody(b3_dynamicBody, (b3Vec3){ -3.0f + 2.0f * (float)k, 0.9f, -1.0f + 0.7f * (float)k },
                              axisAngle(0.0f, 1.0f, 0.0f, 0.3f * (float)k));
      boxShape(box, b3DefaultShapeDef(), 0.4f, 0.4f, 0.4f);
    }
    b3ShapeDef rolling = b3DefaultShapeDef();
    rolling.baseMaterial.rollingResistance = 0.3f;
    b3BodyId ball = makeBody(b3_dynamicBody, (b3Vec3){ -2.5f, 1.2f, -2.0f }, b3Quat_identity);
    sphereShape(ball, rolling, 0.3f);
    b3BodyId pill = makeBody(b3_dynamicBody, (b3Vec3){ 0.0f, 1.0f, 4.0f }, b3Quat_identity);
    capsuleShape(pill, b3DefaultShapeDef(), 0.5f, 0.2f);
  }
  endScene();

  /* b3Body_SetType refuses to move a body with a compound or height shape
     off the static type. (Box3D returns with the world still locked, so each
     world gets one call and is not stepped after it.) */
  printf("# scene settype\n");
  beginScene(false);
  {
    b3BodyId ground = makeBody(b3_staticBody, b3Vec3_zero, b3Quat_identity);
    buildCourse(ground);
    setType(ground, b3_dynamicBody);
  }
  b3DestroyWorld(g_worldId);
  endSceneData();
  beginScene(false);
  {
    b3BodyId terrain = makeBody(b3_staticBody, (b3Vec3){ 10.0f, -1.0f, -4.0f }, b3Quat_identity);
    waveShape(terrain, b3DefaultShapeDef(), 9, 9, (b3Vec3){ 1.0f, 0.3f, 1.0f }, 0.1f, 0.1f);
    setType(terrain, b3_kinematicBody);
  }
  b3DestroyWorld(g_worldId);
  endSceneData();

  return 0;
}
