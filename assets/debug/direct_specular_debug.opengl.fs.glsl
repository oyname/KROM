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

in vec3 vPositionWS;
in vec3 vNormalWS;
in vec4 vTangentWS;
in vec2 vTexCoord;
layout(location = 0) out vec4 fragColor;

const float PI = 3.14159265358979323846;

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

float D_GGX(float NoH, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float d = (NoH * a2 - NoH) * NoH + 1.0;
    return a2 / max(PI * d * d, 1e-6);
}

float G_SchlickGGX(float NoV, float roughness)
{
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return NoV / max(NoV * (1.0 - k) + k, 1e-6);
}

float G_Smith(float NoV, float NoL, float roughness)
{
    return G_SchlickGGX(NoV, roughness) * G_SchlickGGX(NoL, roughness);
}

vec3 F_Schlick(float HoV, vec3 F0)
{
    return F0 + (1.0 - F0) * pow(1.0 - HoV, 5.0);
}

void main()
{
    if (lightCount == 0u)
    {
        fragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    vec3 albedoValue = baseColorFactor.rgb;
    float metallic = clamp(metallicFactor, 0.0, 1.0);
    float roughness = clamp(roughnessFactor, 0.04, 1.0);

    vec3 N = SafeNormalize(vNormalWS);
    N = SampleNormal(vTexCoord, N, vTangentWS);
    vec3 V = SafeNormalize(cameraPositionWS.xyz - vPositionWS);
    vec3 L = SafeNormalize(-lights[0].positionWS.xyz);
    vec3 H = SafeNormalize(V + L);

    float NoV = max(dot(N, V), 1e-4);
    float NoL = max(dot(N, L), 0.0);
    float NoH = max(dot(N, H), 0.0);
    float HoV = max(dot(H, V), 0.0);

    vec3 F0 = mix(vec3(0.04), albedoValue, metallic);
    vec3 F = F_Schlick(HoV, F0);
    float D = D_GGX(NoH, roughness);
    float G = G_Smith(NoV, NoL, roughness);
    vec3 spec = (D * G * F) / max(4.0 * NoV * NoL, 1e-4);
    vec3 lightColor = lights[0].colorIntensity.rgb * lights[0].colorIntensity.w;
    fragColor = vec4(spec * lightColor * NoL, 1.0);
}
