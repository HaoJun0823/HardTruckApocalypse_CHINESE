//////////////////////////////////////////////////////////////////////////////
//
// Workfile: blurShadow.fx
// Created by: Vano
//
// shader for bluring shadow
//
//////////////////////////////////////////////////////////////////////////////

#include "lib.fx"

// Texture
texture 	ShadowMap0		:	DIFFUSE_MAP_0; // Shadow texture
// TexCoord scale
float		TexCoordScale	:	USER_FLOAT_PARAM = 0.001f;

// Shadow sampler
DECLARE_SHADOW_SAMPLER( ShadowSampler, ShadowMap0 )

// Vertex shader input structure
struct vsInput
{
	float3 Pos		: POSITION;		// Position in object space
	float2 uv0		: TexCOORD0;    // Texture coord
};

// Vertex shader output structure
struct vsOutput
{
	float4 Pos		: POSITION;
	float2 Uvs[ 4 ]	: TEXCOORD0;
};

//	Blur shadow vertex shader
vsOutput DiffuseVS( vsInput In )
{
	vsOutput Out	= ( vsOutput )0;
	// Pos with normal addition in object space
	Out.Pos			= float4( In.Pos, 1 );
	// Set texcoords with neighbours
	Out.Uvs[ 0 ]	= In.uv0 + float2(  0,  1 ) * TexCoordScale;
	Out.Uvs[ 1 ]	= In.uv0 + float2(  1,  0 ) * TexCoordScale;
	Out.Uvs[ 2 ]	= In.uv0 + float2(  0, -1 ) * TexCoordScale;
	Out.Uvs[ 3 ]	= In.uv0 + float2( -1,  0 ) * TexCoordScale;
	
	return Out;
}

// Blur shadow pixel shader
float4 SimplePS( vsOutput In ): COLOR
{
	float4	texColor = tex2D( ShadowSampler, In.Uvs[ 0 ] ) * 0.25;
	texColor		+= tex2D( ShadowSampler, In.Uvs[ 1 ] ) * 0.25;
	texColor		+= tex2D( ShadowSampler, In.Uvs[ 2 ] ) * 0.25;
	texColor		+= tex2D( ShadowSampler, In.Uvs[ 3 ] ) * 0.25;
	
	return texColor;  
}

//	Blur shadow vertex shader
vsOutput DiffuseVS_SM( vsInput In )
{
	vsOutput Out	= ( vsOutput )0;
	// Pos with normal addition in object space
	Out.Pos			= float4( In.Pos, 1 );
	
	// Calculate dither offset
	//float2 doff = 0.5f + 0.5f * Out.Pos.xy / Out.Pos.w;
	//doff *= float2( 1024.f, 1024.f );
	//doff = frac( doff* 0.5f ) > 0.25f;
	//doff.y += doff.x;
	//if( doff.y > 1.1 )
	//	doff.y = 0.f;
	float2 doff = float2(0,0);

	// Set texcoords with neighbours
	float2 offset0 = float2(  -1.5, 0.5 ) + doff;
	float2 offset1 = float2(  0.5,  0.5 ) + doff;
	float2 offset2 = float2(  -1.5, -1.5 ) + doff;
	float2 offset3 = float2(  0.5,  -1.5 ) + doff;
	
	Out.Uvs[ 0 ]	= In.uv0 + offset0 * TexCoordScale;
	Out.Uvs[ 1 ]	= In.uv0 + offset1 * TexCoordScale;
	Out.Uvs[ 2 ]	= In.uv0 + offset2 * TexCoordScale;
	Out.Uvs[ 3 ]	= In.uv0 + offset3 * TexCoordScale;

	return Out;
}

technique Tech1
<
	string 	Description		= "blur shadow shader";
	bool   	ComputeTangentSpace 	= false;
	string 	VertexFormat		= "VERTEX_XYZNT1";
	bool	Default			= true;
>
{
	pass P1
	{
		VertexShader	= compile vs_1_1 DiffuseVS();
		PixelShader	= compile ps_1_1 SimplePS();
	}
}

technique Tech2
<
	string 	Description		= "blur shadowmap shader";
	bool   	ComputeTangentSpace 	= false;
	string 	VertexFormat		= "VERTEX_XYZNT1";
	bool	Default			= false;
>
{
	pass P1
	{
		VertexShader = compile vs_1_1 DiffuseVS();
		PixelShader	 = compile ps_1_1 SimplePS();
	}
}