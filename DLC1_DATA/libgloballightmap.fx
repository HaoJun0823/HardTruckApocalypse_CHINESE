
/////////////////////////////////////
// Shared data support
/////////////////////////////////////

#if  defined( SUPPORT_GLOBAL_LIGHTMAP ) || defined( COMPILE_SUPPORT_SHADOWMAP )  

	#define DECLARE_WORLD_MATRIX			row_major float4x4 INSTANCES( mWorld, MAX_INSTANCES ) : WORLD_MATRIX;
	
	#define CALCULATE_SHARED_DATA( oPos )	float4 wPos = mul( float4( oPos, 1 ), INSTANCE_GET( mWorld ) );

#else

	#define DECLARE_WORLD_MATRIX
	
	#define CALCULATE_SHARED_DATA( oPos )

#endif

// Calculation of attenuation at different defines
#ifdef SUPPORT_GLOBAL_LIGHTMAP

	#ifdef COMPILE_SUPPORT_SHADOWMAP
	
		#define CALCULATE_ATTENUATION( diffuse, ambient )			( ( diffuse ) * lm.x * 2.f * shadow + ( ambient ) )
	
		#define CALCULATE_ATTENUATION_NL( diffuse, ambient )		( ( diffuse ) * lm.x * shadow + ( ambient ) )
	
	#else
	
		#define CALCULATE_ATTENUATION( diffuse, ambient )			( ( diffuse ) * lm.x * 2.f + ( ambient ) )
	
		#define CALCULATE_ATTENUATION_NL( diffuse, ambient )		( ( diffuse ) * lm.x + ( ambient ) )

	#endif	

#else

	#ifdef COMPILE_SUPPORT_SHADOWMAP
	
			#define CALCULATE_ATTENUATION( diffuse, ambient )		( ( diffuse ) * shadow + ( ambient ) )
	
			#define CALCULATE_ATTENUATION_NL( diffuse, ambient )	( ambient ) * shadow
	
	#else
	
			#define CALCULATE_ATTENUATION( diffuse, ambient )		( ( diffuse ) + ( ambient ) )
	
			#define CALCULATE_ATTENUATION_NL( diffuse, ambient )	( ambient )

	#endif

#endif

////////////////////////////////////////////
// Global lightmap support
///////////////////////////////////////////

#ifdef SUPPORT_GLOBAL_LIGHTMAP

	// fx support 
	
	#define DECLARE_GLOBAL_LIGHTMAP_DATA					shared texture GlobalLightmap : GLOBAL_LIGHTMAP;					\
															DECLARE_DIFFUSE_SAMPLER( GlobalLightmapSampler, GlobalLightmap )	\
															shared const float4 globalLightmapParams : GLOBAL_LIGHTMAP_PARAMS; 									
																																
	#define DECLARE_GLOBAL_LIGHTMAP_SUPPORT( reg )			float2 globalLightmapTexcoord : reg;	

	#define CALCULATE_GLOBAL_LIGHTMAP_TEXCOORDS( o )		o.globalLightmapTexcoord = ( wPos.xz + globalLightmapParams.zw ) * globalLightmapParams.xy;

	#define GET_GLOBAL_LIGHTMAP( i )						float4 lm = tex2D( GlobalLightmapSampler, i.globalLightmapTexcoord );	
	
	// hlsl support
	
	#define DECLARE_GLOBAL_LIGHTMAP_PARAMS( reg )			const float4 globalLightmapParams: register( reg );
	
	#define DECLARE_GLOBAL_LIGHTMAP_TEXTURE					sampler2D GlobalLightmapSampler;
	
	#define CALCULATE_GLOBAL_LIGHTMAP_TEXCOORDSW( o, wPos )	o.globalLightmapTexcoord = ( wPos.xz + globalLightmapParams.zw ) * globalLightmapParams.xy;

