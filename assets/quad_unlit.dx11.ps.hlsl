// =============================================================================
// KROM Engine — assets/quad_unlit.dx11.ps.hlsl
// Pixel Shader: texturiertes, unbelichtetes Quad (DX11 / HLSL SM 5.0)
//
// Binding-Modell (ShaderBindingModel.hpp):
//   t0  = Albedo / BaseColor Map          (TexSlots::Albedo)
//   s0  = LinearWrap Sampler              (SamplerSlots::LinearWrap)
//   CB2 = PerMaterial                     (baseColorFactor, alphaCutoff, ...)
//
// Varianten-Defines:
//   KROM_BASECOLOR_MAP  – t0 wird gesampelt, Ergebnis mit baseColorFactor moduliert
//   KROM_ALPHA_TEST     – Fragmente unterhalb alphaCutoff werden verworfen
//
// Fehlende Texturen werden immer von ShaderRuntime durch Fallback-Texturen
// ersetzt; der Shader sieht stets valide Handles an t0.
// =============================================================================

// ---------------------------------------------------------------------------
// CB2 – PerMaterial
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// Texturen & Sampler
// ---------------------------------------------------------------------------
Texture2D    tAlbedo  : register(t0);
SamplerState sLinear  : register(s0);

// ---------------------------------------------------------------------------
// Pixel I/O
// ---------------------------------------------------------------------------
struct PSInput
{
    float4 positionCS : SV_POSITION;
    float3 positionWS : TEXCOORD1;
    float3 normalWS   : TEXCOORD2;
    float2 texCoord   : TEXCOORD0;
};

float4 main(PSInput IN) : SV_TARGET
{
    // MaterialFeatureFlag::BaseColorTexture = 1u << 1 = 2
    // Compile-time-Pfad (Varianten-Kompilierung): #ifdef KROM_BASECOLOR_MAP.
    // Laufzeit-Fallback: materialFeatureMask-Bit prüfen, falls der Shader
    // ohne KROM_BASECOLOR_MAP compiliert wurde (z.B. fehlende D3DCompile-DLL).
    // t0 ist durch ShaderRuntime::ResolveBindings immer valide gebunden
    // (ggf. mit der White-Fallback-Textur).
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

    return albedo;
}
