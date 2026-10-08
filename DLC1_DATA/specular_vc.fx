//////////////////////////////////////////////////////////////////////////////
//
// Workfile: specular_vc.fx
// Created by: Vano
//
// simple per-vertex specular hightlight shader with vertex color addition
//
//////////////////////////////////////////////////////////////////////////////

#include "lib.fx"

// Diffuse texture
texture 		DiffMap0 : DIFFUSE_MAP_0;

// Declare global lightmap
DECLARE_GLOBAL_LIGHTMAP_DATA
DECLARE_SHADOWMAP_DATA_MDL
DECLARE_WORLD_MATRIX

// viewer position (object space)
float4	objectViewPos	: VIEW_POS
<
	int 	Space = SPACE_OBJECT;
	bool    Editable = false;
>;

// Light direction( world space )
float3 LightDir			 : TMP_LIGHT0_DIR
<
	int Space = SPACE_OBJECT;
>;

// Diffuse color
shared const float4 g_Ambient		: LIGHT_AMBIENT		= { 0.2f, 0.2f, 0.2f, 1.0f };
shared const float4 g_Diffuse		: LIGHT_DIFFUSE		= { 1.0f, 1.0f, 1.0f, 1.0f };
shared const float3 g_Specular		: LIGHT_SPECULAR 	= { 1.0f, 1.0f, 1.0f };
shared const float3 g_FogTerm		: FOG_TERM		= { 1.0f, 800.0f, 1.f };
shared const float  g_Transparency	: TRANSPARENCY		= 1.f;
shared const float4 g_fogPlane		: FOG_PLANE		= { 1.f, 0.f, 0.f, 10000.f };

// transformations
row_major float4x4 	mFinal		: TOTAL_MATRIX;

// declare base diffuse sampler
DECLARE_DIFFUSE_SAMPLER( DiffSampler, DiffMap0 )

// Material specular power
static	int		SpecularPower	= 8;

// Vertex shader input structure
struct VS_INPUT
{
	float3 Pos			: POSITION;			// position in object space
	float3 Normal		: NORMAL;			// normal in object space
	float2 Tex0			: TEXCOORD0;		// diffuse texcoords
	float4 VertColor	: COLOR0;			// vertex color
};

// Vertex shader output structure (for ps_1_1)
struct VS11_OUTPUT
{
	float4 Pos		     : POSITION;
	float2 Tex0		     : TEXCOORD0;
	float4 Clr			 : COLOR0;		// Diffuse color
	float4 Spec			 : COLOR1;		// Specular color
	float4 VertClr		 : TEXCOORD1;	// Vertex color
	float  fog			 : FOG;
	
	DECLARE_GLOBAL_LIGHTMAP_SUPPORT( TEXCOORD2 )
	DECLARE_SHADOWMAP_SUPPORT( TEXCOORD3, TEXCOORD4, TEXCOORD5 )
};

/**
	Simple specular vertex shader with vertex color addition  for ps_1_1
 */
VS11_OUTPUT PS11_DiffuseVS( VS_INPUT In, uniform float4 diffuse, uniform float3 specular )
{
	VS11_OUTPUT Out = ( VS11_OUTPUT )0;

	// Position ( projected )
	Out.Pos					= mul( float4( In.Pos, 1 ) , mFinal );
	// Texture coordinate
	Out.Tex0				= In.Tex0;
	// Diffuse light color    
	Out.Clr					= saturate( dot( In.Normal , -LightDir ) ) * diffuse;
	// Vertex color addition
	Out.VertClr				= In.VertColor;       
	// Fog coeff
	//Out.fog 				= VertexFog( Out.Pos.z, g_FogTerm );
	Out.fog 	    = ViewLayeredFog( Out.Pos.z, g_fogPlane, In.Pos, g_FogTerm );
	// Calculate view vector in object space
	float3 objectViewDir	= normalize( objectViewPos - In.Pos );
	// Calculate reflection
	float3 Reflection		= normalize( reflect( objectViewDir, In.Normal ) );
	// Specular color
	Out.Spec				= float4( specular * pow( max( 0, dot( Reflection, -objectViewDir ) ), SpecularPower ), 1.f );
	// Global lightmap texcoords
	CALCULATE_SHARED_DATA( In.Pos )
	CALCULATE_GLOBAL_LIGHTMAP_TEXCOORDS( Out )
	CALCULATE_SHADOWMAP_DATA_MDL( Out )

	return Out;
}

/**
	Simple specular vertex shader with vertex color addition  for ps_1_1
 */
float4 PS11_DiffusePS( VS11_OUTPUT In, uniform float4 ambient ): COLOR
{
	// Get global lightmap
	GET_GLOBAL_LIGHTMAP( In )
	CALCULATE_SHADOW_MDL( In )
	// Diffuse color
	float4 Diff		= tex2D( DiffSampler, In.Tex0 );
	// Resulting color
	float3 Res		= ( Diff * CALCULATE_ATTENUATION( In.Clr, ambient ) + In.Spec * Diff.a ) * In.VertClr;  
	return  float4( Res, ambient.w );
}

technique Test1
<
	string 	Description			= "simple specular shader with vertex color addition";
	bool   	ComputeTangentSpace = false;
	string 	VertexFormat		= "VERTEX_XYZNCT1";
	bool	Default				= true;
	bool   UseAlpha				= false;
>
{
	pass P1
	{
		VertexShader	 = COMPILE_VS( vs_1_1 ) PS11_DiffuseVS( g_Diffuse, g_Specular );
		PixelShader		 = COMPILE_PS( ps_1_1 ) PS11_DiffusePS( float4( g_Ambient.xyz, g_Transparency ) );
		
		//FogEnable        = true;
		//CullMode	     = CCW;
		//FillMode	     = Solid;
		//ZWriteEnable     = true;
		//AlphaBlendEnable = false;
		//AlphaTestEnable  = false;
	}
}