// =============================================================================
// KROM Engine — assets/lit.vs.hlsl
// Vertex Shader: Lit (HLSL)
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
    float4       shadowLightMeta[4];
    float4       shadowLightExtra[4];
    float4       shadowViewRect[16];
    float4x4     shadowViewProjArray[16];
    uint         shadowLightCount;
    uint         shadowViewCount;
    float2       _shadowArrayPad;
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
};

struct VSOutput
{
    float4 positionCS     : SV_POSITION;
    float3 positionWS     : TEXCOORD1;
    float3 normalWS       : TEXCOORD2;
    float2 texCoord       : TEXCOORD0;
    float4 positionLightCS: TEXCOORD3;  // Licht-Clip-Raum für Shadow-Sampling
};

VSOutput main(VSInput IN)
{
    VSOutput OUT;
    float4 posWS    = KromObjectPositionWS(IN.position);
    OUT.positionCS  = mul(viewProjMatrix, posWS);
    OUT.positionWS  = posWS.xyz;
    OUT.normalWS    = normalize(KromObjectNormalWS(IN.normal));
    OUT.texCoord    = IN.texCoord;
    OUT.positionLightCS = mul(shadowViewProj, posWS);
    return OUT;
}
