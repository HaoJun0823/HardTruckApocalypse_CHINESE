// 
#define SPACE_OBJECT 	1
#define SPACE_WORLD 	2
#define SPACE_VIEW 	3

// helpers to declare 
#include "libSamplers.fx"
#include "libInstancing.fx"
#include "libGlobalLightmap.fx"

#ifdef NV3x
#	define REAL		half
#	define REAL2	half2
#	define REAL3	half3
#	define REAL4	half4
#	define REAL3x3	half3x3
#	define REAL3x4	half3x4
#	define REAL4x3	half4x3
#	define REAL4x4	half4x4
#else
#	define REAL		float
#	define REAL2	float2
#	define REAL3	float3
#	define REAL4	float4
#	define REAL3x3	float3x3
#	define REAL3x4	float3x4
#	define REAL4x3	float4x3
#	define REAL4x4	float4x4
#endif

#define PROJECTOR_FADE_START_COEFF 0.6f

// Pixel shader compilation definition
#ifdef COMPILE_SUPPORT_SHADOWMAP

	#define COMPILE_PS( version )	compile ps_2_0
	
#else
	
	#define COMPILE_PS( version )	compile version
	
#endif

// Vertex shader compilation definition
#define COMPILE_VS( version ) compile version 	 

// (1-step Newton-Raphson re-normalization correction)
float3 normalizeApprox( const float3 v )
{
	return (1 - saturate(dot(v.xyz, v.xyz))) * (v*0.5) + v; 
}

// bx2
float3 bx2( const float3 v )
{
 	return v * 2.f - 1.f;
}

// pow( f, 16 ) approximation for ps_1_1/ps_1_4
float pow16( const float f )
{
	/**
		actual code:
		
		def c0, 4.0f, 1.0f, 0.0f, -0.75f  
		mad_x4_sat r1.w, r1.w, r1.w, c0.w 
	 */
	return saturate( 4*(f*f - 0.75f) );
}

// convert to tangent space (v, tangent, binormal & normal should be in the same space)
float3 toTangentSpace( const float3 v, const float3 tangent, const float3 binormal, const float3 normal )
{
	return float3( dot( v, tangent ), dot( v, binormal ), dot( v, normal ) );
}

// calculates simple view fog
float VertexFog( const float Z, const float2 FogTerm )
{
	return saturate( FogTerm.x - Z * FogTerm.y );
}

// calculates layered fog from four planes
float LayeredFog4( const float4x4 planes, const float3 pos, const float4 fogStart )
{
	// calculate distances to planes
	float4	dists	= mul( float4( pos, 1.f ), planes );

	// calculate min dist
	float2	mindist	= min( dists.xy, dists.zw );
	mindist.x		= min( mindist.x, mindist.y );
	
	// calculate fog density
	float	t		= saturate( mindist.x * fogStart.x );
	return t * t;
}

// calculates view fog and layered fog from single plane
float ViewLayeredFog( const float z, const float4 plane, const float3 pos, const float3 fogTerm )
{
	// calculate distance to plane
	float dist	= dot( float4( pos, 1.f ), plane );	
	
	// calculate fog densities
	float2 t = saturate( float2( dist * fogTerm.z, fogTerm.x - z * fogTerm.y ) );
	
	// return resulting density
	return min( t.y, t.x * t.x );
} 		

// test constants
//float3 SKY_COLOR		=	float3( 183.0/255.0, 188.0/255.0, 202.0/255.0 );
//float3 GROUND_COLOR		=	float3( 52.0/255.0, 52.0/255.0, 61.0/255.0 );
//float3 SPECULAR_COLOR	=	float3( 1, 1, 1 );