// =============================================================================
// KROM Engine — assets/passthrough.dx11.ps.hlsl
// Passthrough-Tonemap: sampelt t0 (HDR-Eingabe) direkt aus.
// Kein echter Tonemap-Operator — für Beispiele ausreichend.
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
    // Einfacher Reinhard-Tonemap damit HDR-Werte > 1 sichtbar bleiben
    hdr = hdr / (hdr + 1.0f);
    return float4(hdr, 1.0f);
}
