// =============================================================================
// KROM Engine — assets/pbr_lit.opengl.vs.glsl
// Vertex Shader: PBR-Lit (OpenGL / GLSL 4.10)
// =============================================================================
#version 410 core
layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec4 aTangent;
layout(location = 4) in vec2 aTexCoord;

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

layout(std140) uniform PerObject
{
    mat4 worldMatrix;
    mat4 worldMatrixInvT;
    vec4 entityId;
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
    float _pad0;
};

out vec3 vPositionWS;
out vec3 vNormalWS;
out vec4 vTangentWS;
out vec2 vTexCoord;
out vec4 vPositionLightCS;

vec3 SafeNormalizeVS(vec3 v)
{
    float len2 = dot(v, v);
    return (len2 > 1e-12) ? (v * inversesqrt(len2)) : vec3(1.0, 0.0, 0.0);
}

void main()
{
    vec4 posWS = worldMatrix * vec4(aPosition, 1.0);
    mat3 normalMat = mat3(worldMatrixInvT);
    mat3 tangentMat = mat3(worldMatrix);
    vec3 N = SafeNormalizeVS(normalMat * aNormal);
    vec3 T = SafeNormalizeVS(tangentMat * aTangent.xyz);
    T = SafeNormalizeVS(T - N * dot(N, T));

    gl_Position = viewProjMatrix * posWS;
    vPositionWS = posWS.xyz;
    vNormalWS = N;
    vTangentWS = vec4(T, aTangent.w);
    vTexCoord = aTexCoord;
    vPositionLightCS = shadowViewProj * posWS;
}
