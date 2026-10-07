//////////////////////////////////////////////////////////////////////////////
//
// Workfile: road_diffuse_detail.fx
// Created by: Vano
//
// diffuse shader with detail texture addition for roads
//
// $Id: 
//
//////////////////////////////////////////////////////////////////////////////

#include "lib.fx"

// Diffuse and detail textures
texture 		DiffMap0		: DIFFUSE_MAP_0;
texture			DetailMap		: DETAIL_MAP_0;
texture			LightMap0		: LIGHT_MAP_0;

// Params to calculate lightmap texcoords
const float4	lightmapTexParams	: USER_FLOAT4_PARAM;

// Diffuse color
shared const float4 g_Ambient	: LIGHT_AMBIENT	= { 0.2f, 0.2f, 0.2f, 1.0f };
shared const float4 g_Diffuse	: LIGHT_DIFFUSE = { 1.0f, 1.0f, 1.0f, 1.0f };
shared const float3 g_FogTerm	: FOG_TERM	= { 1.0f, 800.0f, 1.f };

// transformations
row_major float4x4 mFinal		: TOTAL_MATRIX;

// declare base diffuse sampler
DECLARE_DIFFUSE_SAMPLER_CLAMP( DiffSampler, DiffMap0 )

// declare detail sampler
DECLARE_DETAIL_SAMPLER( DetailSampler, DetailMap )

// declare lightmap sampler
DECLARE_DETAIL_SAMPLER( LightmapSampler, LightMap0 )

DECLARE_SHADOWMAP_DATA_ROAD

// Vertex shader input structure
struct VS_INPUT
{
	float3 Pos		: POSITION;		// position in object space
	float3 Normal	: NORMAL;		// normal in object space
	float2 Tex0		: TEXCOORD0;	// diffuse texture texcoords
	float2 Tex1		: TEXCOORD1;	// detail texture texcoords
};

// Vertex shader output structure (for ps_1_1)
struct VS11_OUTPUT
{
	float4 Pos		: POSITION;
	float2 Tex0		: TEXCOORD0;
	float2 Tex1		: TEXCOORD1;
	float2 Tex2		: TEXCOORD2;
	float  fog		: FOG;
	
	DECLARE_SHADOWMAP_SUPPORT3( TEXCOORD3, TEXCOORD4, TEXCOORD5, COLOR0 )
};

/**
	diffuse + detail vertex shader for ps_1_1
 */
VS11_OUTPUT PS11_DiffuseVS( VS_INPUT In )
{
	VS11_OUTPUT Out = ( VS11_OUTPUT )0;

	// position ( world space )
	float4 wPos = float4( In.Pos, 1.f );
	// Position ( projected )
	Out.Pos         = mul( wPos , mFinal );
	// Diffuse texture coordinate
	Out.Tex0	    = In.Tex0;
	// Detail texture coordinate
	Out.Tex1	    = In.Tex1;
	// Fog coeff
	Out.fog 	    = VertexFog( Out.Pos.z, g_FogTerm );
	// lightmap and diffuse coords
	Out.Tex2		= ( In.Pos.xz + lightmapTexParams.zw ) * lightmapTexParams.xy;
	
	CALCULATE_SHADOWMAP_DATA_ROAD( Out, wPos )

	return Out;
}

/**
	diffuse + detail vertex shader for ps_1_1
 */
float4 PS11_DiffusePS( VS11_OUTPUT In ) : COLOR
{
	CALCULATE_SHADOW_ALPHA( In )

	// Get diffuse color
	float4 albedo     = tex2D( DiffSampler, In.Tex0 ) * tex2D( DetailSampler, In.Tex1 ) * 2;
	// Get lightmap color
	float4 lightmap   = tex2D( LightmapSampler, In.Tex2 );
	// Calculate attenuation color
	float4 atten      = CALCULATE_ATTENUATION_LS( lightmap, g_Diffuse, g_Ambient );
	// Return resulting color
	return  albedo * atten;
}

technique Test1
<
	string 	Description			= "diffuse + detail road shader";
	bool   	ComputeTangentSpace = false;
	string 	VertexFormat		= "VERTEX_XYZNT2";
	bool	Default				= true;
>
{
	pass P1
	{
		VertexShader	= COMPILE_VS( vs_1_1 ) PS11_DiffuseVS( );
		PixelShader		= COMPILE_PS( ps_1_1 ) PS11_DiffusePS( );
	}
}