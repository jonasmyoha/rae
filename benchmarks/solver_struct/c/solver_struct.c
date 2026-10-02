/* The C side of benchmarks/solver_struct (see rae/Main.rae): the same kernel,
 * operation for operation. Built twice: plain (no bounds checks) and with
 * -DCHECKED, which checks every index the way Rae's safe accessors do. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

typedef struct { float vx, vy, vz, wx, wy, wz, invMass, invInertia; } BodyState;
typedef struct {
  float normalX, normalY, normalZ, anchorAX, anchorAY, anchorAZ, anchorBX, anchorBY, anchorBZ;
  float normalMass, impulse, bias, friction, restitution, relax, spare;
  int64_t bodyA, bodyB;
} ContactConstraint;

enum { constraintCount = 100000, bodyCount = 25000, passes = 20 };

static int64_t next_state(int64_t s) { return (s * 1103515245 + 12345) % 2147483648LL; }
static float unit_float(int64_t s) { return (float)((s % 2001) - 1000) / 1000.0f; }
static int64_t now_ns(void) {
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

#ifdef CHECKED
static const BodyState zero_body;
static BodyState body_at(const BodyState* bodies, int64_t n, int64_t i) { return (i >= 0 && i < n) ? bodies[i] : zero_body; }
static void body_set(BodyState* bodies, int64_t n, int64_t i, BodyState v) { if (i >= 0 && i < n) bodies[i] = v; }
#define BODY(i) body_at(bodies, bodyCount, (i))
#define SET_BODY(i, v) body_set(bodies, bodyCount, (i), (v))
#define CHECK_CONSTRAINT(i) if ((i) < 0 || (i) >= constraintCount) continue;
#else
#define BODY(i) bodies[(i)]
#define SET_BODY(i, v) (bodies[(i)] = (v))
#define CHECK_CONSTRAINT(i)
#endif

int main(void) {
  int64_t state = 12345;
  BodyState* bodies = malloc(sizeof(BodyState) * bodyCount);
  ContactConstraint* constraints = malloc(sizeof(ContactConstraint) * constraintCount);
  for (int64_t i = 0; i < bodyCount; i++) {
    state = next_state(state);
    float velocity = unit_float(state);
    bodies[i] = (BodyState){ velocity, -velocity, 0.5f * velocity, 0.0f, 0.25f * velocity, 0.0f, 1.0f, 0.5f };
  }
  for (int64_t i = 0; i < constraintCount; i++) {
    state = next_state(state); int64_t bodyA = state % bodyCount;
    state = next_state(state); int64_t bodyB = state % bodyCount;
    state = next_state(state); float lever = unit_float(state);
    constraints[i] = (ContactConstraint){ 0.0f, 1.0f, 0.0f, lever, 0.5f, -lever, -lever, -0.5f, lever,
                                          0.4f, 0.0f, 0.01f, 0.6f, 0.0f, 1.0f, 0.0f, bodyA, bodyB };
  }
  int64_t start = now_ns();
  for (int pass = 0; pass < passes; pass++) {
    for (int64_t i = 0; i < constraintCount; i++) {
      CHECK_CONSTRAINT(i)
      ContactConstraint* c = &constraints[i];
      BodyState a = BODY(c->bodyA), b = BODY(c->bodyB);
      float dvx = b.vx + (b.wy * c->anchorBZ - b.wz * c->anchorBY) - (a.vx + (a.wy * c->anchorAZ - a.wz * c->anchorAY));
      float dvy = b.vy + (b.wz * c->anchorBX - b.wx * c->anchorBZ) - (a.vy + (a.wz * c->anchorAX - a.wx * c->anchorAZ));
      float dvz = b.vz + (b.wx * c->anchorBY - b.wy * c->anchorBX) - (a.vz + (a.wx * c->anchorAY - a.wy * c->anchorAX));
      float normalVelocity = dvx * c->normalX + dvy * c->normalY + dvz * c->normalZ;
      float newImpulse = c->impulse - c->normalMass * (normalVelocity + c->bias);
      if (newImpulse < 0.0f) newImpulse = 0.0f;
      float delta = newImpulse - c->impulse;
      c->impulse = newImpulse;
      float px = delta * c->normalX, py = delta * c->normalY, pz = delta * c->normalZ;
      SET_BODY(c->bodyA, ((BodyState){ a.vx - a.invMass * px, a.vy - a.invMass * py, a.vz - a.invMass * pz,
        a.wx - a.invInertia * (c->anchorAY * pz - c->anchorAZ * py),
        a.wy - a.invInertia * (c->anchorAZ * px - c->anchorAX * pz),
        a.wz - a.invInertia * (c->anchorAX * py - c->anchorAY * px), a.invMass, a.invInertia }));
      BodyState b2 = BODY(c->bodyB);
      SET_BODY(c->bodyB, ((BodyState){ b2.vx + b2.invMass * px, b2.vy + b2.invMass * py, b2.vz + b2.invMass * pz,
        b2.wx + b2.invInertia * (c->anchorBY * pz - c->anchorBZ * py),
        b2.wy + b2.invInertia * (c->anchorBZ * px - c->anchorBX * pz),
        b2.wz + b2.invInertia * (c->anchorBX * py - c->anchorBY * px), b2.invMass, b2.invInertia }));
    }
  }
  int64_t elapsed = now_ns() - start;
  float checksum = 0.0f;
  for (int64_t i = 0; i < constraintCount; i++) checksum = checksum + constraints[i].impulse;
#ifdef CHECKED
  printf("RESULT,c_checked,%lld,%lld\n", (long long)elapsed, (long long)(checksum * 1000.0f));
#else
  printf("RESULT,c,%lld,%lld\n", (long long)elapsed, (long long)(checksum * 1000.0f));
#endif
  free(bodies);
  free(constraints);
  return 0;
}
