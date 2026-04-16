#version 450
// =============================================================================
// KROM Engine – assets/pbr_lit.vulkan.fs.glsl
// Fragment Shader: PBR-Lit (Vulkan / GLSL 4.50)
//
// Farb-/Lichtvertrag:
//   - BaseColor / Emissive werden ueber Hardware-sRGB gelesen.
//   - ORM / Normal / BRDF LUT / HDR-IBL bleiben linear.
//   - Dieser Pass schreibt lineares HDR in den Scene-Color-Render-Target.
//   - Tonemap + finale sRGB-Ausgabe passieren spaeter im Present-/Tonemap-Pass.
// =============================================================================

layout(location = 0) out vec4 FragColor;
layout(location = 0) in vec3 outPositionWS;
layout(location = 1) in vec3 outNormalWS;
layout(location = 2) in vec4 outTangentWS;
layout(location = 3) in vec2 outUV;

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
    // IBL prefilter LOD range — packed by FrameConstantStage from kIBLPrefilterMipCount.
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

layout(set = 0, binding = 16) uniform texture2D  albedo;
layout(set = 0, binding = 17) uniform texture2D  normal;
layout(set = 0, binding = 18) uniform texture2D  orm;
layout(set = 0, binding = 19) uniform texture2D  emissive;
layout(set = 0, binding = 21) uniform texture2D  tIBLIrradiance;
layout(set = 0, binding = 22) uniform texture2D  tIBLPrefiltered;
layout(set = 0, binding = 23) uniform texture2D  tBRDFLut;

layout(set = 0, binding = 32) uniform sampler sLinearWrap;
layout(set = 0, binding = 33) uniform sampler sLinearClamp;

const float PI = 3.14159265358979323846;
const float TWO_PI = 6.28318530717958647692;

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

