//////////////////////////////////////////////////////////////////////////////
//
// Workfile: lightmap_detail.fx
// Created by: Vano
//
// diffuse shader with detail and lightmap textures addition
//
// $Id: lightmap_detail.fx,v 1.3 2006/01/17 14:23:49 vano Exp $
//
////////////////////////////////////////////////////////////////////////////// 

#include "lib.fx"

// Diffuse, detail and lightmap textures
texture DiffMap0 	: DIFFUSE_MAP_0;
texture	DetailMap	: DETAIL_MAP_0;
texture	LightMap	: LIGHT_MAP_0;

// Declare global lightmap
DECLARE_GLOBAL_LIGHTMAP_DATA
DECLARE_SHADOWMAP_DATA_MDL
DECLARE_WORLD_MATRIX

// Light direction( world space )
float3 INSTANCES( LightDir, MAX_INSTANCES ) : TMP_LIGHT0_DIR
<
	int Space = SPACE_OBJECT;
>;

// transformations
row_major float4x4 INSTANCES( mFinal, MAX_INSTANCES ) : TOTAL_MATRIX;

// Diffuse color
shared const float4 g_Ambient		: LIGHT_AMBIENT	= { 0.2f, 0.2f, 0.2f, 1.0f };
shared const float4 g_Diffuse		: LIGHT_DIFFUSE	= { 1.0f, 1.0f, 1.0f, 1.0f };
shared const float3 g_FogTerm		: FOG_TERM		= { 1.0f, 800.0f, 1.f };
shared const float  g_Transparency	: TRANSPARENCY	= 1.f;
shared const float4 g_fogPlane		: FOG_PLANE		= { 1.f, 0.f, 0.f, 10000.f };

// declare base diffuse sampler
DECLARE_DIFFUSE_SAMPLER( DiffSampler, DiffMap0 )

// declare detail sampler
DECLARE_DETAIL_SAMPLER( DetailSampler, DetailMap )

// declare lightmap sampler
DECLARE_DETAIL_SAMPLER( LightMapSampler, LightMap )

// Vertex shader input structure
struct VS_INPUT
{
	float3 Pos	     : POSITION;		// position in object space
	float3 Normal	 : NORMAL;			// normal in object space
	float2 Tex0	     : TEXCOORD0;		// diffuse texture texcoords
	float2 Tex1	     : TEXCOORD1;		// lightmap texture texcoords
	float2 Tex2	     : TEXCOORD2;		// detail texture texcoords
	
	DECLARE_INSTANCE_SUPPORT								
};

// Vertex shader output structure (for ps_1_1)
struct VS11_OUTPUT
{
	float4 Pos		    : POSITION;
	float2 Tex0		    : TEXCOORD0;
	float2 Tex1			: TEXCOORD1;
	float2 Tex2			: TEXCOORD2;
	float4 Clr          : COLOR0;
	float  fog			: FOG;
	
	DECLARE_GLOBAL_LIGHTMAP_SUPPORT( TEXCOORD3 )
	DECLARE_SHADOWMAP_SUPPORT( TEXCOORD4, TEXCOORD5, COLOR1 )
};

/**
	diffuse + detail + lightmap vertex shader for ps_1_1
 */
VS11_OUTPUT PS11_DiffuseVS( VS_INPUT In, uniform float4 diffuse )
{
	VS11_OUTPUT Out = ( VS11_OUTPUT )0;

	// Position ( projected )
	Out.Pos         = mul( float4( In.Pos, 1 ), INSTANCE_GET( mFinal ) );
	// Diffuse texture coordinate
	Out.Tex0	    = In.Tex0;
	// Lightmap texture coordinate
	Out.Tex1	    = In.Tex1;
	// Detail texture coordinate
	Out.Tex2	    = In.Tex2;	
	// Light color    
	Out.Clr         = saturate( dot( In.Normal , -INSTANCE_GET( LightDir ) ) ) * diffuse;       
	// Fog coeff
	Out.fog 	    = ViewLayeredFog( Out.Pos.z, g_fogPlane, In.Pos, g_FogTerm );
	// Global lightmap texcoords
	CALCULATE_SHARED_DATA( In.Pos )
	CALCULATE_GLOBAL_LIGHTMAP_TEXCOORDS( Out )
	CALCULATE_SHADOWMAP_DATA_MDL( Out )

	return Out;
}

/**
	diffuse + detail + lightmap pixel shader for ps_1_1
 */
float4 PS11_DiffusePS( VS11_OUTPUT In, uniform float4 ambient ) : COLOR
{	
	// Get global lightmap
	GET_GLOBAL_LIGHTMAP( In )
	CALCULATE_SHADOW_MDL( In )
	// Base color
	float4 bcolor = tex2D( DiffSampler, In.Tex0 ) * tex2D( DetailSampler, In.Tex2 );  
	bcolor *= tex2D( LightMapSampler, In.Tex1 );
	// Add light
	float4 lcolor = bcolor * CALCULATE_ATTENUATION( In.Clr, ambient );
	//Full color
	return float4( lcolor.xyz * 4, bcolor.w * ambient.w );
}

technique Test1
<
	string 	Description			= "diffuse + detail + lightmap shader";
	bool   	ComputeTangentSpace = false;
	string 	VertexFormat		= "VERTEX_XYZNT3";
	bool	Default				= true;
	int		MaxInstances		= MAX_INSTANCES;
>
{
	pass P1
	{
		VertexShader	 = COMPILE_VS( vs_1_1 ) PS11_DiffuseVS( g_Diffuse );
		PixelShader		 = COMPILE_PS( ps_1_1 ) PS11_DiffusePS( float4( g_Ambient.xyz, g_Transparency ) );
		
		//FogEnable        = true;
		//CullMode	     = CCW;
		//FillMode	     = Solid;
		//ZWriteEnable     = true;
		//AlphaBlendEnable = false;
		//AlphaTestEnable  = true;
		//AlphaFunc		 = GreaterEqual;
		//AlphaRef		 = 100;
	}
}