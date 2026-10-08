//////////////////////////////////////////////////////////////////////////////
//
// Workfile: roadProjector.fx
// Created by: Vano
//
// shader for rendering projectors upon landscape and road
//
//////////////////////////////////////////////////////////////////////////////

#include "lib.fx"

//
texture 	DiffMap0		:	DIFFUSE_MAP_0; 
//
texture 	DiffMap1		:	DIFFUSE_MAP_1; 

DECLARE_DIFFUSE_SAMPLER_CLAMP( DiffSampler, DiffMap0 );
DECLARE_DIFFUSE_SAMPLER		( LmSampler, DiffMap1 );


// Texcoord generating matrix
float4x4	TextureTransform:	USER_FLOAT4x4_PARAM;
// Projector position
float3		ProjectorPos	:	USER_FLOAT4_PARAM;
// Projector direction
float3		ProjectorDir	:	USER_FLOAT3_PARAM;
// Projector radius
float		ProjectorRadius :	USER_FLOAT_PARAM;
// Projector fade end
float		FadeEnd		:	USER_FLOAT_PARAM2;
// Total transformation
float4x4 	mFinal		:	TOTAL_MATRIX;
// Viewer position in world space
float4		ViewPos		:	VIEW_POS
<
	//int 	Space		= SPACE_WORLD;
	int 	Space		= SPACE_OBJECT;	
	bool    Editable	= false;
>;
// Data for position generation
float3 initPos			:	USER_FLOAT3_PARAM2		= { 0, 0, 0 };
// Global diffuse texcoord generation params
const float4 diffuseTexParams : USER_FLOAT4_PARAM2;

// Vertex shader input structure
struct vsInput
{
	float Pos	: POSITION;
	int4 AltPos 	: TEXCOORD0;
};

// Vertex shader output structure
struct vsOutput
{
	float4 Pos		: POSITION;
	float4 uv0		: TEXCOORD0;
	float2 uv1		: TEXCOORD1;
	float3 clr		: COLOR0;
};

// projector vertex shader
vsOutput DiffuseVS( vsInput In, uniform bool needZ )
{
	vsOutput Out		= ( vsOutput )0;
	// Calculate world position
	float4 pos;
	int indX =  In.AltPos.x / 128;
	int indY =  In.AltPos.x - indX * 128;
	pos.x  = initPos.x + initPos.z * indX;
 	pos.z  = initPos.y + initPos.z * indY;
	pos.y  = In.Pos.x;
    	pos.w  = 1;
	// Calculate normal
	float3 normal;
	normal.xz = In.AltPos.zw / 1024.f;
	normal.y  = sqrt( 1.f - dot( normal.xz, normal.xz ) );
	// Calculate distance to viewer
	float dist		= distance( ViewPos, pos );
	// Position ( projected )
	Out.Pos			= mul( pos, mFinal );
	// Generate projector texcoords
	Out.uv0			= mul( pos, TextureTransform );
	// global diffuse texture coords
	Out.uv1			= ( pos.xz + diffuseTexParams.zw ) * diffuseTexParams.xy; 
	
	// Calculate dot product
	float   dp = dot( normal, ProjectorDir );

	// Calculate vertex color
	if( needZ )
	{
		if( Out.uv0.z <= 0 )
			dp = -1;
	}
	
	if( dp > 0 )
	{
		// Calculate dist factor
		float  projDist		 = distance( ProjectorPos, pos );
		float  projFadeStart 	= ProjectorRadius * PROJECTOR_FADE_START_COEFF;
		float  df		= saturate( 1.f - ( projDist - projFadeStart ) / ( ProjectorRadius - projFadeStart ) );
	
		// Fade projector in far
		float  FadeStart	= FadeEnd * 0.7;
		float  ff		= saturate( 1.f - ( dist - FadeStart ) / ( FadeEnd - FadeStart ) );
	
		// Calculate color according to angle
		float	c		= saturate( ff * df * pow( dp, 0.3 ) ); 
		Out.clr			= float3( c, c, c );
	}
	else
	{
		Out.clr			= float3( 0, 0, 0 );
	}
	
	return Out;
}

float4 SimplePS( vsOutput In ): COLOR
{
	if( In.uv0.z > 0.0 )
	{
		float2 uv = In.uv0.xy / In.uv0.w;
		float4 col = tex2D( DiffSampler, uv );
		float3 lightMap = tex2D( LmSampler, In.uv1 );
		return  float4( col.rgb * lightMap * In.clr.r * col.a, 1.0f );
	}
	else
	{
		return  float4( 0, 0, 0, 0 );
	}
}

technique Projector_PS11
<
	string 	Description			= "projector shader ps11";
	bool   	ComputeTangentSpace = false;
	string 	VertexFormat		= "VERTEX_XYZNT1";
	bool	Default				= true;
>
{
	pass P1
	{
		VertexShader			= compile vs_1_1 DiffuseVS( true );
		PixelShader				= NULL;
	}
}


technique Projector_PS20
<
	string 	Description			= "projector shader ps20";
	bool   	ComputeTangentSpace = false;
	string 	VertexFormat		= "VERTEX_XYZNT1";
	bool	Default				= true;
>
{
	pass P1
	{
		VertexShader			= compile vs_1_1 DiffuseVS( false );
		PixelShader				= compile ps_2_0  SimplePS();	
	}
}