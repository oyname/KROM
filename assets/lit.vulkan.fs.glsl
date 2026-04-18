#version 450

layout(location = 0) out vec4 FragColor;
layout(location = 0) in vec3 outPositionWS;
layout(location = 1) in vec3 outNormalWS;
layout(location = 2) in vec2 outUV;

struct GpuLightData
{
    vec4 positionWS;
    vec4 directionWS;
    vec4 colorIntensity;
    vec4 params;
};

layout(set = 0, binding = 0, std140) uniform PerFrame
{
    mat4         viewMatrix;
    mat4         projMatrix;
    mat4         viewProjMatrix;
    mat4         invViewProjMatrix;
    vec4         cameraPositionWS;
    vec4         cameraForwardWS;
    vec4         screenSize;
    vec4         timeData;
    vec4         ambientColor;
    uint         lightCount;
    uint         shadowCascadeCount;
    float        nearPlane;
    float        farPlane;
    GpuLightData lights[8];
    float        iblPrefilterLevels;
    float        _padFC0;
    float        _padFC1;
    float        _padFC2;
} perFrame;

layout(set = 0, binding = 2, std140) uniform PerMaterial
{
    vec4  baseColorFactor;
    vec4  emissiveFactor;
    float metallicFactor;
    float roughnessFactor;
    float occlusionStrength;
    float opacityFactor;
    float alphaCutoff;
    int   materialFeatureMask;
    float materialModel;
    float _pad0;
} perMaterial;

layout(set = 0, binding = 16) uniform texture2D albedo;
layout(set = 0, binding = 19) uniform texture2D emissive;
layout(set = 0, binding = 32) uniform sampler sLinearWrap;

vec3 SafeNormalize(vec3 v)
{
    float len2 = dot(v, v);
    return (len2 > 1e-12) ? (v * inversesqrt(len2)) : vec3(0.0, 0.0, 1.0);
}

float CalcAttenuation(float dist, float range)
{
    float ratio = clamp(1.0 - dist / max(range, 0.001), 0.0, 1.0);
    return (ratio * ratio) / max(dist * dist, 1e-4);
}

float CalcSpotAttenuation(vec3 L, vec3 spotDir, float cosInner, float cosOuter)
{
    float cosAngle = dot(-L, spotDir);
    return clamp((cosAngle - cosOuter) / max(cosInner - cosOuter, 1e-4), 0.0, 1.0);
}

void main()
{
#ifdef KROM_BASECOLOR_MAP
    vec4 albedo4 = texture(sampler2D(albedo, sLinearWrap), outUV) * perMaterial.baseColorFactor;
#else
    vec4 albedo4 = perMaterial.baseColorFactor;
#endif

    float opacity = albedo4.a * perMaterial.opacityFactor;
#ifdef KROM_ALPHA_TEST
    if (opacity < perMaterial.alphaCutoff) discard;
#endif

    vec3 N = SafeNormalize(outNormalWS);
    vec3 V = SafeNormalize(perFrame.cameraPositionWS.xyz - outPositionWS);
    float roughness = clamp(perMaterial.roughnessFactor, 0.04, 1.0);
    float shininess = mix(128.0, 8.0, roughness);
    float specularStrength = clamp(perMaterial.metallicFactor, 0.0, 1.0);

    vec3 lighting = albedo4.rgb * perFrame.ambientColor.rgb * perFrame.ambientColor.a;
    for (uint i = 0u; i < perFrame.lightCount; ++i)
    {
        float lightType = perFrame.lights[i].params.w;
        vec3 L;
        float attenuation = 1.0;

        if (lightType < 0.5)
        {
            L = SafeNormalize(-perFrame.lights[i].positionWS.xyz);
        }
        else
        {
            vec3 toLight = perFrame.lights[i].positionWS.xyz - outPositionWS;
            float dist = length(toLight);
            L = (dist > 1e-5) ? (toLight / dist) : vec3(0.0, 1.0, 0.0);
            attenuation = CalcAttenuation(dist, perFrame.lights[i].params.z);
            if (lightType > 1.5)
                attenuation *= CalcSpotAttenuation(L, normalize(perFrame.lights[i].directionWS.xyz), perFrame.lights[i].params.x, perFrame.lights[i].params.y);
        }

        float NoL = max(dot(N, L), 0.0);
        vec3 H = SafeNormalize(V + L);
        float spec = pow(max(dot(N, H), 0.0), shininess) * specularStrength;
        vec3 lightColor = perFrame.lights[i].colorIntensity.rgb * perFrame.lights[i].colorIntensity.w * attenuation;
        lighting += albedo4.rgb * lightColor * NoL;
        lighting += lightColor * spec;
    }

    vec3 emissiveColor = perMaterial.emissiveFactor.rgb;
#ifdef KROM_EMISSIVE_MAP
    emissiveColor *= texture(sampler2D(emissive, sLinearWrap), outUV).rgb;
#endif

    FragColor = vec4(lighting + emissiveColor, opacity);
}
