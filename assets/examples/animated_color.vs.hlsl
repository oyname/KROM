// =============================================================================
// KROM Engine — assets/examples/animated_color.vs.hlsl
// Vertex Shader fuer den animierten Farb-Beispiel-Shader.
//
// Identisch zum Standard-Unlit-VS — transformiert Position, Normal, UV.
// Die eigentliche Logik ist im Fragment-Shader (animated_color.ps.hlsl).
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
    float4       timeParams;   // x=Zeit(s), y=DeltaTime, z=FrameCount, w=0
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

struct VSInput
{
    VK_LOC(0) float3 position : POSITION;
    VK_LOC(1) float3 normal   : NORMAL;
    VK_LOC(4) float2 texCoord : TEXCOORD0;
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
#ifdef KROM_INSTANCED
    float4 posWS   = KromObjectPositionWS(IN.position, IN.instanceId);
    OUT.normalWS   = normalize(KromObjectNormalWS(IN.normal, IN.instanceId));
#else
    float4 posWS   = KromObjectPositionWS(IN.position);
    OUT.normalWS   = normalize(KromObjectNormalWS(IN.normal));
#endif
    OUT.positionCS = mul(viewProjMatrix, posWS);
    OUT.positionWS = posWS.xyz;
    OUT.texCoord   = IN.texCoord;
    return OUT;
}
