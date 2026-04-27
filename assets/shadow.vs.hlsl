// =============================================================================
// KROM Engine — assets/shadow.vs.hlsl
// Vertex Shader: Shadow-Pass (Depth-Only / Alpha-Cutout)
// Varianten:
//   default          — kein UV-Output, kein PS nötig (solid geometry)
//   KROM_ALPHA_TEST  — UV-Output für shadow.ps.hlsl Alpha-Discard (Blätter, Gras, …)
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

VSOutput main(VSInput IN)
{
    VSOutput OUT;
    float4 posWS   = KromObjectPositionWS(IN.position);
    OUT.positionCS = mul(shadowViewProj, posWS);
#ifdef KROM_ALPHA_TEST
    OUT.texCoord   = IN.texCoord;
#endif
    return OUT;
}