#else
	
	// fx support
	
	#define DECLARE_GLOBAL_LIGHTMAP_DATA
	
	#define DECLARE_GLOBAL_LIGHTMAP_SUPPORT( reg )
	
	#define CALCULATE_GLOBAL_LIGHTMAP_TEXCOORDS( o )
	
	#define GET_GLOBAL_LIGHTMAP( i )
	
	// hlsl support
	
	#define DECLARE_GLOBAL_LIGHTMAP_PARAMS( reg )
	
	#define DECLARE_GLOBAL_LIGHTMAP_TEXTURE
	
	#define CALCULATE_GLOBAL_LIGHTMAP_TEXCOORDSW( o, wPos )
	
#endif	

//////////////////////////////////////////
// Shadowmap support
/////////////////////////////////////////

static const float3 unpack = { 1.f, 1.f / 256.f, 1e-50f };

// Return color at offset coord
float3 offset_lookup( sampler2D sm, float3 coord, float2 offset )
{
	return tex2D( sm, coord.xy + offset );
}

// Calculate shadow value
float CalculateShadow( sampler2D shadowmap0, sampler2D shadowmap1, float4 smc0, float4 smc1, float4 params, float3 params2 )
{	
#ifndef SUPPORT_DEPTH_TEXTURES

	// Calculate shadow coord
	bool	 isSmDetailed	= params.y < 1.f;
	float3	 coord			= isSmDetailed ? smc0.xyz / smc0.w : smc1.xyz / smc1.w;
	
	// Calculate offsets
	float2 offset0 = float2(  1.f,  0.f ) * params2.z + coord;
	float2 offset1 = float2(  0.f,  1.f ) * params2.z + coord;
	float2 offset2 = float2(  0.f, -1.f ) * params2.z + coord;
	float2 offset3 = float2( -1.f,  0.f ) * params2.z + coord;

	// Fetch depths
	float4x3 dp;
	dp[ 0 ]	= isSmDetailed ? tex2D( shadowmap0, offset0 ) : tex2D( shadowmap1, offset0 );
	dp[ 1 ] = isSmDetailed ? tex2D( shadowmap0, offset1 ) : tex2D( shadowmap1, offset1 );
	dp[ 2 ] = isSmDetailed ? tex2D( shadowmap0, offset2 ) : tex2D( shadowmap1, offset2 );
	dp[ 3 ] = isSmDetailed ? tex2D( shadowmap0, offset3 ) : tex2D( shadowmap1, offset3 );

	// Calculate shadow
	float4	dpp		= coord.zzzz <= mul( dp, unpack );
	float	shadow	= dot( dpp, float4( 0.25f, 0.25f, 0.25f, 0.25f ) );

#else

	// Calculate shadow coord
	bool	 isSmDetailed	= params.y < 1.f;
	float4	 coord			= isSmDetailed ? smc0 : smc1;
	
	// Calculate offsets
	float  smoothCoeff = params2.z * coord.w;		
	float4 offset0 = float4(  1.f,  0.f, 0.f, 0.f ) * smoothCoeff + coord;
	float4 offset1 = float4(  0.f,  1.f, 0.f, 0.f ) * smoothCoeff + coord;
	float4 offset2 = float4(  0.f, -1.f, 0.f, 0.f ) * smoothCoeff + coord;
	float4 offset3 = float4( -1.f,  0.f, 0.f, 0.f ) * smoothCoeff + coord;

	// Fetch depths
	float4 dp;
	dp.x = isSmDetailed ? tex2Dproj( shadowmap0, offset0 ) : tex2Dproj( shadowmap1, offset0 );
	dp.y = isSmDetailed ? tex2Dproj( shadowmap0, offset1 ) : tex2Dproj( shadowmap1, offset1 );
	dp.z = isSmDetailed ? tex2Dproj( shadowmap0, offset2 ) : tex2Dproj( shadowmap1, offset2 );
	dp.w = isSmDetailed ? tex2Dproj( shadowmap0, offset3 ) : tex2Dproj( shadowmap1, offset3 );

	// Calculate shadow
	float shadow = dot( dp, float4( 0.25f, 0.25f, 0.25f, 0.25f ) );

#endif

	// Shadow with fade 
	shadow = shadow * params2.x + params2.y;
	return max( shadow, params.z );
}

