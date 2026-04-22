#version 410 core
#extension GL_ARB_shading_language_420pack : enable
// =============================================================================
// KROM Engine - assets/lit.opengl.fs.glsl
// Fragment Shader: Lit (OpenGL / GLSL 4.10)
// Separater OpenGL-Shadow-Pfad.
// =============================================================================

struct GpuLightData
{
    vec4 positionWS;
    vec4 directionWS;
    vec4 colorIntensity;
    vec4 params;
};

layout(std140, binding = 0) uniform PerFrame
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

layout(std140, binding = 2) uniform PerMaterial
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
};

uniform sampler2D albedo;
uniform sampler2D emissive;
uniform sampler2DShadow shadowMap;

in vec3 vPositionWS;
in vec3 vNormalWS;
in vec2 vTexCoord;
in vec4 vPositionLightCS;

layout(location = 0) out vec4 fragColor;

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

float SampleDirectionalShadow(vec4 positionLightCS, vec3 normalWS, vec3 lightDirWS)
{
    if (shadowCascadeCount == 0u || shadowStrength <= 0.0)
        return 1.0;
    if (positionLightCS.w <= 1e-6)
        return 1.0;

    vec3 posNDC = positionLightCS.xyz / positionLightCS.w;
    vec2 uv = posNDC.xy * 0.5 + 0.5;
    float depth = posNDC.z * 0.5 + 0.5;

    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0)
        return 1.0;
    if (depth <= 0.0 || depth >= 1.0)
        return 1.0;

    float NoL = clamp(dot(normalWS, lightDirWS), 0.0, 1.0);
    float bias = shadowBias + (1.0 - NoL) * shadowNormalBias;

    float visibility = 0.0;
    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            vec2 offset = vec2(float(x), float(y)) * shadowTexelSize;
            visibility += texture(shadowMap, vec3(uv + offset, depth - bias));
        }
    }
    visibility *= (1.0 / 9.0);
    return mix(1.0, visibility, clamp(shadowStrength, 0.0, 1.0));
}

void main()
{
#ifdef KROM_BASECOLOR_MAP
    vec4 albedo4 = texture(albedo, vTexCoord) * baseColorFactor;
#else
    vec4 albedo4 = baseColorFactor;
#endif

    float opacity = albedo4.a * opacityFactor;
#ifdef KROM_ALPHA_TEST
    if (opacity < alphaCutoff) discard;
#endif

    vec3 N = SafeNormalize(vNormalWS);
    vec3 V = SafeNormalize(cameraPositionWS.xyz - vPositionWS);
    float roughness = clamp(roughnessFactor, 0.04, 1.0);
    float shininess = mix(128.0, 8.0, roughness);
    float specularStrength = clamp(metallicFactor, 0.0, 1.0);

    vec3 lighting = albedo4.rgb * ambientColor.rgb * ambientColor.a;
    float shadowVisibility = 1.0;
    if (lightCount > 0u && lights[0].params.w < 0.5)
        shadowVisibility = SampleDirectionalShadow(vPositionLightCS, N, SafeNormalize(-lights[0].positionWS.xyz));

    for (uint i = 0u; i < lightCount; ++i)
    {
        float type = lights[i].params.w;
        vec3 L = vec3(0.0, 1.0, 0.0);
        float attenuation = 1.0;

        if (type < 0.5)
        {
            L = SafeNormalize(-lights[i].positionWS.xyz);
        }
        else
        {
            vec3 toLight = lights[i].positionWS.xyz - vPositionWS;
            float dist = length(toLight);
            L = (dist > 1e-5) ? (toLight / dist) : vec3(0.0, 1.0, 0.0);
            attenuation = CalcAttenuation(dist, lights[i].params.z);
            if (type > 1.5)
                attenuation *= CalcSpotAttenuation(L, SafeNormalize(lights[i].directionWS.xyz),
                                                   lights[i].params.x, lights[i].params.y);
        }

        if (type < 0.5)
            attenuation *= shadowVisibility;

        float NoL = max(dot(N, L), 0.0);
        vec3 H = SafeNormalize(V + L);
        float spec = pow(max(dot(N, H), 0.0), shininess) * specularStrength;
        vec3 lightColor = lights[i].colorIntensity.rgb * lights[i].colorIntensity.w * attenuation;
        lighting += albedo4.rgb * lightColor * NoL;
        lighting += lightColor * spec;
    }

    vec3 emissiveColor = emissiveFactor.rgb;
#ifdef KROM_EMISSIVE_MAP
    emissiveColor *= texture(emissive, vTexCoord).rgb;
#endif

    fragColor = vec4(lighting + emissiveColor, opacity);
}
