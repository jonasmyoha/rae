/* Box3D oracle driver: convex geometry and distance (docs/physics-rae-port-design.md
 * §4, P2): aabb.c, sphere.c, capsule.c, distance.c (GJK, shape cast, sweeps,
 * time of impact).
 *
 * One line per call, `<function> <inputs> -> <outputs>`, every float a C hex
 * float (`%a`), integers and bools as decimals. Compound values print their
 * fields in declaration order:
 *
 *   transform   position(3) rotation(4: x y z w)
 *   sphere      center(3) radius
 *   capsule     center1(3) center2(3) radius
 *   proxy       count point(3)*count radius
 *   sweep       localCenter(3) c1(3) c2(3) q1(4) q2(4)
 *   cast        normal(3) point(3) fraction iterations triangleIndex childIndex
 *               materialIndex hit
 *   distance    pointA(3) pointB(3) normal(3) distance iterations simplexCount
 *   cache       metric count indexA(4) indexB(4)
 *   toi         state point(3) normal(3) fraction distance distanceIterations
 *               pushBackIterations rootIterations usedFallback
 *   mass        mass center(3) inertia(9, by column)
 *
 * Inputs come from the same fixed xorshift sequence as the math driver
 * (multiples of 1/256), so every machine prints the same file.
 *
 * Compiled against the cached Box3D checkout by tools/box3d-oracle/oracle.sh,
 * never by a Rae build. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "box3d/collision.h"
#include "box3d/math_functions.h"
#include "aabb.h"
#include "math_internal.h"
#include "shape.h"

static uint32_t g_state = 0x2545f491u;

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

static b3Transform next_transform(float range) {
  b3Transform t = { next_vec(range), next_quat() };
  return t;
}

static void f(float x) { printf(" %a", (double)x); }
static void i(long long x) { printf(" %lld", x); }
static void v3(b3Vec3 v) { f(v.x); f(v.y); f(v.z); }
static void q4(b3Quat q) { v3(q.v); f(q.s); }
static void m3(b3Matrix3 m) { v3(m.cx); v3(m.cy); v3(m.cz); }
static void tf(b3Transform t) { v3(t.p); q4(t.q); }
static void box(b3AABB a) { v3(a.lowerBound); v3(a.upperBound); }
static void sphere(b3Sphere s) { v3(s.center); f(s.radius); }
static void capsule(b3Capsule c) { v3(c.center1); v3(c.center2); f(c.radius); }
static void proxy(const b3ShapeProxy* p) {
  i(p->count);
  for (int k = 0; k < p->count; k++) v3(p->points[k]);
  f(p->radius);
}
static void sweep(b3Sweep s) { v3(s.localCenter); v3(s.c1); v3(s.c2); q4(s.q1); q4(s.q2); }
static void cast(b3CastOutput o) {
  v3(o.normal); v3(o.point); f(o.fraction); i(o.iterations); i(o.triangleIndex); i(o.childIndex);
  i(o.materialIndex); i(o.hit);
}
static void distance(b3DistanceOutput o) {
  v3(o.pointA); v3(o.pointB); v3(o.normal); f(o.distance); i(o.iterations); i(o.simplexCount);
}
static void cache(b3SimplexCache c) {
  f(c.metric); i(c.count);
  for (int k = 0; k < 4; k++) i(c.indexA[k]);
  for (int k = 0; k < 4; k++) i(c.indexB[k]);
}
static void toi(b3TOIOutput o) {
  i(o.state); v3(o.point); v3(o.normal); f(o.fraction); f(o.distance); i(o.distanceIterations);
  i(o.pushBackIterations); i(o.rootIterations); i(o.usedFallback);
}
static void mass(b3MassData m) { f(m.mass); v3(m.center); m3(m.inertia); }
static void plane(b3Plane p) { v3(p.normal); f(p.offset); }

static void name(const char* n) { printf("%s", n); }
static void arrow(void) { printf(" ->"); }
static void end(void) { printf("\n"); }

#define CASES 64
#define MAX_POINTS 8

static const float radii[] = { 0.0f, 0.0625f, 0.25f, 0.5f };

/* A point cloud of 1..MAX_POINTS points around `center`. */
static b3ShapeProxy next_proxy(b3Vec3* storage, b3Vec3 center, float spread) {
  int count = 1 + next_int(MAX_POINTS);
  for (int k = 0; k < count; k++) storage[k] = b3Add(center, next_vec(spread));
  b3ShapeProxy p = { storage, count, radii[next_int(4)] };
  return p;
}

