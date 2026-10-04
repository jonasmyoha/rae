/* Box3D oracle driver: the character mover (docs/physics-rae-port-design.md
 * §7 P7a: b3SolvePlanes and b3ClipVector of mover.c, and the loop of
 * samples/mover.cpp: CharacterMover::SolveMove and the jump of
 * CharacterMover::Step, ported to C here without the drawing and the camera,
 * with the pogo ray's hit normal kept as `groundNormal`). Each scene steps the
 * mover with a scripted throttle and then the world (1/60 s, 4 sub-steps,
 * workerCount 1, sleep on), as the sample does, 240 steps each:
 *
 *   wall      walking into a wall at 20 degrees: slides along it
 *   curb      walking onto a 0.3 m curb: the pogo spring steps it up
 *   block     walking into a 0.6 m block: the pogo spring holds the capsule's
 *             bottom 0.6 m up, so this mover steps up it too
 *   tall      walking into a 1.0 m block: blocked
 *   ramp      walking up a 20 degree ramp and stopping on it: grounded, the
 *             ground normal the ramp's
 *   push      walking into a dynamic box: pushes it
 *   jump      a standing jump, then a running one
 *
 * The line formats are queries.c's for the world and bodies, plus
 *
 *   mover.create position(3) ->
 *   mover.step timeStep throttle(2) forward(3) right(3) jump clipVelocity ->
 *              position(3) velocity(3) onGround pogoVelocity planeCount
 *              totalIterations groundNormal(3) (plane push)*planeCount
 *   world.step timeStep subSteps -> hash
 *
 * where the hash is FNV-1a over every body's transform and velocities.
 *
 * Compiled against the cached Box3D checkout by tools/box3d-oracle/oracle.sh,
 * never by a Rae build. */
#include <stdint.h>
#include <stdio.h>
#include <math.h>
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


/* --- The mover (samples/mover.cpp) --- */

#define PLANE_CAPACITY 8

typedef struct Mover {
  b3WorldTransform transform;
  b3Vec3 velocity;
  b3Capsule capsule;
  b3CollisionPlane planes[PLANE_CAPACITY];
  b3Pos planePoints[PLANE_CAPACITY];
  b3ShapeId planeShapeIds[PLANE_CAPACITY];
  int planeCount;
  int totalIterations;
  float pogoVelocity;
  bool onGround;
  bool sprint;
  b3Vec3 groundNormal;
} Mover;

static const float jumpSpeed = 5.0f;
static const float maxSpeed = 6.0f;
static const float minSpeed = 0.01f;
static const float stopSpeed = 1.0f;
static const float accelerate = 30.0f;
static const float friction = 4.0f;
static const float moverGravity = 15.0f;

static Mover g_mover;

static bool moverFilterFcn(b3ShapeId shapeId, void* context) {
  (void)shapeId;
  (void)context;
  return true;
}

static bool planeResultFcn(b3ShapeId shapeId, const b3PlaneResult* planeResults, int planeCount, void* context) {
  Mover* self = (Mover*)context;
  float maxPush = FLT_MAX;
  bool clipVelocity = true;
  for (int k = 0; k < planeCount && self->planeCount < PLANE_CAPACITY; ++k) {
    self->planes[self->planeCount] = (b3CollisionPlane){
      .plane = planeResults[k].plane, .pushLimit = maxPush, .push = 0.0f, .clipVelocity = clipVelocity };
    self->planePoints[self->planeCount] = b3OffsetPos(self->transform.p, planeResults[k].point);
    self->planeShapeIds[self->planeCount] = shapeId;
    self->planeCount += 1;
  }
  return true;
}

static void initializeMover(Mover* mover, b3Pos position) {
  memset(mover, 0, sizeof(*mover));
  mover->transform.p = position;
  mover->transform.q = b3Quat_identity;
  mover->capsule = (b3Capsule){ { 0.0f, -0.5f, 0.0f }, { 0.0f, 0.5f, 0.0f }, 0.3f };
}

