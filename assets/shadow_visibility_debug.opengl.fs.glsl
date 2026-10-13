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

uniform sampler2D shadowMapRaw;

in vec3 vNormalWS;
in vec4 vPositionLightCS;
layout(location = 0) out vec4 fragColor;

vec3 SafeNormalize(vec3 v)
{
    float len2 = dot(v, v);
    return (len2 > 1e-12) ? (v * inversesqrt(len2)) : vec3(0.0, 0.0, 1.0);
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

    float ddx_d  = dFdx(depth);
    float ddy_d  = dFdy(depth);
    float ddx_ux = dFdx(uv.x);
    float ddy_uy = dFdy(uv.y);
    vec2 dDepth_dUV = vec2(
        abs(ddx_ux) > 1e-7 ? ddx_d / ddx_ux : 0.0,
        abs(ddy_uy) > 1e-7 ? ddy_d / ddy_uy : 0.0
    );
    dDepth_dUV = clamp(dDepth_dUV, -1.0, 1.0);

    const vec2 kPoissonDisk[8] = vec2[8](
        vec2(-0.94201624, -0.39906216),
        vec2( 0.94558609, -0.76890725),
        vec2(-0.09418410, -0.92938870),
        vec2( 0.34495938,  0.29387760),
        vec2(-0.91588581,  0.45771432),
        vec2(-0.81544232, -0.87912464),
        vec2(-0.38277543,  0.27676845),
        vec2( 0.97484398,  0.75648379)
    );

    float filterRadius = shadowTexelSize * 2.0;
    float visibility = 0.0;
    for (int i = 0; i < 8; ++i)
    {
        vec2  offset   = kPoissonDisk[i] * filterRadius;
        float cmpDepth = depth + dot(dDepth_dUV, offset) - shadowBias;
        visibility += CompareShadowManualBilinear(uv + offset, cmpDepth);
    }

    visibility *= (1.0 / 8.0);
    return mix(1.0, visibility, clamp(shadowStrength, 0.0, 1.0));
}

void main()
{
    vec3 N = SafeNormalize(vNormalWS);
    float NoL = 0.0;
    float shadowVisibility = 1.0;
    bool hasDirectional = (lightCount > 0u && lights[0].params.w < 0.5);
    if (hasDirectional)
    {
        vec3 L = SafeNormalize(-lights[0].positionWS.xyz);
        NoL = clamp(dot(N, L), 0.0, 1.0);
        shadowVisibility = SampleDirectionalShadow(vPositionLightCS, N, L);
    }

    float shadowAmount = clamp(1.0 - shadowVisibility, 0.0, 1.0);
    vec3 litColor = vec3(0.12, 0.55, 0.18);
    vec3 shadowColor = vec3(1.0, 0.18, 0.05);
    vec3 debugColor = mix(litColor, shadowColor, pow(shadowAmount, 0.5));
    debugColor *= mix(0.35, 1.0, NoL);
    fragColor = vec4(debugColor, 1.0);
}
