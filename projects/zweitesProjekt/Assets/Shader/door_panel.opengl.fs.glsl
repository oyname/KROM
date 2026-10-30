// =============================================================================
// KROM Engine — Testraum: door_panel.opengl.fs.glsl
// Fragment Shader der Schiebetuer (OpenGL / GLSL 4.10), Gegenstueck zu
// door_panel.ps.hlsl.
//
//   • panelTex (Unit 13, eigene Textur aus der .mat) liefert das Blechmuster.
//   • Warnstreifen laufen diagonal ueber die Tuer, Farbe aus stripeColor,
//     Geschwindigkeit aus stripeSpeed (eigene Parameter).
//   • Einfaches Lambert-Licht vom ersten Richtungslicht plus Ambient.
//
// layout(binding = 13) braucht GLSL 4.20 oder GL_ARB_shading_language_420pack
// (der Kontext ist 4.1 core, daher die Extension).
// =============================================================================
#version 410 core
#extension GL_ARB_shading_language_420pack : require

struct GpuLightData { vec4 positionWS; vec4 directionWS; vec4 colorIntensity; vec4 params; };

layout(std140) uniform PerFrame
{
    mat4         viewMatrix;
    mat4         projMatrix;
    mat4         viewProjMatrix;
    mat4         invViewProjMatrix;
    vec4         cameraPositionWS;
    vec4         cameraForwardWS;
    vec4         screenSize;
    vec4         timeParams;   // x = Zeit (s)
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
};

layout(std140) uniform PerMaterial
{
    vec4  baseColorFactor;
    vec4  stripeColor;     // eigener Parameter
    float stripeSpeed;     // eigener Parameter
    float stripeWidth;     // eigener Parameter (Streifen pro UV-Einheit)
};

layout(binding = 13) uniform sampler2D panelTex;

in  vec3 vPositionWS;
in  vec3 vNormalWS;
in  vec2 vTexCoord;
out vec4 FragColor;

void main()
{
    float t = timeParams.x;
    vec2  uv = vTexCoord;
    // OpenGL-Texturursprung unten links: V fuers Sampling drehen.
    vec3 panel = texture(panelTex, vec2(uv.x, 1.0 - uv.y)).rgb * baseColorFactor.rgb;

    // Diagonale Warnstreifen nur am Rand (oben und unten 20 %).
    float band   = step(uv.y, 0.2) + step(0.8, uv.y);
    float stripe = step(0.5, fract((uv.x + uv.y) * stripeWidth + t * stripeSpeed));
    vec3  albedo = mix(panel, stripeColor.rgb, clamp(band, 0.0, 1.0) * stripe);

    // Lambert vom ersten Richtungslicht.
    vec3 N = normalize(vNormalWS);
    vec3 diffuse = ambientColor.rgb * 4.0;
    if (lightCount > 0u && lights[0].params.w < 0.5)
    {
        vec3 L = normalize(-lights[0].positionWS.xyz);
        diffuse += lights[0].colorIntensity.rgb * lights[0].colorIntensity.w * clamp(dot(N, L), 0.0, 1.0);
    }

    // Streifen leuchten leicht, damit die Tuer auch im Schatten auffaellt.
    vec3 glow = stripeColor.rgb * clamp(band, 0.0, 1.0) * stripe * 0.25;
    FragColor = vec4(albedo * diffuse + glow, 1.0);
}
