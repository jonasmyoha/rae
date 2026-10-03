/* Box3D oracle driver: the math layer (docs/physics-rae-port-design.md §4, P0).
 *
 * Calls Box3D's own math functions (include/box3d/math_functions.h, the
 * scalar build) over a fixed input set and prints one line per call:
 *
 *   <function> <input> <input> ... -> <output> <output> ...
 *
 * Every float is a C hex float (`%a`): exact, and `strtof` reads it back to
 * the same bits, so the Rae port (P1) compares its results bit for bit.
 * Inputs come from a fixed xorshift sequence mapped to multiples of 1/256, so
 * they are exactly representable and the same on every machine.
 *
 * This file is a tool: it is compiled against the cached Box3D checkout by
 * tools/box3d-oracle/oracle.sh and never by a Rae build. */
#include <stdint.h>
#include <stdio.h>

#include "box3d/math_functions.h"
#include "math_internal.h"   /* src/: the internal helpers the engine uses */
#include <math.h>

static uint32_t g_state = 0x9e3779b9u;

static uint32_t next_u32(void) {
  uint32_t x = g_state;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  g_state = x;
  return x;
}

/* A float in [-range, range] that is a multiple of 1/256: exact in binary32. */
static float next_float(float range) {
  int steps = (int)(range * 256.0f);
  int k = (int)(next_u32() % (uint32_t)(2 * steps + 1)) - steps;
  return (float)k / 256.0f;
}

static b3Vec3 next_vec(float range) {
  b3Vec3 v = { next_float(range), next_float(range), next_float(range) };
  return v;
}

/* A non-zero vector (normalisation needs one). */
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

static void f(float x) { printf(" %a", (double)x); }
static void v3(b3Vec3 v) { f(v.x); f(v.y); f(v.z); }
static void q4(b3Quat q) { v3(q.v); f(q.s); }
static void m3(b3Matrix3 m) { v3(m.cx); v3(m.cy); v3(m.cz); }
static void tf(b3Transform t) { v3(t.p); q4(t.q); }
static void i(long long x) { printf(" %lld", x); }
static void v2(b3Vec2 v) { f(v.x); f(v.y); }
static void m2(b3Matrix2 m) { v2(m.cx); v2(m.cy); }
static void box(b3AABB a) { v3(a.lowerBound); v3(a.upperBound); }
static void plane(b3Plane p) { v3(p.normal); f(p.offset); }
static b3Vec2 next_vec2(float range) { b3Vec2 v = { next_float(range), next_float(range) }; return v; }
static b3AABB next_box(float range) {
  b3Vec3 a = next_vec(range), b = next_vec(range);
  b3AABB box = { b3Min(a, b), b3Max(a, b) };
  return box;
}

#define MORE 24
static void name(const char* n) { printf("%s", n); }
static void arrow(void) { printf(" ->"); }
static void end(void) { printf("\n"); }

#define CASES 48

