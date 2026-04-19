// =============================================================================
// KROM Engine — assets/passthrough.ps.hlsl
// Passthrough-Tonemap: sampelt t8 (HDR-Eingabe, PassSRV0) direkt aus.
// Targets: DX11 (DXBC), DX12 (DXIL), Vulkan (SPIR-V via DXC -spirv)
// RenderSystem bindet hdrSceneColor vor dem Draw an Slot 8 (PassSRV0).
// =============================================================================

Texture2D    uHDRInput : register(t8);
SamplerState uSampler  : register(s1); // LinearClamp

struct PSInput
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

float4 main(PSInput IN) : SV_TARGET
{
    float3 hdr = uHDRInput.Sample(uSampler, IN.uv).rgb;
    hdr = hdr / (hdr + 1.0f);
    return float4(hdr, 1.0f);
}
