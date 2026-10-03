/* Box3D oracle driver: convex hulls (docs/physics-rae-port-design.md §4, P2c):
 * hull.c (quickhull construction, the generated hulls, box hulls, cloning,
 * mass and queries, the 2D hull) and b3FindIncidentFace of manifold.c.
 *
 * One line per call, `<function> <inputs> -> <outputs>`, floats as C hex
 * floats (`%a`), integers and bools as decimals. Compound values:
 *
 *   transform  position(3) rotation(4: x y z w)
 *   hull       vertexCount edgeCount faceCount, then per vertex point(3)
 *              edge, per half-edge next twin origin face, per face edge
 *              plane(4), then aabb(6) surfaceArea volume innerRadius
 *              center(3) centralInertia(9, by column). A failed build: 0.
 *   spec       how a query's hull is built: `0 count maxVertexCount
 *              point(3)*count` (b3CreateHull) or `1 hx hy hz transform`
 *              (b3MakeTransformedBoxHull)
 *   proxy      count point(3)*count radius
 *   cast       normal(3) point(3) fraction iterations triangleIndex childIndex
 *              materialIndex hit
 *   point2d    x y separation originalIndex
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
#include "shape.h"

int b3FindHullSupportVertex( const b3HullData* hull, b3Vec3 direction );
int b3FindHullSupportFace( const b3HullData* hull, b3Vec3 direction );
b3AABB b3ComputeSweptHullAABB( const b3HullData* shape, b3Transform xf1, b3Transform xf2 );
int b3CollideMoverAndHull( b3PlaneResult* result, const b3HullData* shape, const b3Capsule* mover );
b3ShapeExtent b3ComputeHullExtent( const b3HullData* hull, b3Vec3 origin );
float b3ComputeHullProjectedArea( const b3HullData* hull, b3Vec3 direction );

static uint32_t g_state = 0xbb67ae85u;

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
static void plane(b3Plane p) { v3(p.normal); f(p.offset); }
static void capsule(b3Capsule c) { v3(c.center1); v3(c.center2); f(c.radius); }
static void cast(b3CastOutput o) {
  v3(o.normal); v3(o.point); f(o.fraction); i(o.iterations); i(o.triangleIndex); i(o.childIndex);
  i(o.materialIndex); i(o.hit);
}
static void proxy(const b3ShapeProxy* p) {
  i(p->count);
  for (int k = 0; k < p->count; k++) v3(p->points[k]);
  f(p->radius);
}

static void hull(const b3HullData* h) {
  if (h == NULL) { i(0); return; }
  i(h->vertexCount); i(h->edgeCount); i(h->faceCount);
  const b3Vec3* points = b3GetHullPoints(h);
  const b3HullVertex* vertices = b3GetHullVertices(h);
  const b3HullHalfEdge* edges = b3GetHullEdges(h);
  const b3HullFace* faces = b3GetHullFaces(h);
  const b3Plane* planes = b3GetHullPlanes(h);
  for (int k = 0; k < h->vertexCount; k++) { v3(points[k]); i(vertices[k].edge); }
  for (int k = 0; k < h->edgeCount; k++) { i(edges[k].next); i(edges[k].twin); i(edges[k].origin); i(edges[k].face); }
  for (int k = 0; k < h->faceCount; k++) { i(faces[k].edge); plane(planes[k]); }
  box(h->aabb); f(h->surfaceArea); f(h->volume); f(h->innerRadius); v3(h->center); m3(h->centralInertia);
}

static void name(const char* n) { printf("%s", n); }
static void arrow(void) { printf(" ->"); }
static void end(void) { printf("\n"); }

#define CASES 48
#define MAX_POINTS 64

/* Point clouds of several shapes: random, on a sphere, on a jittered grid
   (coplanar faces to merge), a box with duplicates, flat, a line. */
