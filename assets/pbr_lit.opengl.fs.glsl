#version 410 core
// =============================================================================
// KROM Engine — assets/pbr_lit.opengl.fs.glsl
// Fragment Shader: PBR-Lit (OpenGL / GLSL 4.10)
// =============================================================================

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
    vec4  baseColorFactor;      // byte  0
    vec4  emissiveFactor;       // byte 16
    float metallicFactor;       // byte 32
    float roughnessFactor;      // byte 36
    float normalStrength;       // byte 40
    float occlusionStrength;    // byte 44
    float opacityFactor;        // byte 48
    float alphaCutoff;          // byte 52
    int   materialFeatureMask;  // byte 56
    float materialModel;        // byte 60
    // Row 4 — channel-map constants (zero-default; only meaningful with KROM_CHANNEL_MAP)
    int   occlusionChannel;     // byte 64
    int   roughnessChannel;     // byte 68
    int   metallicChannel;      // byte 72
    float occlusionBias;        // byte 76
    // Row 5
    float roughnessBias;        // byte 80
    float metallicBias;         // byte 84
    float _pad1;                // byte 88
    float _pad2;                // byte 92
};

uniform sampler2D albedo;
uniform sampler2D normal;
uniform sampler2D orm;
uniform sampler2D emissive;
uniform sampler2D shadowMapRaw;
uniform samplerCube tIBLIrradiance;
uniform samplerCube tIBLPrefiltered;
uniform sampler2D tBRDFLut;

in vec3 vPositionWS;
in vec3 vNormalWS;
in vec4 vTangentWS;
in vec2 vTexCoord;
in vec4 vPositionLightCS;
layout(location = 0) out vec4 fragColor;

const float PI = 3.14159265358979323846;
const float TWO_PI = 6.28318530717958647692;

