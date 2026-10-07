//////////////////////////////////////////////////////////////////////////////
//
// Workfile: road.fx
// Created by: Vano
//
// simple diffuse shader for roads ( alpha-blended )
//
// $Id: road.fx,v 1.3 2005/12/20 08:16:34 vano Exp $
//
//////////////////////////////////////////////////////////////////////////////

#include "lib.fx"

// Diffuse texture
texture 		DiffMap0		: DIFFUSE_MAP_0;
texture			LightMap0		: LIGHT_MAP_0;
texture			DiffMap1		: DIFFUSE_MAP_1;

// Params to calculate lightmap and diffuse texcoords
const float4	lightmapTexParams	: USER_FLOAT4_PARAM;
const float4	diffuseTexParams	: USER_FLOAT4_PARAM2;

// Diffuse color
shared const float4	g_Ambient	: LIGHT_AMBIENT = { 0.2f, 0.2f, 0.2f, 1.0f };
shared const float4	g_Diffuse	: LIGHT_DIFFUSE = { 1.0f, 1.0f, 1.0f, 1.0f };
shared const float3	g_FogTerm	: FOG_TERM = { 1.0f, 800.0f, 1.f };

// transformation
row_major float4x4 	mFinal		: TOTAL_MATRIX;

// declare base diffuse sampler
DECLARE_DIFFUSE_SAMPLER_CLAMP( DiffSampler, DiffMap0 )

// declare lightmap sampler
DECLARE_DETAIL_SAMPLER( LightmapSampler, LightMap0 )

// declare diffuse sampler 1
DECLARE_DIFFUSE_SAMPLER( DiffSampler1, DiffMap1 )

DECLARE_SHADOWMAP_DATA_ROAD

// Vertex shader input structure
struct VS_INPUT
{
	float3 Pos	: POSITION;		// position in object space
	float3 Normal	: NORMAL;		// normal in object space
	float2 Tex0	: TEXCOORD0;		// diffuse texcoords
};


// Vertex shader output structure (for ps_1_1)
struct VS11_OUTPUT
{
	float4 Pos	: POSITION;
	float2 Tex0	: TEXCOORD0;
	float2 Tex1	: TEXCOORD1;
	float2 Tex2	: TEXCOORD2;
	float  fog	: FOG;
	
	DECLARE_SHADOWMAP_SUPPORT3( TEXCOORD3, TEXCOORD4, TEXCOORD5, COLOR0 )
};


/**
	Simple diffuse vertex shader for ps_1_1
 */
VS11_OUTPUT PS11_DiffuseVS( VS_INPUT In , uniform float4 ambient, uniform float4 diffuse )
{
	VS11_OUTPUT Out = ( VS11_OUTPUT )0;

	// position ( world space )
	float4 wPos = float4( In.Pos, 1.f );
	// position ( projected )
	Out.Pos		= mul( wPos, mFinal );
	// texture coordinate
	Out.Tex0	= In.Tex0;
	// global lightmap and diffuse coords
	Out.Tex1		= ( In.Pos.xz + lightmapTexParams.zw ) * lightmapTexParams.xy;
	Out.Tex2		= ( In.Pos.xz + diffuseTexParams.zw ) * diffuseTexParams.xy; 
	// fog coeff
	Out.fog 	    = VertexFog( Out.Pos.z, g_FogTerm );
	
	CALCULATE_SHADOWMAP_DATA_ROAD( Out, wPos )

	return Out;
}


/**
	Simple diffuse pixel shader for ps_1_1
 */
float4 PS11_DiffusePS( VS11_OUTPUT In ): COLOR
{
	CALCULATE_SHADOW_ALPHA( In )

	// Get diffuse color
	float4 albedo     = tex2D( DiffSampler, In.Tex0 ) * tex2D( DiffSampler1, In.Tex2 ) * 2;
	// Get lightmap color
	float4 lightmap   = tex2D( LightmapSampler, In.Tex1 );
	// Calculate attenuation color
	float4 atten      = CALCULATE_ATTENUATION_LS( lightmap, g_Diffuse, g_Ambient );
	// Return resulting color
	return  albedo * atten;
}


//
technique Test1
<
	string 	Description			= "road shader";
	bool   	ComputeTangentSpace = false;
	string 	VertexFormat		= "VERTEX_XYZNT1";
	bool	Default				= true;
>
{
	pass P1
	{
		VertexShader	= COMPILE_VS( vs_1_1 )	PS11_DiffuseVS( g_Ambient, g_Diffuse );
		PixelShader		= COMPILE_PS( ps_1_1 )	PS11_DiffusePS();
	}
}