// Calculate shadow value from alpha channel
float CalculateShadowAlpha( sampler2D shadowmap0, sampler2D shadowmap1, float4 smc0, float4 smc1, float4 smc2, float4 params, float3 params2 )
{
	float shadow;
	if( params.y < 1.f )
	{
		float2 coord = smc0.xy / smc0.w;
		shadow		 = tex2D( shadowmap0, coord ).z;
	}
	else 
	{	
		float2 coord = params.x < 1.f ? smc1.xy / smc1.w : smc2.xy / smc2.w;
		float4 clr	 = tex2D( shadowmap1, coord ); 
		shadow		 = params.x < 1.f ? clr.z : clr.w;
	}
	
	shadow = shadow * params2.x + params2.y;
	return max( shadow, params.z );
}

// Calculate shadow value for grass
float CalculateShadowGrass( sampler2D shadowmap0, sampler2D shadowmap1, float4 smc0, float4 smc1, float4 smc2, float4 params, float3 params2 )
{
	float shadow;
	if( params.y < 1.f )
	{
		float2 coord = smc0.xy / smc0.w;
		shadow		 = tex2D( shadowmap0, coord ).z;
	}
	else 
	{
		float2 coord = params.x < 1.f ? smc1.xy / smc1.w : smc2.xy / smc2.w;
		float4 clr	 = tex2D( shadowmap1, coord ); 
		shadow		 = params.x < 1.f ? clr.z : clr.w;
	}
	
	shadow = shadow * params2.x + params2.y;
	return shadow;
}

// models and roads support
#ifdef COMPILE_SUPPORT_SHADOWMAP

	#define DECLARE_SHADOWMAP_DATA_MDL			shared const float4x4 shadowmapMat0	: SHADOWMAP_MATRIX0;		\
												shared const float4x4 shadowmapMat1	: SHADOWMAP_MATRIX1;		\
												shared const float3   smParams		: SHADOWMAP_PARAMS;			\
												shared const float4   viewPlane		: VIEW_PLANE;				\
												shared const float3   viewPosW		: SHADOWMAP_VIEW_POS;		\
												shared const float3	  smParams2		: SHADOWMAP_PARAMS2;		\
												shared texture		  shadowMap0	: SHADOWMAP0;				\
												shared texture		  shadowMap1	: SHADOWMAP1;				\
												DECLARE_SHADOWMAP_SAMPLER( shadowmap0, shadowMap0 )				\
												DECLARE_SHADOWMAP_SAMPLER( shadowmap1, shadowMap1 )
												
	#define DECLARE_SHADOWMAP_DATA_ROAD			shared const float4x4 shadowmapMat0	: SHADOWMAP_MATRIX0;		\
												shared const float4x4 shadowmapMat1	: SHADOWMAP_MATRIX1;		\
												shared const float4x4 shadowmapMat2	: SHADOWMAP_MATRIX2;		\
												shared const float4   smParams		: SHADOWMAP_PARAMS;			\
												shared const float4   viewPlane		: VIEW_PLANE;				\
												shared const float3   viewPosW		: SHADOWMAP_VIEW_POS;		\
												shared const float3	  smParams2		: SHADOWMAP_PARAMS2;		\
												shared texture		  shadowMap0	: SHADOWMAP0;				\
												shared texture		  shadowMap1	: SHADOWMAP1;				\
												DECLARE_SHADOW_SAMPLER( shadowmap0, shadowMap0 )				\
												DECLARE_SHADOW_SAMPLER( shadowmap1, shadowMap1 )
										
	#define CALCULATE_SHADOWMAP_DATA_MDL( Out )	Out.smc0		= mul( wPos, shadowmapMat0 );												\
												Out.smc1		= mul( wPos, shadowmapMat1 );												\
												Out.shmParams.z	= saturate( ( distance( wPos.xyz, viewPosW ) - smParams.x ) / smParams.y );	\
												Out.shmParams.y = dot( viewPlane, wPos ) / smParams.z;
												
	#define CALCULATE_SHADOWMAP_DATA_ROAD( Out, wPos )	Out.shmParams.z	= saturate( ( distance( wPos.xyz, viewPosW ) - smParams.x ) / smParams.y );	 \
														float zDist		= dot( viewPlane, wPos );													 \
														Out.shmParams.y = zDist / smParams.z;														 \
														Out.shmParams.x = zDist / smParams.w;														 \
														Out.smc1		= mul( wPos, shadowmapMat1 );												 \
														Out.smc0		= mul( wPos, shadowmapMat0 );												 \
														Out.smc2		= mul( wPos, shadowmapMat2 );												 
														
	#define CALCULATE_SHADOW_MDL( In )			float shadow = CalculateShadow( shadowmap0, shadowmap1, In.smc0, In.smc1, In.shmParams, smParams2 );		