vec3 SafeNormalize(vec3 v)
{
    float len2 = dot(v, v);
    return (len2 > 1e-12) ? (v * inversesqrt(len2)) : vec3(0.0, 0.0, 1.0);
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

vec3 F_SchlickRoughness(float NoV, vec3 F0, float roughness)
{
    vec3 inv = max(vec3(1.0 - roughness), F0);
    return F0 + (inv - F0) * pow(1.0 - NoV, 5.0);
}


float SampleChannel(vec4 v, int ch)
{
    vec4 mask = vec4(ch == 0 ? 1.0 : 0.0,
                     ch == 1 ? 1.0 : 0.0,
                     ch == 2 ? 1.0 : 0.0,
                     ch == 3 ? 1.0 : 0.0);
    return dot(v, mask);
}

vec3 DecodeNormalRGB(vec4 encodedNormal)
{
    return encodedNormal.xyz * 2.0 - 1.0;
}

vec3 DecodeNormalBC5(vec4 encodedNormal)
{
    vec2 xy = encodedNormal.rg * 2.0 - 1.0;
    float z = sqrt(max(1.0 - dot(xy, xy), 0.0));
    return vec3(xy, z);
}

vec3 SampleDecodedNormal(sampler2D normalTex, vec2 uv)
{
    vec4 encodedNormal = texture(normalTex, uv);
#ifdef KROM_NORMALMAP_BC5
    return DecodeNormalBC5(encodedNormal);
#else
    return DecodeNormalRGB(encodedNormal);
#endif
}

vec3 SampleNormal(vec2 uv, vec3 baseNormalWS, vec4 tangentWS, vec3 positionWS)
{
#ifdef KROM_NORMAL_MAP
    vec3 mapN = SafeNormalize(SampleDecodedNormal(normal, uv));
    mapN.xy *= normalStrength;
    mapN.z = sqrt(clamp(1.0 - dot(mapN.xy, mapN.xy), 0.0, 1.0));
    mapN = SafeNormalize(mapN);

    vec3 T = tangentWS.xyz;
    float tLen2 = dot(T, T);
    if (tLen2 > 1e-8)
    {
        T *= inversesqrt(tLen2);
        T = T - dot(T, baseNormalWS) * baseNormalWS;
        float tOrthoLen2 = dot(T, T);
        if (tOrthoLen2 > 1e-8)
        {
            T *= inversesqrt(tOrthoLen2);
            float handedness = (abs(tangentWS.w) > 0.5) ? tangentWS.w : 1.0;
            vec3 B = SafeNormalize(cross(baseNormalWS, T)) * handedness;
            vec3 shadedN = SafeNormalize(mapN.x * T + mapN.y * B + mapN.z * baseNormalWS);
            float NoNg = dot(shadedN, baseNormalWS);
            if (NoNg < 0.0)
                shadedN = SafeNormalize(shadedN - 2.0 * NoNg * baseNormalWS);
            return shadedN;
        }
    }

    vec3 dp1 = dFdx(positionWS);
    vec3 dp2 = dFdy(positionWS);
    vec2 duv1 = dFdx(uv);
    vec2 duv2 = dFdy(uv);

    vec3 dp2perp = cross(dp2, baseNormalWS);
    vec3 dp1perp = cross(baseNormalWS, dp1);

    vec3 T_raw = dp2perp * duv1.x + dp1perp * duv2.x;
    vec3 B_raw = dp2perp * duv1.y + dp1perp * duv2.y;

    float derivTLen2 = dot(T_raw, T_raw);
    float dpLen2 = max(dot(dp1, dp1), dot(dp2, dp2));
    float uvLen2 = max(dot(duv1, duv1), dot(duv2, duv2));
    float relRef = dpLen2 * uvLen2;

    if (!(derivTLen2 > relRef * 1e-6 && relRef > 0.0))
        return baseNormalWS;

    T = T_raw * inversesqrt(derivTLen2);
    vec3 T_gs = T - dot(T, baseNormalWS) * baseNormalWS;
    float tgs2 = dot(T_gs, T_gs);
    T = (tgs2 > 1e-6) ? (T_gs * inversesqrt(tgs2)) : T;

    vec3 B_cross = cross(baseNormalWS, T);
    float handedness = (dot(B_cross, B_raw) >= 0.0) ? 1.0 : -1.0;
    vec3 B = B_cross * handedness;

    vec3 shadedN = SafeNormalize(mapN.x * T + mapN.y * B + mapN.z * baseNormalWS);
    float NoNg = dot(shadedN, baseNormalWS);
    if (NoNg < 0.0)
        shadedN = SafeNormalize(shadedN - 2.0 * NoNg * baseNormalWS);
    return shadedN;
#else
    return baseNormalWS;
#endif
}

float ApplySpecularAA(vec3 shadingNormalWS, float roughness)
{
#ifdef KROM_NORMAL_MAP
    vec3 dndx = dFdx(shadingNormalWS);
    vec3 dndy = dFdy(shadingNormalWS);
    float variance = max(dot(dndx, dndx), dot(dndy, dndy));
    variance = min(variance, 0.18);
    float roughness2 = roughness * roughness;
    return clamp(sqrt(roughness2 + variance), 0.0, 1.0);
#else
    return roughness;
#endif
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

float CompareShadowManual(vec2 uv, float cmpDepth)
{
    float sampledDepth = texture(shadowMapRaw, uv).r;
    return (cmpDepth <= sampledDepth) ? 1.0 : 0.0;
}

float CompareShadowManualBilinear(vec2 uv, float cmpDepth)
{
    float texelSize = max(shadowTexelSize, 1e-8);
    float invTexelSize = 1.0 / texelSize;

    vec2 texelPos = uv * invTexelSize - 0.5;
    vec2 baseTexel = floor(texelPos);
    vec2 fracTexel = fract(texelPos);

    vec2 uv00 = (baseTexel + vec2(0.5, 0.5)) * texelSize;
    vec2 uv10 = uv00 + vec2(texelSize, 0.0);
    vec2 uv01 = uv00 + vec2(0.0, texelSize);
    vec2 uv11 = uv00 + vec2(texelSize, texelSize);

    float c00 = CompareShadowManual(uv00, cmpDepth);
    float c10 = CompareShadowManual(uv10, cmpDepth);
    float c01 = CompareShadowManual(uv01, cmpDepth);
    float c11 = CompareShadowManual(uv11, cmpDepth);

    float cx0 = mix(c00, c10, fracTexel.x);
    float cx1 = mix(c01, c11, fracTexel.x);
    return mix(cx0, cx1, fracTexel.y);
}

float SampleDirectionalShadow(vec4 positionLightCS, vec3 normalWS, vec3 lightDirWS)
{
    if (shadowCascadeCount == 0u || shadowStrength <= 0.0)
        return 1.0;
    if (positionLightCS.w <= 1e-6)
        return 1.0;

    vec3 posNDC = positionLightCS.xyz / positionLightCS.w;
    vec2 uv = vec2(posNDC.x * 0.5 + 0.5, posNDC.y * 0.5 + 0.5);
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
            visibility += CompareShadowManualBilinear(uv + offset, depth - bias);
        }
    }

    visibility *= (1.0 / 9.0);
    return mix(1.0, visibility, clamp(shadowStrength, 0.0, 1.0));
}

