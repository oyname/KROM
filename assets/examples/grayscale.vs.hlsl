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

#ifdef KROM_VULKAN_PUSH_PER_OBJECT
struct KromPerObjectPushConstantsData {
    float4 worldRow0; float4 worldRow1; float4 worldRow2;
    float4 worldInvTRow0; float4 worldInvTRow1; float4 worldInvTRow2; float4 entityId;
};
[[vk::push_constant]] KromPerObjectPushConstantsData g_kromPerObjectPush;
float4 KromObjectPositionWS(float3 p) {
    float4 lp = float4(p, 1.0f);
    return float4(dot(g_kromPerObjectPush.worldRow0, lp), dot(g_kromPerObjectPush.worldRow1, lp),
                  dot(g_kromPerObjectPush.worldRow2, lp), 1.0f);
}
float3 KromObjectNormalWS(float3 n) {
    float4 ln = float4(n, 0.0f);
    return float3(dot(g_kromPerObjectPush.worldInvTRow0, ln), dot(g_kromPerObjectPush.worldInvTRow1, ln),
                  dot(g_kromPerObjectPush.worldInvTRow2, ln));
}
#else
cbuffer PerObject : register(b1) { float4x4 worldMatrix; float4x4 worldMatrixInvT; float4 entityId; };
float4 KromObjectPositionWS(float3 p) { return mul(worldMatrix, float4(p, 1.0f)); }
float3 KromObjectNormalWS(float3 n)   { return mul((float3x3)worldMatrixInvT, n); }
#endif

#ifdef __spirv__
#define VK_LOC(n) [[vk::location(n)]]
#else
#define VK_LOC(n)
#endif

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
