/* Box3D oracle driver: the C-library track's first scene
 * (docs/physics-two-implementations.md §4): a static ground box and five
 * rotated boxes dropped onto it, stepped 60 times (1/60 s, 4 sub-steps,
 * workerCount 1). Every 10 steps, every box's transform:
 *
 *   box <box> <step> -> px py pz qx qy qz qw
 *
 * every float a C hex float. Fixture 998 builds the same scene through the
 * generated bindings (lib/box3d) and must match it bit for bit.
 */
#include "box3d/box3d.h"

#include <stdio.h>

#define BOX_COUNT 5
#define STEP_COUNT 60

int main( void )
{
	b3WorldDef worldDef = b3DefaultWorldDef();
	worldDef.workerCount = 1;
	b3WorldId worldId = b3CreateWorld( &worldDef );

	b3BodyDef groundDef = b3DefaultBodyDef();
	groundDef.position = ( b3Vec3 ){ 0.0f, -1.0f, 0.0f };
	b3BodyId groundId = b3CreateBody( worldId, &groundDef );
	b3ShapeDef shapeDef = b3DefaultShapeDef();
	b3BoxHull ground = b3MakeBoxHull( 20.0f, 1.0f, 20.0f );
	b3CreateHullShape( groundId, &shapeDef, &ground.base );

	b3BodyId boxes[BOX_COUNT];
	b3BoxHull box = b3MakeBoxHull( 0.5f, 0.5f, 0.5f );
	for ( int i = 0; i < BOX_COUNT; ++i )
	{
		b3BodyDef bodyDef = b3DefaultBodyDef();
		bodyDef.type = b3_dynamicBody;
		bodyDef.position = ( b3Vec3 ){ 0.3f * i - 0.6f, 1.0f + 1.2f * i, 0.1f * i };
		bodyDef.rotation = b3MakeQuatFromAxisAngle( ( b3Vec3 ){ 0.0f, 1.0f, 0.0f }, 0.25f * i );
		boxes[i] = b3CreateBody( worldId, &bodyDef );
		b3CreateHullShape( boxes[i], &shapeDef, &box.base );
	}

	printf( "# box3d %s boxstack\n", BOX3D_ORACLE_COMMIT );
	for ( int step = 1; step <= STEP_COUNT; ++step )
	{
		b3World_Step( worldId, 1.0f / 60.0f, 4 );
		if ( step % 10 == 0 )
		{
			for ( int i = 0; i < BOX_COUNT; ++i )
			{
				b3WorldTransform xf = b3Body_GetTransform( boxes[i] );
				printf( "box %d %d -> %a %a %a %a %a %a %a\n", i, step, xf.p.x, xf.p.y, xf.p.z, xf.q.v.x, xf.q.v.y,
						xf.q.v.z, xf.q.s );
			}
		}
	}

	b3DestroyWorld( worldId );
	return 0;
}
