/* The Float4 prototype's primitives (docs/float4-design.md): what the
 * compiler would emit for a builtin Float4, written by hand for the
 * benchmark. Force-included (-include) into the generated C of the float4
 * build, so each `extern` primitive of float4/physics/dynamics/FloatWide.rae
 * expands in place: a statement expression over the FloatWide struct (four
 * floats, the same layout as a 128-bit vector), loaded and stored around one
 * SIMD instruction, which the C compiler keeps in registers after inlining.
 *
 * Bit-exactness against the scalar FloatWide (Box3D's scalar b3FloatW):
 * - arithmetic, division and square root are IEEE in every lane, no fused
 *   multiply-add (mulAdd is a multiply, then an add);
 * - min and max are `a <= b ? a : b` / `a >= b ? a : b` by compare and
 *   select, not the min/max instructions (those differ on -0 and NaN);
 * - abs is `a < 0 ? -a : a` (keeps -0, unlike the abs instruction);
 * - a mask is all ones or all zeros per lane (the scalar path uses 1.0 and
 *   0.0); masks only feed blend, and, or and anyTrue.
 *
 * NEON (arm64), SSE2 (x86-64) and a scalar fallback. wasm SIMD128 follows
 * the SSE2 shape with wasm_f32x4_* (docs/float4-design.md). */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#if defined( __aarch64__ ) || defined( _M_ARM64 )
#include <arm_neon.h>
typedef float32x4_t rae_f4_vec;
#define RAE_F4_LOAD( pointer ) vld1q_f32( (const float*)( pointer ) )
#define RAE_F4_STORE( pointer, value ) vst1q_f32( (float*)( pointer ), ( value ) )
#define RAE_F4_ADD( a, b ) vaddq_f32( a, b )
#define RAE_F4_SUB( a, b ) vsubq_f32( a, b )
#define RAE_F4_MUL( a, b ) vmulq_f32( a, b )
#define RAE_F4_DIV( a, b ) vdivq_f32( a, b )
#define RAE_F4_SQRT( a ) vsqrtq_f32( a )
#define RAE_F4_NEG( a ) vnegq_f32( a )
#define RAE_F4_SPLAT( s ) vdupq_n_f32( s )
#define RAE_F4_ZERO() vdupq_n_f32( 0.0f )
#define RAE_F4_LE( a, b ) vreinterpretq_f32_u32( vcleq_f32( a, b ) )
#define RAE_F4_GE( a, b ) vreinterpretq_f32_u32( vcgeq_f32( a, b ) )
#define RAE_F4_LT( a, b ) vreinterpretq_f32_u32( vcltq_f32( a, b ) )
#define RAE_F4_GT( a, b ) vreinterpretq_f32_u32( vcgtq_f32( a, b ) )
#define RAE_F4_EQ( a, b ) vreinterpretq_f32_u32( vceqq_f32( a, b ) )
#define RAE_F4_AND( a, b ) vreinterpretq_f32_u32( vandq_u32( vreinterpretq_u32_f32( a ), vreinterpretq_u32_f32( b ) ) )
#define RAE_F4_OR( a, b ) vreinterpretq_f32_u32( vorrq_u32( vreinterpretq_u32_f32( a ), vreinterpretq_u32_f32( b ) ) )
/* mask ? b : a */
#define RAE_F4_SELECT( a, b, mask ) vbslq_f32( vreinterpretq_u32_f32( mask ), b, a )
#define RAE_F4_ALL_BITS_SET( m ) ( vminvq_u32( vreinterpretq_u32_f32( m ) ) != 0 )
#define RAE_F4_ANY_BITS_SET( m ) ( vmaxvq_u32( vreinterpretq_u32_f32( m ) ) != 0 )
#elif defined( __SSE2__ ) || defined( _M_X64 )
#include <emmintrin.h>
typedef __m128 rae_f4_vec;
#define RAE_F4_LOAD( pointer ) _mm_loadu_ps( (const float*)( pointer ) )
#define RAE_F4_STORE( pointer, value ) _mm_storeu_ps( (float*)( pointer ), ( value ) )
#define RAE_F4_ADD( a, b ) _mm_add_ps( a, b )
#define RAE_F4_SUB( a, b ) _mm_sub_ps( a, b )
#define RAE_F4_MUL( a, b ) _mm_mul_ps( a, b )
#define RAE_F4_DIV( a, b ) _mm_div_ps( a, b )
#define RAE_F4_SQRT( a ) _mm_sqrt_ps( a )
#define RAE_F4_NEG( a ) _mm_xor_ps( a, _mm_set1_ps( -0.0f ) )
#define RAE_F4_SPLAT( s ) _mm_set1_ps( s )
#define RAE_F4_ZERO() _mm_setzero_ps()
#define RAE_F4_LE( a, b ) _mm_cmple_ps( a, b )
#define RAE_F4_GE( a, b ) _mm_cmpge_ps( a, b )
#define RAE_F4_LT( a, b ) _mm_cmplt_ps( a, b )
#define RAE_F4_GT( a, b ) _mm_cmpgt_ps( a, b )
#define RAE_F4_EQ( a, b ) _mm_cmpeq_ps( a, b )
#define RAE_F4_AND( a, b ) _mm_and_ps( a, b )
#define RAE_F4_OR( a, b ) _mm_or_ps( a, b )
#define RAE_F4_SELECT( a, b, mask ) _mm_or_ps( _mm_and_ps( mask, b ), _mm_andnot_ps( mask, a ) )
#define RAE_F4_ALL_BITS_SET( m ) ( _mm_movemask_ps( m ) == 0xF )
#define RAE_F4_ANY_BITS_SET( m ) ( _mm_movemask_ps( m ) != 0 )
#else
#define RAE_F4_SCALAR_FALLBACK 1
#endif

