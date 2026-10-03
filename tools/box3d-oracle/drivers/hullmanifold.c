/* Box3D oracle driver: hull manifolds (docs/physics-rae-port-design.md §4, P2d):
 * the rest of convex_manifold.c — hull-sphere, hull-capsule, the separating
 * axis test (scalar path) and hull-hull with its SAT cache.
 *
 * One line per call, `<function> <inputs> -> <outputs>`, floats as C hex
 * floats (`%a`), integers and bools as decimals. Compound values:
 *
 *   transform  position(3) rotation(4: x y z w)
 *   spec       how a hull is built: `0 count point(3)*count` (b3CreateHull,
 *              max 128 vertices) or `1 hx hy hz transform`
 *              (b3MakeTransformedBoxHull)
 *   sphere     center(3) radius;  capsule  center1(3) center2(3) radius
 *   simplex    metric count indexA(4) indexB(4)  (b3SimplexCache)
 *   sat        separation type indexA indexB hit  (b3SATCache)
 *   axis       normal(3) separation indexA indexB type  (b3SeparatingAxis)
 *   manifold   normal(3) pointCount, per point point(3) separation
 *              owner1 index1 owner2 index2 triangleIndex
 *
 * A call with a cache prints the cache it starts from among its inputs and
 * the cache it leaves among its outputs, so a fixture replays warm starts
 * line by line. The manifold buffer starts zeroed, capacity as printed.
 *
 * Compiled against the cached Box3D checkout by tools/box3d-oracle/oracle.sh,
 * never by a Rae build. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "box3d/collision.h"
#include "box3d/math_functions.h"
#include "manifold.h"
#include "math_internal.h"

static uint32_t g_state = 0x3c6ef372u;

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

static b3Quat next_quat(float maxAngle) {
  b3Vec3 axis = b3Normalize(next_nonzero_vec(4.0f));
  return b3MakeQuatFromAxisAngle(axis, next_float(maxAngle));
}

static void f(float x) { printf(" %a", (double)x); }
static void i(long long x) { printf(" %lld", x); }
static void v3(b3Vec3 v) { f(v.x); f(v.y); f(v.z); }
static void q4(b3Quat q) { v3(q.v); f(q.s); }
static void tf(b3Transform t) { v3(t.p); q4(t.q); }
static void sphere(b3Sphere s) { v3(s.center); f(s.radius); }
static void capsule(b3Capsule c) { v3(c.center1); v3(c.center2); f(c.radius); }
static void simplex(b3SimplexCache c) {
  f(c.metric); i(c.count);
  for (int k = 0; k < 4; k++) i(c.indexA[k]);
  for (int k = 0; k < 4; k++) i(c.indexB[k]);
}
static void sat(b3SATCache c) { f(c.separation); i(c.type); i(c.indexA); i(c.indexB); i(c.hit); }
static void axis(b3SeparatingAxis a) { v3(a.normal); f(a.separation); i(a.indexA); i(a.indexB); i(a.type); }
static void manifold(const b3LocalManifold* m) {
  v3(m->normal); i(m->pointCount);
  for (int k = 0; k < m->pointCount; k++) {
    b3LocalManifoldPoint p = m->points[k];
    v3(p.point); f(p.separation); i(p.pair.owner1); i(p.pair.index1); i(p.pair.owner2); i(p.pair.index2);
    i(p.triangleIndex);
  }
}

static void name(const char* n) { printf("%s", n); }
static void arrow(void) { printf(" ->"); }
static void end(void) { printf("\n"); }

#define CASES 48
#define MAX_POINTS 24

typedef struct Spec {
  int kind;
  int count;
  b3Vec3 points[MAX_POINTS];
  float hx, hy, hz;
  b3Transform transform;
} Spec;

static b3HullData* build(const Spec* s, b3BoxHull* storage) {
  if (s->kind == 0) return b3CreateHull(s->points, s->count, 128);
  *storage = b3MakeTransformedBoxHull(s->hx, s->hy, s->hz, s->transform);
  return &storage->base;
}

static void spec(const Spec* s) {
  i(s->kind);
  if (s->kind == 0) {
    i(s->count);
    for (int k = 0; k < s->count; k++) v3(s->points[k]);
  } else {
    f(s->hx); f(s->hy); f(s->hz); tf(s->transform);
  }
}

/* A hull of size about 1: a box (often axis aligned, so faces meet faces)
   or a random cloud. */
static void next_spec(Spec* s) {
  s->kind = next_int(3) == 0 ? 0 : 1;
  if (s->kind == 0) {
    s->count = 6 + next_int(MAX_POINTS - 6);
    for (int k = 0; k < s->count; k++) s->points[k] = next_vec(0.75f);
  } else {
    s->hx = 0.25f + (float)next_int(4) * 0.125f;
    s->hy = 0.25f + (float)next_int(4) * 0.125f;
    s->hz = 0.25f + (float)next_int(4) * 0.125f;
    s->transform.p = next_int(2) ? b3Vec3_zero : next_vec(0.25f);
    s->transform.q = next_int(2) ? b3Quat_identity : next_quat(3.0f);
  }
}

