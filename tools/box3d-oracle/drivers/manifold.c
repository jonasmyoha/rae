/* Box3D oracle driver: contact manifolds without hulls (docs/physics-rae-port-design.md
 * §4, P2): manifold.c's feature pairs and polygon clipping, and from
 * convex_manifold.c the segment clipping, the four-point manifold reduction
 * and the sphere/capsule pair manifolds.
 *
 * convex_manifold.c is #included so its static helpers (b3ClipSegment,
 * b3ReduceManifoldPoints) can be called; the linker then takes every symbol
 * of that file from this driver instead of libbox3d.a.
 *
 * One line per call, `<function> <inputs> -> <outputs>`, floats as C hex
 * floats (`%a`), integers and bools as decimals. Compound values:
 *
 *   transform    position(3) rotation(4: x y z w)
 *   sphere       center(3) radius
 *   capsule      center1(3) center2(3) radius
 *   plane        normal(3) offset
 *   pair         owner1 index1 owner2 index2
 *   clip vertex  position(3) separation pair(4)
 *   point        point(3) separation pair(4) triangleIndex
 *   manifold     normal(3) pointCount point*pointCount
 *
 * Compiled against the cached Box3D checkout by tools/box3d-oracle/oracle.sh,
 * never by a Rae build. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "convex_manifold.c"

static uint32_t g_state = 0x6a09e667u;

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
static void plane(b3Plane p) { v3(p.normal); f(p.offset); }
static void pair(b3FeaturePair p) { i(p.owner1); i(p.index1); i(p.owner2); i(p.index2); }
static void clip(b3ClipVertex c) { v3(c.position); f(c.separation); pair(c.pair); }
static void point(b3LocalManifoldPoint p) { v3(p.point); f(p.separation); pair(p.pair); i(p.triangleIndex); }
static void manifold(const b3LocalManifold* m) {
  v3(m->normal); i(m->pointCount);
  for (int k = 0; k < m->pointCount; k++) point(m->points[k]);
}

static void name(const char* n) { printf("%s", n); }
static void arrow(void) { printf(" ->"); }
static void end(void) { printf("\n"); }

#define CASES 64

static b3FeaturePair next_pair(void) {
  return b3MakeFeaturePair((b3FeatureOwner)next_int(2), next_int(64), (b3FeatureOwner)next_int(2), next_int(64));
}

static b3Plane next_plane(float range) {
  b3Vec3 normal = b3Normalize(next_nonzero_vec(1.0f));
  b3Plane p = { normal, next_float(range) };
  return p;
}

static void reset(b3LocalManifold* m, b3LocalManifoldPoint* buffer) {
  memset(m, 0, sizeof *m);
  memset(buffer, 0, sizeof(b3LocalManifoldPoint) * 8);
  m->points = buffer;
}

static const int capacities[] = { 0, 1, 2, 4 };

int main(void) {
  printf("# box3d oracle: manifolds without hulls, commit %s, scalar, -ffp-contract=off\n", BOX3D_ORACLE_COMMIT);

  /* Feature pairs. */
  for (int k = 0; k < CASES; k++) {
    b3FeaturePair p = next_pair();
    name("b3FlipPair"); pair(p); arrow(); pair(b3FlipPair(p)); end();
    name("b3MakeFeatureId"); pair(p); arrow(); i(b3MakeFeatureId(p)); end();
  }

  /* Sutherland-Hodgman clipping of a planar convex polygon. */
  for (int k = 0; k < CASES; k++) {
    int count = 3 + next_int(6);
    b3ClipVertex polygon[B3_MAX_CLIP_POINTS], out[B3_MAX_CLIP_POINTS];
    b3Vec3 center = next_vec(1.0f);
    float radius = 0.5f + (float)next_int(4) * 0.25f;
    for (int n = 0; n < count; n++) {
      float angle = 2.0f * B3_PI * (float)n / (float)count;
      b3CosSin cs = b3ComputeCosSin(angle);
      polygon[n].position = b3Add(center, (b3Vec3){ radius * cs.cosine, next_float(0.125f), radius * cs.sine });
      polygon[n].separation = next_float(0.5f);
      polygon[n].pair = next_pair();
    }
    b3Plane clipPlane = next_plane(1.0f);
    b3Plane refPlane = next_plane(1.0f);
    int edge = next_int(32);
    int outCount = b3ClipPolygon(out, polygon, count, clipPlane, edge, refPlane);
    name("b3ClipPolygon"); i(count);
    for (int n = 0; n < count; n++) clip(polygon[n]);
    plane(clipPlane); i(edge); plane(refPlane); arrow(); i(outCount);
    for (int n = 0; n < outCount; n++) clip(out[n]);
    end();
  }

  /* Clipping a segment against a plane (the slot past the kept points keeps
     its separation, so both inputs print in full). */
  for (int k = 0; k < CASES; k++) {
    b3ClipVertex segment[2];
    for (int n = 0; n < 2; n++) {
      segment[n].position = next_vec(1.0f);
      segment[n].separation = next_float(0.5f);
      segment[n].pair = next_pair();
    }
    b3Plane p = next_plane(0.5f);
    name("b3ClipSegment"); clip(segment[0]); clip(segment[1]); plane(p); arrow();
    int count = b3ClipSegment(segment, p);
    i(count); clip(segment[0]); clip(segment[1]); end();
  }

  /* Reducing a contact patch to at most four points. */
  for (int k = 0; k < CASES; k++) {
    int capacity = k % 8 == 0 ? 2 : 4;
    int count = 1 + next_int(16);
    b3LocalManifoldPoint points[16], kept[8];
    b3LocalManifold m; reset(&m, kept);
    m.normal = b3Normalize(next_nonzero_vec(1.0f));
    b3Vec3 tangent1 = b3Perp(m.normal);
    b3Vec3 tangent2 = b3Cross(m.normal, tangent1);
    for (int n = 0; n < count; n++) {
      b3Vec3 p = b3Add(b3MulSV(next_float(1.0f), tangent1), b3MulSV(next_float(1.0f), tangent2));
      points[n].point = b3MulAdd(p, next_float(0.0625f), m.normal);
      points[n].separation = next_float(0.0625f);
      points[n].pair = next_pair();
      points[n].triangleIndex = next_int(100);
    }
    name("b3ReduceManifoldPoints"); i(capacity); v3(m.normal); i(count);
    for (int n = 0; n < count; n++) point(points[n]);
    arrow();
    b3ReduceManifoldPoints(&m, capacity, points, count);
    manifold(&m); end();
  }

  /* Sphere and capsule pairs. */
  for (int k = 0; k < 2 * CASES; k++) {
    b3LocalManifoldPoint buffer[8];
    b3LocalManifold m;
    int capacity = capacities[next_int(4)];
    b3Sphere a = { next_vec(1.0f), 0.25f + (float)next_int(4) * 0.25f };
    b3Sphere b = { next_vec(1.0f), 0.25f + (float)next_int(4) * 0.25f };
    b3Transform t = { next_vec(1.0f), next_quat(3.0f) };
    if (k % 16 == 0) { b.center = b3InvTransformPoint(t, a.center); } /* concentric */
    reset(&m, buffer);
    b3CollideSpheres(&m, capacity, &a, &b, t);
    name("b3CollideSpheres"); i(capacity); sphere(a); sphere(b); tf(t); arrow(); manifold(&m); end();

    b3Capsule c = { next_vec(1.0f), next_vec(1.0f), 0.125f + (float)next_int(4) * 0.125f };
    reset(&m, buffer);
    b3CollideCapsuleAndSphere(&m, capacity, &c, &b, t);
    name("b3CollideCapsuleAndSphere"); i(capacity); capsule(c); sphere(b); tf(t); arrow(); manifold(&m); end();
  }
  for (int k = 0; k < 4 * CASES; k++) {
    b3LocalManifoldPoint buffer[8];
    b3LocalManifold m;
    int capacity = k % 16 == 0 ? 1 : 2 + next_int(3);
    b3Capsule a = { next_vec(1.0f), next_vec(1.0f), 0.125f + (float)next_int(4) * 0.125f };
    b3Capsule b;
    b3Transform t;
    if (k % 2 == 0) {
      /* Nearly parallel and close: the two-point branch. */
      b3Vec3 side = b3MulSV(a.radius * 1.5f, b3Perp(b3Normalize(b3Sub(a.center2, a.center1))));
      b3Vec3 shift = next_vec(0.25f);
      b.center1 = b3Add(a.center1, shift);
      b.center2 = b3Add(b3Add(a.center2, shift), next_vec(0.015625f));
      b.center1 = b3Add(b.center1, side);
      b.center2 = b3Add(b.center2, side);
      b.radius = 0.125f + (float)next_int(4) * 0.125f;
      t.p = b3Vec3_zero; t.q = next_int(2) ? b3Quat_identity : next_quat(0.03125f);
    } else {
      b.center1 = next_vec(1.0f); b.center2 = next_vec(1.0f);
      b.radius = 0.125f + (float)next_int(4) * 0.125f;
      t.p = next_vec(1.0f); t.q = next_quat(3.0f);
    }
    if (k % 32 == 1) { b.center2 = b.center1; } /* too short */
    reset(&m, buffer);
    b3CollideCapsules(&m, capacity, &a, &b, t);
    name("b3CollideCapsules"); i(capacity); capsule(a); capsule(b); tf(t); arrow(); manifold(&m); end();
  }
  return 0;
}
