// =============================================================================
// KROM Engine — assets/shadow.ps.hlsl
// Pixel Shader: Shadow-Pass
// Varianten:
//   default          — void (kein Output, nur Depth-Write durch Rasterizer)
//   KROM_ALPHA_TEST  — Albedo samplen, discard wenn Opacity < alphaCutoff
//                      Für transparente Geometrie: Blätter, Gitter, Gras, …
// Targets: DX11 (DXBC), DX12 (DXIL), Vulkan (SPIR-V via DXC -spirv)
// =============================================================================

#ifdef KROM_ALPHA_TEST

Texture2D    albedo  : register(t0);
SamplerState sLinear : register(s0);

cbuffer PerMaterial : register(b2)
{
    float4 baseColorFactor;
    float4 emissiveFactor;
    float  metallicFactor;
    float  roughnessFactor;
    float  occlusionStrength;
    float  opacityFactor;
    float  alphaCutoff;
    int    materialFeatureMask;
    float  materialModel;
    float  _pad0;
};

struct PSInput
{
    float4 positionCS : SV_POSITION;
    float2 texCoord   : TEXCOORD0;
};

void main(PSInput IN)
{
    float4 c = albedo.Sample(sLinear, IN.texCoord) * baseColorFactor;
    float opacity = c.a * opacityFactor;
    clip(opacity - alphaCutoff);
    // kein Color-Output — Depth-Write wird vom Rasterizer durchgeführt
}

#else

void main() {}

#endif
