//////////////////////////////////////////////////////////////////////////////
//
// Workfile: lsSprite.fx
// Created by: Vano
//
// shader for rendering sprites upon landscape
//
//////////////////////////////////////////////////////////////////////////////

#include "lib.fx"

// Total transformation
float4x4 	mFinal			:	TOTAL_MATRIX;
// init data for position
float3		initPos			:	USER_FLOAT3_PARAM2		= { 0, 0, 0 };
// Light direction( object space )
float3 LightDir				: TMP_LIGHT0_DIR
<
	int Space = SPACE_OBJECT;
>;
// Weather params
shared const float4 g_Ambient		: LIGHT_AMBIENT	= { 0.2f, 0.2f, 0.2f, 1.0f };
shared const float4 g_Diffuse		: LIGHT_DIFFUSE	= { 0.5f, 0.5f, 0.5f, 1.0f };
shared const float2 g_FogTerm		: FOG_TERM		= { 1.0f, 800.0f };
// texture transform martix
shared row_major float4x4 texTransform	: USER_FLOAT4x4_PARAM;
// vertex color
shared const float4	      VertColor		: USER_FLOAT4_PARAM;

// Vertex shader input structure
struct vsInput
{
	float Pos		: POSITION;
	int4 AltPos 	: TEXCOORD0;
};

// Vertex shader output structure
struct vsOutput
{
	float4 Pos		: POSITION;
	float2 Tex0		: TEXCOORD0;
	float4 Clr		: COLOR0;
};

// projector vertex shader
vsOutput DiffuseVS( vsInput In )
{
	vsOutput Out	= ( vsOutput )0;
	
	// Calculate position ( world space )
	float4 pos;
	int indX		= In.AltPos.x / 128;
	int indY		= In.AltPos.x - indX * 128;
	pos.x			= initPos.x + initPos.z * indX;
 	pos.z			= initPos.y + initPos.z * indY;
	pos.y			= In.Pos.x;
    pos.w			= 1;

	// Position ( projected )
	Out.Pos         = mul( pos, mFinal );
	// Texture coordinate
	Out.Tex0	= mul( pos, texTransform );
	// Light color    
	Out.Clr		= float4( VertColor.xyz * ( g_Diffuse * saturate( dot( float3( 0.f, 1.f, 0.f ), -LightDir ) ) + g_Ambient ), VertColor.w );       
	// Fog coeff
	Out.Clr	       *= VertexFog( Out.Pos.z, g_FogTerm );

	return Out;
}

technique Test1
<
	string 	Description			= "landscape sprite shader";
	bool   	ComputeTangentSpace = false;
	string 	VertexFormat		= "VERTEX_XYZNT1";
	bool	Default				= true;
>
{
	pass P1
	{
		VertexShader				= compile vs_1_1 DiffuseVS();
		PixelShader					= NULL;			
	}
}