/* Box3D oracle driver: triangle manifolds (docs/physics-rae-port-design.md
 * §4, P5a): triangle_manifold.c — a triangle against a sphere, a capsule and
 * a hull (with its SAT cache) — and b3ClosestPointOnTriangle.
 *
 * Line formats as in hullmanifold.c, except that a manifold also carries its
 * triangle feature and squaredDistance after pointCount:
 *
 *   manifold   normal(3) pointCount feature squaredDistance, per point
 *              point(3) separation owner1 index1 owner2 index2 triangleIndex
 *   triangle   v1(3) v2(3) v3(3)
 *
 * Each collision runs over three nudged frames, carrying its cache, so the
 * cached paths (and the back-side hysteresis of the hull) are exercised.
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

static uint32_t g_state = 0xa54ff53au;

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
  v3(m->normal); i(m->pointCount); i(m->feature); f(m->squaredDistance);
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

static void tri(const b3Vec3* t) { v3(t[0]); v3(t[1]); v3(t[2]); }

/* A triangle about 1 to 2 across: often flat in the XZ plane (a floor, so
   faces meet faces), else random; counter-clockwise seen from +y for the
   floor, so its front side faces up. */
static void next_triangle(b3Vec3* t) {
  if (next_int(2) == 0) {
    float s = 0.75f + (float)next_int(4) * 0.25f;
    t[0] = (b3Vec3){ -s, 0.0f, s };
    t[1] = (b3Vec3){ s, 0.0f, s };
    t[2] = (b3Vec3){ 0.0f, 0.0f, -s };
    if (next_int(3) == 0) {
      b3Vec3 shift = next_vec(0.25f);
      for (int k = 0; k < 3; k++) t[k] = b3Add(t[k], shift);
    }
  } else {
    for (;;) {
      t[0] = next_vec(1.0f);
      t[1] = next_vec(1.0f);
      t[2] = next_vec(1.0f);
      b3Vec3 n = b3Cross(b3Sub(t[1], t[0]), b3Sub(t[2], t[0]));
      if (b3LengthSquared(n) > 0.25f) break;
    }
  }
}

/* A point in front of (or, now and then, behind) the triangle's plane. */
static b3Vec3 next_position(const b3Vec3* t, float height, float lateral) {
  b3Vec3 n = b3Normalize(b3Cross(b3Sub(t[1], t[0]), b3Sub(t[2], t[0])));
  b3Vec3 center = b3MulSV(1.0f / 3.0f, b3Add(b3Add(t[0], t[1]), t[2]));
  b3Vec3 offset = next_vec(lateral);
  offset = b3Sub(offset, b3MulSV(b3Dot(offset, n), n));
  float side = next_int(6) == 0 ? -1.0f : 1.0f;
  return b3Add(b3Add(center, offset), b3MulSV(side * height, n));
}

static void nudge_triangle(b3Vec3* t) {
  b3Vec3 shift = next_vec(0.015625f);
  for (int k = 0; k < 3; k++) t[k] = b3Add(t[k], shift);
}