vec3 EvalBRDF_GGX(vec3 N, vec3 V, vec3 L, vec3 albedo, float metallic, float roughness)
{
    vec3 H = normalize(V + L);
    float NoV = max(dot(N, V), 1e-4);
    float NoL = max(dot(N, L), 0.0);
    float NoH = max(dot(N, H), 0.0);
    float HoV = max(dot(H, V), 0.0);

    vec3 F0 = mix(vec3(0.04), albedo, metallic);
    float D = D_GGX(NoH, roughness);
    float G = G_Smith(NoV, NoL, roughness);
    vec3 F = F_Schlick(HoV, F0);
    vec3 kD = (1.0 - F) * (1.0 - metallic);
    vec3 diff = kD * albedo / PI;
    vec3 spec = (D * G * F) / max(4.0 * NoV * NoL, 1e-4);
    return (diff + spec) * NoL;
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

vec3 SafeNormalize(vec3 v)
{
    float len2 = dot(v, v);
    return (len2 > 1e-12) ? (v * inversesqrt(len2)) : vec3(0.0, 0.0, 1.0);
}

// BuildTBN — matches the DX11/HLSL BuildTBN semantics exactly:
//   1. Degenerate-tangent guard: if dot(T,T) <= 1e-10, build orthonormal fallback via up×N.
//   2. Re-orthogonalise T against the (already normalized) N from the PS.
//   3. B = cross(N, T) * handedness.
mat3 BuildTBN(vec3 N, vec4 tangentWS)
{
    vec3 T = tangentWS.xyz;
    float handedness = tangentWS.w;
    if (dot(T, T) <= 1e-10)
    {
        vec3 up = (abs(N.y) < 0.999) ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
        T = normalize(cross(up, N));
    }
    else
    {
        T = normalize(T - N * dot(N, T));
    }
    vec3 B = normalize(cross(N, T)) * handedness;
    return mat3(T, B, N);
}

vec3 SampleNormal(vec2 uv, vec3 baseNormalWS, vec4 tangentWS)
{
#ifdef KROM_NORMAL_MAP
    vec3 mapN = texture(sampler2D(normal, sLinearWrap), uv).xyz * 2.0 - 1.0;
    mat3 tbn = BuildTBN(baseNormalWS, tangentWS);
    return SafeNormalize(tbn * mapN);
#else
    return baseNormalWS;
#endif
}

vec2 SphereUV(vec3 dir)
{
    dir = normalize(dir);
    float phi = atan(dir.z, dir.x);
    float u = 0.5 + phi / TWO_PI;
    float v = acos(clamp(dir.y, -1.0, 1.0)) / PI;
    return vec2(fract(u), clamp(v, 0.0, 1.0));
}

void main()
{
#ifdef KROM_BASECOLOR_MAP
    vec4 albedo4 = texture(sampler2D(albedo, sLinearWrap), outUV) * perMaterial.baseColorFactor;
#else
    vec4 albedo4 = perMaterial.baseColorFactor;
#endif

    vec3 albedo = albedo4.rgb;
    float opacity = albedo4.a * perMaterial.opacityFactor;
#ifdef KROM_ALPHA_TEST
    if (opacity < perMaterial.alphaCutoff) discard;
#endif

#ifdef KROM_UNLIT
    vec3 emissiveColor = perMaterial.emissiveFactor.rgb;
#ifdef KROM_EMISSIVE_MAP
    emissiveColor *= texture(sampler2D(emissive, sLinearWrap), outUV).rgb;
#endif
    FragColor = vec4(albedo + emissiveColor, opacity);
    return;
#endif

    float ao = 1.0;
    float roughness = clamp(perMaterial.roughnessFactor, 0.0, 1.0);
    float metallic = clamp(perMaterial.metallicFactor, 0.0, 1.0);
#ifdef KROM_ORM_MAP
    vec3 ormSample = texture(sampler2D(orm, sLinearWrap), outUV).rgb;
    ao = mix(1.0, ormSample.r, clamp(perMaterial.occlusionStrength, 0.0, 1.0));
    roughness = clamp(ormSample.g * perMaterial.roughnessFactor, 0.0, 1.0);
    metallic = clamp(ormSample.b * perMaterial.metallicFactor, 0.0, 1.0);
#endif
    roughness = clamp(roughness, 0.04, 1.0);

    vec3 N = SafeNormalize(outNormalWS);
    N = SampleNormal(outUV, N, outTangentWS);

    vec3 V = SafeNormalize(perFrame.cameraPositionWS.xyz - outPositionWS);
    float NoV = max(dot(N, V), 1e-4);
    vec3 F0 = mix(vec3(0.04), albedo, metallic);
    vec3 F_a = F_SchlickRoughness(NoV, F0, roughness);
    vec3 kD_a = (1.0 - F_a) * (1.0 - metallic);

#ifdef KROM_IBL
    vec3 R = reflect(-V, N);
    vec3 irradiance = texture(sampler2D(tIBLIrradiance, sLinearClamp), SphereUV(N)).rgb;
    vec3 prefilt = textureLod(sampler2D(tIBLPrefiltered, sLinearClamp), SphereUV(R), roughness * perFrame.iblPrefilterLevels).rgb;
    vec2 brdfSamp = texture(sampler2D(tBRDFLut, sLinearClamp), vec2(NoV, roughness)).rg;
    vec3 specIBL = prefilt * (F_a * brdfSamp.x + brdfSamp.y);
    vec3 indirect = (kD_a * albedo * irradiance + specIBL) * ao;
#else
    vec3 indirect = kD_a * albedo * ao;
#endif
    vec3 ambientTerm = kD_a * albedo * (perFrame.ambientColor.rgb * perFrame.ambientColor.a) * ao;

    vec3 Lo = indirect + ambientTerm;
    for (uint i = 0u; i < perFrame.lightCount; ++i)
    {
        float lightType = perFrame.lights[i].params.w;
        vec3 lColor = perFrame.lights[i].colorIntensity.rgb * perFrame.lights[i].colorIntensity.w;
        vec3 L;
        float atten = 1.0;

        if (lightType < 0.5)
        {
            L = SafeNormalize(-perFrame.lights[i].positionWS.xyz);
        }
        else
        {
            vec3 toLight = perFrame.lights[i].positionWS.xyz - outPositionWS;
            float dist = length(toLight);
            L = (dist > 1e-5) ? (toLight / dist) : vec3(0.0, 1.0, 0.0);
            atten = CalcAttenuation(dist, perFrame.lights[i].params.z);
            if (lightType > 1.5)
            {
                atten *= CalcSpotAttenuation(L, normalize(perFrame.lights[i].directionWS.xyz), perFrame.lights[i].params.x, perFrame.lights[i].params.y);
            }
        }

        Lo += EvalBRDF_GGX(N, V, L, albedo, metallic, roughness) * lColor * atten;
    }

    vec3 emissiveColor = perMaterial.emissiveFactor.rgb;
#ifdef KROM_EMISSIVE_MAP
    emissiveColor *= texture(sampler2D(emissive, sLinearWrap), outUV).rgb;
#endif

    FragColor = vec4(Lo + emissiveColor, opacity);
}
