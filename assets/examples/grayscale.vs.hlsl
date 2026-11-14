// =============================================================================
// KROM Engine — assets/examples/grayscale.vs.hlsl
// Standard Vertex Shader — unveraendert, Logik ist im Fragment Shader.
// =============================================================================

struct GpuLightData { float4 p; float4 d; float4 c; float4 x; };

cbuffer PerFrame : register(b0)
{
    float4x4 viewMatrix; float4x4 projMatrix; float4x4 viewProjMatrix;
    float4x4 invViewProjMatrix; float4 cameraPositionWS; float4 cameraForwardWS;
    float4 screenSize; float4 timeParams; float4 ambientColor;
    uint lightCount; uint shadowCascadeCount; float nearPlane; float farPlane;
    GpuLightData lights[7]; float4x4 shadowViewProj;
    float iblPrefilterLevels; float shadowBias; float shadowNormalBias; float shadowStrength;
};

#include "per_object_binding.hlsl"

struct VSInput  { VK_LOC(0) float3 position : POSITION; VK_LOC(1) float3 normal : NORMAL; VK_LOC(4) float2 texCoord : TEXCOORD0; };
struct VSOutput { float4 positionCS : SV_POSITION; float3 positionWS : TEXCOORD1; float3 normalWS : TEXCOORD2; float2 texCoord : TEXCOORD0; };

VSOutput main(VSInput IN)
{
    VSOutput OUT;
    float4 posWS   = KromObjectPositionWS(IN.position);
    OUT.positionCS = mul(viewProjMatrix, posWS);
    OUT.positionWS = posWS.xyz;
    OUT.normalWS   = normalize(KromObjectNormalWS(IN.normal));
    OUT.texCoord   = IN.texCoord;
    return OUT;
}
