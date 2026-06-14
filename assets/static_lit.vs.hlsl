// =============================================================================
// KROM Engine — assets/static_lit.vs.hlsl
// Vertex Shader: nur Position + Normal, kein UV, kein Tangent.
// Kompatibel mit lit.ps.hlsl (texCoord wird als 0,0 ausgegeben).
// =============================================================================

struct GpuLightData
{
    float4 positionWS;
    float4 directionWS;
    float4 colorIntensity;
    float4 params;
};

cbuffer PerFrame : register(b0)
{
    float4x4     viewMatrix;
    float4x4     projMatrix;
    float4x4     viewProjMatrix;
    float4x4     invViewProjMatrix;
    float4       cameraPositionWS;
    float4       cameraForwardWS;
    float4       screenSize;
    float4       timeData;
    float4       ambientColor;
    uint         lightCount;
    uint         shadowCascadeCount;
    float        nearPlane;
    float        farPlane;
    GpuLightData lights[7];
    float4x4     shadowViewProj;
    float        iblPrefilterLevels;
    float        shadowBias;
    float        shadowNormalBias;
    float        shadowStrength;
    float        shadowTexelSize;
    uint         debugFlags;
    uint         shadowFilterMode;
    float        _shadowPad;
    float4       shadowLightMeta[4];
    float4       shadowLightExtra[4];
    float4       shadowViewRect[16];
    float4x4     shadowViewProjArray[16];
    uint         shadowLightCount;
    uint         shadowViewCount;
    float2       _shadowArrayPad;
};

#include "per_object_binding.hlsl"

#ifdef __spirv__
#define VK_LOC(n) [[vk::location(n)]]
#else
#define VK_LOC(n)
#endif

struct VSInput
{
    VK_LOC(0) float3 position : POSITION;
    VK_LOC(1) float3 normal   : NORMAL;
};

struct VSOutput
{
    float4 positionCS      : SV_POSITION;
    float3 positionWS      : TEXCOORD1;
    float3 normalWS        : TEXCOORD2;
    float2 texCoord        : TEXCOORD0;
    float4 positionLightCS : TEXCOORD3;
};

VSOutput main(VSInput IN)
{
    VSOutput OUT;
    float4 posWS        = KromObjectPositionWS(IN.position);
    OUT.positionCS      = mul(viewProjMatrix, posWS);
    OUT.positionWS      = posWS.xyz;
    OUT.normalWS        = normalize(KromObjectNormalWS(IN.normal));
    OUT.texCoord        = float2(0.0f, 0.0f);
    OUT.positionLightCS = mul(shadowViewProj, posWS);
    return OUT;
}