static void solveMove(Mover* mover, float timeStep, b3Vec3 forward, b3Vec3 right, b3Vec2 throttle, bool clipVelocity) {
  /* Friction */
  float speed = b3Length(mover->velocity);
  if (speed < minSpeed) {
    mover->velocity.x = 0.0f;
    mover->velocity.z = 0.0f;
  } else {
    float control = speed < stopSpeed ? stopSpeed : speed;
    float drop = control * friction * timeStep;
    float newSpeed = b3MaxFloat(0.0f, speed - drop);
    float ratio = newSpeed / speed;
    mover->velocity.x *= ratio;
    mover->velocity.z *= ratio;
  }

  float speedLimit = mover->sprint ? 1.5f * maxSpeed : maxSpeed;
  b3Vec3 desiredVelocity = b3Add(b3MulSV(speedLimit * throttle.x, forward), b3MulSV(speedLimit * throttle.y, right));
  float desiredSpeed;
  b3Vec3 desiredDirection = b3GetLengthAndNormalize(&desiredSpeed, desiredVelocity);
  if (desiredSpeed > speedLimit) {
    desiredSpeed = speedLimit;
  }

  if (mover->onGround) {
    mover->velocity.y = 0.0f;
  }

  /* Accelerate */
  float currentSpeed = b3Dot(mover->velocity, desiredDirection);
  float addSpeed = desiredSpeed - currentSpeed;
  if (addSpeed > 0.0f) {
    float accelSpeed = accelerate * speedLimit * timeStep;
    if (accelSpeed > addSpeed) {
      accelSpeed = addSpeed;
    }
    b3Vec3 add = b3MulSV(accelSpeed, desiredDirection);
    mover->velocity.x += add.x;
    mover->velocity.y += add.y;
    mover->velocity.z += add.z;
  }

  mover->velocity.y -= moverGravity * timeStep;

  float pogoRestLength = 3.0f * mover->capsule.radius;
  float rayLength = pogoRestLength + mover->capsule.radius;
  b3Pos rayOrigin = b3TransformWorldPoint(mover->transform, mover->capsule.center1);
  b3Vec3 rayTranslation = b3MulSV(-rayLength, b3Vec3_axisY);
  b3QueryFilter skipTeamFilter = { 1, ~2u, 0, NULL };
  b3RayResult rayResult = b3World_CastRayClosest(g_worldId, rayOrigin, rayTranslation, skipTeamFilter);

  bool suppressPogo = mover->velocity.y > 0.0f;
  if (rayResult.hit == false || suppressPogo) {
    mover->onGround = false;
    mover->pogoVelocity = 0.0f;
    mover->groundNormal = b3Vec3_zero;
  } else {
    mover->onGround = true;
    mover->groundNormal = rayResult.normal;
    float pogoCurrentLength = rayResult.fraction * rayLength;
    float zeta = 0.7f;
    float hertz = 4.0f;
    float omega = 2.0f * B3_PI * hertz;
    float omegaH = omega * timeStep;
    mover->pogoVelocity = (mover->pogoVelocity - omega * omegaH * (pogoCurrentLength - pogoRestLength)) /
                          (1.0f + 2.0f * zeta * omegaH + omegaH * omegaH);
  }

  b3Pos startPosition = mover->transform.p;
  b3Pos target = b3Add(b3Add(mover->transform.p, b3MulSV(timeStep, mover->velocity)),
                       b3MulSV(timeStep * mover->pogoVelocity, b3Vec3_axisY));

  b3QueryFilter moverFilter = { .categoryBits = 1, .maskBits = ~0u, .id = 1, .name = "mover_collide" };
  b3QueryFilter castFilter = { .categoryBits = 1, .maskBits = ~2u, .id = 1, .name = "mover_cast" };

  mover->totalIterations = 0;
  float tolerance = 0.01f;
  for (int iteration = 0; iteration < 5; ++iteration) {
    mover->planeCount = 0;
    b3Capsule capsule = mover->capsule;
    b3World_CollideMover(g_worldId, mover->transform.p, &capsule, moverFilter, planeResultFcn, mover);

    b3Vec3 targetDelta = b3Sub(target, mover->transform.p);
    b3PlaneSolverResult result = b3SolvePlanes(targetDelta, mover->planes, mover->planeCount);
    mover->totalIterations += result.iterationCount;

    b3Vec3 delta = result.delta;
    float fraction = b3World_CastMover(g_worldId, mover->transform.p, &capsule, delta, castFilter, moverFilterFcn, mover);
    delta.x *= fraction;
    delta.y *= fraction;
    delta.z *= fraction;
    mover->transform.p = b3Add(mover->transform.p, delta);
    if (b3LengthSquared(delta) < tolerance * tolerance) {
      break;
    }
  }

  for (int k = 0; k < mover->planeCount; ++k) {
    b3BodyId bodyId = b3Shape_GetBody(mover->planeShapeIds[k]);
    if (b3Body_GetType(bodyId) != b3_dynamicBody) {
      continue;
    }
    b3Pos point = mover->planePoints[k];
    b3Vec3 normal = b3Neg(mover->planes[k].plane.normal);
    float invMassA = 0.0f;
    float invMassB = b3Body_GetInverseMass(bodyId);
    b3Matrix3 invIB = b3Body_GetWorldInverseRotationalInertia(bodyId);
    b3Pos pB = b3Body_GetWorldCenter(bodyId);
    b3Vec3 rB = b3SubPos(point, pB);
    b3Vec3 rnB = b3Cross(rB, normal);
    float kNormal = invMassA + invMassB + b3Dot(rnB, b3MulMV(invIB, rnB));
    float normalMass = kNormal > 0.0f ? 1.0f / kNormal : 0.0f;
    b3Vec3 vB = b3Body_GetLinearVelocity(bodyId);
    b3Vec3 omegaB = b3Body_GetAngularVelocity(bodyId);
    b3Vec3 vrB = b3Add(vB, b3Cross(omegaB, rB));
    float vn = b3Dot(b3Sub(vrB, mover->velocity), normal);
    float impulse = b3MaxFloat(-normalMass * vn, 0.0f);
    b3Vec3 P = b3MulSV(impulse, normal);
    mover->velocity = b3MulSub(mover->velocity, invMassA, P);
    b3Body_ApplyLinearImpulse(bodyId, P, point, true);
  }

  if (clipVelocity) {
    mover->velocity = b3ClipVector(mover->velocity, mover->planes, mover->planeCount);
  } else if (timeStep > 0.0f) {
    mover->velocity = b3MulSV(1.0f / timeStep, b3Sub(mover->transform.p, startPosition));
  }
}