vec3 EvalSpecularGGX(vec3 N, vec3 V, vec3 L, vec3 albedoValue, float metallic, float roughness)
{
    vec3 H = SafeNormalize(V + L);
    float NoV = max(dot(N, V), 1e-4);
    float NoL = max(dot(N, L), 0.0);
    float NoH = max(dot(N, H), 0.0);
    float HoV = max(dot(H, V), 0.0);

    vec3 F0 = mix(vec3(0.04), albedoValue, metallic);
    vec3 F = F_Schlick(HoV, F0);
    float D = D_GGX(NoH, roughness);
    float G = G_Smith(NoV, NoL, roughness);
    return (D * G * F) / max(4.0 * NoV * NoL, 1e-4) * NoL;
}

vec3 SamplePrefilteredIBL(vec3 R, float roughness)
{
    vec3 dRdx = dFdx(R);
    vec3 dRdy = dFdy(R);
    float reflectionVariance = max(dot(dRdx, dRdx), dot(dRdy, dRdy));
    float reflectionMipBias = clamp(0.5 * log2(1.0 + reflectionVariance * 512.0), 0.0, 2.0);
    float lod = roughness * max(iblPrefilterLevels - 1.0, 0.0) + reflectionMipBias;
    return textureLod(tIBLPrefiltered, SafeNormalize(R), lod).rgb;
}

