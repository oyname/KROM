// =============================================================================
// KROM Engine - assets/pbr_ssr_output.hlsl
// SSR inputs written by the opaque pass (KROM_SSR_OUTPUT variant, MRT).
//
// The PBR pixel shaders name their shading function KROM_PBR_ENTRY. Without
// KROM_SSR_OUTPUT that is "main" and nothing changes. With it, the shading
// function stores the SSR inputs in the statics below through
// KromStoreSsrInputs, and KROM_PBR_SSR_MAIN defines the real entry point that
// writes them as SV_Target1..3:
//   SV_Target1  xy = octahedral shading normal (world), z = roughness, w = 1
//   SV_Target2  rgb = weight of the specular environment light, i.e. the
//               factor the shader applied to the prefiltered radiance
//               (BRDF term, energy compensation and specular occlusion)
//   SV_Target3  rgb = specular environment light added to the colour
//               (prefiltered probe/IBL radiance times that weight)
// The SSR pass replaces SV_Target3 with its hit radiance times SV_Target2.
// Pixels without a stored value (debug views, other shaders) keep w = 0.
// =============================================================================
#ifndef KROM_PBR_SSR_OUTPUT_HLSL
#define KROM_PBR_SSR_OUTPUT_HLSL

#ifdef KROM_SSR_OUTPUT

static float4 gKromSsrNormalRoughness = float4(0.0f, 0.0f, 0.0f, 0.0f);
static float4 gKromSsrSpecularWeight  = float4(0.0f, 0.0f, 0.0f, 0.0f);
static float4 gKromSsrSpecularLight   = float4(0.0f, 0.0f, 0.0f, 0.0f);

float2 KromOctWrap(float2 v)
{
    return (1.0f - abs(v.yx)) * float2(v.x >= 0.0f ? 1.0f : -1.0f, v.y >= 0.0f ? 1.0f : -1.0f);
}

// Unit vector -> [-1, 1]^2 (octahedral mapping).
float2 KromOctEncode(float3 n)
{
    n /= max(abs(n.x) + abs(n.y) + abs(n.z), 1e-6f);
    return n.z >= 0.0f ? n.xy : KromOctWrap(n.xy);
}

void KromStoreSsrInputs(float3 N, float roughness, float3 specularWeight, float3 specularLight)
{
    gKromSsrNormalRoughness = float4(KromOctEncode(N), saturate(roughness), 1.0f);
    gKromSsrSpecularWeight  = float4(max(specularWeight, 0.0f), 0.0f);
    gKromSsrSpecularLight   = float4(max(specularLight, 0.0f), 0.0f);
}

struct KromSsrPSOutput
{
    float4 color          : SV_Target0;
    float4 normalRoughness : SV_Target1;
    float4 specularWeight : SV_Target2;
    float4 specularLight  : SV_Target3;
};

#define KROM_PBR_ENTRY KromShadePixel
#define KROM_PBR_SSR_MAIN(InputType)                     \
    KromSsrPSOutput main(InputType IN)                   \
    {                                                    \
        KromSsrPSOutput o;                               \
        o.color           = KromShadePixel(IN);          \
        o.normalRoughness = gKromSsrNormalRoughness;     \
        o.specularWeight  = gKromSsrSpecularWeight;      \
        o.specularLight   = gKromSsrSpecularLight;       \
        return o;                                        \
    }

#else

#define KROM_PBR_ENTRY main
#define KROM_PBR_SSR_MAIN(InputType)

#endif // KROM_SSR_OUTPUT

#endif // KROM_PBR_SSR_OUTPUT_HLSL
