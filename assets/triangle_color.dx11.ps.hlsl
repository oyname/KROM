// =============================================================================
// KROM Engine — assets/triangle_color.dx11.ps.hlsl
// Pixel Shader: Dreieck mit Vertex-Farben (DX11 / HLSL SM 5.0)
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
