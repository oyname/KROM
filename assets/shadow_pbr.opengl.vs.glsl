// =============================================================================
// KROM Engine — assets/shadow_pbr.opengl.vs.glsl
// Vertex Shader: Shadow-Pass für PBR-Materialien (OpenGL / GLSL 4.10)
// Transformiert UV mit uvScale/uvOffset aus dem PBR-PerMaterial-Block.
// =============================================================================
#version 410 core

layout(location = 0) in vec3 aPosition;
#ifdef KROM_ALPHA_TEST
layout(location = 4) in vec2 aTexCoord;
out vec2 vTexCoord;
#endif

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
    uint         debugFlags;
    uint         shadowFilterMode;
    float        _shadowPad;
    vec4         shadowLightMeta[4];
    vec4         shadowLightExtra[4];
    vec4         shadowViewRect[16];
    mat4         shadowViewProjArray[16];
    uint         shadowLightCount;
    uint         shadowViewCount;
    vec2         _shadowArrayPad;
};

layout(std140) uniform PerObject
{
    mat4 worldMatrix;
    mat4 worldMatrixInvT;
    vec4 entityId;
};

#ifdef KROM_ALPHA_TEST
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
    vec2  uvScale;
    vec2  uvOffset;
};
#endif

void main()
{
    gl_Position = shadowViewProj * worldMatrix * vec4(aPosition, 1.0);
#ifdef KROM_ALPHA_TEST
    vec2 uv = vec2(aTexCoord.x, 1.0 - aTexCoord.y);
    vTexCoord = uv * uvScale + uvOffset;
#endif
}
