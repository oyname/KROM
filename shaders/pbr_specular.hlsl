#ifndef KROM_PBR_SPECULAR
#define KROM_PBR_SPECULAR

// Unreal-style dielectric reflectance: Specular 0.5 is F0 0.04; 1 is 0.08.
// Metals keep their albedo-derived F0 and grazing reflectance.
float3 KromSpecularF0(float3 albedoValue, float metallic, float specular)
{
    return lerp((0.08f * saturate(specular)).xxx, albedoValue, metallic);
}

float KromSpecularF90(float metallic, float specular)
{
    // Suppress the grazing lobe as reflectance tends to zero. F0 alone does not
    // turn Schlick Fresnel off, nor the additive bias in the split-sum BRDF LUT.
    return lerp(saturate(4.0f * specular), 1.0f, metallic);
}

#endif