int main(void) {
  printf("# box3d oracle: triangle manifolds, commit %s, scalar, -ffp-contract=off\n", BOX3D_ORACLE_COMMIT);

  /* The closest point of a triangle: every region. */
  for (int k = 0; k < 96; k++) {
    b3Vec3 t[3];
    next_triangle(t);
    b3Vec3 q = next_vec(2.0f);
    b3TrianglePoint p = b3ClosestPointOnTriangle(t[0], t[1], t[2], q);
    name("b3ClosestPointOnTriangle"); tri(t); v3(q); arrow(); v3(p.point); i(p.feature); end();
  }

  /* Triangle-sphere. */
  for (int k = 0; k < 96; k++) {
    b3Vec3 t[3];
    next_triangle(t);
    b3Sphere ball = { b3Vec3_zero, 0.125f + (float)next_int(4) * 0.125f };
    ball.center = next_position(t, ball.radius + next_float(0.0625f), 0.5f);
    if (k % 8 == 0) ball.center = b3MulAdd(b3MulSV(1.0f / 3.0f, b3Add(b3Add(t[0], t[1]), t[2])), 0.0f, ball.center); /* on the face */
    for (int pass = 0; pass < 3; pass++) {
      b3LocalManifoldPoint buffer[8];
      b3LocalManifold m;
      int capacity = k % 24 == 5 ? 0 : 4;
      memset(&m, 0, sizeof m); memset(buffer, 0, sizeof buffer); m.points = buffer;
      name("b3CollideTriangleAndSphere"); i(capacity); tri(t); sphere(ball); arrow();
      b3CollideTriangleAndSphere(&m, capacity, t, &ball);
      manifold(&m); end();
      ball.center = b3Add(ball.center, next_vec(0.015625f));
    }
  }

  /* Triangle-capsule: shallow (closest points), parallel to the face (two
     clipped points) and deep (face or edge), with the simplex cache. */
  for (int k = 0; k < 144; k++) {
    b3Vec3 t[3];
    next_triangle(t);
    float radius = 0.0625f + (float)next_int(4) * 0.0625f;
    b3Vec3 center = next_position(t, radius + next_float(0.0625f), 0.25f);
    b3Vec3 half = next_vec(0.375f);
    if (k % 3 == 0) {
      /* parallel to the triangle plane */
      b3Vec3 n = b3Normalize(b3Cross(b3Sub(t[1], t[0]), b3Sub(t[2], t[0])));
      half = b3Sub(half, b3MulSV(b3Dot(half, n), n));
    }
    if (k % 5 == 0) {
      /* deep: the center pushed below the surface */
      b3Vec3 n = b3Normalize(b3Cross(b3Sub(t[1], t[0]), b3Sub(t[2], t[0])));
      center = b3MulSub(center, radius + 0.03125f, n);
    }
    b3Capsule pill = { b3Sub(center, half), b3Add(center, half), radius };
    b3SimplexCache c = { 0 };
    for (int pass = 0; pass < 3; pass++) {
      b3LocalManifoldPoint buffer[8];
      b3LocalManifold m;
      int capacity = k % 24 == 7 ? 1 : 4;
      memset(&m, 0, sizeof m); memset(buffer, 0, sizeof buffer); m.points = buffer;
      name("b3CollideTriangleAndCapsule"); i(capacity); tri(t); capsule(pill); simplex(c); arrow();
      b3CollideTriangleAndCapsule(&m, capacity, t, &pill, &c);
      manifold(&m); simplex(c); end();
      b3Vec3 shift = next_vec(0.015625f);
      pill.center1 = b3Add(pill.center1, shift);
      pill.center2 = b3Add(pill.center2, shift);
    }
  }

  /* Triangle-hull, the triangle in the hull's frame, with the SAT cache. */
  for (int k = 0; k < 192; k++) {
    Spec s; b3BoxHull storage;
    next_spec(&s);
    b3HullData* h = build(&s, &storage);
    if (h == NULL) continue;
    /* The triangle placed under the hull: shifted so the hull rests on,
       sinks into, floats above or sits behind it. */
    b3Vec3 t[3];
    next_triangle(t);
    b3Vec3 n = b3Normalize(b3Cross(b3Sub(t[1], t[0]), b3Sub(t[2], t[0])));
    float drop = h->innerRadius + 0.25f + next_float(0.25f);
    if (k % 4 == 0) drop = 0.125f + next_float(0.0625f); /* deep */
    if (k % 9 == 0) drop = -drop; /* behind */
    b3Vec3 centroid = b3MulSV(1.0f / 3.0f, b3Add(b3Add(t[0], t[1]), t[2]));
    b3Vec3 shift = b3Sub(b3MulSub(h->center, drop, n), centroid);
    shift = b3Add(shift, b3MulSV(0.5f, next_vec(1.0f)));
    for (int v = 0; v < 3; v++) t[v] = b3Add(t[v], shift);
    int flags = next_int(8);
    bool speculative = next_int(4) != 0;
    b3SATCache c = { 0 };
    if (k % 16 == 3) c.type = b3_manualFaceAxisA;
    if (k % 16 == 7) c.type = b3_manualFaceAxisB;
    if (k % 16 == 11) c.type = b3_manualEdgePairAxis;
    for (int pass = 0; pass < 3; pass++) {
      b3LocalManifoldPoint buffer[8];
      b3LocalManifold m;
      int capacity = k % 40 == 9 ? 2 : 8;
      memset(&m, 0, sizeof m); memset(buffer, 0, sizeof buffer); m.points = buffer;
      name("b3CollideTriangleAndHull"); i(capacity); tri(t); i(flags); spec(&s); sat(c); i(speculative); arrow();
      b3CollideTriangleAndHull(&m, capacity, t[0], t[1], t[2], flags, h, &c, speculative);
      manifold(&m); sat(c); end();
      nudge_triangle(t);
    }
    if (s.kind == 0) b3DestroyHull(h);
  }
  return 0;
}