#ifndef RAE_F4_SCALAR_FALLBACK

#define RAE_F4_UNARY( a, expression )                                                                                            \
	( { rae_FloatWide result__; rae_f4_vec x__ = RAE_F4_LOAD( a ); RAE_F4_STORE( &result__, ( expression ) ); result__; } )
#define RAE_F4_BINARY( a, b, expression )                                                                                        \
	( { rae_FloatWide result__; rae_f4_vec x__ = RAE_F4_LOAD( a ); rae_f4_vec y__ = RAE_F4_LOAD( b );                            \
		RAE_F4_STORE( &result__, ( expression ) ); result__; } )

#define rae_f4_zero() ( { rae_FloatWide result__; RAE_F4_STORE( &result__, RAE_F4_ZERO() ); result__; } )
#define rae_f4_splat( s ) ( { rae_FloatWide result__; RAE_F4_STORE( &result__, RAE_F4_SPLAT( s ) ); result__; } )
#define rae_f4_set( x, y, z, w ) ( (rae_FloatWide){ ( x ), ( y ), ( z ), ( w ) } )
#define rae_f4_lane( a, lane ) ( ( (const float*)( a ) )[lane] )
#define rae_f4_negate( a ) RAE_F4_UNARY( a, RAE_F4_NEG( x__ ) )
#define rae_f4_abs( a ) RAE_F4_UNARY( a, RAE_F4_SELECT( x__, RAE_F4_NEG( x__ ), RAE_F4_LT( x__, RAE_F4_ZERO() ) ) )
#define rae_f4_add( a, b ) RAE_F4_BINARY( a, b, RAE_F4_ADD( x__, y__ ) )
#define rae_f4_subtract( a, b ) RAE_F4_BINARY( a, b, RAE_F4_SUB( x__, y__ ) )
#define rae_f4_multiply( a, b ) RAE_F4_BINARY( a, b, RAE_F4_MUL( x__, y__ ) )
#define rae_f4_divide( a, b ) RAE_F4_BINARY( a, b, RAE_F4_DIV( x__, y__ ) )
#define rae_f4_sqrt( a ) RAE_F4_UNARY( a, RAE_F4_SQRT( x__ ) )
#define rae_f4_mul_add( base, a, b )                                                                                             \
	( { rae_FloatWide result__; rae_f4_vec base__ = RAE_F4_LOAD( base );                                                         \
		RAE_F4_STORE( &result__, RAE_F4_ADD( base__, RAE_F4_MUL( RAE_F4_LOAD( a ), RAE_F4_LOAD( b ) ) ) ); result__; } )
#define rae_f4_min( a, b ) RAE_F4_BINARY( a, b, RAE_F4_SELECT( y__, x__, RAE_F4_LE( x__, y__ ) ) )
#define rae_f4_max( a, b ) RAE_F4_BINARY( a, b, RAE_F4_SELECT( y__, x__, RAE_F4_GE( x__, y__ ) ) )
/* r = a <= b ? a : b; r = r <= -b ? -b : r */
#define rae_f4_sym_clamp( a, b )                                                                                                 \
	( { rae_FloatWide result__; rae_f4_vec x__ = RAE_F4_LOAD( a ); rae_f4_vec y__ = RAE_F4_LOAD( b );                            \
		rae_f4_vec r__ = RAE_F4_SELECT( y__, x__, RAE_F4_LE( x__, y__ ) );                                                       \
		rae_f4_vec n__ = RAE_F4_NEG( y__ );                                                                                      \
		RAE_F4_STORE( &result__, RAE_F4_SELECT( r__, n__, RAE_F4_LE( r__, n__ ) ) ); result__; } )
#define rae_f4_and( a, b ) RAE_F4_BINARY( a, b, RAE_F4_AND( x__, y__ ) )
#define rae_f4_or( a, b ) RAE_F4_BINARY( a, b, RAE_F4_OR( x__, y__ ) )
#define rae_f4_greater( a, b ) RAE_F4_BINARY( a, b, RAE_F4_GT( x__, y__ ) )
#define rae_f4_less( a, b ) RAE_F4_BINARY( a, b, RAE_F4_LT( x__, y__ ) )
#define rae_f4_blend( a, b, mask )                                                                                               \
	( { rae_FloatWide result__;                                                                                                  \
		RAE_F4_STORE( &result__, RAE_F4_SELECT( RAE_F4_LOAD( a ), RAE_F4_LOAD( b ), RAE_F4_LOAD( mask ) ) ); result__; } )
