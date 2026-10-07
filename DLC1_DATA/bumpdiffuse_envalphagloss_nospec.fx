//////////////////////////////////////////////////////////////////////////////
//
// Workfile: BumpDiffuse_EnvAlphaGloss_NoSpec.fx
// Created by: Plus
//
// Diffuse bump mapping (one directional light) + per-vertex env mapping.
// final_color = lerp( diffuse_color, envmap_color, diffuse_color_alpha ).
//
// $Id: BumpDiffuse_EnvAlphaGloss_NoSpec.fx,v 1.3 2006/01/09 15:16:14 vano Exp $
//
//////////////////////////////////////////////////////////////////////////////

#include "lib.fx"

// textures
texture 		texDiffuse	:	DIFFUSE_MAP_0;
texture			texBumpGloss	:	BUMP_MAP_0;
texture			texEnvMap 	:	CUBE_MAP_0;

DECLARE_SHADOWMAP_DATA_MDL

// viewer position (object space)
float4	objectViewPos	: VIEW_POS
<
	int Space = SPACE_OBJECT;
>;

// light directions (object space)
float3 DirFromLight: TMP_LIGHT0_DIR 
<
	int Space = SPACE_OBJECT;
>;

// transformations
row_major float4x4 mFinal		: TOTAL_MATRIX; 
float4x4 mWorld					: WORLD_MATRIX;

shared const float4 g_Ambient		: LIGHT_AMBIENT = { 0.2f, 0.2f, 0.2f, 1.0f };
shared const float4 g_Diffuse		: LIGHT_DIFFUSE = { 1.0f, 1.0f, 1.0f, 1.0f };
shared const float3 g_FogTerm		: FOG_TERM		= { 1.0f, 800.0f, 1.f };
shared const float  g_Transparency	: TRANSPARENCY	= 1.f;
static const float  g_CubemapRadius = 1.f;
shared const float4 g_fogPlane		: FOG_PLANE		= { 1.f, 0.f, 0.f, 10000.f };

// declare samplers
DECLARE_DIFFUSE_SAMPLER( DiffSampler, texDiffuse )
DECLARE_BUMP_SAMPLER( BumpSampler, texBumpGloss )
DECLARE_CUBEMAP_SAMPLER( EnvSampler, texEnvMap )

// vertex shader input structure
struct VS_INPUT
{
	float3  Pos		: POSITION;	 // position in object space
	float3  Normal	: NORMAL;    // normal in object space
	float4  Tangent	: TANGENT;   // tangent in object space with a sign of binormal as a w component
	float2  Tex0	: TEXCOORD0; // diffuse/bump texcoords
};

// vertex shader output structure (for ps_1_1)
struct VS11_OUTPUT
{
	float4 Pos			: POSITION;
	float2 uv0			: TEXCOORD0;
	float2 uv0Diff		: TEXCOORD1;
	float3 Light		: COLOR0;
	float3 EnvCoords	: TEXCOORD2;
	
	DECLARE_SHADOWMAP_SUPPORT( TEXCOORD3, TEXCOORD4, COLOR1 )
};

/**
	Blinn-Phong simple vertex shader for ps_1_1
 */
VS11_OUTPUT PS11_BumpBlinnDiffuseSpecularVS( VS_INPUT v )
{
	VS11_OUTPUT o = (VS11_OUTPUT)0;

	// position (projected)
	o.Pos               = mul( float4( v.Pos, 1) , mFinal );
	// texcoords
	o.uv0 = v.Tex0;
	o.uv0Diff = v.Tex0;

	// directional light to object space
	//float3 objectLight    = normalize( objectViewPos );
	// light direction should come already normalized from the engine...
	float3 objectLight = -DirFromLight;
	//float3 objectLight  = objectViewPos;
	
	// calculate binormal
	float3 Binormal = cross( v.Normal, v.Tangent ) * v.Tangent.w; 

	// calculate light vector in texture space
	float3 tangentLight   = toTangentSpace( objectLight, v.Tangent, Binormal, v.Normal );

	// output light vector in tangent space
	o.Light = 0.5f + 0.5f * tangentLight;

	// calculate view vector in object space
	float3 objectViewDir  = objectViewPos - v.Pos;
                              
	// calculate environment map texcoords
	float3 objectEnvCoords	= normalize( reflect( -objectViewDir, v.Normal ) ) + v.Pos / g_CubemapRadius;
	o.EnvCoords				= normalize( mul( objectEnvCoords, mWorld ) ).xzy;
	
	CALCULATE_SHADOWMAP_DATA_MDL( o )

	return o;
}

/**
	Blinn-Phong simple pixel shader for ps_1_1

	TODO: renormalize interpolated vectors via normalizing cubemap
	TODO: replace multiplications w/ specular power texture lookup
 */
float4 PS11_BumpBlinnDiffuseSpecularPS( VS11_OUTPUT i, uniform float4 ambient, uniform float4 diffuse ): COLOR
{
	CALCULATE_SHADOW_MDL( i )

	// fetch normal + gloss factor from texture
	float4	texBump      = tex2D( BumpSampler, i.uv0 );

	// fetch diffuse color + alpha factor from texture
	float4 texDiffuse    = tex2D( DiffSampler, i.uv0Diff );

	// normal (unpack from texture)
	float3  Normal       = bx2( texBump );

	// light
	float3 Light         = bx2( i.Light );

	// dot product of light	with bump
	float cosA           = saturate( dot( Normal, Light ) );
	
	// fetch em texture
	float3	tEM = texCUBE( EnvSampler, i.EnvCoords.xyz );

	// return resulting color
	return float4( lerp( texDiffuse, tEM, texBump.a ) * ( ambient.xyz + cosA * diffuse.xyz ), texDiffuse.a * ambient.w );
}

technique Test1
<
	string Description = "diffuse bump mapping + per-vertex environment mapping";
	bool   ComputeTangentSpace = true;
	string VertexFormat = "VERTEX_XYZNT1T";
	bool   Default = true;
>
{
	pass P1
	{
		VertexShader = COMPILE_VS( vs_1_1 ) PS11_BumpBlinnDiffuseSpecularVS();
		PixelShader = COMPILE_PS( ps_1_1 ) PS11_BumpBlinnDiffuseSpecularPS( float4( g_Ambient.xyz, g_Transparency ), g_Diffuse );
	}
}