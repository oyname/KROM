// =============================================================================
// KROM Engine — assets/skinned.vs.hlsl
// Vertex Shader: Skeletal Skinning (HLSL)
// Targets: DX11 (DXBC), DX12 (DXIL), Vulkan (SPIR-V via DXC -spirv)
//
// Erwartet VertexLayout: Position + Normal + Tangent + UV + BoneWeight + BoneIndex
// Bone-Palette: StructuredBuffer<float4x4> an register(t13) = BufSRVSlots::BonePalette
// =============================================================================

cbuffer PerFrame : register(b0)
{
    float4x4 viewMatrix;
    float4x4 projMatrix;
    float4x4 viewProjMatrix;
    float4x4 invViewProjMatrix;
    float4   cameraPositionWS;
    float4   cameraForwardWS;
    float4   screenSize;
    float4   timeParams;
    float4   ambientColor;
    uint     lightCount;
    uint     shadowEnabled;
    float    nearPlane;
    float    farPlane;
};

#include "per_object_binding.hlsl"

// Bone-Palette: eine Mat4 pro Knochen (skinningMatrix = world * inverseBindMatrix)
// BufSRVSlots::BonePalette = 13 (VS-only; kein Konflikt mit LightBuffer=10 der nur FS/CS gebunden wird)
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
    VK_LOC(2) float4 tangent    : TANGENT;
    VK_LOC(4) float2 texCoord   : TEXCOORD0;
    VK_LOC(7) float4 boneWeight : BLENDWEIGHT;
    VK_LOC(8) uint4  boneIndex  : BLENDINDICES;
};

struct VSOutput
{
    float4 positionCS  : SV_POSITION;
    float3 positionWS  : TEXCOORD1;
    float3 normalWS    : TEXCOORD2;
    float2 texCoord    : TEXCOORD0;
};

// Wendet bis zu 4 gewichtete Knochen-Matrizen auf eine Position an.
float4 SkinPosition(float4 localPos, uint4 indices, float4 weights)
{
    float4 result = float4(0.0f, 0.0f, 0.0f, 0.0f);
    result += weights.x * mul(g_bonePalette[indices.x], localPos);
    result += weights.y * mul(g_bonePalette[indices.y], localPos);
    result += weights.z * mul(g_bonePalette[indices.z], localPos);
    result += weights.w * mul(g_bonePalette[indices.w], localPos);
    return result;
}

float3 SkinNormal(float3 localNormal, uint4 indices, float4 weights)
{
    float3 result = float3(0.0f, 0.0f, 0.0f);
    result += weights.x * mul((float3x3)g_bonePalette[indices.x], localNormal);
    result += weights.y * mul((float3x3)g_bonePalette[indices.y], localNormal);
    result += weights.z * mul((float3x3)g_bonePalette[indices.z], localNormal);
    result += weights.w * mul((float3x3)g_bonePalette[indices.w], localNormal);
    return result;
}

VSOutput main(VSInput IN)
{
    VSOutput OUT;

    const float4 localPos    = float4(IN.position, 1.0f);
    const float4 skinnedPos  = SkinPosition(localPos, IN.boneIndex, IN.boneWeight);
    const float3 skinnedNorm = normalize(SkinNormal(IN.normal, IN.boneIndex, IN.boneWeight));

    // PerObject-Matrix auf bereits geskintete World-Position anwenden
    // (bei Skinning ist PerObject typischerweise Identity, aber die Pipeline bleibt kompatibel)
    const float4 posWS  = KromObjectPositionWS(skinnedPos.xyz);
    OUT.positionCS      = mul(viewProjMatrix, posWS);
    OUT.positionWS      = posWS.xyz;
    OUT.normalWS        = normalize(KromObjectNormalWS(skinnedNorm));
    OUT.texCoord        = IN.texCoord;

    return OUT;
}
