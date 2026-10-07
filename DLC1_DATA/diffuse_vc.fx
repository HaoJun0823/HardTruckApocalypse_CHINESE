//////////////////////////////////////////////////////////////////////////////
//
// Workfile: diffuse_vc.fx
// Created by: Vano
//
// simple diffuse shader with vertex coloring
//
// $Id: diffuse_vc.fx,v 1.3 2006/01/17 14:23:48 vano Exp $
//
//////////////////////////////////////////////////////////////////////////////

#include "lib.fx"

// Diffuse texture
texture DiffMap0	: DIFFUSE_MAP_0;

// Declare global lightmap
DECLARE_GLOBAL_LIGHTMAP_DATA
DECLARE_SHADOWMAP_DATA_MDL
DECLARE_WORLD_MATRIX

// Light direction( world space )
float3 INSTANCES( LightDir, MAX_INSTANCES )	: TMP_LIGHT0_DIR
<
	int Space = SPACE_OBJECT;
>;

// Diffuse color
shared const float4 g_Ambient		: LIGHT_AMBIENT = { 0.2f, 0.2f, 0.2f, 1.0f };
shared const float4 g_Diffuse		: LIGHT_DIFFUSE = { 1.0f, 1.0f, 1.0f, 1.0f };
shared const float3 g_FogTerm		: FOG_TERM		= { 1.0f, 800.0f, 1.f };
shared const float  g_Transparency	: TRANSPARENCY	= 1.f;
shared const float4 g_fogPlane		: FOG_PLANE		= { 1.f, 0.f, 0.f, 10000.f };

// transformations
row_major float4x4 INSTANCES( mFinal, MAX_INSTANCES )	: TOTAL_MATRIX;

// declare base diffuse sampler
DECLARE_DIFFUSE_SAMPLER( DiffSampler, DiffMap0 )

// Vertex shader input structure
struct VS_INPUT
{
	float3 Pos		     : POSITION;		// position in object space
	float3 Normal	     : NORMAL;			// normal in object space
	float2 Tex0		     : TEXCOORD0;		// diffuse texcoords
	float4 VertColor	 : COLOR0;			// vertex color
	
	DECLARE_INSTANCE_SUPPORT
};

// Vertex shader output structure (for ps_1_1)
struct VS11_OUTPUT
{
	float4 Pos		     : POSITION;
	float2 Tex0		     : TEXCOORD0;
	float4 Clr			 : COLOR1;		// Diffuse color
	float4 VertColor	 : COLOR0;		// Vertex color
	float  fog			 : FOG;
	
	DECLARE_GLOBAL_LIGHTMAP_SUPPORT( TEXCOORD1 )
	DECLARE_SHADOWMAP_SUPPORT( TEXCOORD2, TEXCOORD3, TEXCOORD4 )
};

/**
	Simple diffuse + vertex color vertex shader for ps_1_1
 */
VS11_OUTPUT PS11_Diffuse_VC_VS( VS_INPUT In, uniform float4 diffuse )
{
	VS11_OUTPUT Out = ( VS11_OUTPUT )0;

	// Position ( projected )
	Out.Pos         = mul( float4( In.Pos, 1 ) , INSTANCE_GET( mFinal ) );
	// Texture coordinate
	Out.Tex0	    = In.Tex0;
	// Diffuse light color     
	Out.Clr         = saturate( dot( In.Normal, -INSTANCE_GET( LightDir ) ) ) * diffuse;
	// Vertex color 
	Out.VertColor   = In.VertColor;
	// Fog coeff
	Out.fog 	    = ViewLayeredFog( Out.Pos.z, g_fogPlane, In.Pos, g_FogTerm );  
	// Global lightmap texcoords
	CALCULATE_SHARED_DATA( In.Pos )
	CALCULATE_GLOBAL_LIGHTMAP_TEXCOORDS( Out )
	CALCULATE_SHADOWMAP_DATA_MDL( Out )

	return Out;
}

/**
	Simple diffuse + vertex color pixel shader for ps_1_1
 */
float4 PS11_Diffuse_VC_PS( VS11_OUTPUT In, uniform float4 ambient ): COLOR
{
	GET_GLOBAL_LIGHTMAP( In )
	CALCULATE_SHADOW_MDL( In )

	//return tex2D( DiffSampler, In.Tex0 ) * float4( CALCULATE_ATTENUATION( In.Clr, ambient ).xyz, g_Transparency ) * In.VertColor;
	return tex2D( DiffSampler, In.Tex0 ) * CALCULATE_ATTENUATION( In.Clr, ambient ) * In.VertColor;
}

technique Test1
<
	string 	Description			= "simple diffuse shader + vertex colors";
	bool   	ComputeTangentSpace = false;
	string 	VertexFormat		= "VERTEX_XYZNCT1";
	bool	Default				= true;
	int		MaxInstances		= MAX_INSTANCES;
>
{
	pass P1
	{
		VertexShader = COMPILE_VS( vs_1_1 ) PS11_Diffuse_VC_VS( float4( g_Diffuse.xyz, 0.f ) );
		PixelShader  = COMPILE_PS( ps_1_1 ) PS11_Diffuse_VC_PS( float4( g_Ambient.xyz, g_Transparency ) );

		//AlphaBlendEnable = false;
		//AlphaTestEnable  = true;
		//FogEnable        = false;
		//CullMode         = CCW;
		//FillMode         = Solid;
		//ZWriteEnable     = True;
		//AlphaTestEnable  = true;
		//AlphaFunc = GreaterEqual;
		//AlphaRef = 100;
	}
}
