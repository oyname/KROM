// =============================================================================
// KROM Engine — assets/triangle_color.ps.hlsl
// Pixel Shader: Dreieck mit Vertex-Farben (HLSL)
// Targets: DX11 (DXBC), DX12 (DXIL), Vulkan (SPIR-V via DXC -spirv)
// =============================================================================

struct PSInput
{
    float4 positionCS : SV_POSITION;
    float4 color      : COLOR;
};

float4 main(PSInput IN) : SV_TARGET
{
    return IN.color;
}
