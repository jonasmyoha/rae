/* The C glue of the C-library physics track (lib/box3d/Box3dGlue.rae,
 * docs/physics-two-implementations.md §4). Rae cannot pass a function to C
 * yet, and cannot index a C array behind a pointer, so the few Box3D calls
 * that need either go through these helpers: each callback is implemented
 * here and its results copied into arrays the caller owns, and each event
 * array is read one element at a time. tools/box3d/build.sh compiles this
 * into libbox3d.a and installs this header next to Box3D's. No state lives
 * here: everything is passed in. */
#ifndef RAE_BOX3D_GLUE_H
#define RAE_BOX3D_GLUE_H

#include "box3d/box3d.h"

/* Event array elements (index within the count of the same struct) */
b3ContactBeginTouchEvent rae_b3ContactBeginAt( const b3ContactEvents* events, int index );
b3ContactEndTouchEvent rae_b3ContactEndAt( const b3ContactEvents* events, int index );
b3ContactHitEvent rae_b3ContactHitAt( const b3ContactEvents* events, int index );
b3SensorBeginTouchEvent rae_b3SensorBeginAt( const b3SensorEvents* events, int index );
b3SensorEndTouchEvent rae_b3SensorEndAt( const b3SensorEvents* events, int index );
b3BodyMoveEvent rae_b3BodyMoveAt( const b3BodyEvents* events, int index );

/* b3World_CollideMover: every plane, up to `capacity`, into `planes` with the
 * shape of each in `shapeIds`. Returns the number written. */
int rae_b3World_CollideMoverInto( b3WorldId worldId, b3Vec3 origin, const b3Capsule* mover, b3QueryFilter filter,
								  b3PlaneResult* planes, b3ShapeId* shapeIds, int capacity );

/* b3World_CastMover skipping the `ignoreCount` shapes in `ignore`. */
float rae_b3World_CastMoverIgnoring( b3WorldId worldId, b3Vec3 origin, const b3Capsule* mover, b3Vec3 translation,
									 b3QueryFilter filter, const b3ShapeId* ignore, int ignoreCount );

/* b3World_OverlapAABB / b3World_OverlapShape: the shapes found, up to
 * `capacity`, into `shapeIds`. Returns how many were found (it may exceed
 * `capacity`; only `capacity` are written). */
int rae_b3World_OverlapAABBInto( b3WorldId worldId, b3AABB aabb, b3QueryFilter filter, b3ShapeId* shapeIds, int capacity );
int rae_b3World_OverlapShapeInto( b3WorldId worldId, b3Vec3 origin, const b3ShapeProxy* proxy, b3QueryFilter filter,
								  b3ShapeId* shapeIds, int capacity );

/* b3World_CastShape keeping the closest hit (the callback clips to each
 * hit's fraction), as a b3RayResult. */
b3RayResult rae_b3World_CastShapeClosest( b3WorldId worldId, b3Vec3 origin, const b3ShapeProxy* proxy, b3Vec3 translation,
										  b3QueryFilter filter );

/* Meshes and hulls Box3D allocates, as opaque handles (the pointer's
 * address; 0 when Box3D returned none), so the Rae structs that keep them hold
 * no raw pointer. Box3D copies a hull into the world when a shape is made from
 * it; a mesh must outlive its shapes. */
uint64_t rae_b3CreateMesh( const b3MeshDef* def );
b3ShapeId rae_b3CreateMeshShape( b3BodyId bodyId, const b3ShapeDef* def, uint64_t mesh, b3Vec3 scale );
void rae_b3DestroyMesh( uint64_t mesh );
uint64_t rae_b3CreateHull( const b3Vec3* points, int pointCount, int maxVertexCount );
b3ShapeId rae_b3CreateHullShape( b3BodyId bodyId, const b3ShapeDef* def, uint64_t hull );
void rae_b3DestroyHull( uint64_t hull );
/* A height field, the same way: like a mesh, it must outlive its shapes
 * (the shape points at it). */
uint64_t rae_b3CreateHeightField( const b3HeightFieldDef* def );
b3ShapeId rae_b3CreateHeightFieldShape( b3BodyId bodyId, const b3ShapeDef* def, uint64_t heightField );
void rae_b3DestroyHeightField( uint64_t heightField );

#endif
