/* See rae_glue.h. */
#include "rae_glue.h"

b3ContactBeginTouchEvent rae_b3ContactBeginAt( const b3ContactEvents* events, int index )
{
	return events->beginEvents[index];
}

b3ContactEndTouchEvent rae_b3ContactEndAt( const b3ContactEvents* events, int index )
{
	return events->endEvents[index];
}

b3ContactHitEvent rae_b3ContactHitAt( const b3ContactEvents* events, int index )
{
	return events->hitEvents[index];
}

b3SensorBeginTouchEvent rae_b3SensorBeginAt( const b3SensorEvents* events, int index )
{
	return events->beginEvents[index];
}

b3SensorEndTouchEvent rae_b3SensorEndAt( const b3SensorEvents* events, int index )
{
	return events->endEvents[index];
}

b3BodyMoveEvent rae_b3BodyMoveAt( const b3BodyEvents* events, int index )
{
	return events->moveEvents[index];
}

typedef struct PlaneCollector
{
	b3PlaneResult* planes;
	b3ShapeId* shapeIds;
	int count;
	int capacity;
} PlaneCollector;

static bool CollectPlanes( b3ShapeId shapeId, const b3PlaneResult* planes, int planeCount, void* context )
{
	PlaneCollector* collector = context;
	for ( int i = 0; i < planeCount && collector->count < collector->capacity; ++i )
	{
		collector->planes[collector->count] = planes[i];
		collector->shapeIds[collector->count] = shapeId;
		collector->count += 1;
	}
	return true;
}

int rae_b3World_CollideMoverInto( b3WorldId worldId, b3Vec3 origin, const b3Capsule* mover, b3QueryFilter filter,
								  b3PlaneResult* planes, b3ShapeId* shapeIds, int capacity )
{
	PlaneCollector collector = { planes, shapeIds, 0, capacity };
	b3World_CollideMover( worldId, origin, mover, filter, CollectPlanes, &collector );
	return collector.count;
}

typedef struct IgnoreList
{
	const b3ShapeId* shapeIds;
	int count;
} IgnoreList;

static bool SkipIgnored( b3ShapeId shapeId, void* context )
{
	const IgnoreList* ignore = context;
	for ( int i = 0; i < ignore->count; ++i )
	{
		if ( B3_ID_EQUALS( shapeId, ignore->shapeIds[i] ) )
		{
			return false;
		}
	}
	return true;
}

float rae_b3World_CastMoverIgnoring( b3WorldId worldId, b3Vec3 origin, const b3Capsule* mover, b3Vec3 translation,
									 b3QueryFilter filter, const b3ShapeId* ignore, int ignoreCount )
{
	IgnoreList list = { ignore, ignoreCount };
	return b3World_CastMover( worldId, origin, mover, translation, filter, ignoreCount > 0 ? SkipIgnored : NULL, &list );
}

typedef struct ShapeCollector
{
	b3ShapeId* shapeIds;
	int count;
	int capacity;
} ShapeCollector;

static bool CollectShape( b3ShapeId shapeId, void* context )
{
	ShapeCollector* collector = context;
	if ( collector->count < collector->capacity )
	{
		collector->shapeIds[collector->count] = shapeId;
	}
	collector->count += 1;
	return true;
}

int rae_b3World_OverlapAABBInto( b3WorldId worldId, b3AABB aabb, b3QueryFilter filter, b3ShapeId* shapeIds, int capacity )
{
	ShapeCollector collector = { shapeIds, 0, capacity };
	b3World_OverlapAABB( worldId, aabb, filter, CollectShape, &collector );
	return collector.count;
}

int rae_b3World_OverlapShapeInto( b3WorldId worldId, b3Vec3 origin, const b3ShapeProxy* proxy, b3QueryFilter filter,
								  b3ShapeId* shapeIds, int capacity )
{
	ShapeCollector collector = { shapeIds, 0, capacity };
	b3World_OverlapShape( worldId, origin, proxy, filter, CollectShape, &collector );
	return collector.count;
}

static float KeepClosest( b3ShapeId shapeId, b3Pos point, b3Vec3 normal, float fraction, uint64_t userMaterialId,
						  int triangleIndex, int childIndex, void* context )
{
	b3RayResult* result = context;
	result->shapeId = shapeId;
	result->point = point;
	result->normal = normal;
	result->fraction = fraction;
	result->userMaterialId = userMaterialId;
	result->triangleIndex = triangleIndex;
	result->childIndex = childIndex;
	result->hit = true;
	return fraction;
}

b3RayResult rae_b3World_CastShapeClosest( b3WorldId worldId, b3Vec3 origin, const b3ShapeProxy* proxy, b3Vec3 translation,
										  b3QueryFilter filter )
{
	b3RayResult result = { 0 };
	b3World_CastShape( worldId, origin, proxy, translation, filter, KeepClosest, &result );
	return result;
}

uint64_t rae_b3CreateMesh( const b3MeshDef* def )
{
	return (uint64_t)(uintptr_t)b3CreateMesh( def, NULL, 0 );
}

b3ShapeId rae_b3CreateMeshShape( b3BodyId bodyId, const b3ShapeDef* def, uint64_t mesh, b3Vec3 scale )
{
	return b3CreateMeshShape( bodyId, def, (const b3MeshData*)(uintptr_t)mesh, scale );
}

void rae_b3DestroyMesh( uint64_t mesh )
{
	b3DestroyMesh( (b3MeshData*)(uintptr_t)mesh );
}

uint64_t rae_b3CreateHull( const b3Vec3* points, int pointCount, int maxVertexCount )
{
	return (uint64_t)(uintptr_t)b3CreateHull( points, pointCount, maxVertexCount );
}

b3ShapeId rae_b3CreateHullShape( b3BodyId bodyId, const b3ShapeDef* def, uint64_t hull )
{
	return b3CreateHullShape( bodyId, def, (const b3HullData*)(uintptr_t)hull );
}

void rae_b3DestroyHull( uint64_t hull )
{
	b3DestroyHull( (b3HullData*)(uintptr_t)hull );
}

uint64_t rae_b3CreateHeightField( const b3HeightFieldDef* def )
{
	return (uint64_t)(uintptr_t)b3CreateHeightField( def );
}

b3ShapeId rae_b3CreateHeightFieldShape( b3BodyId bodyId, const b3ShapeDef* def, uint64_t heightField )
{
	return b3CreateHeightFieldShape( bodyId, def, (const b3HeightFieldData*)(uintptr_t)heightField );
}

void rae_b3DestroyHeightField( uint64_t heightField )
{
	b3DestroyHeightField( (b3HeightFieldData*)(uintptr_t)heightField );
}
