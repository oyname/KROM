#version 410 core

struct GpuLightData
{
    vec4 positionWS;
    vec4 directionWS;
    vec4 colorIntensity;
    vec4 params;
};

layout(std140) uniform PerFrame
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
    GpuLightData lights[7];
    mat4         shadowViewProj;
    float        iblPrefilterLevels;
    float        shadowBias;
    float        shadowNormalBias;
    float        shadowStrength;
    float        shadowTexelSize;
};

layout(std140) uniform PerMaterial
{
    vec4  baseColorFactor;
    vec4  emissiveFactor;
    float metallicFactor;
    float roughnessFactor;
    float normalStrength;
    float occlusionStrength;
    float opacityFactor;
    float alphaCutoff;
    int   materialFeatureMask;
    float materialModel;
    int   occlusionChannel;
    int   roughnessChannel;
    int   metallicChannel;
    float occlusionBias;
    float roughnessBias;
    float metallicBias;
    float _pad1;
    float _pad2;
};

uniform sampler2D normal;
uniform samplerCube tIBLPrefiltered;
uniform sampler2D tBRDFLut;

in vec3 vPositionWS;
in vec3 vNormalWS;
in vec4 vTangentWS;
in vec2 vTexCoord;
layout(location = 0) out vec4 fragColor;

vec3 SafeNormalize(vec3 v)
{
    float len2 = dot(v, v);
    return (len2 > 1e-12) ? (v * inversesqrt(len2)) : vec3(0.0, 0.0, 1.0);
}

vec3 DecodeNormalRGB(vec4 encodedNormal)
{
    return encodedNormal.xyz * 2.0 - 1.0;
}

mat3 BuildTBN(vec3 N, vec4 tangentWS)
{
    vec3 T = tangentWS.xyz;
    float handedness = (abs(tangentWS.w) > 0.5) ? tangentWS.w : 1.0;
    if (dot(T, T) <= 1e-10)
    {
        vec3 up = (abs(N.y) < 0.999) ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
        T = SafeNormalize(cross(up, N));
    }
    else
    {
        T = SafeNormalize(T - N * dot(N, T));
    }

    vec3 B = SafeNormalize(cross(N, T)) * handedness;
    return mat3(T, B, N);
}

vec3 SampleNormal(vec2 uv, vec3 baseNormalWS, vec4 tangentWS)
{
#ifdef KROM_NORMAL_MAP
    vec3 mapN = SafeNormalize(DecodeNormalRGB(texture(normal, uv)));
    mapN.xy *= normalStrength;
    mapN.z = sqrt(clamp(1.0 - dot(mapN.xy, mapN.xy), 0.0, 1.0));
    mapN = SafeNormalize(mapN);
    mat3 tbn = BuildTBN(baseNormalWS, tangentWS);
    return SafeNormalize(tbn * mapN);
#else
    return baseNormalWS;
#endif
}

vec3 F_SchlickRoughness(float NoV, vec3 F0, float roughness)
{
    vec3 inv = max(vec3(1.0 - roughness), F0);
    return F0 + (inv - F0) * pow(1.0 - NoV, 5.0);
}

void main()
{
    vec3 albedoValue = baseColorFactor.rgb;
    float metallic = clamp(metallicFactor, 0.0, 1.0);
    float roughness = clamp(roughnessFactor, 0.04, 1.0);

    vec3 N = SafeNormalize(vNormalWS);
    N = SampleNormal(vTexCoord, N, vTangentWS);
    vec3 V = SafeNormalize(cameraPositionWS.xyz - vPositionWS);
    float NoV = max(dot(N, V), 1e-4);
    vec3 F0 = mix(vec3(0.04), albedoValue, metallic);
    vec3 F = F_SchlickRoughness(NoV, F0, roughness);
    vec3 R = reflect(-V, N);
    vec3 prefilt = textureLod(tIBLPrefiltered, SafeNormalize(R), roughness * iblPrefilterLevels).rgb;
    vec2 brdfSamp = texture(tBRDFLut, vec2(NoV, roughness)).rg;
    vec3 specIBL = prefilt * (F * brdfSamp.x + brdfSamp.y);
    fragColor = vec4(specIBL, 1.0);
}