static void moverStep(b3Vec2 throttle, b3Vec3 forward, b3Vec3 right, bool jump, bool clipVelocity) {
  float timeStep = 1.0f / 60.0f;
  if (jump && g_mover.onGround) {
    g_mover.velocity.y = jumpSpeed;
    g_mover.onGround = false;
  }
  solveMove(&g_mover, timeStep, forward, right, throttle, clipVelocity);
  name("mover.step"); f(timeStep); f(throttle.x); f(throttle.y); v3(forward); v3(right); i(jump); i(clipVelocity); arrow();
  v3(g_mover.transform.p); v3(g_mover.velocity); i(g_mover.onGround); f(g_mover.pogoVelocity); i(g_mover.planeCount);
  i(g_mover.totalIterations); v3(g_mover.groundNormal);
  for (int k = 0; k < g_mover.planeCount; k++) {
    v3(g_mover.planes[k].plane.normal); f(g_mover.planes[k].plane.offset); f(g_mover.planes[k].push);
  }
  end();
}

static uint64_t hashFloat(uint64_t hash, float value) {
  uint32_t bits;
  memcpy(&bits, &value, sizeof(bits));
  hash ^= bits;
  hash *= 0x100000001b3ull;
  return hash;
}

static void worldStep(void) {
  b3World_Step(g_worldId, 1.0f / 60.0f, 4);
  uint64_t hash = 0xcbf29ce484222325ull;
  for (int k = 0; k < g_bodyCount; k++) {
    b3WorldTransform t = b3Body_GetTransform(g_bodies[k]);
    b3Vec3 v = b3Body_GetLinearVelocity(g_bodies[k]);
    b3Vec3 w = b3Body_GetAngularVelocity(g_bodies[k]);
    float values[13] = { t.p.x, t.p.y, t.p.z, t.q.v.x, t.q.v.y, t.q.v.z, t.q.s, v.x, v.y, v.z, w.x, w.y, w.z };
    for (int n = 0; n < 13; n++) hash = hashFloat(hash, values[n]);
  }
  name("world.step"); f(1.0f / 60.0f); i(4); arrow(); u64(hash); end();
}

static void beginScene(b3Pos moverPosition) {
  b3WorldDef worldDef = b3DefaultWorldDef();
  worldDef.gravity = (b3Vec3){ 0.0f, -10.0f, 0.0f };
  worldDef.workerCount = 1;
  g_worldId = b3CreateWorld(&worldDef);
  g_world = b3GetWorldFromId(g_worldId);
  g_bodyCount = 0;
  name("world.scene"); v3(worldDef.gravity); i(worldDef.enableSleep); arrow(); end();
  b3BodyId ground = makeBody(b3_staticBody, (b3Vec3){ 0.0f, -0.5f, 0.0f });
  boxShapeAt(ground, b3DefaultShapeDef(), 20.0f, 0.5f, 20.0f, identityAt(0.0f, 0.0f, 0.0f));
  initializeMover(&g_mover, moverPosition);
  name("mover.create"); v3(moverPosition); arrow(); end();
}

/* Walk with `throttle` along +x for `count` steps (stepping the world after
 * each), jumping on the step `jumpAt` (-1: never). */
static void walk(int count, b3Vec2 throttle, b3Vec3 forward, int jumpAt, bool clipVelocity) {
  b3Vec3 right = b3Cross(forward, b3Vec3_axisY);
  for (int k = 0; k < count; k++) {
    moverStep(throttle, forward, right, k == jumpAt, clipVelocity);
    worldStep();
  }
}