static b3Sphere next_sphere(void) {
  b3Sphere s = { next_vec(2.0f), 0.125f + (float)next_int(16) / 16.0f };
  return s;
}

static b3Capsule next_capsule(void) {
  b3Vec3 c1 = next_vec(2.0f);
  /* Some capsules are short enough to take the sphere fallback. */
  b3Vec3 c2 = next_int(8) == 0 ? c1 : b3Add(c1, next_vec(1.5f));
  b3Capsule c = { c1, c2, 0.125f + (float)next_int(16) / 16.0f };
  return c;
}

/* A ray that starts outside [-range, range]^3 and aims near the origin. */
static b3RayCastInput next_ray(b3Vec3 target, float range) {
  b3Vec3 origin = next_vec(range);
  b3Vec3 aim = b3Add(target, next_vec(0.375f));
  b3RayCastInput r;
  r.origin = origin;
  r.translation = b3MulSV(1.0f + (float)next_int(4) * 0.5f, b3Sub(aim, origin));
  r.maxFraction = next_int(4) == 0 ? 0.5f : 1.0f;
  if (next_int(16) == 0) r.translation = b3Vec3_zero;
  return r;
}

static void ray(b3RayCastInput r) { v3(r.origin); v3(r.translation); f(r.maxFraction); }

static b3Sweep next_sweep(b3Vec3 start, b3Vec3 travel) {
  b3Sweep s;
  s.localCenter = next_vec(0.5f);
  s.c1 = start;
  s.c2 = b3Add(start, travel);
  s.q1 = next_quat();
  s.q2 = next_int(3) == 0 ? s.q1 : next_quat();
  return s;
}

