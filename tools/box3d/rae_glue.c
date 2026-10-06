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

/* ----- The task system (rae_glue.h) ------------------------------------- */

#if !defined( __wasm__ )

#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdlib.h>
#if defined( __APPLE__ )
#include <pthread/qos.h>
#endif

enum
{
	raeTaskFree = 0,
	raeTaskPending = 1,
	raeTaskClaimed = 2,
	raeTaskComplete = 3,
};

typedef struct RaeTask
{
	b3TaskCallback* callback;
	void* taskContext;
	_Atomic int status;
} RaeTask;

typedef struct RaeTaskSystem
{
	pthread_t threads[B3_MAX_WORKERS];
	int threadCount;
	RaeTask tasks[B3_MAX_TASKS];
	_Atomic int nextSlot;
	/* Enqueued and not yet finished; the slots are reused once it is 0 */
	_Atomic int outstanding;
	/* A counting semaphore: one wake per enqueue */
	pthread_mutex_t lock;
	pthread_cond_t wake;
	int wakeCount;
	_Atomic int shutdown;
} RaeTaskSystem;

/* Claim and run one pending task; false when there was none. */
static int raeRunOneTask( RaeTaskSystem* system )
{
	int taskCount = atomic_load( &system->nextSlot );
	if ( taskCount > B3_MAX_TASKS )
	{
		taskCount = B3_MAX_TASKS;
	}
	for ( int i = 0; i < taskCount; ++i )
	{
		RaeTask* task = system->tasks + i;
		int expected = raeTaskPending;
		if ( atomic_load( &task->status ) != raeTaskPending ||
			 !atomic_compare_exchange_strong( &task->status, &expected, raeTaskClaimed ) )
		{
			continue;
		}
		task->callback( task->taskContext );
		atomic_store( &task->status, raeTaskComplete );
		return 1;
	}
	return 0;
}

static void* raeTaskWorker( void* argument )
{
	RaeTaskSystem* system = argument;
	for ( ;; )
	{
		pthread_mutex_lock( &system->lock );
		while ( system->wakeCount == 0 && !atomic_load( &system->shutdown ) )
		{
			pthread_cond_wait( &system->wake, &system->lock );
		}
		if ( atomic_load( &system->shutdown ) )
		{
			pthread_mutex_unlock( &system->lock );
			return NULL;
		}
		system->wakeCount -= 1;
		pthread_mutex_unlock( &system->lock );
		while ( raeRunOneTask( system ) )
		{
		}
	}
}

static void* raeEnqueueTask( b3TaskCallback* task, void* taskContext, void* userContext, const char* name )
{
	(void)name;
	RaeTaskSystem* system = userContext;
	int slot = atomic_fetch_add( &system->nextSlot, 1 );
	if ( slot >= B3_MAX_TASKS )
	{
		/* Box3D caps a step at B3_MAX_TASKS; run it here (NULL: already done) */
		task( taskContext );
		return NULL;
	}
	RaeTask* scheduled = system->tasks + slot;
	scheduled->callback = task;
	scheduled->taskContext = taskContext;
	atomic_fetch_add( &system->outstanding, 1 );
	/* Published after the callback and context are written */
	atomic_store( &scheduled->status, raeTaskPending );
	pthread_mutex_lock( &system->lock );
	system->wakeCount += 1;
	pthread_cond_signal( &system->wake );
	pthread_mutex_unlock( &system->lock );
	return scheduled;
}

static void raeFinishTask( void* userTask, void* userContext )
{
	if ( userTask == NULL )
	{
		return;
	}
	RaeTaskSystem* system = userContext;
	RaeTask* task = userTask;
	/* Help with pending work while waiting, as Box3D's scheduler does */
	while ( atomic_load( &task->status ) != raeTaskComplete )
	{
		if ( !raeRunOneTask( system ) )
		{
			sched_yield();
		}
	}
	/* Box3D enqueues and finishes from the stepping thread only, so once
	 * nothing is outstanding no slot is in use and they can start over */
	if ( atomic_fetch_sub( &system->outstanding, 1 ) == 1 )
	{
		atomic_store( &system->nextSlot, 0 );
	}
}

uint64_t rae_b3CreateTaskSystem( int workerCount )
{
	if ( workerCount < 2 )
	{
		return 0;
	}
	if ( workerCount > B3_MAX_WORKERS )
	{
		workerCount = B3_MAX_WORKERS;
	}
	RaeTaskSystem* system = calloc( 1, sizeof( RaeTaskSystem ) );
	if ( system == NULL )
	{
		return 0;
	}
	pthread_mutex_init( &system->lock, NULL );
	pthread_cond_init( &system->wake, NULL );
	pthread_attr_t attributes;
	pthread_attr_t* threadAttributes = NULL;
	if ( pthread_attr_init( &attributes ) == 0 )
	{
		threadAttributes = &attributes;
#if defined( __APPLE__ )
		qos_class_t creatorQos = qos_class_self();
		if ( creatorQos != QOS_CLASS_UNSPECIFIED )
		{
			pthread_attr_set_qos_class_np( &attributes, creatorQos, 0 );
		}
#endif
	}
	for ( int i = 0; i < workerCount - 1; ++i )
	{
		if ( pthread_create( system->threads + system->threadCount, threadAttributes, raeTaskWorker, system ) != 0 )
		{
			break;
		}
		system->threadCount += 1;
	}
	if ( threadAttributes != NULL )
	{
		pthread_attr_destroy( threadAttributes );
	}
	return (uint64_t)(uintptr_t)system;
}

void rae_b3DestroyTaskSystem( uint64_t taskSystem )
{
	RaeTaskSystem* system = (RaeTaskSystem*)(uintptr_t)taskSystem;
	if ( system == NULL )
	{
		return;
	}
	pthread_mutex_lock( &system->lock );
	atomic_store( &system->shutdown, 1 );
	pthread_cond_broadcast( &system->wake );
	pthread_mutex_unlock( &system->lock );
	for ( int i = 0; i < system->threadCount; ++i )
	{
		pthread_join( system->threads[i], NULL );
	}
	pthread_cond_destroy( &system->wake );
	pthread_mutex_destroy( &system->lock );
	free( system );
}

#else /* WebAssembly without threads: one worker, Box3D's serial path */

uint64_t rae_b3CreateTaskSystem( int workerCount )
{
	(void)workerCount;
	return 0;
}

void rae_b3DestroyTaskSystem( uint64_t taskSystem )
{
	(void)taskSystem;
}

static void* raeEnqueueTask( b3TaskCallback* task, void* taskContext, void* userContext, const char* name )
{
	(void)userContext;
	(void)name;
	task( taskContext );
	return NULL;
}

static void raeFinishTask( void* userTask, void* userContext )
{
	(void)userTask;
	(void)userContext;
}

#endif

b3WorldId rae_b3CreateWorldWithTasks( const b3WorldDef* def, uint64_t taskSystem )
{
	b3WorldDef withTasks = *def;
	if ( taskSystem != 0 )
	{
		withTasks.enqueueTask = raeEnqueueTask;
		withTasks.finishTask = raeFinishTask;
		withTasks.userTaskContext = (void*)(uintptr_t)taskSystem;
	}
	return b3CreateWorld( &withTasks );
}