int main(void) {
  printf("# box3d oracle: character mover, commit %s, scalar, -ffp-contract=off\n", BOX3D_ORACLE_COMMIT);
  b3Vec2 go = { 1.0f, 0.0f };
  b3Vec2 stop = { 0.0f, 0.0f };
  b3Vec3 alongX = { 1.0f, 0.0f, 0.0f };

  printf("# scene wall\n");
  beginScene((b3Pos){ 0.0f, 1.4f, 0.0f });
  {
    b3BodyId wall = makeBody(b3_staticBody, (b3Vec3){ 3.0f, 1.5f, 0.0f });
    boxShapeAt(wall, b3DefaultShapeDef(), 0.2f, 1.5f, 6.0f, identityAt(0.0f, 0.0f, 0.0f));
    b3Vec3 slanted = b3Normalize((b3Vec3){ 0.9396926f, 0.0f, 0.3420201f });
    walk(180, go, slanted, -1, true);
    walk(60, stop, slanted, -1, true);
  }
  b3DestroyWorld(g_worldId);

  printf("# scene curb\n");
  beginScene((b3Pos){ 0.0f, 1.4f, 0.0f });
  {
    b3BodyId curb = makeBody(b3_staticBody, (b3Vec3){ 4.0f, 0.15f, 0.0f });
    boxShapeAt(curb, b3DefaultShapeDef(), 2.0f, 0.15f, 3.0f, identityAt(0.0f, 0.0f, 0.0f));
    walk(90, go, alongX, -1, true);
    walk(150, stop, alongX, -1, true);
  }
  b3DestroyWorld(g_worldId);

  printf("# scene block\n");
  beginScene((b3Pos){ 0.0f, 1.4f, 0.0f });
  {
    b3BodyId block = makeBody(b3_staticBody, (b3Vec3){ 4.0f, 0.3f, 0.0f });
    boxShapeAt(block, b3DefaultShapeDef(), 2.0f, 0.3f, 3.0f, identityAt(0.0f, 0.0f, 0.0f));
    walk(120, go, alongX, -1, true);
    walk(120, stop, alongX, -1, true);
  }
  b3DestroyWorld(g_worldId);

  printf("# scene tall\n");
  beginScene((b3Pos){ 0.0f, 1.4f, 0.0f });
  {
    b3BodyId block = makeBody(b3_staticBody, (b3Vec3){ 4.0f, 0.5f, 0.0f });
    boxShapeAt(block, b3DefaultShapeDef(), 2.0f, 0.5f, 3.0f, identityAt(0.0f, 0.0f, 0.0f));
    walk(120, go, alongX, -1, true);
    walk(120, stop, alongX, -1, true);
  }
  b3DestroyWorld(g_worldId);

  printf("# scene ramp\n");
  beginScene((b3Pos){ 0.0f, 1.4f, 0.0f });
  {
    /* An exact 20 degrees (b3MakeQuatFromAxisAngle's cosine and sine are
     * approximations, 0.19 degrees off here) */
    float halfAngle = 0.5f * 0.34906585f;
    b3Quat twenty = { { 0.0f, 0.0f, sinf(halfAngle) }, cosf(halfAngle) };
    b3BodyId ramp = makeMovingBody(b3_staticBody, (b3Vec3){ 5.0f, 1.2f, 0.0f }, twenty, b3Vec3_zero, b3Vec3_zero);
    boxShapeAt(ramp, b3DefaultShapeDef(), 4.0f, 0.2f, 2.0f, identityAt(0.0f, 0.0f, 0.0f));
    walk(45, go, alongX, -1, true);
    walk(195, stop, alongX, -1, true);
  }
  b3DestroyWorld(g_worldId);

  printf("# scene push\n");
  beginScene((b3Pos){ 0.0f, 1.4f, 0.0f });
  {
    b3BodyId crate = makeBody(b3_dynamicBody, (b3Vec3){ 2.5f, 0.4f, 0.2f });
    b3ShapeDef light = b3DefaultShapeDef();
    light.density = 50.0f;
    boxShapeAt(crate, light, 0.4f, 0.4f, 0.4f, identityAt(0.0f, 0.0f, 0.0f));
    walk(150, go, alongX, -1, false);
    walk(90, stop, alongX, -1, false);
  }
  b3DestroyWorld(g_worldId);

  printf("# scene jump\n");
  beginScene((b3Pos){ 0.0f, 1.4f, 0.0f });
  {
    walk(60, stop, alongX, 40, true);
    walk(180, go, alongX, 60, true);
  }
  b3DestroyWorld(g_worldId);
  return 0;
}
