// =============================================================================
// KROM Engine — assets/skinned_unlit.vs.hlsl (skinned variant of quad_unlit.vs.hlsl)
// Vertex Shader: Unlit Quad (HLSL)
// Targets: DX11 (DXBC), DX12 (DXIL), Vulkan (SPIR-V via DXC -spirv)
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
    float4       timeParams;
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
};

#include "per_object_binding.hlsl"

cbuffer PerMaterial : register(b2)
{
    float4 baseColorFactor;
    float4 emissiveFactor;
    float  metallicFactor;
    float  roughnessFactor;
    float  occlusionStrength;
    float  opacityFactor;
    float  alphaCutoff;
    int    materialFeatureMask;
    float  materialModel;
    float  _pad0;
};

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

float3 SkinDirection(float3 v, uint4 idx, float4 w)
{
    if (!KromBonesValid(idx))
        return v;
    return w.x * mul((float3x3)g_bonePalette[idx.x], v)
         + w.y * mul((float3x3)g_bonePalette[idx.y], v)
         + w.z * mul((float3x3)g_bonePalette[idx.z], v)
         + w.w * mul((float3x3)g_bonePalette[idx.w], v);
}

#ifdef __spirv__
#define VK_LOC(n) [[vk::location(n)]]
#else
#define VK_LOC(n)
#endif

struct VSInput
{
    VK_LOC(0) float3 position : POSITION;
    VK_LOC(1) float3 normal   : NORMAL;
    VK_LOC(4) float2 texCoord : TEXCOORD0;
    VK_LOC(7) float4 boneWeight : BLENDWEIGHT;
    VK_LOC(8) uint4  boneIndex  : BLENDINDICES;
#ifdef KROM_INSTANCED
    uint instanceId : SV_InstanceID;
#endif
};

struct VSOutput
{
    float4 positionCS : SV_POSITION;
    float3 positionWS : TEXCOORD1;
    float3 normalWS   : TEXCOORD2;
    float2 texCoord   : TEXCOORD0;
};

VSOutput main(VSInput IN)
{
    VSOutput OUT;
    const float3 skinnedPos = SkinPosition(float4(IN.position, 1.0f), IN.boneIndex, IN.boneWeight).xyz;
    const float3 skinnedNrm = SkinDirection(IN.normal, IN.boneIndex, IN.boneWeight);
#ifdef KROM_INSTANCED
    const uint objectIndex = IN.instanceId;
    float4 posWS   = KromObjectPositionWS(skinnedPos, objectIndex);
    OUT.normalWS   = normalize(KromObjectNormalWS(skinnedNrm, objectIndex));
#else
    float4 posWS   = KromObjectPositionWS(skinnedPos);
    OUT.normalWS   = normalize(KromObjectNormalWS(skinnedNrm));
#endif
    OUT.positionCS = mul(viewProjMatrix, posWS);
    OUT.positionWS = posWS.xyz;
    OUT.texCoord   = IN.texCoord;
    return OUT;
}