static int next_cloud(b3Vec3* points, int kind) {
  int count = 4 + next_int(36);
  switch (kind) {
    case 0:
      for (int k = 0; k < count; k++) points[k] = next_vec(1.0f);
      break;
    case 1:
      for (int k = 0; k < count; k++) points[k] = b3Normalize(next_nonzero_vec(1.0f));
      break;
    case 2: {
      count = 0;
      for (int x = 0; x < 3; x++)
        for (int y = 0; y < 3; y++)
          for (int z = 0; z < 3; z++)
            if (next_int(3) != 0) points[count++] = (b3Vec3){ (float)x * 0.5f, (float)y * 0.5f, (float)z * 0.5f };
      if (count < 4) { points[count++] = (b3Vec3){ 0.0f, 0.0f, 0.0f }; points[count++] = (b3Vec3){ 1.0f, 1.0f, 1.0f }; }
      break;
    }
    case 3: {
      count = 0;
      for (int k = 0; k < 8; k++) {
        b3Vec3 corner = { (k & 1) ? 1.0f : -1.0f, (k & 2) ? 0.5f : -0.5f, (k & 4) ? 0.75f : -0.75f };
        points[count++] = corner;
        if (next_int(2)) points[count++] = corner; /* a duplicate */
        if (next_int(3) == 0) points[count++] = b3Add(corner, next_vec(0.00390625f)); /* a near duplicate */
      }
      break;
    }
    case 4:
      for (int k = 0; k < count; k++) points[k] = (b3Vec3){ next_float(1.0f), next_float(1.0f), 0.25f };
      break;
    default:
      for (int k = 0; k < count; k++) { float t = next_float(1.0f); points[k] = (b3Vec3){ t, 2.0f * t, -t }; }
      break;
  }
  return count;
}

/* Query hulls, built from a printed spec. */
typedef struct Spec {
  int kind;
  int count, maxVertexCount;
  b3Vec3 points[MAX_POINTS];
  float hx, hy, hz;
  b3Transform transform;
} Spec;

static b3HullData* build(const Spec* s, b3BoxHull* storage) {
  if (s->kind == 0) return b3CreateHull(s->points, s->count, s->maxVertexCount);
  *storage = b3MakeTransformedBoxHull(s->hx, s->hy, s->hz, s->transform);
  return &storage->base;
}

static void spec(const Spec* s) {
  i(s->kind);
  if (s->kind == 0) {
    i(s->count); i(s->maxVertexCount);
    for (int k = 0; k < s->count; k++) v3(s->points[k]);
  } else {
    f(s->hx); f(s->hy); f(s->hz); tf(s->transform);
  }
}

static void next_spec(Spec* s) {
  s->kind = next_int(3) == 0 ? 1 : 0;
  if (s->kind == 0) {
    s->count = next_cloud(s->points, next_int(2));
    s->maxVertexCount = 128;
    b3Vec3 offset = next_vec(1.0f);
    for (int k = 0; k < s->count; k++) s->points[k] = b3Add(s->points[k], offset);
  } else {
    s->hx = 0.25f + (float)next_int(8) * 0.125f;
    s->hy = 0.25f + (float)next_int(8) * 0.125f;
    s->hz = 0.25f + (float)next_int(8) * 0.125f;
    s->transform = next_transform(1.0f);
  }
}

