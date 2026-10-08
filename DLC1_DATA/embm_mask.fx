//////////////////////////////////////////////////////////////////////////////
//
// Workfile: embm_mask.fx
// Created by: Vano
//
// diffuse + bump + cubemap + specular + mask
//
// $Id: embm_mask.fx,v 1.3 2006/01/09 15:16:15 vano Exp $
//
////////////////////////////////////////////////////////////////////////////// 

#include "lib.fx"

// textures
texture   texDiffuse   : DIFFUSE_MAP_0;
texture   texBumpGloss : BUMP_MAP_0;
texture   texEnvMap    : CUBE_MAP_0;
texture   texMask      : MASK_MAP_0;   

// Declare global lightmap
DECLARE_GLOBAL_LIGHTMAP_DATA
DECLARE_SHADOWMAP_DATA_MDL

// declare samplers
DECLARE_DIFFUSE_SAMPLER( DiffSampler, texDiffuse )
DECLARE_BUMP_SAMPLER   ( BumpSampler, texBumpGloss )
DECLARE_CUBEMAP_SAMPLER( EnvSampler, texEnvMap )
DECLARE_DIFFUSE_SAMPLER( MaskSampler, texMask )

// viewer position ( object space )
float4   objectViewPos   : VIEW_POS
<
   int Space = SPACE_OBJECT;
>;

// light directions ( object space )
float3 DirFromLight: TMP_LIGHT0_DIR 
<
   int Space = SPACE_OBJECT;
>;

// transformations
row_major float4x4 mFinal: TOTAL_MATRIX; 
row_major float4x4 mWorld: WORLD_MATRIX;

// render parameters
shared const float4 g_Ambient      : LIGHT_AMBIENT    = { 0.2f, 0.2f, 0.2f, 1.0f };
shared const float4 g_Diffuse      : LIGHT_DIFFUSE    = { 1.0f, 1.0f, 1.0f, 1.0f };
shared const float3 g_Specular     : LIGHT_SPECULAR   = { 1.0f, 1.0f, 1.0f };
shared const float3 g_FogTerm	   : FOG_TERM		= { 1.0f, 800.0f, 1.f };
shared const float  g_Transparency : TRANSPARENCY     = 1.f;
static const float  g_CubemapRadius = 1.f;
shared const float4 g_fogPlane		: FOG_PLANE		= { 1.f, 0.f, 0.f, 10000.f };

// mask colors
row_major const float4x4 maskColors : MASK_COLORS;

// vertex shader input structure
struct VS_INPUT
{
   float3   Pos       : POSITION;  // position in object space
   float3   Normal    : NORMAL;    // normal in object space
   float4   Tangent   : TANGENT;   // tangent in object space with a sign of binormal as a w component
   float2   Tex0      : TEXCOORD0; // diffuse / bump texcoords
};

////////////////////////////////////////////////////////////////
// PS_1_1 technique
////////////////////////////////////////////////////////////////

// vertex shader output structure ( for ps_1_1 ) first pass
struct VS11_OUTPUT_1
{
   float4 Pos			: POSITION;
   float2 uv0Diff		: TEXCOORD1;
   float3 EnvCoords		: TEXCOORD2;
   float2 maskCoords	: TEXCOORD3;
   float  fog			: FOG; 	
};

//
VS11_OUTPUT_1 PS11_MaskDiffuseVS( VS_INPUT v )
{
   VS11_OUTPUT_1 o = ( VS11_OUTPUT_1 )0;

   // position ( projected )
   o.Pos      = mul( float4( v.Pos, 1 ), mFinal );

   // texcoords
   o.uv0Diff	= v.Tex0;
   o.maskCoords = v.Tex0;

   // fog term
   //o.fog      = VertexFog( o.Pos.z, g_FogTerm );
   o.fog 	    = ViewLayeredFog( o.Pos.z, g_fogPlane, v.Pos, g_FogTerm );   

   // calculate view vector in object space
   float3 objectViewDir  = objectViewPos - v.Pos;
                           
	// calculate environment map texcoords
	float3 objectEnvCoords	= normalize( reflect( -objectViewDir, v.Normal ) ) + v.Pos / g_CubemapRadius;
	o.EnvCoords				= normalize( mul( objectEnvCoords, mWorld ) ).xzy;

   return o;
}

