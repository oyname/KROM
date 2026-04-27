#ifndef KROM_PER_OBJECT_BINDING_HLSL
#define KROM_PER_OBJECT_BINDING_HLSL

#ifdef KROM_VULKAN_PUSH_PER_OBJECT
struct KromPerObjectPushConstantsData
{
    float4 worldRow0;
    float4 worldRow1;
    float4 worldRow2;
    float4 worldInvTRow0;
    float4 worldInvTRow1;
    float4 worldInvTRow2;
    float4 entityId;
};

[[vk::push_constant]]
KromPerObjectPushConstantsData g_kromPerObjectPush;

float4 KromObjectPositionWS(float3 localPosition)
{
    const float4 p = float4(localPosition, 1.0f);
    return float4(dot(g_kromPerObjectPush.worldRow0, p),
                  dot(g_kromPerObjectPush.worldRow1, p),
                  dot(g_kromPerObjectPush.worldRow2, p),
                  1.0f);
}

float3 KromObjectNormalWS(float3 localNormal)
{
    const float4 n = float4(localNormal, 0.0f);
    return float3(dot(g_kromPerObjectPush.worldInvTRow0, n),
                  dot(g_kromPerObjectPush.worldInvTRow1, n),
                  dot(g_kromPerObjectPush.worldInvTRow2, n));
}

float3 KromObjectTangentWS(float3 localTangent)
{
    const float4 t = float4(localTangent, 0.0f);
    return float3(dot(g_kromPerObjectPush.worldRow0, t),
                  dot(g_kromPerObjectPush.worldRow1, t),
                  dot(g_kromPerObjectPush.worldRow2, t));
}

float4 KromObjectEntityId()
{
    return g_kromPerObjectPush.entityId;
}
#else
cbuffer PerObject : register(b1)
{
    float4x4 worldMatrix;
    float4x4 worldMatrixInvT;
    float4 entityId;
};

float4 KromObjectPositionWS(float3 localPosition)
{
    return mul(worldMatrix, float4(localPosition, 1.0f));
}

float3 KromObjectNormalWS(float3 localNormal)
{
    return mul((float3x3)worldMatrixInvT, localNormal);
}

float3 KromObjectTangentWS(float3 localTangent)
{
    return mul((float3x3)worldMatrix, localTangent);
}

float4 KromObjectEntityId()
{
    return entityId;
}
#endif

#endif
