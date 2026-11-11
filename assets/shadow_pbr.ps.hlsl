// =============================================================================
// KROM Engine — assets/shadow_pbr.ps.hlsl
// Pixel Shader: Shadow-Pass für PBR-Materialien (KROM_ALPHA_TEST)
// Sampelt die Albedo-Textur (UV bereits durch shadow_pbr.vs.hlsl transformiert)
// und verwirft Fragmente mit Opacity < alphaCutoff.
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
    int    occlusionChannel;
    int    roughnessChannel;
    int    metallicChannel;
    float  occlusionBias;
    float  roughnessBias;
    float  metallicBias;
    float2 uvScale;
    float2 uvOffset;
    float  boxTileSize;
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
    float3 scaledLocalPosition : TEXCOORD1;
    float2 meshUV : TEXCOORD2;
    float3 positionWS : TEXCOORD3;
};

#include "pbr_box_projection.hlsl"
#define KROM_GRAPH_DECLARE_TIME
#include "pbr_material_graph.hlsl"

void main(PSInput IN)
{
    float2 uv = KromPbrUV(IN.texCoord, IN.scaledLocalPosition, boxTileSize);
#ifdef KROM_MATERIAL_GRAPH
    EvaluateMaterialGraph(boxTileSize > 0.0f ? uv : IN.meshUV, IN.positionWS);
    if (boxTileSize <= 0.0f)
        uv = KromGraphDirectUV(uv, IN.meshUV, uvScale, uvOffset);
#endif
    float4 c = albedo.Sample(sLinear, uv) * baseColorFactor;
#ifdef KROM_MATERIAL_GRAPH
    float3 graphEmission;
    ApplyMaterialGraph(c, graphEmission);
#endif
    float opacity = c.a * opacityFactor;
    clip(opacity - alphaCutoff);
}

#else

void main() {}

#endif
