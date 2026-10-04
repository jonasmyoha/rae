/* Box3D oracle driver: joints (docs/physics-rae-port-design.md §7 P6a-P6b:
 * joint.c, distance_joint.c, revolute_joint.c, prismatic_joint.c,
 * wheel_joint.c, the filter joint, the joint
 * parts of island.c, solver_set.c, constraint_graph.c, solver.c and body.c),
 * stepped with b3World_Step (1/60 s, 4 sub-steps, workerCount 1), 600 steps
 * each, sleep on:
 *
 *   pendulum  a revolute pendulum, a rigid distance pendulum, a double
 *             revolute pendulum and a filter joint between two overlapping
 *             boxes (collideConnected off), settling to sleep
 *   hinge     a plank falling against its revolute limits, a wheel spun up
 *             by a revolute motor, a revolute spring around a target angle
 *   distance  distance joints with a spring, with limits, with a motor, and
 *             a rope between two dynamic bodies
 *   bridge    a revolute chain bridge between two static posts, boxes dropped
 *             on it; joint event thresholds on two links
 *   split     a hanging chain that falls asleep, then a joint in the middle
 *             is destroyed: the island splits, the halves fall and sleep
 *   changes   jointed bodies through the body changes: a body destroyed (its
 *             joints with it), a body disabled and enabled, the static
 *             anchor made dynamic and back, and a joint created between two
 *             sleeping sets (merging them)
 *   prismatic a slider down a slope to its limits, a sprung slider, a
 *             motorised elevator carrying a box, a telescope between two
 *             dynamic bodies
 *   car       a car on four wheel joints (suspension springs and limits,
 *             spin motors on the rear, steering with limits on the front)
 *             driving over a bump, and a parked car settling to sleep
 *
 * The line formats are meshscenes.c's, plus
 *
 *   joint.revolute base targetAngle enableSpring hertz dampingRatio enableLimit
 *                  lowerAngle upperAngle enableMotor maxMotorTorque motorSpeed -> joint
 *   joint.distance base length enableSpring lowerSpringForce upperSpringForce
 *                  hertz dampingRatio enableLimit minLength maxLength enableMotor
 *                  maxMotorForce motorSpeed -> joint
 *   joint.prismatic base enableSpring hertz dampingRatio targetTranslation
 *                   enableLimit lowerTranslation upperTranslation enableMotor
 *                   maxMotorForce motorSpeed -> joint
 *   joint.wheel base enableSuspensionSpring suspensionHertz
 *               suspensionDampingRatio enableSuspensionLimit lowerSuspensionLimit
 *               upperSuspensionLimit enableSpinMotor maxSpinTorque spinSpeed
 *               enableSteering steeringHertz steeringDampingRatio
 *               targetSteeringAngle maxSteeringTorque enableSteeringLimit
 *               lowerSteeringLimit upperSteeringLimit -> joint
 *   joint.filter base -> joint
 *   joint.destroy joint wakeAttached ->
 *   body.destroy body ->      body.disable body ->      body.enable body ->
 *   body.setType body type ->
 *
 * where base is bodyA bodyB localFrameA(7) localFrameB(7) forceThreshold
 * torqueThreshold constraintHertz constraintDampingRatio drawScale
 * collideConnected, and a joint or body is its index and generation. A
 * destroyed body leaves the hashed body list. Every step reports the body
 * hash and the step's joint events,
 *
 *   world.step timeStep subSteps -> hash count (joint)*count
 *
 * and every 20 steps, after the body dump,
 *
 *   world.joints -> count (joint)*count bodyCount (setIndex islandId)*bodyCount
 *
 * dumps every live joint in id order: id, generation, type, set, colour,
 * local index, island, island index, collideConnected, the sim's inverse
 * masses and its accumulated impulses (distance: impulse, lower, upper,
 * motor; revolute: linear(3), perpendicular(2), spring, motor, lower,
 * upper; prismatic: perpendicular(2), angular(3), spring, motor, lower,
 * upper; wheel: linear(2), angular(2), spin, suspension spring, lower,
 * upper, steering spring, lower, upper), then each hashed body's set and
 * island.
 *
 * Compiled against the cached Box3D checkout by tools/box3d-oracle/oracle.sh,
 * never by a Rae build. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "body.h"
#include "joint.h"
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
static void jointRef(b3JointId id) { i(id.index1 - 1); i(id.generation); }

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

static void boxShape(b3BodyId body, float hx, float hy, float hz) {
  b3ShapeDef def = b3DefaultShapeDef();
  b3Transform identity = { b3Vec3_zero, b3Quat_identity };
  b3BoxHull boxHull = b3MakeTransformedBoxHull(hx, hy, hz, identity);
  b3ShapeId id = b3CreateHullShape(body, &def, &boxHull.base);
  name("shape.hull"); bodyRef(body); defOut(&def); i(1); f(hx); f(hy); f(hz); tf(identity); arrow(); shapeRef(id); end();
}

static void sphereShape(b3BodyId body, float radius) {
  b3ShapeDef def = b3DefaultShapeDef();
  b3Sphere s = { b3Vec3_zero, radius };
  b3ShapeId id = b3CreateSphereShape(body, &def, &s);
  name("shape.sphere"); bodyRef(body); defOut(&def); v3(s.center); f(s.radius); arrow(); shapeRef(id); end();
}

static void capsuleShape(b3BodyId body, float halfLength, float radius) {
  b3ShapeDef def = b3DefaultShapeDef();
  b3Capsule c = { { -halfLength, 0.0f, 0.0f }, { halfLength, 0.0f, 0.0f }, radius };
  b3ShapeId id = b3CreateCapsuleShape(body, &def, &c);
  name("shape.capsule"); bodyRef(body); defOut(&def); v3(c.center1); v3(c.center2); f(c.radius); arrow(); shapeRef(id); end();
}

static b3Transform frameAt(float x, float y, float z) {
  b3Transform t = { { x, y, z }, b3Quat_identity };
  return t;
}

static void baseOut(const b3JointDef* base) {
  bodyRef(base->bodyIdA); bodyRef(base->bodyIdB); tf(base->localFrameA); tf(base->localFrameB); f(base->forceThreshold);
  f(base->torqueThreshold); f(base->constraintHertz); f(base->constraintDampingRatio); f(base->drawScale);
  i(base->collideConnected);
}

static b3JointId revolute(b3RevoluteJointDef def) {
  b3JointId id = b3CreateRevoluteJoint(g_worldId, &def);
  name("joint.revolute"); baseOut(&def.base); f(def.targetAngle); i(def.enableSpring); f(def.hertz); f(def.dampingRatio);
  i(def.enableLimit); f(def.lowerAngle); f(def.upperAngle); i(def.enableMotor); f(def.maxMotorTorque); f(def.motorSpeed);
  arrow(); jointRef(id); end();
  return id;
}

static b3RevoluteJointDef revoluteDef(b3BodyId bodyA, b3BodyId bodyB, b3Transform frameA, b3Transform frameB) {
  b3RevoluteJointDef def = b3DefaultRevoluteJointDef();
  def.base.bodyIdA = bodyA;
  def.base.bodyIdB = bodyB;
  def.base.localFrameA = frameA;
  def.base.localFrameB = frameB;
  return def;
}

static b3JointId distance(b3DistanceJointDef def) {
  b3JointId id = b3CreateDistanceJoint(g_worldId, &def);
  name("joint.distance"); baseOut(&def.base); f(def.length); i(def.enableSpring); f(def.lowerSpringForce);
  f(def.upperSpringForce); f(def.hertz); f(def.dampingRatio); i(def.enableLimit); f(def.minLength); f(def.maxLength);
  i(def.enableMotor); f(def.maxMotorForce); f(def.motorSpeed); arrow(); jointRef(id); end();
  return id;
}

static b3DistanceJointDef distanceDef(b3BodyId bodyA, b3BodyId bodyB, b3Transform frameA, b3Transform frameB, float length) {
  b3DistanceJointDef def = b3DefaultDistanceJointDef();
  def.base.bodyIdA = bodyA;
  def.base.bodyIdB = bodyB;
  def.base.localFrameA = frameA;
  def.base.localFrameB = frameB;
  def.length = length;
  return def;
}

static b3JointId prismatic(b3PrismaticJointDef def) {
  b3JointId id = b3CreatePrismaticJoint(g_worldId, &def);
  name("joint.prismatic"); baseOut(&def.base); i(def.enableSpring); f(def.hertz); f(def.dampingRatio);
  f(def.targetTranslation); i(def.enableLimit); f(def.lowerTranslation); f(def.upperTranslation); i(def.enableMotor);
  f(def.maxMotorForce); f(def.motorSpeed); arrow(); jointRef(id); end();
  return id;
}

static b3PrismaticJointDef prismaticDef(b3BodyId bodyA, b3BodyId bodyB, b3Transform frameA, b3Transform frameB) {
  b3PrismaticJointDef def = b3DefaultPrismaticJointDef();
  def.base.bodyIdA = bodyA;
  def.base.bodyIdB = bodyB;
  def.base.localFrameA = frameA;
  def.base.localFrameB = frameB;
  return def;
}

static b3JointId wheel(b3WheelJointDef def) {
  b3JointId id = b3CreateWheelJoint(g_worldId, &def);
  name("joint.wheel"); baseOut(&def.base); i(def.enableSuspensionSpring); f(def.suspensionHertz);
  f(def.suspensionDampingRatio); i(def.enableSuspensionLimit); f(def.lowerSuspensionLimit); f(def.upperSuspensionLimit);
  i(def.enableSpinMotor); f(def.maxSpinTorque); f(def.spinSpeed); i(def.enableSteering); f(def.steeringHertz);
  f(def.steeringDampingRatio); f(def.targetSteeringAngle); f(def.maxSteeringTorque); i(def.enableSteeringLimit);
  f(def.lowerSteeringLimit); f(def.upperSteeringLimit); arrow(); jointRef(id); end();
  return id;
}

static b3JointId filterJoint(b3BodyId bodyA, b3BodyId bodyB) {
  b3FilterJointDef def = b3DefaultFilterJointDef();
  def.base.bodyIdA = bodyA;
  def.base.bodyIdB = bodyB;
  b3JointId id = b3CreateFilterJoint(g_worldId, &def);
  name("joint.filter"); baseOut(&def.base); arrow(); jointRef(id); end();
  return id;
}

static void destroyJoint(b3JointId id, bool wakeAttached) {
  b3DestroyJoint(id, wakeAttached);
  name("joint.destroy"); jointRef(id); i(wakeAttached); arrow(); end();
}

/* The body leaves the hashed list, keeping the order of the rest. */
static void forgetBody(b3BodyId id) {
  int k = 0;
  while (k < g_bodyCount && g_bodies[k].index1 != id.index1) k += 1;
  for (; k + 1 < g_bodyCount; k++) g_bodies[k] = g_bodies[k + 1];
  g_bodyCount -= 1;
}

