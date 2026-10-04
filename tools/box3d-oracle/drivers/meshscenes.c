/* Box3D oracle driver: simulated scenes on triangle meshes
 * (docs/physics-rae-port-design.md §7 P5b2: mesh_contact.c, the mesh shape,
 * the mesh cases of contact.c), stepped with b3World_Step (1/60 s, 4
 * sub-steps, workerCount 1), 600 steps each:
 *
 *   grid      spheres, capsules and boxes dropped, rolling and sliding
 *             across a 20 x 20 grid mesh (ghost-collision reduction over the
 *             flat triangle edges)
 *   platform  a frustum platform and a closed box mesh on a grid: bodies
 *             sliding down the sloped faces, rolling over the top edges, a
 *             bouncing sphere; sleep enabled
 *   scaled    a grid scaled non-uniformly and mirrored (inverted winding),
 *             tilted, with boxes and capsules sliding down it
 *
 * The bodies move slowly enough that none is fast (continuous collision
 * against meshes is not part of this step). The line formats are scenes.c's,
 * plus
 *
 *   shape.mesh body def kind args scale(3) -> shape
 *
 * kind 0 a grid (xCount zCount cellWidth materialCount identifyEdges),
 * 1 a box mesh (center(3) extent(3) identifyEdges), 2 a platform mesh
 * (center(3) height topWidth bottomWidth). Every 20 steps, after the body
 * dump,
 *
 *   world.meshContacts -> count (contact)*count
 *
 * dumps every live mesh contact in contact id order: id, flags, colour and
 * local index, the manifold count and each manifold (normal, impulses, point
 * count, each point's anchors, separation, impulses, base separation,
 * feature id, triangle index and persisted), friction, restitution, rolling
 * resistance and tangent velocity, then the cached triangle count.
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

static void meshShape(b3BodyId body, b3ShapeDef def, b3MeshData* mesh, b3Vec3 scale) {
  g_meshes[g_meshCount++] = mesh;
  b3ShapeId id = b3CreateMeshShape(body, &def, mesh, scale);
  shapeRef(id); end();
}

static void gridShape(b3BodyId body, b3ShapeDef def, int xCount, int zCount, float cellWidth, b3Vec3 scale) {
  b3MeshData* mesh = b3CreateGridMesh(xCount, zCount, cellWidth, 1, true);
  name("shape.mesh"); bodyRef(body); defOut(&def); i(0); i(xCount); i(zCount); f(cellWidth); i(1); i(1); v3(scale); arrow();
  meshShape(body, def, mesh, scale);
}

static void boxMeshShape(b3BodyId body, b3ShapeDef def, b3Vec3 center, b3Vec3 extent) {
  b3MeshData* mesh = b3CreateBoxMesh(center, extent, true);
  b3Vec3 scale = { 1.0f, 1.0f, 1.0f };
  name("shape.mesh"); bodyRef(body); defOut(&def); i(1); v3(center); v3(extent); i(1); v3(scale); arrow();
  meshShape(body, def, mesh, scale);
}

static void platformShape(b3BodyId body, b3ShapeDef def, b3Vec3 center, float height, float top, float bottom) {
  b3MeshData* mesh = b3CreatePlatformMesh(center, height, top, bottom);
  b3Vec3 scale = { 1.0f, 1.0f, 1.0f };
  name("shape.mesh"); bodyRef(body); defOut(&def); i(2); v3(center); f(height); f(top); f(bottom); v3(scale); arrow();
  meshShape(body, def, mesh, scale);
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
  name("world.scene"); v3(worldDef.gravity); i(worldDef.enableSleep); arrow(); end();
}

static void endScene(void) {
  for (int k = 0; k < 600; k++) step(k);
  b3DestroyWorld(g_worldId);
  for (int k = 0; k < g_meshCount; k++) b3DestroyMesh(g_meshes[k]);
}

static b3Quat axisAngle(float x, float y, float z, float angle) {
  return b3MakeQuatFromAxisAngle(b3Normalize((b3Vec3){ x, y, z }), angle);
}

int main(void) {
  printf("# box3d oracle: mesh scenes, commit %s, scalar, -ffp-contract=off\n", BOX3D_ORACLE_COMMIT);
  b3Vec3 unit = { 1.0f, 1.0f, 1.0f };

  /* Rolling and sliding across a grid */
  printf("# scene grid\n");
  beginScene(false);
  {
    b3BodyId ground = makeBody(b3_staticBody, b3Vec3_zero, b3Quat_identity);
    gridShape(ground, b3DefaultShapeDef(), 20, 20, 1.0f, unit);

    b3BodyId ball = makeMovingBody(b3_dynamicBody, (b3Vec3){ -6.0f, 1.0f, -3.0f }, b3Quat_identity,
                                   (b3Vec3){ 3.0f, 0.0f, 1.0f }, b3Vec3_zero);
    sphereShape(ball, b3DefaultShapeDef(), 0.5f);

    b3BodyId pill = makeMovingBody(b3_dynamicBody, (b3Vec3){ -5.0f, 1.5f, 2.0f }, axisAngle(0.0f, 1.0f, 0.0f, 0.4f),
                                   (b3Vec3){ 2.5f, 0.0f, -0.5f }, (b3Vec3){ 0.0f, 0.0f, -1.0f });
    capsuleShape(pill, b3DefaultShapeDef(), 0.5f, 0.35f);

    b3BodyId slider = makeMovingBody(b3_dynamicBody, (b3Vec3){ -7.0f, 0.55f, 5.5f }, b3Quat_identity,
                                     (b3Vec3){ 5.0f, 0.0f, 0.0f }, b3Vec3_zero);
    b3ShapeDef slick = b3DefaultShapeDef();
    slick.baseMaterial.friction = 0.2f;
    boxShape(slider, slick, 0.5f, 0.5f, 0.5f);

    b3BodyId tumbler = makeMovingBody(b3_dynamicBody, (b3Vec3){ 2.0f, 2.0f, -1.0f }, axisAngle(1.0f, 0.5f, 0.25f, 0.7f),
                                      (b3Vec3){ -1.0f, 0.0f, 1.5f }, (b3Vec3){ 1.0f, 2.0f, 0.0f });
    boxShape(tumbler, b3DefaultShapeDef(), 0.6f, 0.4f, 0.5f);

    b3BodyId rest = makeBody(b3_dynamicBody, (b3Vec3){ 4.5f, 1.0f, 4.5f }, b3Quat_identity);
    sphereShape(rest, b3DefaultShapeDef(), 0.75f);
  }
  endScene();

  /* A platform and a box mesh on a grid */
  printf("# scene platform\n");
  beginScene(true);
  {
    b3BodyId ground = makeBody(b3_staticBody, b3Vec3_zero, b3Quat_identity);
    gridShape(ground, b3DefaultShapeDef(), 30, 30, 1.0f, unit);
    platformShape(ground, b3DefaultShapeDef(), (b3Vec3){ 0.0f, 1.0f, 0.0f }, 2.0f, 3.0f, 7.0f);
    boxMeshShape(ground, b3DefaultShapeDef(), (b3Vec3){ 5.5f, 0.5f, 5.5f }, (b3Vec3){ 1.0f, 0.5f, 1.0f });

    /* On the top face, pushed over the edge */
    b3BodyId pusher = makeMovingBody(b3_dynamicBody, (b3Vec3){ 0.0f, 2.45f, 0.0f }, b3Quat_identity,
                                     (b3Vec3){ 2.0f, 0.0f, 0.5f }, b3Vec3_zero);
    boxShape(pusher, b3DefaultShapeDef(), 0.4f, 0.4f, 0.4f);

    /* Dropped on the slope, rolling resistance stops them on the grid */
    b3ShapeDef rolling = b3DefaultShapeDef();
    rolling.baseMaterial.rollingResistance = 0.3f;
    b3BodyId ball = makeBody(b3_dynamicBody, (b3Vec3){ -2.6f, 2.5f, 0.3f }, b3Quat_identity);
    sphereShape(ball, rolling, 0.4f);

    b3BodyId pill = makeBody(b3_dynamicBody, (b3Vec3){ 0.4f, 2.0f, -2.6f }, axisAngle(0.0f, 1.0f, 0.0f, 0.3f));
    capsuleShape(pill, rolling, 0.6f, 0.3f);

    /* Across the top edge of the platform */
    b3BodyId edge = makeBody(b3_dynamicBody, (b3Vec3){ 1.5f, 2.6f, 1.0f }, axisAngle(0.0f, 0.0f, 1.0f, 0.5f));
    boxShape(edge, b3DefaultShapeDef(), 0.5f, 0.25f, 0.5f);

    /* Bouncing on the box mesh */
    b3BodyId bouncer = makeBody(b3_dynamicBody, (b3Vec3){ 5.3f, 2.5f, 5.6f }, b3Quat_identity);
    b3ShapeDef bouncy = b3DefaultShapeDef();
    bouncy.baseMaterial.restitution = 0.5f;
    bouncy.baseMaterial.rollingResistance = 0.1f;
    sphereShape(bouncer, bouncy, 0.35f);

    /* Resting on the box mesh's corner */
    b3BodyId corner = makeBody(b3_dynamicBody, (b3Vec3){ 4.6f, 1.6f, 4.6f }, axisAngle(0.0f, 1.0f, 0.0f, 0.785f));
    boxShape(corner, b3DefaultShapeDef(), 0.5f, 0.5f, 0.5f);
  }
  endScene();

  /* A scaled, mirrored and tilted grid */
  printf("# scene scaled\n");
  beginScene(false);
  {
    b3BodyId ramp = makeBody(b3_staticBody, (b3Vec3){ 0.0f, 0.0f, 0.0f }, axisAngle(0.0f, 0.0f, 1.0f, 0.25f));
    gridShape(ramp, b3DefaultShapeDef(), 12, 8, 1.0f, (b3Vec3){ 1.5f, 1.0f, -1.0f });
    b3BodyId floor = makeBody(b3_staticBody, (b3Vec3){ 0.0f, -2.5f, 0.0f }, b3Quat_identity);
    gridShape(floor, b3DefaultShapeDef(), 40, 20, 2.0f, (b3Vec3){ 1.0f, 1.0f, 1.0f });

    for (int k = 0; k < 3; k++) {
      float z = -2.0f + 2.0f * (float)k;
      b3Quat tilt = axisAngle(0.0f, 0.0f, 1.0f, 0.25f);
      b3Vec3 along = b3RotateVector(tilt, (b3Vec3){ 1.0f, 0.0f, 0.0f });
      b3Vec3 up = b3RotateVector(tilt, (b3Vec3){ 0.0f, 1.0f, 0.0f });
      b3Vec3 p = b3MulAdd(b3MulSV(4.0f, along), 0.45f, up);
      p.z = z;
      b3BodyId box = makeBody(b3_dynamicBody, p, tilt);
      b3ShapeDef def = b3DefaultShapeDef();
      def.baseMaterial.friction = 0.1f + 0.15f * (float)k;
      boxShape(box, def, 0.4f, 0.4f, 0.4f);
    }
    b3BodyId pill = makeMovingBody(b3_dynamicBody, (b3Vec3){ 3.0f, 2.0f, 3.0f }, axisAngle(0.0f, 1.0f, 0.0f, 1.5707963f),
                                   b3Vec3_zero, b3Vec3_zero);
    b3ShapeDef rolling = b3DefaultShapeDef();
    rolling.baseMaterial.rollingResistance = 0.3f;
    capsuleShape(pill, rolling, 0.5f, 0.3f);
    b3BodyId ball = makeBody(b3_dynamicBody, (b3Vec3){ 2.0f, 2.5f, -3.0f }, b3Quat_identity);
    sphereShape(ball, rolling, 0.5f);
  }
  endScene();

  return 0;
}
