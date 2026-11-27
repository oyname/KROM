// =============================================================================
// KROM Engine - shaders/gi_texel.hlsl
// Alpha channel of the surface cache GI texture (TexSlots::GlobalIllumination,
// RGBA32F): one whole number that holds the linear view depth of the texel
// (12 bit, logarithmic, steps of 0.36 %) and the normal of its surface
// (octahedral, 2 x 5 bit, about 4 degrees). Negative where the far field filled
// in, 0 for the sky. Written by surface_cache_apply.hlsl, read by pbr_gi.hlsl.
//
// The normal is what tells two surfaces apart that touch (a box against a
// wall): along such an edge both have the same depth, and a pixel of the one
// took the irradiance of the other (dark teeth along the edge under MSAA).
// =============================================================================
#ifndef KROM_GI_TEXEL_HLSL
#define KROM_GI_TEXEL_HLSL

static const float GI_TEXEL_DEPTH_MIN_LOG2 = -7.0f;          // 0.0078 m
static const float GI_TEXEL_DEPTH_STEPS_PER_OCTAVE = 195.0f; // 4095 steps over 21 octaves (up to 16 km)

float2 GiTexelOctWrap(float2 v)
{
    return (1.0f - abs(v.yx)) * float2(v.x >= 0.0f ? 1.0f : -1.0f, v.y >= 0.0f ? 1.0f : -1.0f);
}

// viewDepth > 0, normal normalised. Returns a whole number >= 1.
float GiTexelEncode(float viewDepth, float3 normal)
{
    const float depthCode = clamp(floor((log2(max(viewDepth, 1e-6f)) - GI_TEXEL_DEPTH_MIN_LOG2) *
                                        GI_TEXEL_DEPTH_STEPS_PER_OCTAVE + 0.5f), 0.0f, 4095.0f);
    const float3 n = normal / max(abs(normal.x) + abs(normal.y) + abs(normal.z), 1e-6f);
    const float2 oct = n.z >= 0.0f ? n.xy : GiTexelOctWrap(n.xy);
    const float2 q = clamp(floor((oct * 0.5f + 0.5f) * 31.0f + 0.5f), 0.0f, 31.0f);
    return depthCode + q.x * 4096.0f + q.y * 131072.0f + 1.0f;
}

// alpha: the texel's alpha (any sign, not 0).
void GiTexelDecode(float alpha, out float viewDepth, out float3 normal)
{
    const uint code = (uint)(abs(alpha) + 0.5f) - 1u;
    viewDepth = exp2((float)(code & 4095u) / GI_TEXEL_DEPTH_STEPS_PER_OCTAVE + GI_TEXEL_DEPTH_MIN_LOG2);
    const float2 oct = float2((float)((code >> 12u) & 31u), (float)((code >> 17u) & 31u)) / 31.0f * 2.0f - 1.0f;
    float3 n = float3(oct, 1.0f - abs(oct.x) - abs(oct.y));
    if (n.z < 0.0f)
        n.xy = GiTexelOctWrap(n.xy);
    normal = normalize(n);
}

#endif // KROM_GI_TEXEL_HLSL
