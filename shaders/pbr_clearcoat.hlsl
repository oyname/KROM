// =============================================================================
// KROM Engine - shaders/pbr_clearcoat.hlsl
// Clear coat layer of the pbr-lit template (PerMaterial clearcoatFactor and
// clearcoatRoughness), as in glTF KHR_materials_clearcoat and Filament: a
// dielectric coat (IOR 1.5, F0 = 0.04) with its own GGX lobe on top of the
// base. The coat uses the geometric normal (no coat normal map yet), so it
// stays smooth over a normal-mapped base. The base is attenuated by 1 - Fc.
// Include after D_GGX and SafeNormalize.
// =============================================================================

// Coat Fresnel for F0 = 0.04, scaled by the coat strength.
float KromClearcoatFresnel(float cosTheta, float strength)
{
    float f = 1.0f - saturate(cosTheta);
    float f5 = f * f;
    f5 = f5 * f5 * f;
    return (0.04f + 0.96f * f5) * strength;
}

// Kelemen visibility (cheap, for the smooth coat; contains 1 / (4 NoV NoL)).
float V_Kelemen(float LoH)
{
    return 0.25f / max(LoH * LoH, 1e-4f);
}

// Direct light on the coat: returns the coat specular (times NoL) and its
// Fresnel Fc, by which the caller attenuates the base of this light.
float KromClearcoatSpecular(float3 coatN, float3 V, float3 L, float coatRoughness, float strength,
                            out float Fc)
{
    float3 H = SafeNormalize(V + L);
    float NoL = saturate(dot(coatN, L));
    float NoH = saturate(dot(coatN, H));
    float LoH = saturate(dot(L, H));
    Fc = KromClearcoatFresnel(LoH, strength);
    return D_GGX(NoH, coatRoughness) * V_Kelemen(LoH) * Fc * NoL;
}