//
float4 PS11_MaskDiffusePS( VS11_OUTPUT_1 i ): COLOR
{	
	// fetch diffuse color + specular factor from texture
	float4 texDiffuse   = tex2D( DiffSampler, i.uv0Diff );
	
	// Get mask coeffs
	float4 maskCoeffs	 = tex2D( MaskSampler, i.maskCoords );
	
	// Get mask color
	float4 mask		 = mul( maskCoeffs, ( float3x4 ) maskColors );
	
	// fetch environement texture
	float4 tEM          = texCUBE( EnvSampler, i.EnvCoords.xyz );
	
	// calculate diffuse color
	float4 diff = texDiffuse + float4( mask.xyz, 1.f );
        //float4 diff = texDiffuse + mask;

	// Resulting color
	return lerp( diff, tEM, texDiffuse.a );
}

// vertex shader output structure ( for ps_1_1 ) second pass
struct VS11_OUTPUT_2
{
   float4 Pos			: POSITION;
   float2 uv0			: TEXCOORD0;
   float3 Light			: COLOR0;
   float  fog			: FOG;
   float3  transFog		: COLOR1;
};

//
VS11_OUTPUT_2 PS11_BumpVS( VS_INPUT v )
{
   VS11_OUTPUT_2 o = ( VS11_OUTPUT_2 )0;

   // position ( projected )
   o.Pos      = mul( float4( v.Pos, 1 ), mFinal );

   // texcoords
   o.uv0		= v.Tex0;

   // as fog is set in previous pass we define fog transparency 
   o.fog	  = 1.f; 
   //o.transFog = VertexFog( o.Pos.z, g_FogTerm );
   o.transFog = ViewLayeredFog( o.Pos.z, g_fogPlane, v.Pos, g_FogTerm );  

   // light direction, should come already normalized from the engine...
   float3 objectLight  = -DirFromLight;
   
   // calculate binormal
   float3 binormal     = cross( v.Normal, v.Tangent ) * v.Tangent.w; 

   // calculate light vector in tangent space
   float3 tangentLight = toTangentSpace( objectLight, v.Tangent, binormal, v.Normal );

   // output light vector in tangent space ( shift to 0.f..1.f because COLOR0 clamps to this interval )
   o.Light.xyz = 0.5f + 0.5f * tangentLight;

   return o;
}

//
float4 PS11_BumpPS( VS11_OUTPUT_2 i, uniform float4 ambient, uniform float4 diffuse ): COLOR
{
   // fetch normal + gloss factor from texture
   float4 texBump      = tex2D( BumpSampler, i.uv0 );

   // normal ( unpack from texture )
   float3 normal       = bx2( texBump );

   // light ( converts back to -1.f..+1.f )
   float3 light        = bx2( i.Light );

   // calculate light attenuation
   float  cosA         = saturate( dot( normal, light ) );

   // resulting color
   return float4( ambient.xyz + cosA * diffuse.xyz, i.transFog.x );
}

//
technique PS11_techique
<
   string Description          = "embm + mask";
   bool   ComputeTangentSpace  = true;
   string VertexFormat         = "VERTEX_XYZNT1T";
   bool   Default              = true;
   bool   UseAlpha             = false;
>
{
   pass DiffuseMask
   {
		VertexShader = compile vs_1_1 PS11_MaskDiffuseVS();
		PixelShader  = compile ps_1_1 PS11_MaskDiffusePS();
      
		AlphaBlendEnable	= False;
		AlphaTestEnable		= False;
   }
   
   pass Bump
   {
		VertexShader = compile vs_1_1 PS11_BumpVS();
		PixelShader  = compile ps_1_1 PS11_BumpPS( g_Ambient, g_Diffuse );
      
		AlphaTestEnable		= True;
		AlphaBlendEnable	= True;
		SrcBlend			= DestColor;
		DestBlend			= Zero;
   }
}

////////////////////////////////////////////////////////////////
// PS_2_0 technique
////////////////////////////////////////////////////////////////

// vertex shader output structure ( for ps_2_0 )
struct VS20_OUTPUT
{
   float4 Pos         : POSITION;   
   float2 uv0         : TEXCOORD0;
   float3 Light       : COLOR0;
   float3 EyeTS       : TEXCOORD3;
   float4 Tangent     : TEXCOORD5;
   float4 Binormal    : TEXCOORD6;
   float4 Normal      : TEXCOORD7;
   float  fog         : FOG;
   
   DECLARE_GLOBAL_LIGHTMAP_SUPPORT( TEXCOORD1 )
   DECLARE_SHADOWMAP_SUPPORT( TEXCOORD2, TEXCOORD4, COLOR1 )
};

