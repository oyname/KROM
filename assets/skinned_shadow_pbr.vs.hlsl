// =============================================================================
// KROM Engine — assets/skinned_shadow_pbr.vs.hlsl
// Vertex Shader: shadow pass for skinned PBR materials (shadow follows the pose).
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

// Bone palette written by SkinningSystem (BufSRVSlots::BonePalette = 13).
// Vulkan: buffer SRVs bind at 64 + slot (BonePalette = 13 -> 77), like per_object_binding.hlsl.
#ifdef __spirv__
[[vk::binding(77)]]
#endif
StructuredBuffer<float4x4> g_bonePalette : register(t13);

// Indices outside the bound palette (no palette: DX11 null SRV reports 0 entries,
// or a rig with more bones than uploaded) keep the vertex in its bind pose.
bool KromBonesValid(uint4 idx)
{
    uint count = 0u;
    uint stride = 0u;
    g_bonePalette.GetDimensions(count, stride);
    return all(idx < uint4(count, count, count, count));
}

float4 SkinPosition(float4 localPos, uint4 idx, float4 w)
{
    if (!KromBonesValid(idx))
        return localPos;
    return w.x * mul(g_bonePalette[idx.x], localPos)
         + w.y * mul(g_bonePalette[idx.y], localPos)
         + w.z * mul(g_bonePalette[idx.z], localPos)
         + w.w * mul(g_bonePalette[idx.w], localPos);
}

#ifdef __spirv__
#define VK_LOC(n) [[vk::location(n)]]
#else
#define VK_LOC(n)
#endif

struct VSInput
{
    VK_LOC(0) float3 position : POSITION;
    VK_LOC(7) float4 boneWeight : BLENDWEIGHT;
    VK_LOC(8) uint4  boneIndex  : BLENDINDICES;
#ifdef KROM_INSTANCED
    uint instanceId : SV_InstanceID;
#endif
#ifdef KROM_ALPHA_TEST
    VK_LOC(4) float2 texCoord : TEXCOORD0;
#endif
};

struct VSOutput
{
    float4 positionCS : SV_POSITION;
#ifdef KROM_ALPHA_TEST
    float2 texCoord   : TEXCOORD0;
    float3 scaledLocalPosition : TEXCOORD1;
    float2 meshUV : TEXCOORD2;
    float3 positionWS : TEXCOORD3;
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
    float  boxTileSize;
};
#endif

VSOutput main(VSInput IN)
{
    VSOutput OUT;
    const float3 skinnedPos = SkinPosition(float4(IN.position, 1.0f), IN.boneIndex, IN.boneWeight).xyz;
#ifdef KROM_INSTANCED
    float4 posWS   = KromObjectPositionWS(skinnedPos, IN.instanceId);
#else
    float4 posWS   = KromObjectPositionWS(skinnedPos);
#endif
    OUT.positionCS = mul(viewProjMatrix, posWS); // Shadow-Pass: viewProjMatrix = Licht-VP der Atlas-View
#ifdef KROM_ALPHA_TEST
    OUT.texCoord   = IN.texCoord * uvScale + uvOffset;
    OUT.meshUV = IN.texCoord;
    OUT.positionWS = posWS.xyz;
#ifdef KROM_INSTANCED
    OUT.scaledLocalPosition = boxTileSize > 0.0f ? KromScaledLocalPosition(IN.position, IN.instanceId) : 0.0f;
#else
    OUT.scaledLocalPosition = boxTileSize > 0.0f ? KromScaledLocalPosition(IN.position) : 0.0f;
#endif
#endif
    return OUT;
}