#else

	#define DECLARE_SHADOWMAP_DATA_MDL
	
	#define DECLARE_SHADOWMAP_DATA_ROAD
	
	#define CALCULATE_SHADOWMAP_DATA_MDL( Out )
	
	#define CALCULATE_SHADOWMAP_DATA_ROAD( Out, wPos )
	
	#define CALCULATE_SHADOW_MDL( In )

#endif

// landscape and grass support
#ifdef COMPILE_SUPPORT_SHADOWMAP

	#define DECLARE_SHADOWMAP_DATA_VS	float4x4 shadowmapMat0;	\
										float4x4 shadowmapMat1;	\
										float4x4 shadowmapMat2;	\
										float4   smParams;		\
										float4   viewPlane;
										
	#define DECLARE_SHADOWMAP_SUPPORT( regCoord0, regCoord1, regSmParams )	float4 smc0		 : regCoord0;	\
																			float4 smc1		 : regCoord1;	\
																			float4 shmParams : regSmParams; 
																			
	#define DECLARE_SHADOWMAP_SUPPORT3( regCoord0, regCoord1, regCoord2, regSmParams )	float4 smc0		 : regCoord0;	\
																						float4 smc1		 : regCoord1;	\
																						float4 smc2		 : regCoord2;	\
																						float4 shmParams : regSmParams;

	#define CALCULATE_SHADOWMAP_DATA_LS( Out, wPos )	Out.shmParams.z	= saturate( ( distance( wPos.xyz, viewPos ) - smParams.x ) / smParams.y );	 \
														float zDist		= dot( viewPlane, wPos );													 \
														Out.shmParams.x = zDist / smParams.w;														 \
														Out.shmParams.y = zDist / smParams.z;														 \
														Out.smc1		= mul( wPos, shadowmapMat1 );												 \
														Out.smc0		= mul( wPos, shadowmapMat0 );												 \
														Out.smc2		= mul( wPos, shadowmapMat2 );			
																																																
	#define DECLARE_SHADOWMAP_DATA_PS	float3		smParams2;		\
										sampler2D	shadowmap0;		\
										sampler2D	shadowmap1;		
	
	#define CALCULATE_SHADOW_ALPHA( In )	float shadow = CalculateShadowAlpha( shadowmap0, shadowmap1, In.smc0, In.smc1, In.smc2, In.shmParams, smParams2 );			

	#define CALCULATE_SHADOW_GRASS( In )	float shadow = CalculateShadowGrass( shadowmap0, shadowmap1, In.smc0, In.smc1, In.smc2, In.shmParams, smParams2 );

	#define CALCULATE_ATTENUATION_LS( lightmap, cDiffuse, cAmbient ) lightmap.y * cDiffuse * shadow + lightmap.z * cAmbient

#else
	
	#define DECLARE_SHADOWMAP_DATA_VS
	
	#define DECLARE_SHADOWMAP_SUPPORT( regCoord0, regCoord1, regSmParams )
	
	#define DECLARE_SHADOWMAP_SUPPORT3( regCoord0, regCoord1, regCoord2, regSmParams )
	
	#define CALCULATE_SHADOWMAP_DATA_LS( Out, wPos )
	
	#define DECLARE_SHADOWMAP_DATA_PS
	
	#define CALCULATE_SHADOW_ALPHA( In )
	
	#define CALCULATE_SHADOW_GRASS( In )
	
	#define CALCULATE_ATTENUATION_LS( lightmap, cDiffuse, cAmbient ) lightmap.y * cDiffuse + lightmap.z * cAmbient

#endif	
