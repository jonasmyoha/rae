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
  return 0;
}
