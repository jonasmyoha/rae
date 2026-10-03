/* benchmarks/float4_solver: Box3D's own wide convex contact kernels
 * (b3WarmStartContacts_Convex, b3PushContacts_Convex, b3SolveContacts_Convex
 * of contact_solver.c at 9f998c8) on synthetic constraints, built twice by
 * run.sh: scalar (BOX3D_DISABLE_SIMD, a b3FloatW is a struct of four floats)
 * and SIMD (NEON / SSE2). No Box3D source is in this repository: the file is
 * #included from the oracle cache at build time (tools/box3d-oracle).
 *
 * The constraints and body states come from the integer generator below, in
 * the same order as rae/Main.rae, so every build starts from the same bits
 * and the checksums must match. One step is 4 sub-steps of warm start, push
 * and relax over every wide constraint (Box3D's per-colour stage work for a
 * single colour, without integration). Prints
 * RESULT,<name>,<nanoseconds>,<checksum>. */
#include "contact_solver.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { wideCount = 8192, bodyCount = 16384, stepCount = 30, subStepCount = 4 };

static uint32_t g_state = 0x9e3779b9u;

static uint32_t nextBits( void )
{
	g_state = g_state * 1664525u + 1013904223u;
	return g_state;
}

/* [0, 1) in steps of 1/65536: exact in a float */
static float unit( void ) { return (float)( ( nextBits() >> 8 ) & 0xFFFFu ) / 65536.0f; }
static float signedUnit( void ) { return unit() * 2.0f - 1.0f; }
static float positive( float scale ) { return unit() * scale + 0.0625f; }

static b3FloatW wideOf( float x, float y, float z, float w ) { return b3SetW( x, y, z, w ); }

static b3FloatW widePositive( float scale )
{
	float x = positive( scale ), y = positive( scale ), z = positive( scale ), w = positive( scale );
	return wideOf( x, y, z, w );
}

static b3FloatW wideSigned( float scale )
{
	float x = signedUnit() * scale, y = signedUnit() * scale, z = signedUnit() * scale, w = signedUnit() * scale;
	return wideOf( x, y, z, w );
}

static b3Vec3W vectorSigned( float scale )
{
	b3Vec3W v;
	v.X = wideSigned( scale );
	v.Y = wideSigned( scale );
	v.Z = wideSigned( scale );
	return v;
}

static b3SymMatrix3W symPositive( float scale )
{
	b3SymMatrix3W m;
	m.cxx = widePositive( scale );
	m.cxy = widePositive( scale );
	m.cxz = widePositive( scale );
	m.cyy = widePositive( scale );
	m.cyz = widePositive( scale );
	m.czz = widePositive( scale );
	return m;
}

static uint64_t hashFloat( uint64_t hash, float value )
{
	uint32_t bits;
	memcpy( &bits, &value, sizeof( bits ) );
	hash ^= bits;
	hash *= 0x100000001b3ull;
	return hash;
}

