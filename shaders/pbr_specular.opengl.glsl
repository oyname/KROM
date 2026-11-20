#ifndef KROM_PBR_SPECULAR
#define KROM_PBR_SPECULAR

vec3 KromSpecularF0(vec3 albedoValue, float metallic, float specular)
{
    return mix(vec3(0.08 * clamp(specular, 0.0, 1.0)), albedoValue, metallic);
}

float KromSpecularF90(float metallic, float specular)
{
    return mix(clamp(4.0 * specular, 0.0, 1.0), 1.0, metallic);
}

#endif