void main()
{
#ifdef KROM_BASECOLOR_MAP
    vec4 albedo4 = texture(albedo, vTexCoord) * baseColorFactor;
#else
    vec4 albedo4 = baseColorFactor;
#endif

    vec3 albedoValue = albedo4.rgb;
    float opacity = albedo4.a * opacityFactor;
#ifdef KROM_ALPHA_TEST
    if (opacity < alphaCutoff) discard;
#endif

    float metallic = clamp(metallicFactor, 0.0, 1.0);
    float roughness = clamp(roughnessFactor, 0.0, 1.0);
    float ao = 1.0;
#ifdef KROM_ORM_MAP
    vec3 ormSample = texture(orm, vTexCoord).rgb;
    ao        = mix(1.0, ormSample.r, clamp(occlusionStrength, 0.0, 1.0));
    roughness = clamp(ormSample.g * roughnessFactor, 0.0, 1.0);
    metallic  = clamp(ormSample.b * metallicFactor, 0.0, 1.0);
#elif defined(KROM_CHANNEL_MAP)
    // channel < 0 means "use scalar constant directly, no texture sampling for this slot"
    vec4 maskSample = texture(orm, vTexCoord);
    ao        = (occlusionChannel >= 0) ? clamp(SampleChannel(maskSample, occlusionChannel) * occlusionStrength + occlusionBias, 0.0, 1.0)
                                        : clamp(occlusionStrength, 0.0, 1.0);
    roughness = (roughnessChannel >= 0) ? clamp(SampleChannel(maskSample, roughnessChannel) * roughnessFactor + roughnessBias, 0.0, 1.0)
                                        : clamp(roughnessFactor, 0.0, 1.0);
    metallic  = (metallicChannel  >= 0) ? clamp(SampleChannel(maskSample, metallicChannel)  * metallicFactor  + metallicBias,  0.0, 1.0)
                                        : clamp(metallicFactor, 0.0, 1.0);
#endif
    roughness = clamp(roughness, 0.04, 1.0);

    vec3 geomN = SafeNormalize(vNormalWS);
    vec3 N = SampleNormal(vTexCoord, geomN, vTangentWS, vPositionWS);
    roughness = ApplySpecularAA(N, roughness);
    vec3 V = SafeNormalize(cameraPositionWS.xyz - vPositionWS);
    float NoV = max(dot(N, V), 1e-4);

    vec3 F0 = mix(vec3(0.04), albedoValue, metallic);
    vec3 F_a = F_SchlickRoughness(NoV, F0, roughness);
    vec3 kD_a = (vec3(1.0) - F_a) * (1.0 - metallic);

#ifdef KROM_IBL
    vec3 R = reflect(-V, N);
    vec3 irradiance = texture(tIBLIrradiance, N).rgb;
    vec3 prefilt = SamplePrefilteredIBL(R, roughness);
    vec2 brdfSamp = texture(tBRDFLut, vec2(NoV, roughness)).rg;
    vec3 specIBL = prefilt * (F_a * brdfSamp.x + brdfSamp.y);
    vec3 diffuseIBL = kD_a * albedoValue * irradiance;
    vec3 ambientIBL = (diffuseIBL + specIBL) * ao;
#else
    vec3 ambientIBL = ambientColor.rgb * ambientColor.a * albedoValue * ao;
#endif

    vec3 lighting = ambientIBL;
    float shadowVisibility = 1.0;
    if (lightCount > 0u && lights[0].params.w < 0.5)
        shadowVisibility = SampleDirectionalShadow(vPositionLightCS, geomN, SafeNormalize(-lights[0].positionWS.xyz));

    for (uint i = 0u; i < lightCount; ++i)
    {
        float type = lights[i].params.w;
        vec3 L = vec3(0.0, 1.0, 0.0);
        float attenuation = 1.0;

        if (type < 0.5)
        {
            L = SafeNormalize(-lights[i].positionWS.xyz);
            attenuation *= shadowVisibility;
        }
        else
        {
            vec3 toL = lights[i].positionWS.xyz - vPositionWS;
            float dist = length(toL);
            L = (dist > 1e-5) ? (toL / dist) : vec3(0.0, 1.0, 0.0);
            attenuation = CalcAttenuation(dist, lights[i].params.z);
            if (type > 1.5)
                attenuation *= CalcSpotAttenuation(L, SafeNormalize(lights[i].directionWS.xyz),
                                                   lights[i].params.x, lights[i].params.y);
        }

        vec3 lightColor = lights[i].colorIntensity.rgb * lights[i].colorIntensity.w * attenuation;
        float NoL_diff = max(dot(N, L), 0.0);
        vec3 diffuse = (kD_a * albedoValue / PI) * NoL_diff;
        vec3 specular = EvalSpecularGGX(N, V, L, albedoValue, metallic, roughness);
        lighting += (diffuse + specular) * lightColor;
    }

    vec3 emissiveColor = emissiveFactor.rgb;
#ifdef KROM_EMISSIVE_MAP
    emissiveColor *= texture(emissive, vTexCoord).rgb;
#endif

    fragColor = vec4(lighting + emissiveColor, opacity);
}