static int64_t nowNs( void )
{
	struct timespec ts;
	clock_gettime( CLOCK_MONOTONIC, &ts );
	return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

int main( void )
{
	b3BodyState* states = calloc( bodyCount, sizeof( b3BodyState ) );
	for ( int i = 0; i < bodyCount; ++i )
	{
		b3BodyState* s = states + i;
		/* One draw per statement: C does not order an initializer list's calls */
		s->linearVelocity.x = signedUnit();
		s->linearVelocity.y = signedUnit();
		s->linearVelocity.z = signedUnit();
		s->angularVelocity.x = signedUnit();
		s->angularVelocity.y = signedUnit();
		s->angularVelocity.z = signedUnit();
		s->deltaPosition.x = signedUnit() * 0.0625f;
		s->deltaPosition.y = signedUnit() * 0.0625f;
		s->deltaPosition.z = signedUnit() * 0.0625f;
		s->flags = b3_dynamicFlag;
		s->deltaRotation.v.x = signedUnit() * 0.0625f;
		s->deltaRotation.v.y = signedUnit() * 0.0625f;
		s->deltaRotation.v.z = signedUnit() * 0.0625f;
		s->deltaRotation.s = 1.0f;
	}

	b3ContactConstraintWide* constraints = calloc( wideCount, sizeof( b3ContactConstraintWide ) );
	for ( int i = 0; i < wideCount; ++i )
	{
		b3ContactConstraintWide* c = constraints + i;
		for ( int lane = 0; lane < 4; ++lane )
		{
			c->indexA[lane] = ( nextBits() & 7u ) == 0 ? 0 : 1 + (int)( nextBits() % bodyCount );
			c->indexB[lane] = 1 + (int)( nextBits() % bodyCount );
			c->pointCounts[lane] = 1 + (int)( nextBits() & 3u );
		}
		c->invMassA = widePositive( 1.0f );
		c->invMassB = widePositive( 1.0f );
		c->invIA = symPositive( 0.5f );
		c->invIB = symPositive( 0.5f );
		c->normal = vectorSigned( 1.0f );
		c->tangent1 = vectorSigned( 1.0f );
		c->tangent2 = vectorSigned( 1.0f );
		c->centerA = vectorSigned( 0.5f );
		c->centerB = vectorSigned( 0.5f );
		c->twistMass = widePositive( 1.0f );
		c->twistImpulse = widePositive( 0.125f );
		c->tangentMass.cxx = widePositive( 1.0f );
		c->tangentMass.cxy = widePositive( 0.25f );
		c->tangentMass.cyy = widePositive( 1.0f );
		c->frictionImpulse.x = wideSigned( 0.125f );
		c->frictionImpulse.y = wideSigned( 0.125f );
		c->rollingMass = symPositive( 0.5f );
		c->rollingImpulse = vectorSigned( 0.0625f );
		c->friction = widePositive( 0.5f );
		float rolling[4];
		for ( int lane = 0; lane < 4; ++lane )
		{
			rolling[lane] = ( nextBits() & 3u ) == 0 ? positive( 0.125f ) : 0.0f;
		}
		c->rollingResistance = wideOf( rolling[0], rolling[1], rolling[2], rolling[3] );
		c->tangentVelocity1 = wideSigned( 0.125f );
		c->tangentVelocity2 = wideSigned( 0.125f );
		c->restitution = b3ZeroW();
		for ( int p = 0; p < 4; ++p )
		{
			b3ContactConstraintPointWide* cp = c->points + p;
			cp->anchorAs = vectorSigned( 0.5f );
			cp->anchorBs = vectorSigned( 0.5f );
			cp->baseSeparations = wideSigned( 0.0625f );
			cp->normalImpulses = widePositive( 0.125f );
			cp->totalNormalImpulses = b3ZeroW();
			cp->normalMasses = widePositive( 1.0f );
			cp->leverArms = widePositive( 0.5f );
			cp->relativeVelocities = b3ZeroW();
			cp->restitutionImpulses = b3ZeroW();
		}
	}

	/* The step context: one colour holding every wide constraint */
	static b3ConstraintGraph graph;
	graph.colors[0].wideConstraints = constraints;
	graph.colors[0].wideConstraintCount = wideCount;
	b3World* world = calloc( 1, sizeof( b3World ) );
	world->contactSpeed = 3.0f;
	b3StepContext context = { 0 };
	context.world = world;
	context.graph = &graph;
	context.states = states;
	float h = 1.0f / 240.0f;
	context.h = h;
	context.inv_h = 240.0f;
	context.contactSoftness = b3MakeSoft( 30.0f, 10.0f, h );
	context.staticSoftness = b3MakeSoft( 60.0f, 5.0f, h );

	b3SolverBlock block = { .startIndex = 0, .count = (uint16_t)wideCount, .blockType = b3_graphWideContactBlock, .colorIndex = 0 };

	int64_t start = nowNs();
	for ( int step = 0; step < stepCount; ++step )
	{
		for ( int sub = 0; sub < subStepCount; ++sub )
		{
			b3WarmStartContacts_Convex( block, &context );
			b3PushContacts_Convex( block, &context );
			b3SolveContacts_Convex( block, &context );
		}
	}
	int64_t elapsed = nowNs() - start;

	uint64_t hash = 0xcbf29ce484222325ull;
	for ( int i = 0; i < bodyCount; ++i )
	{
		b3BodyState* s = states + i;
		hash = hashFloat( hash, s->linearVelocity.x );
		hash = hashFloat( hash, s->linearVelocity.y );
		hash = hashFloat( hash, s->linearVelocity.z );
		hash = hashFloat( hash, s->angularVelocity.x );
		hash = hashFloat( hash, s->angularVelocity.y );
		hash = hashFloat( hash, s->angularVelocity.z );
	}
	printf( "RESULT,%s,%lld,%llu\n", BENCH_NAME, (long long)elapsed, (unsigned long long)hash );
	return 0;
}