int main(void) {
  printf("# box3d oracle: math layer, commit %s, scalar (BOX3D_DISABLE_SIMD), -ffp-contract=off\n",
         BOX3D_ORACLE_COMMIT);

  /* Scalars: the deterministic trig of Box3D (b3Atan2, b3ComputeCosSin). */
  const float edges[] = { 0.0f, 1.0f, -1.0f, 0.5f, -0.5f, 2.0f, -2.0f, 1e-3f, -1e-3f, 100.0f, -100.0f };
  const int edge_count = (int)(sizeof edges / sizeof edges[0]);
  for (int i = 0; i < edge_count; i++) {
    for (int j = 0; j < edge_count; j++) {
      float y = edges[i], x = edges[j];
      name("b3Atan2"); f(y); f(x); arrow(); f(b3Atan2(y, x)); end();
    }
  }
  for (int i = 0; i < CASES; i++) {
    float y = next_float(8.0f), x = next_float(8.0f);
    name("b3Atan2"); f(y); f(x); arrow(); f(b3Atan2(y, x)); end();
  }
  const float angles[] = { 0.0f, 0.5f, 1.0f, 1.5707963f, 3.1415927f, -3.1415927f, 6.2831855f, -10.0f, 100.0f };
  for (int i = 0; i < (int)(sizeof angles / sizeof angles[0]); i++) {
    b3CosSin cs = b3ComputeCosSin(angles[i]);
    name("b3ComputeCosSin"); f(angles[i]); arrow(); f(cs.cosine); f(cs.sine); end();
  }
  for (int i = 0; i < CASES; i++) {
    float a = next_float(12.0f);
    b3CosSin cs = b3ComputeCosSin(a);
    name("b3ComputeCosSin"); f(a); arrow(); f(cs.cosine); f(cs.sine); end();
    name("b3UnwindAngle"); f(a); arrow(); f(b3UnwindAngle(a)); end();
  }

  /* Vectors. */
  for (int i = 0; i < CASES; i++) {
    b3Vec3 a = next_nonzero_vec(8.0f), b = next_nonzero_vec(8.0f);
    float s = next_float(4.0f), t = next_float(1.0f);
    float length = 0.0f;
    name("b3Add"); v3(a); v3(b); arrow(); v3(b3Add(a, b)); end();
    name("b3Sub"); v3(a); v3(b); arrow(); v3(b3Sub(a, b)); end();
    name("b3Mul"); v3(a); v3(b); arrow(); v3(b3Mul(a, b)); end();
    name("b3Neg"); v3(a); arrow(); v3(b3Neg(a)); end();
    name("b3Dot"); v3(a); v3(b); arrow(); f(b3Dot(a, b)); end();
    name("b3Length"); v3(a); arrow(); f(b3Length(a)); end();
    name("b3LengthSquared"); v3(a); arrow(); f(b3LengthSquared(a)); end();
    name("b3Distance"); v3(a); v3(b); arrow(); f(b3Distance(a, b)); end();
    name("b3Normalize"); v3(a); arrow(); v3(b3Normalize(a)); end();
    b3Vec3 unit = b3GetLengthAndNormalize(&length, a);
    name("b3GetLengthAndNormalize"); v3(a); arrow(); v3(unit); f(length); end();
    name("b3Perp"); v3(a); arrow(); v3(b3Perp(a)); end();
    name("b3Cross"); v3(a); v3(b); arrow(); v3(b3Cross(a, b)); end();
    name("b3Lerp"); v3(a); v3(b); f(t); arrow(); v3(b3Lerp(a, b, t)); end();
    name("b3MulAdd"); v3(a); f(s); v3(b); arrow(); v3(b3MulAdd(a, s, b)); end();
    name("b3MulSub"); v3(a); f(s); v3(b); arrow(); v3(b3MulSub(a, s, b)); end();
    name("b3MulSV"); f(s); v3(a); arrow(); v3(b3MulSV(s, a)); end();
    name("b3Abs"); v3(a); arrow(); v3(b3Abs(a)); end();
    name("b3Min"); v3(a); v3(b); arrow(); v3(b3Min(a, b)); end();
    name("b3Max"); v3(a); v3(b); arrow(); v3(b3Max(a, b)); end();
    b3Vec3 lower = b3Min(a, b), upper = b3Max(a, b), p = next_vec(10.0f);
    name("b3Clamp"); v3(p); v3(lower); v3(upper); arrow(); v3(b3Clamp(p, lower, upper)); end();
  }

  /* Quaternions. */
  for (int i = 0; i < CASES; i++) {
    b3Vec3 axis = b3Normalize(next_nonzero_vec(4.0f));
    float angle = next_float(3.0f);
    b3Quat q = b3MakeQuatFromAxisAngle(axis, angle);
    b3Quat r = next_quat();
    b3Vec3 v = next_vec(8.0f);
    float radians = 0.0f;
    float alpha = next_float(1.0f);
    name("b3MakeQuatFromAxisAngle"); v3(axis); f(angle); arrow(); q4(q); end();
    name("b3RotateVector"); q4(q); v3(v); arrow(); v3(b3RotateVector(q, v)); end();
    name("b3InvRotateVector"); q4(q); v3(v); arrow(); v3(b3InvRotateVector(q, v)); end();
    name("b3DotQuat"); q4(q); q4(r); arrow(); f(b3DotQuat(q, r)); end();
    name("b3MulQuat"); q4(q); q4(r); arrow(); q4(b3MulQuat(q, r)); end();
    name("b3InvMulQuat"); q4(q); q4(r); arrow(); q4(b3InvMulQuat(q, r)); end();
    name("b3Conjugate"); q4(q); arrow(); q4(b3Conjugate(q)); end();
    b3Quat scaled = { b3MulSV(1.5f, q.v), 1.5f * q.s };
    name("b3NormalizeQuat"); q4(scaled); arrow(); q4(b3NormalizeQuat(scaled)); end();
    b3Vec3 got_axis = b3GetAxisAngle(&radians, q);
    name("b3GetAxisAngle"); q4(q); arrow(); v3(got_axis); f(radians); end();
    name("b3GetQuatAngle"); q4(q); arrow(); f(b3GetQuatAngle(q)); end();
    name("b3GetTwistAngle"); q4(q); arrow(); f(b3GetTwistAngle(q)); end();
    name("b3NLerp"); q4(q); q4(r); f(alpha); arrow(); q4(b3NLerp(q, r, alpha)); end();
    b3Vec3 u1 = b3Normalize(next_nonzero_vec(4.0f)), u2 = b3Normalize(next_nonzero_vec(4.0f));
    name("b3ComputeQuatBetweenUnitVectors"); v3(u1); v3(u2); arrow(); q4(b3ComputeQuatBetweenUnitVectors(u1, u2)); end();
    b3Matrix3 rotation = { b3RotateVector(q, b3Vec3_axisX), b3RotateVector(q, b3Vec3_axisY), b3RotateVector(q, b3Vec3_axisZ) };
    name("b3MakeQuatFromMatrix"); m3(rotation); arrow(); q4(b3MakeQuatFromMatrix(&rotation)); end();
  }

  /* Transforms. */
  for (int i = 0; i < CASES; i++) {
    b3Transform a = { next_vec(10.0f), next_quat() };
    b3Transform b = { next_vec(10.0f), next_quat() };
    b3Vec3 p = next_vec(10.0f);
    name("b3MulTransforms"); tf(a); tf(b); arrow(); tf(b3MulTransforms(a, b)); end();
    name("b3InvertTransform"); tf(a); arrow(); tf(b3InvertTransform(a)); end();
    name("b3TransformPoint"); tf(a); v3(p); arrow(); v3(b3TransformPoint(a, p)); end();
    name("b3InvTransformPoint"); tf(a); v3(p); arrow(); v3(b3InvTransformPoint(a, p)); end();
  }

  /* 3x3 matrices. */
  for (int i = 0; i < CASES; i++) {
    b3Matrix3 m = { next_vec(4.0f), next_vec(4.0f), next_vec(4.0f) };
    b3Matrix3 n = { next_vec(4.0f), next_vec(4.0f), next_vec(4.0f) };
    b3Vec3 v = next_vec(4.0f);
    float mass = next_float(10.0f);
    if (mass < 0.0f) mass = -mass;
    name("b3MulMV"); m3(m); v3(v); arrow(); v3(b3MulMV(m, v)); end();
    name("b3MulMM"); m3(m); m3(n); arrow(); m3(b3MulMM(m, n)); end();
    name("b3Transpose"); m3(m); arrow(); m3(b3Transpose(m)); end();
    name("b3Det"); m3(m); arrow(); f(b3Det(m)); end();
    name("b3InvertMatrix"); m3(m); arrow(); m3(b3InvertMatrix(m)); end();
    name("b3Steiner"); f(mass); v3(v); arrow(); m3(b3Steiner(mass, v)); end();
  }

  /* Segments. */
  for (int i = 0; i < CASES; i++) {
    b3Vec3 p1 = next_vec(6.0f), q1 = next_vec(6.0f), p2 = next_vec(6.0f), q2 = next_vec(6.0f);
    b3Vec3 target = next_vec(6.0f);
    name("b3PointToSegmentDistance"); v3(p1); v3(q1); v3(target); arrow(); v3(b3PointToSegmentDistance(p1, q1, target)); end();
    b3SegmentDistanceResult r = b3SegmentDistance(p1, q1, p2, q2);
    name("b3SegmentDistance"); v3(p1); v3(q1); v3(p2); v3(q2); arrow(); v3(r.point1); f(r.fraction1); v3(r.point2); f(r.fraction2); end();
  }

  /* ---- Everything else the Rae port has (P1): scalar helpers ---- */
  for (int k = 0; k < MORE; k++) {
    int a = (int)(next_u32() % 2001) - 1000, b = (int)(next_u32() % 2001) - 1000;
    int lo = a < b ? a : b, hi = a < b ? b : a, x = (int)(next_u32() % 3001) - 1500;
    name("b3MinInt"); i(a); i(b); arrow(); i(b3MinInt(a, b)); end();
    name("b3MaxInt"); i(a); i(b); arrow(); i(b3MaxInt(a, b)); end();
    name("b3ClampInt"); i(x); i(lo); i(hi); arrow(); i(b3ClampInt(x, lo, hi)); end();
    float fa = next_float(8.0f), fb = next_float(8.0f), fx = next_float(10.0f), alpha = next_float(1.0f);
    float flo = fa < fb ? fa : fb, fhi = fa < fb ? fb : fa;
    name("b3AbsFloat"); f(fa); arrow(); f(b3AbsFloat(fa)); end();
    name("b3MinFloat"); f(fa); f(fb); arrow(); f(b3MinFloat(fa, fb)); end();
    name("b3MaxFloat"); f(fa); f(fb); arrow(); f(b3MaxFloat(fa, fb)); end();
    name("b3ClampFloat"); f(fx); f(flo); f(fhi); arrow(); f(b3ClampFloat(fx, flo, fhi)); end();
    name("b3LerpFloat"); f(fa); f(fb); f(alpha); arrow(); f(b3LerpFloat(fa, fb, alpha)); end();
    name("b3Sin"); f(fx); arrow(); f(b3Sin(fx)); end();
    name("b3Cos"); f(fx); arrow(); f(b3Cos(fx)); end();
    int numerator = (int)(next_u32() % 1000), exponent = 1 + (int)(next_u32() % 5);
    name("b3CeilingInt"); i(numerator); i(1 << exponent); arrow(); i(b3CeilingInt(numerator, 1 << exponent)); end();
    name("b3CeilingPow2"); i(numerator); i(1 << exponent); i(exponent); arrow(); i(b3CeilingPow2(numerator, 1 << exponent, exponent)); end();
  }
  {
    const float specials[] = { 0.0f, -1.5f, 3.0e38f, NAN, INFINITY, -INFINITY };
    for (int k = 0; k < 6; k++) { name("b3IsValidFloat"); f(specials[k]); arrow(); i(b3IsValidFloat(specials[k])); end(); }
  }

  /* ---- vectors ---- */
  for (int k = 0; k < MORE; k++) {
    b3Vec3 a = next_nonzero_vec(8.0f), b = next_vec(8.0f), c = next_vec(8.0f);
    float s = next_float(4.0f), t = next_float(4.0f), u = next_float(4.0f), maxLength = next_float(6.0f);
    if (maxLength < 0.0f) maxLength = -maxLength;
    b3Vec3 unit = b3Normalize(a);
    name("b3DistanceSquared"); v3(a); v3(b); arrow(); f(b3DistanceSquared(a, b)); end();
    name("b3Blend2"); f(s); v3(a); f(t); v3(b); arrow(); v3(b3Blend2(s, a, t, b)); end();
    name("b3Blend3"); f(s); v3(a); f(t); v3(b); f(u); v3(c); arrow(); v3(b3Blend3(s, a, t, b, u, c)); end();
    name("b3Sign"); v3(b); arrow(); v3(b3Sign(b)); end();
    name("b3SafeScale"); v3(b); arrow(); v3(b3SafeScale(b)); end();
    name("b3IsNormalized"); v3(unit); arrow(); i(b3IsNormalized(unit)); end();
    name("b3IsNormalized"); v3(a); arrow(); i(b3IsNormalized(a)); end();
    name("b3ClampLength"); v3(a); f(maxLength); arrow(); v3(b3ClampLength(a, maxLength)); end();
    name("b3ArbitraryPerp"); v3(unit); arrow(); v3(b3ArbitraryPerp(unit)); end();
    name("b3ScalarTripleProduct"); v3(a); v3(b); v3(c); arrow(); f(b3ScalarTripleProduct(a, b, c)); end();
    name("b3MajorAxis"); v3(b); arrow(); i(b3MajorAxis(b)); end();
    name("b3MinElement"); v3(b); arrow(); f(b3MinElement(b)); end();
    name("b3MaxElement"); v3(b); arrow(); f(b3MaxElement(b)); end();
    name("b3ModifiedCross"); v3(a); v3(b); arrow(); v3(b3ModifiedCross(a, b)); end();
  }

  /* ---- quaternions ---- */
  for (int k = 0; k < MORE; k++) {
    b3Quat q = next_quat(), r = next_quat();
    b3Vec3 small = b3MulSV(0.001f, next_vec(4.0f)), big = next_nonzero_vec(2.0f), delta = b3MulSV(0.05f, next_vec(4.0f));
    name("b3IsNormalizedQuat"); q4(q); arrow(); i(b3IsNormalizedQuat(q)); end();
    name("b3NegateQuat"); q4(q); arrow(); q4(b3NegateQuat(q)); end();
    name("b3GetSwingAngle"); q4(q); arrow(); f(b3GetSwingAngle(q)); end();
    name("b3QuatFromExponentialMap"); v3(small); arrow(); q4(b3QuatFromExponentialMap(small)); end();
    name("b3QuatFromExponentialMap"); v3(big); arrow(); q4(b3QuatFromExponentialMap(big)); end();
    name("b3IntegrateRotation"); q4(q); v3(delta); arrow(); q4(b3IntegrateRotation(q, delta)); end();
    name("b3DeltaQuatToRotation"); q4(q); q4(r); arrow(); v3(b3DeltaQuatToRotation(q, r)); end();
  }

  /* ---- transforms and world positions (single precision) ---- */
  for (int k = 0; k < MORE; k++) {
    b3Transform a = { next_vec(10.0f), next_quat() };
    b3Transform b = { next_vec(10.0f), next_quat() };
    b3Vec3 p = next_vec(10.0f), d = next_vec(4.0f), base = next_vec(10.0f);
    float t = next_float(1.0f);
    name("b3InvMulTransforms"); tf(a); tf(b); arrow(); tf(b3InvMulTransforms(a, b)); end();
    name("b3MulWorldTransforms"); tf(a); tf(b); arrow(); tf(b3MulWorldTransforms(a, b)); end();
    name("b3InvMulWorldTransforms"); tf(a); tf(b); arrow(); tf(b3InvMulWorldTransforms(a, b)); end();
    name("b3TransformWorldPoint"); tf(a); v3(p); arrow(); v3(b3TransformWorldPoint(a, p)); end();
    name("b3InvTransformWorldPoint"); tf(a); v3(p); arrow(); v3(b3InvTransformWorldPoint(a, p)); end();
    name("b3ToRelativeTransform"); tf(a); v3(base); arrow(); tf(b3ToRelativeTransform(a, base)); end();
    name("b3LerpPosition"); v3(p); v3(base); f(t); arrow(); v3(b3LerpPosition(p, base, t)); end();
    name("b3SubPos"); v3(p); v3(base); arrow(); v3(b3SubPos(p, base)); end();
    name("b3OffsetPos"); v3(p); v3(d); arrow(); v3(b3OffsetPos(p, d)); end();
  }

  /* ---- matrices and inertia ---- */
  for (int k = 0; k < MORE; k++) {
    b3Matrix3 m = { next_vec(4.0f), next_vec(4.0f), next_vec(4.0f) };
    b3Matrix3 n = { next_vec(4.0f), next_vec(4.0f), next_vec(4.0f) };
    b3Vec3 v = next_vec(4.0f), lower = next_vec(2.0f);
    b3Vec3 upper = b3Add(lower, b3Abs(next_nonzero_vec(2.0f)));
    float s = next_float(3.0f), mass = b3AbsFloat(next_float(10.0f)) + 0.5f, radius = b3AbsFloat(next_float(3.0f)) + 0.1f;
    float height = b3AbsFloat(next_float(4.0f)) + 0.1f;
    b3Quat q = next_quat();
    b3Transform t = { next_vec(5.0f), next_quat() };
    name("b3NegateMat3"); m3(m); arrow(); m3(b3NegateMat3(m)); end();
    name("b3AddMM"); m3(m); m3(n); arrow(); m3(b3AddMM(m, n)); end();
    name("b3SubMM"); m3(m); m3(n); arrow(); m3(b3SubMM(m, n)); end();
    name("b3MulSM"); f(s); m3(m); arrow(); m3(b3MulSM(s, m)); end();
    name("b3Solve3"); m3(m); v3(v); arrow(); v3(b3Solve3(m, v)); end();
    name("b3InvertT"); m3(m); arrow(); m3(b3InvertT(m)); end();
    name("b3AbsMatrix3"); m3(m); arrow(); m3(b3AbsMatrix3(m)); end();
    name("b3MakeMatrixFromQuat"); q4(q); arrow(); m3(b3MakeMatrixFromQuat(q)); end();
    name("b3MakeDiagonalMatrix"); v3(v); arrow(); m3(b3MakeDiagonalMatrix(v.x, v.y, v.z)); end();
    name("b3Skew"); v3(v); arrow(); m3(b3Skew(v)); end();
    name("b3SphereInertia"); f(mass); f(radius); arrow(); m3(b3SphereInertia(mass, radius)); end();
    name("b3CylinderInertia"); f(mass); f(radius); f(height); arrow(); m3(b3CylinderInertia(mass, radius, height)); end();
    name("b3BoxInertia"); f(mass); v3(lower); v3(upper); arrow(); m3(b3BoxInertia(mass, lower, upper)); end();
    name("b3RotateInertia"); q4(q); m3(m); arrow(); m3(b3RotateInertia(q, m)); end();
    name("b3TransformInertia"); tf(t); m3(m); f(mass); arrow(); m3(b3TransformInertia(t, m, mass)); end();
  }

  /* ---- boxes ---- */
  for (int k = 0; k < MORE; k++) {
    b3Vec3 points[5];
    for (int j = 0; j < 5; j++) points[j] = next_vec(6.0f);
    float radius = b3AbsFloat(next_float(1.0f));
    b3AABB a = next_box(6.0f), b = next_box(6.0f);
    b3Vec3 p = next_vec(8.0f), origin = next_vec(20.0f);
    float extension = b3AbsFloat(next_float(2.0f));
    b3Transform t = { next_vec(5.0f), next_quat() };
    name("b3MakeAABB"); for (int j = 0; j < 5; j++) v3(points[j]); f(radius); arrow(); box(b3MakeAABB(points, 5, radius)); end();
    name("b3AABB_Contains"); box(a); box(b); arrow(); i(b3AABB_Contains(a, b)); end();
    name("b3AABB_Contains"); box(b3AABB_Union(a, b)); box(b); arrow(); i(b3AABB_Contains(b3AABB_Union(a, b), b)); end();
    name("b3AABB_Area"); box(a); arrow(); f(b3AABB_Area(a)); end();
    name("b3AABB_Center"); box(a); arrow(); v3(b3AABB_Center(a)); end();
    name("b3AABB_Extents"); box(a); arrow(); v3(b3AABB_Extents(a)); end();
    name("b3AABB_Union"); box(a); box(b); arrow(); box(b3AABB_Union(a, b)); end();
    name("b3AABB_Inflate"); box(a); f(extension); arrow(); box(b3AABB_Inflate(a, extension)); end();
    name("b3AABB_Overlaps"); box(a); box(b); arrow(); i(b3AABB_Overlaps(a, b)); end();
    name("b3AABB_Transform"); tf(t); box(a); arrow(); box(b3AABB_Transform(t, a)); end();
    name("b3ClosestPointToAABB"); v3(p); box(a); arrow(); v3(b3ClosestPointToAABB(p, a)); end();
    name("b3AABB_AddPoint"); box(a); v3(p); arrow(); box(b3AABB_AddPoint(a, p)); end();
    name("b3OffsetAABB"); box(a); v3(origin); arrow(); box(b3OffsetAABB(a, origin)); end();
  }

  /* ---- lines, planes ---- */
  for (int k = 0; k < MORE; k++) {
    b3Vec3 p1 = next_vec(6.0f), d1 = next_vec(3.0f), p2 = next_vec(6.0f), d2 = next_vec(3.0f);
    b3Vec3 a = next_vec(6.0f), b = next_vec(6.0f), c = next_vec(6.0f), q = next_vec(6.0f);
    b3Transform t = { next_vec(5.0f), next_quat() };
    b3Plane plane1 = b3MakePlaneFromPoints(a, b, c);
    b3Plane scaled = { b3MulSV(2.5f, plane1.normal), 2.5f * plane1.offset };
    b3SegmentDistanceResult line = b3LineDistance(p1, d1, p2, d2);
    b3SegmentDistanceResult segment = b3SegmentDistance(p1, b3Add(p1, d1), p2, b3Add(p2, d2));
    name("b3LineDistance"); v3(p1); v3(d1); v3(p2); v3(d2); arrow(); v3(line.point1); f(line.fraction1); v3(line.point2); f(line.fraction2); end();
    name("b3LineDistance"); v3(p1); v3(d1); v3(p2); v3(d1); arrow();
    line = b3LineDistance(p1, d1, p2, d1); v3(line.point1); f(line.fraction1); v3(line.point2); f(line.fraction2); end();
    name("b3IsWithinSegments"); f(segment.fraction1); f(segment.fraction2); arrow(); i(b3IsWithinSegments(&segment)); end();
    name("b3NormalizePlane"); plane(scaled); arrow(); plane(b3NormalizePlane(scaled)); end();
    name("b3MakePlaneFromNormalAndPoint"); v3(plane1.normal); v3(q); arrow(); plane(b3MakePlaneFromNormalAndPoint(plane1.normal, q)); end();
    name("b3MakePlaneFromPoints"); v3(a); v3(b); v3(c); arrow(); plane(plane1); end();
    name("b3MakeNormalFromPoints"); v3(a); v3(b); v3(c); arrow(); v3(b3MakeNormalFromPoints(a, b, c)); end();
    name("b3TransformPlane"); tf(t); plane(plane1); arrow(); plane(b3TransformPlane(t, plane1)); end();
    name("b3PlaneSeparation"); plane(plane1); v3(q); arrow(); f(b3PlaneSeparation(plane1, q)); end();
    name("b3SignedVolume"); v3(a); v3(b); v3(c); v3(q); arrow(); f(b3SignedVolume(a, b, c, q)); end();
  }

  /* ---- 2D ---- */
  for (int k = 0; k < MORE; k++) {
    b3Vec2 a = next_vec2(6.0f), b = next_vec2(6.0f);
    float s = next_float(3.0f);
    b3Matrix2 m = { next_vec2(4.0f), next_vec2(4.0f) }, n = { next_vec2(4.0f), next_vec2(4.0f) };
    b3Matrix2 spd = { { b3AbsFloat(a.x) + 1.0f, 0.25f }, { 0.25f, b3AbsFloat(b.y) + 1.0f } };
    name("b3Dot2"); v2(a); v2(b); arrow(); f(b3Dot2(a, b)); end();
    name("b3Length2"); v2(a); arrow(); f(b3Length2(a)); end();
    name("b3LengthSquared2"); v2(a); arrow(); f(b3LengthSquared2(a)); end();
    name("b3MinVec2"); v2(a); v2(b); arrow(); v2(b3MinVec2(a, b)); end();
    name("b3MaxVec2"); v2(a); v2(b); arrow(); v2(b3MaxVec2(a, b)); end();
    name("b3Add2"); v2(a); v2(b); arrow(); v2(b3Add2(a, b)); end();
    name("b3Sub2"); v2(a); v2(b); arrow(); v2(b3Sub2(a, b)); end();
    name("b3Neg2"); v2(a); arrow(); v2(b3Neg2(a)); end();
    name("b3MulSV2"); f(s); v2(a); arrow(); v2(b3MulSV2(s, a)); end();
    name("b3MulAdd2"); v2(a); f(s); v2(b); arrow(); v2(b3MulAdd2(a, s, b)); end();
    name("b3MulSub2"); v2(a); f(s); v2(b); arrow(); v2(b3MulSub2(a, s, b)); end();
    name("b3Cross2"); v2(a); v2(b); arrow(); f(b3Cross2(a, b)); end();
    name("b3DistanceSquared2"); v2(a); v2(b); arrow(); f(b3DistanceSquared2(a, b)); end();
    name("b3MulMV2"); m2(m); v2(a); arrow(); v2(b3MulMV2(m, a)); end();
    name("b3MulMM2"); m2(m); m2(n); arrow(); m2(b3MulMM2(m, n)); end();
    name("b3Det2"); m2(m); arrow(); f(b3Det2(m)); end();
    name("b3Invert2"); m2(m); arrow(); m2(b3Invert2(m)); end();
    name("b3Solve2"); m2(spd); v2(a); arrow(); v2(b3Solve2(spd, a)); end();
  }

  /* ---- validity ---- */
  for (int k = 0; k < 8; k++) {
    b3Vec3 v = next_vec(4.0f);
    b3Quat q = next_quat();
    b3Quat bad = { { q.v.x * 2.0f, q.v.y, q.v.z }, q.s };
    b3AABB a = next_box(4.0f);
    b3AABB inverted = { a.upperBound, a.lowerBound };
    b3AABB far = { { -2.0e5f, 0.0f, 0.0f }, { 1.0f, 1.0f, 1.0f } };
    b3Vec3 nanVector = { v.x, NAN, v.z };
    b3Transform t = { v, q };
    b3Matrix3 m = { v, v, nanVector };
    b3Plane plane1 = { b3Normalize(b3Add(v, b3Vec3_one)), next_float(3.0f) };
    name("b3IsValidVec3"); v3(v); arrow(); i(b3IsValidVec3(v)); end();
    name("b3IsValidVec3"); v3(nanVector); arrow(); i(b3IsValidVec3(nanVector)); end();
    name("b3IsValidQuat"); q4(q); arrow(); i(b3IsValidQuat(q)); end();
    name("b3IsValidQuat"); q4(bad); arrow(); i(b3IsValidQuat(bad)); end();
    name("b3IsValidTransform"); tf(t); arrow(); i(b3IsValidTransform(t)); end();
    name("b3IsValidMatrix3"); m3(m); arrow(); i(b3IsValidMatrix3(m)); end();
    name("b3IsValidAABB"); box(a); arrow(); i(b3IsValidAABB(a)); end();
    name("b3IsValidAABB"); box(inverted); arrow(); i(b3IsValidAABB(inverted)); end();
    name("b3IsBoundedAABB"); box(far); arrow(); i(b3IsBoundedAABB(far)); end();
    name("b3IsSaneAABB"); box(a); arrow(); i(b3IsSaneAABB(a)); end();
    name("b3IsValidPlane"); plane(plane1); arrow(); i(b3IsValidPlane(plane1)); end();
    name("b3IsValidPosition"); v3(v); arrow(); i(b3IsValidPosition(v)); end();
    name("b3IsValidWorldTransform"); tf(t); arrow(); i(b3IsValidWorldTransform(t)); end();
  }
  return 0;
}
