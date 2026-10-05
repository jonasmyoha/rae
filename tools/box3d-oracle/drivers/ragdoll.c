/* Box3D oracle driver: the ragdoll (shared/human.c) on one tile of the rain
 * benchmark (shared/benchmarks.c CreateRain: an 8 x 8 grid mesh and a torus
 * mesh on a static body), stepped with b3World_Step (1/60 s, 4 sub-steps,
 * workerCount 1). Three humans (CreateHuman: friction torque 5, spring 1 Hz,
 * damping 0.7, group 1) drop from 8 m; at step 120 the first is destroyed
 * and created again (DestroyHuman). Every 20 steps, every bone's transform:
 *
 *   bone <human> <bone> <step> -> px py pz qx qy qz qw
 *
 * every float a C hex float. Checked by fixture 996.
 */
#include "box3d/box3d.h"

#include "human.c"
#include "utils.c"

#include <stdio.h>

#define HUMAN_COUNT 3
#define STEP_COUNT 240
#define RESPAWN_STEP 120
#define GRID_SIZE 15.0f

static void SpawnHuman( Human* human, b3WorldId worldId, int index )
{
	b3Pos position = { -0.75f + 0.75f * index, 8.0f, 0.0f };
	CreateHuman( human, worldId, position, 5.0f, 1.0f, 0.7f, 1, NULL, false );
}

int main( void )
{
	b3WorldDef worldDef = b3DefaultWorldDef();
	worldDef.workerCount = 1;
	b3WorldId worldId = b3CreateWorld( &worldDef );

	int halfMeshGridRows = 4;
	float cellWidth = GRID_SIZE / ( 2.0f * halfMeshGridRows );
	b3MeshData* gridMesh = b3CreateGridMesh( 2 * halfMeshGridRows, 2 * halfMeshGridRows, cellWidth, 1, true );
	b3MeshData* torusMesh = b3CreateTorusMesh( 16, 16, 0.25f * GRID_SIZE, 1.0f );

	b3BodyDef bodyDef = b3DefaultBodyDef();
	b3ShapeDef shapeDef = b3DefaultShapeDef();
	b3BodyId groundId = b3CreateBody( worldId, &bodyDef );
	b3CreateMeshShape( groundId, &shapeDef, gridMesh, b3Vec3_one );
	b3CreateMeshShape( groundId, &shapeDef, torusMesh, b3Vec3_one );

	Human humans[HUMAN_COUNT] = { 0 };
	for ( int i = 0; i < HUMAN_COUNT; ++i )
	{
		SpawnHuman( humans + i, worldId, i );
	}

	printf( "# box3d %s ragdoll\n", BOX3D_ORACLE_COMMIT );
	for ( int step = 0; step < STEP_COUNT; ++step )
	{
		if ( step == RESPAWN_STEP )
		{
			DestroyHuman( humans + 0 );
			SpawnHuman( humans + 0, worldId, 0 );
		}

		b3World_Step( worldId, 1.0f / 60.0f, 4 );

		if ( ( step + 1 ) % 20 == 0 )
		{
			for ( int h = 0; h < HUMAN_COUNT; ++h )
			{
				for ( int b = 0; b < bone_count; ++b )
				{
					b3WorldTransform xf = b3Body_GetTransform( humans[h].bones[b].bodyId );
					printf( "bone %d %d %d -> %a %a %a %a %a %a %a\n", h, b, step + 1, xf.p.x, xf.p.y, xf.p.z, xf.q.v.x,
							xf.q.v.y, xf.q.v.z, xf.q.s );
				}
			}
		}
	}

	b3DestroyWorld( worldId );
	b3DestroyMesh( gridMesh );
	b3DestroyMesh( torusMesh );
	return 0;
}
