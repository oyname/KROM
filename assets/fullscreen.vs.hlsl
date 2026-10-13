// =============================================================================
// KROM Engine — assets/fullscreen.vs.hlsl
// Fullscreen-Dreieck aus SV_VertexID — kein Vertex-Buffer noetig.
// Targets: DX11 (DXBC), DX12 (DXIL), Vulkan (SPIR-V via DXC -spirv)
// Draw(3, 1, 0, 0) genuegt.
// =============================================================================

struct VSOutput
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

VSOutput main(uint id : SV_VertexID)
{
    VSOutput o;

    if (id == 0u)
    {
        o.pos = float4(-1.0f,  1.0f, 0.0f, 1.0f);
        o.uv  = float2(0.0f, 0.0f);
    }
    else if (id == 1u)
    {
        o.pos = float4(-1.0f, -3.0f, 0.0f, 1.0f);
        o.uv  = float2(0.0f, 2.0f);
    }
    else
    {
        o.pos = float4( 3.0f,  1.0f, 0.0f, 1.0f);
        o.uv  = float2(2.0f, 0.0f);
    }

    return o;
}
