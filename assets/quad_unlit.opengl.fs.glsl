// =============================================================================
// KROM Engine — assets/quad_unlit.opengl.fs.glsl
// Fragment Shader: texturiertes, unbelichtetes Quad (OpenGL / GLSL 4.10)
// =============================================================================
#version 410 core

// ---------------------------------------------------------------------------
// UBO 2 – PerMaterial
// ---------------------------------------------------------------------------
layout(std140) uniform PerMaterial
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
#ifdef KROM_MATERIAL_GRAPH
    vec4 graphOutputs;
    vec4 graphNodes[16];
    vec4 graphUV[8];
#endif
};

// ---------------------------------------------------------------------------
// Texturen & Sampler
// ---------------------------------------------------------------------------
uniform sampler2D uAlbedo;
uniform sampler2D emissive;

in  vec3 vPositionWS;
in  vec3 vNormalWS;
in  vec2 vTexCoord;

layout(location = 0) out vec4 fragColor;

#define KROM_GRAPH_DECLARE_TIME
#include "pbr_material_graph.opengl.glsl"

void main()
{
    vec2 uv = vTexCoord;
#ifdef KROM_MATERIAL_GRAPH
    EvaluateMaterialGraph(vTexCoord, vPositionWS);
    uv = KromGraphDirectUV(vTexCoord);
#endif
    // MaterialFeatureFlag::BaseColorTexture = 1u << 1 = 2
    // uAlbedo ist immer valide gebunden (ShaderRuntime: Fallback White-Textur).
#ifdef KROM_BASECOLOR_MAP
    vec4 albedo = texture(uAlbedo, uv) * baseColorFactor;
#else
    vec4 albedo;
    if ((materialFeatureMask & 2) != 0)
        albedo = texture(uAlbedo, uv) * baseColorFactor;
    else
        albedo = baseColorFactor;
#endif

#ifdef KROM_MATERIAL_GRAPH
    vec3 graphEmission;
    ApplyMaterialGraph(albedo, graphEmission);
#endif

#ifdef KROM_ALPHA_TEST
    if (albedo.a < alphaCutoff) discard;
#endif

    albedo.a *= opacityFactor;

    vec3 emissiveColor = emissiveFactor.rgb;
#ifdef KROM_EMISSIVE_MAP
    emissiveColor *= texture(emissive, uv).rgb;
#endif

#ifdef KROM_MATERIAL_GRAPH
    if (graphOutputs.y >= 0.0)
        emissiveColor = graphEmission * emissiveFactor.rgb;
#endif

    fragColor = vec4(albedo.rgb + emissiveColor, albedo.a);
}
