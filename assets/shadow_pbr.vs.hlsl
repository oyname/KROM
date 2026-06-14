// =============================================================================
// KROM Engine — assets/shadow_pbr.vs.hlsl
// Vertex Shader: Shadow-Pass für PBR-Materialien
// Liest uvScale/uvOffset aus dem PBR-PerMaterial-CB und transformiert die UV,
// damit der Alpha-Discard im shadow_pbr.ps.hlsl mit dem sichtbaren Rendering
// übereinstimmt (z.B. bei gekachelten Texturen).
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
#ifdef KROM_ALPHA_TEST
    VK_LOC(4) float2 texCoord : TEXCOORD0;
#endif
};

struct VSOutput
{
    float4 positionCS : SV_POSITION;
#ifdef KROM_ALPHA_TEST
    float2 texCoord   : TEXCOORD0;
#endif
};

#ifdef KROM_ALPHA_TEST
cbuffer PerMaterial : register(b2)
{
    float4 baseColorFactor;
    float4 emissiveFactor;
    float  metallicFactor;
    float  roughnessFactor;
    float  normalStrength;
    float  occlusionStrength;
    float  opacityFactor;
    float  alphaCutoff;
    int    materialFeatureMask;
    float  materialModel;
    int    occlusionChannel;
    int    roughnessChannel;
    int    metallicChannel;
    float  occlusionBias;
    float  roughnessBias;
    float  metallicBias;
    float2 uvScale;
    float2 uvOffset;
};
#endif

VSOutput main(VSInput IN)
{
    VSOutput OUT;
    float4 posWS   = KromObjectPositionWS(IN.position);
    OUT.positionCS = mul(shadowViewProj, posWS);
#ifdef KROM_ALPHA_TEST
    OUT.texCoord   = IN.texCoord * uvScale + uvOffset;
#endif
    return OUT;
}
