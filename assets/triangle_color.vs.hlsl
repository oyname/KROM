// =============================================================================
// KROM Engine — assets/triangle_color.vs.hlsl
// Vertex Shader: Dreieck mit Vertex-Farben (HLSL)
// Targets: DX11 (DXBC), DX12 (DXIL), Vulkan (SPIR-V via DXC -spirv)
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

cbuffer PerObject : register(b1)
{
    float4x4 worldMatrix;
    float4x4 worldMatrixInvT;
    float4   entityId;
};

#ifdef __spirv__
#define VK_LOC(n) [[vk::location(n)]]
#else
#define VK_LOC(n)
#endif

struct VSInput
{
    VK_LOC(0) float3 position : POSITION;
    VK_LOC(6) float4 color    : COLOR;
};

struct VSOutput
{
    float4 positionCS : SV_POSITION;
    float4 color      : COLOR;
};

VSOutput main(VSInput IN)
{
    VSOutput OUT;
    float4 posWS   = mul(worldMatrix, float4(IN.position, 1.0));
    OUT.positionCS = mul(viewProjMatrix, posWS);
    OUT.color      = IN.color;
    return OUT;
}