#define rae_f4_all_zero( a ) ( { rae_f4_vec x__ = RAE_F4_LOAD( a ); RAE_F4_ALL_BITS_SET( RAE_F4_EQ( x__, RAE_F4_ZERO() ) ); } )
#define rae_f4_any_true( mask ) ( { RAE_F4_ANY_BITS_SET( RAE_F4_LOAD( mask ) ); } )

#else

/* The scalar fallback: the lib's FloatWide semantics, lane by lane, with
 * all-ones masks so the float4 FloatWide.rae works unchanged. */
static inline float rae_f4_mask_bits( bool condition )
{
	uint32_t bits = condition ? 0xFFFFFFFFu : 0u;
	float value;
	memcpy( &value, &bits, sizeof( value ) );
	return value;
}
static inline uint32_t rae_f4_bits( float value )
{
	uint32_t bits;
	memcpy( &bits, &value, sizeof( bits ) );
	return bits;
}
#define RAE_F4_LANES( a, b, expression )                                                                                         \
	( { const float* x__ = (const float*)( a ); const float* y__ = (const float*)( b ); rae_FloatWide result__;                    \
		float* r__ = (float*)&result__; for ( int i__ = 0; i__ < 4; ++i__ ) r__[i__] = ( expression ); result__; } )
#define rae_f4_zero() ( (rae_FloatWide){ 0.0f, 0.0f, 0.0f, 0.0f } )
#define rae_f4_splat( s ) ( { float s__ = ( s ); (rae_FloatWide){ s__, s__, s__, s__ }; } )
#define rae_f4_set( x, y, z, w ) ( (rae_FloatWide){ ( x ), ( y ), ( z ), ( w ) } )
#define rae_f4_lane( a, lane ) ( ( (const float*)( a ) )[lane] )
#define rae_f4_negate( a ) RAE_F4_LANES( a, a, -x__[i__] )
#define rae_f4_abs( a ) RAE_F4_LANES( a, a, x__[i__] < 0.0f ? -x__[i__] : x__[i__] )
#define rae_f4_add( a, b ) RAE_F4_LANES( a, b, x__[i__] + y__[i__] )
#define rae_f4_subtract( a, b ) RAE_F4_LANES( a, b, x__[i__] - y__[i__] )
#define rae_f4_multiply( a, b ) RAE_F4_LANES( a, b, x__[i__] * y__[i__] )
#define rae_f4_divide( a, b ) RAE_F4_LANES( a, b, x__[i__] / y__[i__] )
#define rae_f4_sqrt( a ) RAE_F4_LANES( a, a, sqrtf( x__[i__] ) )
#define rae_f4_mul_add( base, a, b )                                                                                             \
	( { const float* b__ = (const float*)( base ); RAE_F4_LANES( a, b, b__[i__] + x__[i__] * y__[i__] ); } )
#define rae_f4_min( a, b ) RAE_F4_LANES( a, b, x__[i__] <= y__[i__] ? x__[i__] : y__[i__] )
#define rae_f4_max( a, b ) RAE_F4_LANES( a, b, x__[i__] >= y__[i__] ? x__[i__] : y__[i__] )
#define rae_f4_sym_clamp( a, b )                                                                                                 \
	RAE_F4_LANES( a, b, ( x__[i__] <= y__[i__] ? x__[i__] : y__[i__] ) <= -y__[i__] ? -y__[i__]                                  \
						: ( x__[i__] <= y__[i__] ? x__[i__] : y__[i__] ) )
#define rae_f4_and( a, b ) RAE_F4_LANES( a, b, rae_f4_mask_bits( ( rae_f4_bits( x__[i__] ) & rae_f4_bits( y__[i__] ) ) != 0 ) )
#define rae_f4_or( a, b ) RAE_F4_LANES( a, b, rae_f4_mask_bits( ( rae_f4_bits( x__[i__] ) | rae_f4_bits( y__[i__] ) ) != 0 ) )
#define rae_f4_greater( a, b ) RAE_F4_LANES( a, b, rae_f4_mask_bits( x__[i__] > y__[i__] ) )
#define rae_f4_less( a, b ) RAE_F4_LANES( a, b, rae_f4_mask_bits( x__[i__] < y__[i__] ) )
#define rae_f4_blend( a, b, mask )                                                                                               \
	( { const float* m__ = (const float*)( mask ); RAE_F4_LANES( a, b, rae_f4_bits( m__[i__] ) != 0 ? y__[i__] : x__[i__] ); } )
#define rae_f4_all_zero( a )                                                                                                     \
	( { const float* x__ = (const float*)( a ); x__[0] == 0.0f && x__[1] == 0.0f && x__[2] == 0.0f && x__[3] == 0.0f; } )
#define rae_f4_any_true( mask )                                                                                                  \
	( { const float* x__ = (const float*)( mask ); ( rae_f4_bits( x__[0] ) | rae_f4_bits( x__[1] ) | rae_f4_bits( x__[2] ) |    \
		rae_f4_bits( x__[3] ) ) != 0; } )

#endif
