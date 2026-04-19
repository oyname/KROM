// =============================================================================
// KROM Engine — assets/quad_unlit.ps.hlsl
// Pixel Shader: Unlit Quad (HLSL)
// Targets: DX11 (DXBC), DX12 (DXIL), Vulkan (SPIR-V via DXC -spirv)
//
// Binding-Modell (ShaderBindingModel.hpp):
//   t0  = Albedo / BaseColor Map  (TexSlots::Albedo)
//   t3  = Emissive Map            (TexSlots::Emissive)
//   s0  = LinearWrap Sampler      (SamplerSlots::LinearWrap)
//   CB2 = PerMaterial
// =============================================================================

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

Texture2D    tAlbedo  : register(t0);
Texture2D    emissive : register(t3);
SamplerState sLinear  : register(s0);

struct PSInput
{
    float4 positionCS : SV_POSITION;
    float3 positionWS : TEXCOORD1;
    float3 normalWS   : TEXCOORD2;
    float2 texCoord   : TEXCOORD0;
};

float4 main(PSInput IN) : SV_TARGET
{
#ifdef KROM_BASECOLOR_MAP
    float4 albedo = tAlbedo.Sample(sLinear, IN.texCoord) * baseColorFactor;
#else
    float4 albedo;
    if (materialFeatureMask & 2)
        albedo = tAlbedo.Sample(sLinear, IN.texCoord) * baseColorFactor;
    else
        albedo = baseColorFactor;
#endif

#ifdef KROM_ALPHA_TEST
    clip(albedo.a - alphaCutoff);
#endif

    albedo.a *= opacityFactor;

    float3 emissiveColor = emissiveFactor.rgb;
#ifdef KROM_EMISSIVE_MAP
    emissiveColor *= emissive.Sample(sLinear, IN.texCoord).rgb;
#endif

    return float4(albedo.rgb + emissiveColor, albedo.a);
}