static void destroyBody(b3BodyId id) {
  b3DestroyBody(id);
  forgetBody(id);
  name("body.destroy"); bodyRef(id); arrow(); end();
}

static void disableBody(b3BodyId id) {
  b3Body_Disable(id);
  name("body.disable"); bodyRef(id); arrow(); end();
}

static void enableBody(b3BodyId id) {
  b3Body_Enable(id);
  name("body.enable"); bodyRef(id); arrow(); end();
}

static void setType(b3BodyId id, b3BodyType type) {
  b3Body_SetType(id, type);
  name("body.setType"); bodyRef(id); i(type); arrow(); end();
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

static void joints(void) {
  int count = 0;
  for (int k = 0; k < g_world->joints.count; k++) {
    if (g_world->joints.data[k].jointId != B3_NULL_INDEX) count += 1;
  }
  name("world.joints"); arrow(); i(count);
  for (int k = 0; k < g_world->joints.count; k++) {
    b3Joint* joint = g_world->joints.data + k;
    if (joint->jointId == B3_NULL_INDEX) continue;
    i(joint->jointId); i(joint->generation); i(joint->type); i(joint->setIndex); i(joint->colorIndex);
    i(joint->localIndex); i(joint->islandId); i(joint->islandIndex); i(joint->collideConnected);
    b3JointSim* sim = b3GetJointSim(g_world, joint);
    f(sim->invMassA); f(sim->invMassB);
    if (joint->type == b3_distanceJoint) {
      b3DistanceJoint* d = &sim->distanceJoint;
      f(d->impulse); f(d->lowerImpulse); f(d->upperImpulse); f(d->motorImpulse);
    } else if (joint->type == b3_revoluteJoint) {
      b3RevoluteJoint* r = &sim->revoluteJoint;
      v3(r->linearImpulse); f(r->perpImpulse.x); f(r->perpImpulse.y); f(r->springImpulse); f(r->motorImpulse);
      f(r->lowerImpulse); f(r->upperImpulse);
    } else if (joint->type == b3_prismaticJoint) {
      b3PrismaticJoint* p = &sim->prismaticJoint;
      f(p->perpImpulse.x); f(p->perpImpulse.y); v3(p->angularImpulse); f(p->springImpulse); f(p->motorImpulse);
      f(p->lowerImpulse); f(p->upperImpulse);
    } else if (joint->type == b3_wheelJoint) {
      b3WheelJoint* w = &sim->wheelJoint;
      f(w->linearImpulse.x); f(w->linearImpulse.y); f(w->angularImpulse.x); f(w->angularImpulse.y); f(w->spinImpulse);
      f(w->suspensionSpringImpulse); f(w->lowerSuspensionImpulse); f(w->upperSuspensionImpulse);
      f(w->steeringSpringImpulse); f(w->lowerSteeringImpulse); f(w->upperSteeringImpulse);
    }
  }
  i(g_bodyCount);
  for (int k = 0; k < g_bodyCount; k++) {
    b3Body* body = g_world->bodies.data + (g_bodies[k].index1 - 1);
    i(body->setIndex); i(body->islandId);
  }
  end();
}

static void step(void) {
  float timeStep = 1.0f / 60.0f;
  int subSteps = 4;
  b3World_Step(g_worldId, timeStep, subSteps);
  uint64_t hash = 0xcbf29ce484222325ull;
  for (int k = 0; k < g_bodyCount; k++) {
    float values[13];
    bodyValues(g_bodies[k], values);
    for (int n = 0; n < 13; n++) hash = hashFloat(hash, values[n]);
  }
  b3JointEvents events = b3World_GetJointEvents(g_worldId);
  name("world.step"); f(timeStep); i(subSteps); arrow(); u64(hash); i(events.count);
  for (int k = 0; k < events.count; k++) jointRef(events.jointEvents[k].jointId);
  end();
  if (g_stepIndex % DUMP_EVERY == DUMP_EVERY - 1) {
    name("world.bodies"); arrow(); i(g_bodyCount);
    for (int k = 0; k < g_bodyCount; k++) {
      float values[13];
      bodyValues(g_bodies[k], values);
      for (int n = 0; n < 13; n++) f(values[n]);
    }
    end();
    joints();
  }
  g_stepIndex += 1;
}

static void steps(int count) {
  for (int k = 0; k < count; k++) step();
}

static void beginScene(void) {
  b3WorldDef worldDef = b3DefaultWorldDef();
  worldDef.gravity = (b3Vec3){ 0.0f, -10.0f, 0.0f };
  worldDef.enableSleep = true;
  worldDef.workerCount = 1;
  g_worldId = b3CreateWorld(&worldDef);
  g_world = b3GetWorldFromId(g_worldId);
  g_bodyCount = 0;
  g_stepIndex = 0;
  name("world.scene"); v3(worldDef.gravity); i(worldDef.enableSleep); arrow(); end();
}

static void endScene(void) {
  steps(600 - g_stepIndex);
  b3DestroyWorld(g_worldId);
}

static b3Transform rotatedFrame(float x, float y, float z, b3Vec3 axis, float angle) {
  b3Transform t = { { x, y, z }, b3MakeQuatFromAxisAngle(b3Normalize(axis), angle) };
  return t;
}

/* A car: a box chassis on four sphere wheels. The suspension axis (frame A's
 * x-axis) points up, the wheels spin about their z-axis. */
static void makeCar(b3Vec3 position, bool driven) {
  b3BodyId chassis = makeBody(b3_dynamicBody, position);
  boxShape(chassis, 1.0f, 0.25f, 0.6f);
  b3Vec3 up = { 0.0f, 0.0f, 1.0f };
  for (int k = 0; k < 4; k++) {
    float x = k < 2 ? 0.8f : -0.8f;
    float z = (k % 2 == 0) ? 0.7f : -0.7f;
    b3BodyId wheelBody = makeBody(b3_dynamicBody, (b3Vec3){ position.x + x, position.y - 0.4f, position.z + z });
    sphereShape(wheelBody, 0.35f);
    b3WheelJointDef def = b3DefaultWheelJointDef();
    def.base.bodyIdA = chassis;
    def.base.bodyIdB = wheelBody;
    def.base.localFrameA = rotatedFrame(x, -0.4f, z, up, 1.5707963f);
    def.base.localFrameB = frameAt(0.0f, 0.0f, 0.0f);
    def.suspensionHertz = 4.0f;
    def.suspensionDampingRatio = 0.7f;
    def.enableSuspensionLimit = true;
    def.lowerSuspensionLimit = -0.25f;
    def.upperSuspensionLimit = 0.2f;
    if (driven && k >= 2) {
      def.enableSpinMotor = true;
      def.maxSpinTorque = 3000.0f;
      def.spinSpeed = -5.0f;
    }
    if (driven && k < 2) {
      def.enableSteering = true;
      def.steeringHertz = 3.0f;
      def.steeringDampingRatio = 0.8f;
      def.targetSteeringAngle = 0.15f;
      def.maxSteeringTorque = 2000.0f;
      def.enableSteeringLimit = true;
      def.lowerSteeringLimit = -0.3f;
      def.upperSteeringLimit = 0.3f;
    }
    wheel(def);
  }
}

static b3BodyId makeGround(void) {
  b3BodyId ground = makeBody(b3_staticBody, (b3Vec3){ 0.0f, -0.5f, 0.0f });
  boxShape(ground, 20.0f, 0.5f, 20.0f);
  return ground;
}

int main(void) {
  printf("# box3d oracle: joints, commit %s, scalar, -ffp-contract=off\n", BOX3D_ORACLE_COMMIT);

  /* Pendulums and a filter joint */
  printf("# scene pendulum\n");
  beginScene();
  {
    b3BodyId ground = makeGround();

    b3BodyId bob = makeBody(b3_dynamicBody, (b3Vec3){ -6.0f, 6.0f, 0.0f });
    sphereShape(bob, 0.3f);
    revolute(revoluteDef(ground, bob, frameAt(-8.0f, 6.5f, 0.0f), frameAt(-2.0f, 0.5f, 0.0f)));

    b3BodyId weight = makeBody(b3_dynamicBody, (b3Vec3){ -1.0f, 5.0f, 1.0f });
    boxShape(weight, 0.25f, 0.25f, 0.25f);
    distance(distanceDef(ground, weight, frameAt(-3.0f, 7.5f, 0.0f), frameAt(0.0f, 0.0f, 0.0f), 3.2f));

    b3BodyId upper = makeBody(b3_dynamicBody, (b3Vec3){ 4.0f, 7.5f, 0.0f });
    capsuleShape(upper, 0.75f, 0.15f);
    b3BodyId lower = makeBody(b3_dynamicBody, (b3Vec3){ 5.5f, 7.5f, 0.0f });
    capsuleShape(lower, 0.75f, 0.15f);
    revolute(revoluteDef(ground, upper, frameAt(3.25f, 8.0f, 0.0f), frameAt(-0.75f, 0.0f, 0.0f)));
    revolute(revoluteDef(upper, lower, frameAt(0.75f, 0.0f, 0.0f), frameAt(-0.75f, 0.0f, 0.0f)));

    /* Overlapping boxes that would push apart without the filter joint */
    b3BodyId left = makeBody(b3_dynamicBody, (b3Vec3){ 8.0f, 0.5f, 3.0f });
    boxShape(left, 0.5f, 0.5f, 0.5f);
    b3BodyId right = makeBody(b3_dynamicBody, (b3Vec3){ 8.6f, 0.5f, 3.0f });
    boxShape(right, 0.5f, 0.5f, 0.5f);
    filterJoint(left, right);
  }
  endScene();

  /* Revolute limits, motor and spring */
  printf("# scene hinge\n");
  beginScene();
  {
    b3BodyId ground = makeGround();

    b3BodyId plank = makeBody(b3_dynamicBody, (b3Vec3){ -4.0f, 4.0f, 0.0f });
    boxShape(plank, 1.5f, 0.1f, 0.5f);
    b3RevoluteJointDef limited = revoluteDef(ground, plank, frameAt(-5.5f, 4.5f, 0.0f), frameAt(-1.5f, 0.0f, 0.0f));
    limited.enableLimit = true;
    limited.lowerAngle = -0.6f;
    limited.upperAngle = 0.4f;
    revolute(limited);

    b3BodyId wheel = makeBody(b3_dynamicBody, (b3Vec3){ 0.0f, 3.0f, 0.0f });
    boxShape(wheel, 1.0f, 0.2f, 0.2f);
    b3RevoluteJointDef motor = revoluteDef(ground, wheel, frameAt(0.0f, 3.5f, 0.0f), frameAt(0.0f, 0.0f, 0.0f));
    motor.enableMotor = true;
    motor.maxMotorTorque = 8.0f;
    motor.motorSpeed = 3.0f;
    revolute(motor);

    b3BodyId flap = makeBody(b3_dynamicBody, (b3Vec3){ 5.0f, 3.0f, 0.0f });
    boxShape(flap, 1.0f, 0.1f, 0.4f);
    b3RevoluteJointDef spring = revoluteDef(ground, flap, frameAt(4.0f, 3.5f, 0.0f), frameAt(-1.0f, 0.0f, 0.0f));
    spring.enableSpring = true;
    spring.hertz = 1.5f;
    spring.dampingRatio = 0.3f;
    spring.targetAngle = 0.5f;
    spring.enableLimit = true;
    spring.lowerAngle = -0.2f;
    spring.upperAngle = 1.2f;
    revolute(spring);

    /* A swinging door, its hinge axis along y */
    b3BodyId door = makeMovingBody(b3_dynamicBody, (b3Vec3){ 9.0f, 1.0f, 0.0f }, b3Quat_identity,
                                   b3Vec3_zero, (b3Vec3){ 0.0f, 2.0f, 0.0f });
    boxShape(door, 0.8f, 0.9f, 0.05f);
    b3Transform hingeA = { { 8.2f, 1.5f, 0.0f }, b3MakeQuatFromAxisAngle((b3Vec3){ 1.0f, 0.0f, 0.0f }, 1.5707963f) };
    b3Transform hingeB = { { -0.8f, 0.0f, 0.0f }, b3MakeQuatFromAxisAngle((b3Vec3){ 1.0f, 0.0f, 0.0f }, 1.5707963f) };
    b3RevoluteJointDef doorDef = revoluteDef(ground, door, hingeA, hingeB);
    doorDef.enableLimit = true;
    doorDef.lowerAngle = -1.0f;
    doorDef.upperAngle = 1.0f;
    revolute(doorDef);
  }
  endScene();

  /* Distance joints: spring, limits, motor, rope */
  printf("# scene distance\n");
  beginScene();
  {
    b3BodyId ground = makeGround();

    b3BodyId sprung = makeBody(b3_dynamicBody, (b3Vec3){ -6.0f, 4.0f, 0.0f });
    boxShape(sprung, 0.3f, 0.3f, 0.3f);
    b3DistanceJointDef springDef = distanceDef(ground, sprung, frameAt(-6.0f, 6.0f, 0.0f), frameAt(0.0f, 0.3f, 0.0f), 1.5f);
    springDef.enableSpring = true;
    springDef.hertz = 2.0f;
    springDef.dampingRatio = 0.1f;
    distance(springDef);

    b3BodyId limited = makeMovingBody(b3_dynamicBody, (b3Vec3){ -2.0f, 4.0f, 0.0f }, b3Quat_identity,
                                      (b3Vec3){ 2.0f, 3.0f, 0.0f }, b3Vec3_zero);
    sphereShape(limited, 0.3f);
    b3DistanceJointDef limitDef = distanceDef(ground, limited, frameAt(-2.0f, 6.0f, 0.0f), frameAt(0.0f, 0.0f, 0.0f), 1.5f);
    limitDef.enableSpring = true;
    limitDef.hertz = 1.0f;
    limitDef.dampingRatio = 0.5f;
    limitDef.enableLimit = true;
    limitDef.minLength = 1.0f;
    limitDef.maxLength = 2.5f;
    distance(limitDef);

    b3BodyId motored = makeBody(b3_dynamicBody, (b3Vec3){ 2.0f, 4.0f, 0.0f });
    boxShape(motored, 0.3f, 0.3f, 0.3f);
    b3DistanceJointDef motorDef = distanceDef(ground, motored, frameAt(2.0f, 6.0f, 0.0f), frameAt(0.0f, 0.0f, 0.0f), 2.0f);
    motorDef.enableSpring = true;
    motorDef.hertz = 0.0f;
    motorDef.enableLimit = true;
    motorDef.minLength = 0.5f;
    motorDef.maxLength = 3.0f;
    motorDef.enableMotor = true;
    motorDef.maxMotorForce = 30.0f;
    motorDef.motorSpeed = -0.5f;
    distance(motorDef);

    b3BodyId ropeA = makeMovingBody(b3_dynamicBody, (b3Vec3){ 6.0f, 3.0f, 0.0f }, b3Quat_identity,
                                    (b3Vec3){ 0.0f, 0.0f, 2.0f }, b3Vec3_zero);
    boxShape(ropeA, 0.4f, 0.4f, 0.4f);
    b3BodyId ropeB = makeBody(b3_dynamicBody, (b3Vec3){ 8.0f, 3.5f, 0.0f });
    sphereShape(ropeB, 0.4f);
    b3DistanceJointDef ropeDef = distanceDef(ropeA, ropeB, frameAt(0.4f, 0.0f, 0.0f), frameAt(0.0f, 0.0f, 0.0f), 2.0f);
    ropeDef.base.collideConnected = true;
    distance(ropeDef);
  }
  endScene();

  /* A chain bridge */
  printf("# scene bridge\n");
  beginScene();
  {
    b3BodyId ground = makeGround();
    b3BodyId previous = ground;
    b3Transform previousFrame = frameAt(-5.0f, 3.0f, 0.0f);
    b3BodyId links[10];
    for (int k = 0; k < 10; k++) {
      b3BodyId link = makeBody(b3_dynamicBody, (b3Vec3){ -4.5f + (float)k, 3.0f, 0.0f });
      boxShape(link, 0.5f, 0.1f, 0.6f);
      b3RevoluteJointDef def = revoluteDef(previous, link, previousFrame, frameAt(-0.5f, 0.0f, 0.0f));
      if (k == 4) def.base.forceThreshold = 40.0f;
      if (k == 6) def.base.torqueThreshold = 0.0f;
      revolute(def);
      previous = link;
      previousFrame = frameAt(0.5f, 0.0f, 0.0f);
      links[k] = link;
    }
    revolute(revoluteDef(links[9], ground, frameAt(0.5f, 0.0f, 0.0f), frameAt(5.0f, 3.5f, 0.0f)));

    b3BodyId load = makeBody(b3_dynamicBody, (b3Vec3){ -1.0f, 5.0f, 0.0f });
    boxShape(load, 0.4f, 0.4f, 0.4f);
    b3BodyId ball = makeBody(b3_dynamicBody, (b3Vec3){ 2.0f, 6.0f, 0.2f });
    sphereShape(ball, 0.35f);
  }
  endScene();

  /* A chain that sleeps, then breaks */
  printf("# scene split\n");
  beginScene();
  {
    b3BodyId ground = makeGround();
    b3BodyId previous = ground;
    b3Transform previousFrame = frameAt(0.0f, 8.0f, 0.0f);
    b3JointId middle = { 0 };
    for (int k = 0; k < 8; k++) {
      b3BodyId link = makeBody(b3_dynamicBody, (b3Vec3){ 0.0f, 7.5f - (float)k, 0.0f });
      sphereShape(link, 0.2f);
      b3JointId id = revolute(revoluteDef(previous, link, previousFrame, frameAt(0.0f, 0.5f, 0.0f)));
      if (k == 4) middle = id;
      previous = link;
      previousFrame = frameAt(0.0f, -0.5f, 0.0f);
    }
    steps(240);
    destroyJoint(middle, true);
  }
  endScene();

  /* Jointed bodies through the body changes */
  printf("# scene changes\n");
  beginScene();
  {
    makeGround();
    b3BodyId anchor = makeBody(b3_staticBody, (b3Vec3){ 0.0f, 6.0f, 0.0f });
    boxShape(anchor, 0.2f, 0.2f, 0.2f);
    b3BodyId first = makeBody(b3_dynamicBody, (b3Vec3){ 1.0f, 6.0f, 0.0f });
    boxShape(first, 0.4f, 0.1f, 0.1f);
    b3BodyId second = makeBody(b3_dynamicBody, (b3Vec3){ 2.0f, 6.0f, 0.0f });
    boxShape(second, 0.4f, 0.1f, 0.1f);
    b3BodyId third = makeBody(b3_dynamicBody, (b3Vec3){ 3.0f, 6.0f, 0.0f });
    boxShape(third, 0.4f, 0.1f, 0.1f);
    revolute(revoluteDef(anchor, first, frameAt(0.5f, 0.0f, 0.0f), frameAt(-0.5f, 0.0f, 0.0f)));
    revolute(revoluteDef(first, second, frameAt(0.5f, 0.0f, 0.0f), frameAt(-0.5f, 0.0f, 0.0f)));
    revolute(revoluteDef(second, third, frameAt(0.5f, 0.0f, 0.0f), frameAt(-0.5f, 0.0f, 0.0f)));

    /* Two resting boxes, apart, each in its own island */
    b3BodyId restA = makeBody(b3_dynamicBody, (b3Vec3){ -6.0f, 0.5f, 4.0f });
    boxShape(restA, 0.5f, 0.5f, 0.5f);
    b3BodyId restB = makeBody(b3_dynamicBody, (b3Vec3){ -3.0f, 0.5f, 4.0f });
    boxShape(restB, 0.5f, 0.5f, 0.5f);

    steps(100);
    disableBody(second);
    steps(40);
    enableBody(second);
    steps(60);
    setType(anchor, b3_dynamicBody);
    steps(40);
    setType(anchor, b3_staticBody);
    steps(60);
    destroyBody(third);
    steps(100);
    /* Both boxes asleep by now: the joint merges their sets */
    b3DistanceJointDef join = distanceDef(restA, restB, frameAt(0.5f, 0.0f, 0.0f), frameAt(-0.5f, 0.0f, 0.0f), 2.0f);
    distance(join);
    steps(60);
    setType(first, b3_kinematicBody);
    steps(40);
    setType(first, b3_dynamicBody);
  }
  endScene();

  /* Prismatic sliders and an elevator */
  printf("# scene prismatic\n");
  beginScene();
  {
    b3BodyId ground = makeGround();
    b3Vec3 alongZ = { 0.0f, 0.0f, 1.0f };

    /* Down a slope to the lower limit */
    b3BodyId slider = makeBody(b3_dynamicBody, (b3Vec3){ -6.0f, 3.0f, 0.0f });
    boxShape(slider, 0.3f, 0.3f, 0.3f);
    b3PrismaticJointDef slope = prismaticDef(ground, slider, rotatedFrame(-6.0f, 3.5f, 0.0f, alongZ, -0.5f), frameAt(0.0f, 0.0f, 0.0f));
    slope.enableLimit = true;
    slope.lowerTranslation = -1.0f;
    slope.upperTranslation = 1.5f;
    prismatic(slope);

    /* A sprung slider pulled to its target */
    b3BodyId sprung = makeMovingBody(b3_dynamicBody, (b3Vec3){ -2.0f, 3.0f, 0.0f }, b3Quat_identity,
                                     (b3Vec3){ 0.0f, 0.0f, 0.0f }, (b3Vec3){ 0.0f, 0.0f, 0.0f });
    boxShape(sprung, 0.3f, 0.2f, 0.2f);
    b3PrismaticJointDef spring = prismaticDef(ground, sprung, frameAt(-2.0f, 3.5f, 0.0f), frameAt(0.0f, 0.0f, 0.0f));
    spring.enableSpring = true;
    spring.hertz = 2.0f;
    spring.dampingRatio = 0.2f;
    spring.targetTranslation = 1.0f;
    prismatic(spring);

    /* An elevator: the axis points up, the motor lifts it to its upper limit */
    b3BodyId platform = makeBody(b3_dynamicBody, (b3Vec3){ 3.0f, 0.5f, 0.0f });
    boxShape(platform, 1.0f, 0.1f, 1.0f);
    b3PrismaticJointDef lift = prismaticDef(ground, platform, rotatedFrame(3.0f, 1.0f, 0.0f, alongZ, 1.5707963f),
                                            rotatedFrame(0.0f, 0.0f, 0.0f, alongZ, 1.5707963f));
    lift.enableLimit = true;
    lift.lowerTranslation = 0.0f;
    lift.upperTranslation = 3.0f;
    lift.enableMotor = true;
    lift.maxMotorForce = 30000.0f;
    lift.motorSpeed = 1.0f;
    prismatic(lift);
    b3BodyId cargo = makeBody(b3_dynamicBody, (b3Vec3){ 3.2f, 0.9f, 0.1f });
    boxShape(cargo, 0.3f, 0.3f, 0.3f);

    /* A telescope between two dynamic bodies, one hanging from a hinge */
    b3BodyId outer = makeBody(b3_dynamicBody, (b3Vec3){ 8.0f, 4.0f, 0.0f });
    capsuleShape(outer, 0.5f, 0.15f);
    revolute(revoluteDef(ground, outer, frameAt(7.0f, 4.5f, 0.0f), frameAt(-1.0f, 0.0f, 0.0f)));
    b3BodyId inner = makeMovingBody(b3_dynamicBody, (b3Vec3){ 9.0f, 4.0f, 0.0f }, b3Quat_identity,
                                    (b3Vec3){ 1.0f, 0.0f, 0.5f }, b3Vec3_zero);
    sphereShape(inner, 0.2f);
    b3PrismaticJointDef telescope = prismaticDef(outer, inner, frameAt(0.0f, 0.0f, 0.0f), frameAt(0.0f, 0.0f, 0.0f));
    telescope.enableLimit = true;
    telescope.lowerTranslation = 0.5f;
    telescope.upperTranslation = 1.5f;
    telescope.enableSpring = true;
    telescope.hertz = 1.0f;
    telescope.dampingRatio = 0.5f;
    telescope.targetTranslation = 1.2f;
    prismatic(telescope);
  }
  endScene();

  /* A car over a bump and a parked car */
  printf("# scene car\n");
  beginScene();
  {
    makeGround();
    b3BodyId bump = makeBody(b3_staticBody, (b3Vec3){ -2.0f, 0.0f, 0.0f });
    boxShape(bump, 0.4f, 0.12f, 3.0f);
    makeCar((b3Vec3){ -8.0f, 1.0f, 0.0f }, true);
    makeCar((b3Vec3){ 6.0f, 1.0f, 5.0f }, false);
  }
  endScene();

  return 0;
}
