// =============================================================================
// KROM Engine — assets/skinned_lit.vs.hlsl
// Skinned Vertex Shader fuer Lit-Pipeline (kein Tangent, kein UV).
// Kompatibel mit lit.ps.hlsl.
// BonePalette: BufSRVSlots::BonePalette = 13
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

StructuredBuffer<float4x4> g_bonePalette : register(t13);

#ifdef __spirv__
#define VK_LOC(n) [[vk::location(n)]]
#else
#define VK_LOC(n)
#endif

struct VSInput
{
    VK_LOC(0) float3 position   : POSITION;
    VK_LOC(1) float3 normal     : NORMAL;
    VK_LOC(7) float4 boneWeight : BLENDWEIGHT;
    VK_LOC(8) uint4  boneIndex  : BLENDINDICES;
};

struct VSOutput
{
    float4 positionCS      : SV_POSITION;
    float3 positionWS      : TEXCOORD1;
    float3 normalWS        : TEXCOORD2;
    float2 texCoord        : TEXCOORD0;
    float4 positionLightCS : TEXCOORD3;
};

float4 SkinPosition(float4 localPos, uint4 idx, float4 w)
{
    return w.x * mul(g_bonePalette[idx.x], localPos)
         + w.y * mul(g_bonePalette[idx.y], localPos)
         + w.z * mul(g_bonePalette[idx.z], localPos)
         + w.w * mul(g_bonePalette[idx.w], localPos);
}

float3 SkinNormal(float3 n, uint4 idx, float4 w)
{
    return w.x * mul((float3x3)g_bonePalette[idx.x], n)
         + w.y * mul((float3x3)g_bonePalette[idx.y], n)
         + w.z * mul((float3x3)g_bonePalette[idx.z], n)
         + w.w * mul((float3x3)g_bonePalette[idx.w], n);
}

VSOutput main(VSInput IN)
{
    VSOutput OUT;
    const float4 skinnedPos  = SkinPosition(float4(IN.position, 1.0f), IN.boneIndex, IN.boneWeight);
    const float3 skinnedNorm = normalize(SkinNormal(IN.normal, IN.boneIndex, IN.boneWeight));

    const float4 posWS  = KromObjectPositionWS(skinnedPos.xyz);
    OUT.positionCS      = mul(viewProjMatrix, posWS);
    OUT.positionWS      = posWS.xyz;
    OUT.normalWS        = normalize(KromObjectNormalWS(skinnedNorm));
    OUT.texCoord        = float2(0.0f, 0.0f);
    OUT.positionLightCS = mul(shadowViewProj, posWS);
    return OUT;
}