int main(void) {
  printf("# box3d oracle: geometry (aabb, sphere, capsule, distance), commit %s, scalar, "
         "-ffp-contract=off\n", BOX3D_ORACLE_COMMIT);

  /* aabb.c */
  for (int k = 0; k < CASES; k++) {
    b3Vec3 a = next_vec(2.0f), b = next_vec(2.0f);
    b3AABB bounds = { b3Min(a, b), b3Max(a, b) };
    b3Vec3 p1 = next_vec(4.0f);
    b3Vec3 p2 = next_int(8) == 0 ? p1 : b3Add(next_vec(1.0f), b3MulSV(-1.0f, p1));
    if (next_int(8) == 0) p2.y = p1.y; /* a ray parallel to a slab */
    float minFraction = -1.0f, maxFraction = -1.0f;
    bool hit = b3RayCastAABB(bounds, p1, p2, &minFraction, &maxFraction);
    name("b3RayCastAABB"); box(bounds); v3(p1); v3(p2); arrow(); i(hit); f(minFraction); f(maxFraction); end();
  }

  /* sphere.c */
  for (int k = 0; k < CASES; k++) {
    b3Sphere s = next_sphere();
    float density = 0.5f + (float)next_int(8) * 0.25f;
    name("b3ComputeSphereMass"); sphere(s); f(density); arrow(); mass(b3ComputeSphereMass(&s, density)); end();
    b3Transform t1 = next_transform(3.0f), t2 = next_transform(3.0f);
    name("b3ComputeSphereAABB"); sphere(s); tf(t1); arrow(); box(b3ComputeSphereAABB(&s, t1)); end();
    name("b3ComputeSweptSphereAABB"); sphere(s); tf(t1); tf(t2); arrow(); box(b3ComputeSweptSphereAABB(&s, t1, t2)); end();

    b3Vec3 storage[MAX_POINTS];
    b3ShapeProxy p = next_proxy(storage, next_vec(2.5f), 0.75f);
    name("b3OverlapSphere"); sphere(s); tf(t1); proxy(&p); arrow(); i(b3OverlapSphere(&s, t1, &p)); end();

    b3RayCastInput r = next_ray(s.center, 4.0f);
    name("b3RayCastSphere"); sphere(s); ray(r); arrow(); cast(b3RayCastSphere(&s, &r)); end();
    if (b3LengthSquared(r.translation) > 0.0f) {
      name("b3RayCastHollowSphere"); sphere(s); ray(r); arrow(); cast(b3RayCastHollowSphere(&s, &r)); end();
      /* From inside: the hollow sphere's interior hit, the solid sphere's initial overlap. */
      b3RayCastInput inside = r;
      inside.origin = b3Add(s.center, b3MulSV(0.5f * s.radius, b3Normalize(next_nonzero_vec(1.0f))));
      inside.translation = b3MulSV(s.radius, next_nonzero_vec(1.0f));
      inside.maxFraction = 4.0f;
      name("b3RayCastHollowSphere"); sphere(s); ray(inside); arrow(); cast(b3RayCastHollowSphere(&s, &inside)); end();
      name("b3RayCastSphere"); sphere(s); ray(inside); arrow(); cast(b3RayCastSphere(&s, &inside)); end();
    }

    b3ShapeCastInput castInput = { p, b3MulSV(-1.5f, p.points[0]), next_int(4) == 0 ? 0.5f : 1.0f, next_int(2) == 1 };
    castInput.translation = b3Add(castInput.translation, s.center);
    name("b3ShapeCastSphere"); sphere(s); proxy(&p); v3(castInput.translation); f(castInput.maxFraction);
    i(castInput.canEncroach); arrow(); cast(b3ShapeCastSphere(&s, &castInput)); end();

    b3Capsule mover = next_capsule();
    b3PlaneResult result; memset(&result, 0, sizeof result);
    int count = b3CollideMoverAndSphere(&result, &s, &mover);
    name("b3CollideMoverAndSphere"); sphere(s); capsule(mover); arrow(); i(count); plane(result.plane);
    v3(result.point); end();
  }

  /* capsule.c */
  for (int k = 0; k < CASES; k++) {
    b3Capsule c = next_capsule();
    float density = 0.5f + (float)next_int(8) * 0.25f;
    name("b3ComputeCapsuleMass"); capsule(c); f(density); arrow(); mass(b3ComputeCapsuleMass(&c, density)); end();
    b3Transform t1 = next_transform(3.0f), t2 = next_transform(3.0f);
    name("b3ComputeCapsuleAABB"); capsule(c); tf(t1); arrow(); box(b3ComputeCapsuleAABB(&c, t1)); end();
    name("b3ComputeSweptCapsuleAABB"); capsule(c); tf(t1); tf(t2); arrow(); box(b3ComputeSweptCapsuleAABB(&c, t1, t2)); end();

    b3Vec3 storage[MAX_POINTS];
    b3ShapeProxy p = next_proxy(storage, next_vec(2.5f), 0.75f);
    name("b3OverlapCapsule"); capsule(c); tf(t1); proxy(&p); arrow(); i(b3OverlapCapsule(&c, t1, &p)); end();

    b3Vec3 middle = b3MulSV(0.5f, b3Add(c.center1, c.center2));
    for (int n = 0; n < 3; n++) {
      b3RayCastInput r = next_ray(middle, 4.0f);
      if (n == 2) { r.origin = b3Add(c.center1, next_vec(0.25f)); } /* starts inside or near */
      name("b3RayCastCapsule"); capsule(c); ray(r); arrow(); cast(b3RayCastCapsule(&c, &r)); end();
    }
    {
      /* A ray parallel to the axis: the det < FLT_EPSILON branch. */
      b3RayCastInput r;
      b3Vec3 axis = b3Sub(c.center2, c.center1);
      r.origin = b3Add(b3Sub(c.center1, b3MulSV(2.0f, axis)), b3MulSV(c.radius * 0.5f, b3Perp(b3Normalize(axis))));
      r.translation = b3MulSV(3.0f, axis);
      r.maxFraction = 1.0f;
      if (b3LengthSquared(axis) > 0.0f) {
        name("b3RayCastCapsule"); capsule(c); ray(r); arrow(); cast(b3RayCastCapsule(&c, &r)); end();
      }
    }

    b3ShapeCastInput castInput = { p, b3Sub(middle, p.points[0]), next_int(4) == 0 ? 0.5f : 1.0f, next_int(2) == 1 };
    castInput.translation = b3MulSV(1.5f, castInput.translation);
    name("b3ShapeCastCapsule"); capsule(c); proxy(&p); v3(castInput.translation); f(castInput.maxFraction);
    i(castInput.canEncroach); arrow(); cast(b3ShapeCastCapsule(&c, &castInput)); end();

    b3Capsule mover = next_capsule();
    b3PlaneResult result; memset(&result, 0, sizeof result);
    int count = b3CollideMoverAndCapsule(&result, &c, &mover);
    name("b3CollideMoverAndCapsule"); capsule(c); capsule(mover); arrow(); i(count); plane(result.plane);
    v3(result.point); end();
  }

  /* distance.c: support, GJK (cold and warm-started), shape cast, sweeps, time of impact. */
  for (int k = 0; k < CASES; k++) {
    b3Vec3 storage[MAX_POINTS];
    b3ShapeProxy p = next_proxy(storage, next_vec(1.0f), 1.0f);
    b3Vec3 axis = next_vec(2.0f);
    name("b3GetProxySupport"); proxy(&p); v3(axis); arrow(); i(b3GetProxySupport(&p, axis)); end();
  }
  for (int k = 0; k < 4 * CASES; k++) {
    b3Vec3 storageA[MAX_POINTS], storageB[MAX_POINTS];
    b3DistanceInput input;
    input.proxyA = next_proxy(storageA, b3Vec3_zero, 1.0f);
    input.proxyB = next_proxy(storageB, b3Vec3_zero, 1.0f);
    input.transform = next_transform(k < CASES ? 1.0f : 3.0f);
    input.useRadii = next_int(2) == 1;
    b3SimplexCache c = { 0 };
    b3DistanceOutput out = b3ShapeDistance(&input, &c, NULL, 0);
    name("b3ShapeDistance"); proxy(&input.proxyA); proxy(&input.proxyB); tf(input.transform); i(input.useRadii);
    arrow(); distance(out); cache(c); end();

    /* Warm start from that cache with B moved a little. */
    b3SimplexCache warm = c;
    b3Transform before = input.transform;
    input.transform.p = b3Add(input.transform.p, next_vec(0.25f));
    out = b3ShapeDistance(&input, &warm, NULL, 0);
    name("b3ShapeDistanceWarm"); proxy(&input.proxyA); proxy(&input.proxyB); tf(before); tf(input.transform);
    i(input.useRadii); arrow(); distance(out); cache(warm); end();
  }
  for (int k = 0; k < 2 * CASES; k++) {
    b3Vec3 storageA[MAX_POINTS], storageB[MAX_POINTS];
    b3ShapeCastPairInput input;
    input.proxyA = next_proxy(storageA, b3Vec3_zero, 0.75f);
    input.proxyB = next_proxy(storageB, b3Vec3_zero, 0.75f);
    input.transform = next_transform(3.0f);
    input.translationB = b3Add(b3MulSV(-1.25f, input.transform.p), next_vec(0.5f));
    input.maxFraction = next_int(4) == 0 ? 0.5f : 1.0f;
    input.canEncroach = next_int(2) == 1;
    name("b3ShapeCast"); proxy(&input.proxyA); proxy(&input.proxyB); tf(input.transform); v3(input.translationB);
    f(input.maxFraction); i(input.canEncroach); arrow(); cast(b3ShapeCast(&input)); end();
  }
  for (int k = 0; k < CASES; k++) {
    b3Sweep s = next_sweep(next_vec(2.0f), next_vec(2.0f));
    float time = (float)next_int(17) / 16.0f;
    name("b3GetSweepTransform"); sweep(s); f(time); arrow(); tf(b3GetSweepTransform(&s, time)); end();
  }
  for (int k = 0; k < 4 * CASES; k++) {
    b3Vec3 storageA[MAX_POINTS], storageB[MAX_POINTS];
    b3TOIInput input;
    input.proxyA = next_proxy(storageA, b3Vec3_zero, 0.5f + (float)next_int(3) * 0.25f);
    input.proxyB = next_proxy(storageB, b3Vec3_zero, 0.5f + (float)next_int(3) * 0.25f);
    b3Vec3 startA = next_vec(1.0f);
    input.sweepA = next_sweep(startA, next_vec(0.5f));
    /* B starts away from A and travels through it (or past it). */
    /* Every eighth pair starts overlapped. */
    float startDistance = k % 8 == 0 ? 0.25f : 3.0f;
    b3Vec3 offset = b3MulSV(startDistance, b3Normalize(next_nonzero_vec(1.0f)));
    b3Vec3 startB = b3Add(startA, offset);
    input.sweepB = next_sweep(startB, b3Add(b3MulSV(-2.0f, offset), next_vec(1.0f)));
    input.maxFraction = next_int(4) == 0 ? 0.5f : 1.0f;
    name("b3TimeOfImpact"); proxy(&input.proxyA); proxy(&input.proxyB); sweep(input.sweepA); sweep(input.sweepB);
    f(input.maxFraction); arrow(); toi(b3TimeOfImpact(&input)); end();
  }
  return 0;
}