/* B near A: touching, slightly apart, or overlapping. Some rotations are
   identity so faces line up (the clipping and the edge-pair paths). */
static b3Transform next_relative(float reach) {
  b3Transform t;
  t.p = b3MulSV(reach, b3Normalize(next_nonzero_vec(1.0f)));
  t.q = next_int(3) == 0 ? b3Quat_identity : next_quat(3.0f);
  return t;
}

static b3Transform nudge(b3Transform t) {
  t.p = b3Add(t.p, next_vec(0.015625f));
  t.q = b3NormalizeQuat(b3MulQuat(next_quat(0.03125f), t.q));
  return t;
}

int main(void) {
  printf("# box3d oracle: hull manifolds, commit %s, scalar, -ffp-contract=off\n", BOX3D_ORACLE_COMMIT);

  /* Hull-sphere and hull-capsule, each followed by a warm call. */
  for (int k = 0; k < CASES; k++) {
    Spec s; b3BoxHull storage;
    next_spec(&s);
    b3HullData* h = build(&s, &storage);
    if (h == NULL) continue;
    b3LocalManifoldPoint buffer[8];
    b3LocalManifold m;

    b3Sphere ball = { b3Vec3_zero, 0.125f + (float)next_int(4) * 0.125f };
    b3Transform t = next_relative(0.25f + (float)next_int(8) * 0.125f);
    if (k % 8 == 0) t.p = b3MulSV(0.0625f, t.p); /* deep */
    b3SimplexCache c = { 0 };
    for (int pass = 0; pass < 2; pass++) {
      int capacity = k % 12 == 5 ? 0 : 4;
      memset(&m, 0, sizeof m); memset(buffer, 0, sizeof buffer); m.points = buffer;
      name("b3CollideHullAndSphere"); i(capacity); spec(&s); sphere(ball); tf(t); simplex(c); arrow();
      b3CollideHullAndSphere(&m, capacity, h, &ball, t, &c);
      manifold(&m); simplex(c); end();
      t = nudge(t);
    }

    b3Capsule pill = { next_vec(0.5f), next_vec(0.5f), 0.0625f + (float)next_int(4) * 0.0625f };
    if (k % 3 == 0) { pill.center1 = (b3Vec3){ -0.5f, 0.0f, 0.0f }; pill.center2 = (b3Vec3){ 0.5f, 0.0f, 0.0f }; }
    t = next_relative(0.375f + (float)next_int(8) * 0.125f);
    if (k % 3 == 0) t.q = b3Quat_identity;
    if (k % 7 == 0) t.p = b3MulSV(0.0625f, t.p); /* deep */
    c = (b3SimplexCache){ 0 };
    for (int pass = 0; pass < 2; pass++) {
      int capacity = k % 12 == 7 ? 1 : 4;
      memset(&m, 0, sizeof m); memset(buffer, 0, sizeof buffer); m.points = buffer;
      name("b3CollideHullAndCapsule"); i(capacity); spec(&s); capsule(pill); tf(t); simplex(c); arrow();
      b3CollideHullAndCapsule(&m, capacity, h, &pill, t, &c);
      manifold(&m); simplex(c); end();
      t = nudge(t);
    }
    if (s.kind == 0) b3DestroyHull(h);
  }

  /* The separating axis test and hull-hull, with the SAT cache carried over
     three steps of a slowly moving pair. */
  for (int k = 0; k < 3 * CASES; k++) {
    Spec sa, sb; b3BoxHull storageA, storageB;
    next_spec(&sa); next_spec(&sb);
    b3HullData* ha = build(&sa, &storageA);
    b3HullData* hb = build(&sb, &storageB);
    if (ha && hb) {
      b3Transform t = next_relative(0.5f + (float)next_int(8) * 0.125f);
      if (k % 5 == 0) t.p = b3MulSV(0.25f, t.p);
      bool earlyReturn = next_int(2) == 1;
      b3AxisQuery q = b3ComputeSeparatingAxis(ha, hb, t, earlyReturn);
      name("b3ComputeSeparatingAxis"); spec(&sa); spec(&sb); tf(t); i(earlyReturn); arrow();
      axis(q.faceA); axis(q.faceB); axis(q.edge); i(q.separatedFeature); end();

      b3SATCache c = { 0 };
      if (k % 16 == 3) c.type = b3_manualFaceAxisA;
      if (k % 16 == 7) c.type = b3_manualFaceAxisB;
      if (k % 16 == 11) c.type = b3_manualEdgePairAxis;
      for (int pass = 0; pass < 3; pass++) {
        b3LocalManifoldPoint buffer[8];
        b3LocalManifold m;
        memset(&m, 0, sizeof m); memset(buffer, 0, sizeof buffer); m.points = buffer;
        int capacity = k % 20 == 9 ? 2 : 4;
        name("b3CollideHulls"); i(capacity); spec(&sa); spec(&sb); tf(t); sat(c); arrow();
        b3CollideHulls(&m, capacity, ha, hb, t, &c);
        manifold(&m); sat(c); end();
        t = nudge(t);
      }
    }
    if (ha && sa.kind == 0) b3DestroyHull(ha);
    if (hb && sb.kind == 0) b3DestroyHull(hb);
  }
  return 0;
}
