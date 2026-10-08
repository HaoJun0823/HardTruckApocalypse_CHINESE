//////////////////////////////////////////////////////////////////////////////
//
// Workfile: shadowMap.fx
// Created by: Vano
//
// shader for rendering shadowmap
//
// $Id: shadowMap.fx,v 1.1 2005/12/02 13:18:33 vano Exp $
//
//////////////////////////////////////////////////////////////////////////////

#include "lib.fx"

// Diffuse texture
texture 			DiffMap0		: DIFFUSE_MAP_0;
// total transform
row_major float4x4	mFinal			: TOTAL_MATRIX;
// world transform
row_major float4x4	mWorld			: WORLD_MATRIX;
// model to view transform
row_major float4x4	mWorldView		: MODEL_VIEW_MATRIX;
// projection matrix
row_major float4x4	mProj			: PROJECTION_MATRIX;
// Texel size matrix ( from view space )
row_major float4x4	TexelSizeMat	: USER_FLOAT4x4_PARAM; 	 
// bias values
const	  float3	ZBias			: USER_FLOAT3_PARAM;
// light direction ( view space )
const	  float3    lightDir		: USER_FLOAT3_PARAM2;
// Alpha addition
const	float		alphaAddition	: USER_FLOAT_PARAM = { 0.f };
// depth pack constants
static const float2 pack1 = { 256.f, 65536.f };
static const float  pack2 = { 1.f / 256.f };

// declare base diffuse sampler
DECLARE_DIFFUSE_SAMPLER( DiffSampler, DiffMap0 )

// Vertex shader input structure
struct VS_INPUT
{
	float3 Pos	  : POSITION;	// position in object space
	float2 Tex0	  : TEXCOORD0;	// diffuse texcoords
	float3 Normal : NORMAL;		// normal in object space
};

// Vertex shader output structure
struct VS11_OUTPUT
{
	float4 Pos	: POSITION;
	float2 Tex0	: TEXCOORD0;
	float4 Smd	: TEXCOORD1;
};

// Shadowmap vertex shader for ps_1_1 
VS11_OUTPUT PS11_DiffuseVS( VS_INPUT In )
{
	VS11_OUTPUT Out = ( VS11_OUTPUT )0;

	// Calculate world position
	Out.Pos			 = mul( float4( In.Pos, 1 ), mWorldView );		
	// Calculate texel size
	float  texelSize = length( mul( Out.Pos, TexelSizeMat ).xyz );
	// Calculate biased position
	Out.Pos			+= float4( lightDir, 0.f ) * ( ZBias.x + ZBias.y * texelSize );	
	// Calculate world normal
	float3 wNormal = normalize( mul( In.Normal, ( float3x3 ) mWorld ) );
	Out.Pos -= float4( wNormal * texelSize * ZBias.z, 0.f );
	// Position ( projected ) + bias
	Out.Pos         = mul( Out.Pos, mProj );
	// Shadowmap data
	Out.Smd			= float4( Out.Pos.zzz, Out.Pos.w );
	// Texture coordinate
	Out.Tex0	    = In.Tex0;
	// Position ( projected )
	Out.Pos = mul( float4( In.Pos, 1 ) , mFinal );

	return Out;
}

// Shadowmap pixel shader for ps_1_1
float4 PS11_DiffusePS_DT( VS11_OUTPUT In ): COLOR
{  
	float4 diff  = tex2D( DiffSampler, In.Tex0 );
	float  alpha = diff.w + alphaAddition;
	
	return alpha.xxxx;	
}

// Shadowmap pixel shader for ps_1_1
float4 PS11_DiffusePS_NDT( VS11_OUTPUT In ): COLOR
{  
	// Get diffuse color
	float4 diff	  = tex2D( DiffSampler, In.Tex0 );

	// Calculate alpha
	float  alpha  = diff.w + alphaAddition;

	// Calculate depth
	float depth = In.Smd.z / In.Smd.w;	
	
	// Pack depth
	//float2 depthPacked = frac( floor( depth * pack1 ) * pack2 );
	depth *= 256.f;
	float2 depthPacked;
	depthPacked.x = floor( depth ) * pack2;
	depthPacked.y = frac( depth );
	
	// Return packed depth and model alpha
	return float4( depthPacked, alpha.xx );
}

technique Tech1
<
	string 	Description			= "shadowmap shader";
	bool   	ComputeTangentSpace = false;
	string 	VertexFormat		= "VERTEX_XYZNT1";
	bool	Default				= true;
>
{
	pass P1
	{
		VertexShader	= compile vs_1_1 PS11_DiffuseVS();
		
#ifdef SUPPORT_DEPTH_TEXTURES
		
		PixelShader		= compile ps_1_1 PS11_DiffusePS_DT();
#else

		PixelShader		= compile ps_2_0 PS11_DiffusePS_NDT();
#endif		
	}
}

technique Tech2
<
	string 	Description			= "shadowmap shader";
	bool   	ComputeTangentSpace = false;
	string 	VertexFormat		= "VERTEX_XYZNT1";
	bool	Default				= false;
>
{
	pass P1
	{
		VertexShader	= compile vs_1_1 PS11_DiffuseVS();
		PixelShader		= compile ps_1_1 PS11_DiffusePS_DT();		
	}
}