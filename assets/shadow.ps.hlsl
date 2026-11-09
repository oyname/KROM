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
    float  normalStrength;
    float  occlusionStrength;
    float  opacityFactor;
    float  alphaCutoff;
    int    materialFeatureMask;
    float  materialModel;
#ifdef KROM_MATERIAL_GRAPH
    float4 graphOutputs;
    float4 graphNodes[16];
    float4 graphUV[8];
#endif
};

struct PSInput
{
    float4 positionCS : SV_POSITION;
    float2 texCoord   : TEXCOORD0;
};

#define KROM_GRAPH_DECLARE_TIME
#include "pbr_material_graph.hlsl"

void main(PSInput IN)
{
    float4 c = albedo.Sample(sLinear, IN.texCoord) * baseColorFactor;
#ifdef KROM_MATERIAL_GRAPH
    float3 graphEmission;
    EvaluateMaterialGraph(IN.texCoord, c, graphEmission);
#endif
    float opacity = c.a * opacityFactor;
    clip(opacity - alphaCutoff);
    // kein Color-Output — Depth-Write wird vom Rasterizer durchgeführt
}

#else

void main() {}

#endif