//
VS20_OUTPUT PS20_BumpDiffuseSpecularVS( VS_INPUT v )
{
   VS20_OUTPUT o = (VS20_OUTPUT)0;

   // position ( projected )
   o.Pos   = mul( float4( v.Pos, 1 ), mFinal );
   
   // texcoords
   o.uv0   = v.Tex0;
   
   // fog term
   //o.fog   = VertexFog( o.Pos.z, g_FogTerm );
   o.fog 	 = ViewLayeredFog( o.Pos.z, g_fogPlane, v.Pos, g_FogTerm );   

   // light direction should come already normalized from the engine...
   float3 objectLight = -DirFromLight;
   
   // calculate binormal
   float3 binormal = cross( v.Normal, v.Tangent ) * v.Tangent.w; 

   // calculate light vector in tangent space
   float3 tangentLight   = toTangentSpace( objectLight, v.Tangent, binormal, v.Normal );

   // output light vector in tangent space
   o.Light.xyz = 0.5f + 0.5f * tangentLight;

   // calculate view vector in object space
   float3 objectViewDir  = objectViewPos - v.Pos;
                              
   // calculate view vector in tangent space
   float3 tangentViewDir = toTangentSpace( objectViewDir, v.Tangent, binormal, v.Normal );

	// output tangent space basis ( in world space )
   o.Tangent.xzy  = mul( v.Tangent.xyz, mWorld );
   o.Binormal.xzy = mul( binormal.xyz, mWorld );
   o.Normal.xzy   = mul( v.Normal.xyz, mWorld );

   // eye vector ( in object and tangent spaces )
   o.EyeTS = normalize( tangentViewDir );
  
   // calculate cubemap addition
   float3 cma	= v.Pos / g_CubemapRadius;
   cma.xzy  = mul( cma.xyz, mWorld );
   o.Tangent.w	= cma.x;
   o.Binormal.w = cma.y;
   o.Normal.w	= cma.z;
   
   // Global lightmap texcoords
   CALCULATE_SHARED_DATA( v.Pos )
   CALCULATE_GLOBAL_LIGHTMAP_TEXCOORDS( o )
   CALCULATE_SHADOWMAP_DATA_MDL( o )

   return o;
}

//
float4 PS20_BumpDiffuseSpecularPS( VS20_OUTPUT i, uniform float4 ambient, uniform float4 diffuse ): COLOR
{
	// Get global lightmap
	GET_GLOBAL_LIGHTMAP( i )
	CALCULATE_SHADOW_MDL( i )

   // fetch normal + gloss factor from texture
   float4   texBump			= tex2D( BumpSampler, i.uv0 );

   // fetch diffuse color + specular factor from texture
   float4 texDiffuse		= tex2D( DiffSampler, i.uv0 );

   // normal ( unpack from texture )
   float3  normal			= bx2( texBump );

   // light vector ( tangent space )
   float3 Light				= normalize( bx2( i.Light ) );

   // calculate light attenuation
   float cosA				= saturate( dot( normal, Light ) );

   // calculate reflected light
   float3 reflectedLight	= 2.f * cosA * normal - Light;
   
   // cosine between reflected light and eye vector
   float  cosB				= saturate( dot( i.EyeTS, reflectedLight ) );      

   // calculate specular color
   float3 Spec				= pow( cosB, 4 ) * texBump.a * cosA * g_Specular;   

   // Calculate reflected view vector
   float3 cma				 = float3( i.Tangent.w, i.Binormal.w, i.Normal.w );
   float3 reflectedView		 = reflect( -i.EyeTS, normal );
   float3 worldReflectedView = mul( reflectedView, float3x3( i.Tangent.xyz, i.Binormal.xyz, i.Normal.xyz ) ) + cma; 
   
   // fetch environement color
   float3 tEM				= texCUBE( EnvSampler, worldReflectedView );

   // Get mask coeffs
   float3 maskCoeffs		= tex2D( MaskSampler, i.uv0 );
   
   // Get mask color
   float3 mask				= mul( maskCoeffs, ( float3x3 ) maskColors );
   
   // Get color
   float3 color				= lerp( texDiffuse + mask, tEM, texDiffuse.a ) * CALCULATE_ATTENUATION( cosA * diffuse, ambient );
   
   // resulting color
   return float4( color + Spec, g_Transparency ); 
}

//
technique PS20_technique
<
   string Description          = "embm + mask ( PS20 )";
   bool   ComputeTangentSpace  = true;
   string VertexFormat         = "VERTEX_XYZNT1T";
   bool   Default              = true;
   bool   IsPs20               = true;
   bool   UseAlpha             = false;
   string UserParams		   = "MASK_COLORS MATRIX";
>
{
   pass p0
   {
      VertexShader   = compile vs_2_0 PS20_BumpDiffuseSpecularVS();
      PixelShader    = compile ps_2_0 PS20_BumpDiffuseSpecularPS( g_Ambient, g_Diffuse );
   }
}