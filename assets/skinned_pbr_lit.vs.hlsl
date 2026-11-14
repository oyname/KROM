// =============================================================================
// KROM Engine — assets/skinned_pbr_lit.vs.hlsl
// Vertex Shader: skinned PBR-Lit (HLSL). Same outputs as pbr_lit.vs.hlsl, so it
// pairs with pbr_lit.ps.hlsl / pbr_forward_plus.ps.hlsl. Vertices are skinned
// with the bone palette before the object transform.
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
    float4       featureParams[8]; // add-on parameter blocks (FrameConstants::featureParams)
};

#include "per_object_binding.hlsl"

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

// Bone palette written by SkinningSystem (BufSRVSlots::BonePalette = 13).
KROM_BUFFER_SRV(StructuredBuffer<float4x4>, g_bonePalette, 13);

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

struct VSInput
{
    VK_LOC(0) float3 position : POSITION;
    VK_LOC(1) float3 normal   : NORMAL;
    VK_LOC(2) float4 tangent  : TANGENT;
    VK_LOC(4) float2 texCoord : TEXCOORD0;
    VK_LOC(7) float4 boneWeight : BLENDWEIGHT;
    VK_LOC(8) uint4  boneIndex  : BLENDINDICES;
#ifdef KROM_INSTANCED
    uint instanceId : SV_InstanceID;
#endif
};

struct VSOutput
{
    float4 positionCS     : SV_POSITION;
    float3 positionWS     : TEXCOORD1;
    float3 normalWS       : TEXCOORD2;
    float4 tangentWS      : TEXCOORD3;
    float2 texCoord       : TEXCOORD0;
    float3 scaledLocalPosition : TEXCOORD4;
    float2 meshUV : TEXCOORD5;
};

float3 SafeNormalizeVS(float3 v)
{
    float len2 = dot(v, v);
    return (len2 > 1e-12f) ? (v * rsqrt(len2)) : float3(1.0f, 0.0f, 0.0f);
}

VSOutput main(VSInput IN)
{
    VSOutput OUT;
    const float3 skinnedPos = SkinPosition(float4(IN.position, 1.0f), IN.boneIndex, IN.boneWeight).xyz;
    const float3 skinnedNrm = SkinDirection(IN.normal, IN.boneIndex, IN.boneWeight);
    const float3 skinnedTan = SkinDirection(IN.tangent.xyz, IN.boneIndex, IN.boneWeight);
#ifdef KROM_INSTANCED
    const uint objectIndex = IN.instanceId;
    float4 posWS   = KromObjectPositionWS(skinnedPos, objectIndex);
    float3 N = SafeNormalizeVS(KromObjectNormalWS(skinnedNrm, objectIndex));
    float3 T = SafeNormalizeVS(KromObjectTangentWS(skinnedTan, objectIndex));
#else
    float4 posWS   = KromObjectPositionWS(skinnedPos);
    float3 N = SafeNormalizeVS(KromObjectNormalWS(skinnedNrm));
    float3 T = SafeNormalizeVS(KromObjectTangentWS(skinnedTan));
#endif
    T = SafeNormalizeVS(T - N * dot(N, T));
    OUT.positionCS      = mul(viewProjMatrix, posWS);
    OUT.positionWS      = posWS.xyz;
    OUT.normalWS        = N;
    OUT.tangentWS       = float4(T, IN.tangent.w);
    OUT.texCoord        = IN.texCoord * uvScale + uvOffset;
    OUT.meshUV          = IN.texCoord;
#ifdef KROM_INSTANCED
    OUT.scaledLocalPosition = boxTileSize > 0.0f ? KromScaledLocalPosition(IN.position, IN.instanceId) : 0.0f;
#else
    OUT.scaledLocalPosition = boxTileSize > 0.0f ? KromScaledLocalPosition(IN.position) : 0.0f;
#endif
    return OUT;
}