int main(void) {
  printf("# box3d oracle: hulls, commit %s, scalar, -ffp-contract=off\n", BOX3D_ORACLE_COMMIT);

  /* Construction from point clouds. */
  for (int k = 0; k < 6 * CASES; k++) {
    b3Vec3 points[MAX_POINTS];
    int count = next_cloud(points, k % 6);
    int maxVertexCount = k % 5 == 0 ? 4 + next_int(8) : 128;
    b3HullData* h = b3CreateHull(points, count, maxVertexCount);
    name("b3CreateHull"); i(count); i(maxVertexCount);
    for (int n = 0; n < count; n++) v3(points[n]);
    arrow(); hull(h); end();
    if (h) b3DestroyHull(h);
  }

  /* The generated hulls. */
  for (int sides = 3; sides <= 12; sides++) {
    float height = 0.5f + (float)next_int(4) * 0.25f, radius = 0.25f + (float)next_int(4) * 0.25f;
    float yOffset = next_float(1.0f);
    b3HullData* h = b3CreateCylinder(height, radius, yOffset, sides);
    name("b3CreateCylinder"); f(height); f(radius); f(yOffset); i(sides); arrow(); hull(h); end();
    b3DestroyHull(h);
  }
  for (int slices = 4; slices <= 12; slices++) {
    float height = 0.5f + (float)next_int(4) * 0.25f;
    float radius1 = 0.25f + (float)next_int(4) * 0.25f, radius2 = 0.125f + (float)next_int(4) * 0.125f;
    b3HullData* h = b3CreateCone(height, radius1, radius2, slices);
    name("b3CreateCone"); f(height); f(radius1); f(radius2); i(slices); arrow(); hull(h); end();
    b3DestroyHull(h);
  }
  for (int k = 0; k < 4; k++) {
    float radius = 0.25f + (float)k * 0.5f;
    b3HullData* h = b3CreateRock(radius);
    name("b3CreateRock"); f(radius); arrow(); hull(h); end();
    b3DestroyHull(h);
    h = b3CreateComplexHull(radius);
    name("b3CreateComplexHull"); f(radius); arrow(); hull(h); end();
    b3DestroyHull(h);
  }

  /* Box hulls. */
  for (int k = 0; k < CASES / 2; k++) {
    float hx = next_float(1.0f), hy = next_float(1.0f), hz = next_float(1.0f);
    b3Transform t = next_transform(2.0f);
    b3BoxHull b = b3MakeTransformedBoxHull(hx, hy, hz, t);
    name("b3MakeTransformedBoxHull"); f(hx); f(hy); f(hz); tf(t); arrow(); hull(&b.base); end();
    b = b3MakeBoxHull(hx, hy, hz);
    name("b3MakeBoxHull"); f(hx); f(hy); f(hz); arrow(); hull(&b.base); end();
    b = b3MakeCubeHull(hx);
    name("b3MakeCubeHull"); f(hx); arrow(); hull(&b.base); end();
    b = b3MakeOffsetBoxHull(hx, hy, hz, t.p);
    name("b3MakeOffsetBoxHull"); f(hx); f(hy); f(hz); v3(t.p); arrow(); hull(&b.base); end();

    b3Vec3 halfWidths = { 0.25f + (float)next_int(8) * 0.125f, 0.25f + (float)next_int(8) * 0.125f,
                          0.25f + (float)next_int(8) * 0.125f };
    b3Vec3 postScale = next_vec(2.0f);
    if (k % 4 == 0) postScale = (b3Vec3){ 1.0f, 2.0f, 0.5f };
    b3Vec3 scaledHalf = halfWidths;
    b3Transform scaledTransform = t;
    float minHalfWidth = 0.0625f;
    b3ScaleBox(&scaledHalf, &scaledTransform, postScale, minHalfWidth);
    name("b3ScaleBox"); v3(halfWidths); tf(t); v3(postScale); f(minHalfWidth); arrow(); v3(scaledHalf);
    tf(scaledTransform); end();
    b = b3MakeScaledBoxHull(halfWidths, t, postScale);
    name("b3MakeScaledBoxHull"); v3(halfWidths); tf(t); v3(postScale); arrow(); hull(&b.base); end();
  }

  /* Clone and transform (mirroring scales reverse the winding). */
  for (int k = 0; k < CASES; k++) {
    Spec s; b3BoxHull storage;
    next_spec(&s);
    b3HullData* h = build(&s, &storage);
    if (h == NULL) continue;
    b3Transform t = next_transform(1.0f);
    b3Vec3 scale = { 0.5f + (float)next_int(4) * 0.25f, 0.5f + (float)next_int(4) * 0.25f, 0.5f + (float)next_int(4) * 0.25f };
    if (k % 3 == 1) scale.x = -scale.x;
    if (k % 5 == 2) { scale.y = -scale.y; scale.z = -scale.z; }
    if (k % 7 == 3) scale.z = 0.0f;
    b3HullData* clone = b3CloneAndTransformHull(h, t, scale);
    name("b3CloneAndTransformHull"); spec(&s); tf(t); v3(scale); arrow(); hull(clone); end();
    if (clone) b3DestroyHull(clone);
    if (s.kind == 0) b3DestroyHull(h);
  }

  /* Mass and queries. */
  for (int k = 0; k < CASES; k++) {
    Spec s; b3BoxHull storage;
    next_spec(&s);
    b3HullData* h = build(&s, &storage);
    if (h == NULL) continue;
    float density = 0.5f + (float)next_int(4) * 0.5f;
    b3MassData m = b3ComputeHullMass(h, density);
    name("b3ComputeHullMass"); spec(&s); f(density); arrow(); f(m.mass); v3(m.center); m3(m.inertia); end();

    b3Transform t1 = next_transform(2.0f), t2 = next_transform(2.0f);
    name("b3ComputeHullAABB"); spec(&s); tf(t1); arrow(); box(b3ComputeHullAABB(h, t1)); end();
    name("b3ComputeSweptHullAABB"); spec(&s); tf(t1); tf(t2); arrow(); box(b3ComputeSweptHullAABB(h, t1, t2)); end();

    b3Vec3 cloud[8];
    int cloudCount = 1 + next_int(8);
    b3Vec3 near = b3Add(h->center, next_vec(1.5f));
    for (int n = 0; n < cloudCount; n++) cloud[n] = b3Add(near, next_vec(0.375f));
    b3ShapeProxy p = { cloud, cloudCount, (float)next_int(3) * 0.125f };
    b3Transform identity = b3Transform_identity;
    b3Transform shapeTransform = next_int(2) ? identity : next_transform(0.25f);
    name("b3OverlapHull"); spec(&s); tf(shapeTransform); proxy(&p); arrow(); i(b3OverlapHull(h, shapeTransform, &p)); end();

    b3RayCastInput ray;
    ray.origin = b3Add(h->center, next_vec(3.0f));
    ray.translation = b3MulSV(1.0f + (float)next_int(3) * 0.5f, b3Sub(b3Add(h->center, next_vec(0.5f)), ray.origin));
    ray.maxFraction = next_int(4) == 0 ? 0.5f : 1.0f;
    if (k % 8 == 0) ray.origin = h->center; /* starts inside */
    name("b3RayCastHull"); spec(&s); v3(ray.origin); v3(ray.translation); f(ray.maxFraction); arrow();
    cast(b3RayCastHull(h, &ray)); end();

    b3ShapeCastInput castInput = { p, b3MulSV(1.5f, b3Sub(h->center, near)), next_int(4) == 0 ? 0.5f : 1.0f, next_int(2) == 1 };
    name("b3ShapeCastHull"); spec(&s); proxy(&castInput.proxy); v3(castInput.translation); f(castInput.maxFraction);
    i(castInput.canEncroach); arrow(); cast(b3ShapeCastHull(h, &castInput)); end();

    b3Capsule mover = { b3Add(h->center, next_vec(1.5f)), b3Add(h->center, next_vec(1.5f)), 0.25f + (float)next_int(4) * 0.25f };
    b3PlaneResult result; memset(&result, 0, sizeof result);
    int count = b3CollideMoverAndHull(&result, h, &mover);
    name("b3CollideMoverAndHull"); spec(&s); capsule(mover); arrow(); i(count); plane(result.plane); v3(result.point); end();

    b3Vec3 direction = next_nonzero_vec(1.0f);
    name("b3FindHullSupportVertex"); spec(&s); v3(direction); arrow(); i(b3FindHullSupportVertex(h, direction)); end();
    name("b3FindHullSupportFace"); spec(&s); v3(direction); arrow(); i(b3FindHullSupportFace(h, direction)); end();

    b3Vec3 origin = next_vec(0.5f);
    b3ShapeExtent extent = b3ComputeHullExtent(h, origin);
    name("b3ComputeHullExtent"); spec(&s); v3(origin); arrow(); f(extent.minExtent); v3(extent.maxExtent); end();
    b3Vec3 unit = b3Normalize(direction);
    name("b3ComputeHullProjectedArea"); spec(&s); v3(unit); arrow(); f(b3ComputeHullProjectedArea(h, unit)); end();

    int vertexIndex = next_int(h->vertexCount);
    name("b3FindIncidentFace"); spec(&s); v3(unit); i(vertexIndex); arrow(); i(b3FindIncidentFace(h, unit, vertexIndex)); end();

    if (s.kind == 0) b3DestroyHull(h);
  }

  /* The 2D hull and its simplification. */
  for (int k = 0; k < CASES; k++) {
    b3Point2D pts[MAX_POINTS], out[2 * MAX_POINTS];
    int count = 1 + next_int(24);
    for (int n = 0; n < count; n++) {
      pts[n].p = (b3Vec2){ next_float(1.0f), next_float(1.0f) };
      if (n > 0 && next_int(6) == 0) pts[n].p = pts[next_int(n)].p; /* a duplicate to weld */
      pts[n].separation = next_float(0.0625f);
      pts[n].originalIndex = n;
    }
    name("b3Hull2D"); i(count);
    for (int n = 0; n < count; n++) { f(pts[n].p.x); f(pts[n].p.y); f(pts[n].separation); i(pts[n].originalIndex); }
    int hullCount = b3Hull2D(pts, count, out);
    arrow(); i(hullCount);
    for (int n = 0; n < hullCount; n++) { f(out[n].p.x); f(out[n].p.y); f(out[n].separation); i(out[n].originalIndex); }
    end();

    if (hullCount >= 3) {
      int target = 3 + next_int(4);
      name("b3SimplifyHull2D"); i(hullCount);
      for (int n = 0; n < hullCount; n++) { f(out[n].p.x); f(out[n].p.y); f(out[n].separation); i(out[n].originalIndex); }
      i(target);
      int simplified = b3SimplifyHull2D(out, hullCount, target);
      arrow(); i(simplified);
      for (int n = 0; n < simplified; n++) { f(out[n].p.x); f(out[n].p.y); f(out[n].separation); i(out[n].originalIndex); }
      end();
    }
  }
  return 0;
}
