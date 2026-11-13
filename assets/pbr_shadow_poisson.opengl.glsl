// =============================================================================
// KROM Engine - assets/pbr_shadow_poisson.opengl.glsl
// Shadow filter mode 3 (ShadowFilter::Poisson): 12-tap Poisson disc, rotated
// per pixel by interleaved gradient noise. Include after
// CompareShadowManualBilinear; that function clamps every tap to the atlas
// rect of the shadow view, so the kernel never reads a neighbouring view.
// HLSL twin: pbr_shadow_poisson.hlsl.
// =============================================================================

// Kernel radius in shadow map texels. Each tap is a bilinear 2x2 compare, so
// the filtered footprint is about one texel wider.
const float kShadowPoissonRadiusTexels = 2.0;
const int   kShadowPoissonTapCount     = 12;

const vec2 kShadowPoissonDisk[12] = vec2[12](
    vec2(-0.326212, -0.405805),
    vec2(-0.840144, -0.073580),
    vec2(-0.695914,  0.457137),
    vec2(-0.203345,  0.620716),
    vec2( 0.962340, -0.194983),
    vec2( 0.473434, -0.480026),
    vec2( 0.519456,  0.767022),
    vec2( 0.185461, -0.893124),
    vec2( 0.507431,  0.064425),
    vec2( 0.896420,  0.412458),
    vec2(-0.321940, -0.932615),
    vec2(-0.791559, -0.597705));

// Jimenez 2014, "Next Generation Post Processing in Call of Duty".
float KromInterleavedGradientNoise(vec2 pixelPos)
{
    return fract(52.9829189 * fract(dot(pixelPos, vec2(0.06711056, 0.00583715))));
}

float SampleShadowPoisson(vec2 atlasUv, float depth, vec2 dDepth_dUV, float baseBias, vec4 atlasRect)
{
    // Top-left pixel origin as in the HLSL version (SV_Position).
    vec2 pixelPos = vec2(gl_FragCoord.x, screenSize.y - gl_FragCoord.y);
    float angle = KromInterleavedGradientNoise(pixelPos) * 6.28318530718;
    float sinA = sin(angle);
    float cosA = cos(angle);
    float radius = kShadowPoissonRadiusTexels * shadowTexelSize;

    float visibility = 0.0;
    for (int tap = 0; tap < kShadowPoissonTapCount; ++tap)
    {
        vec2 disk = kShadowPoissonDisk[tap];
        vec2 offset = vec2(disk.x * cosA - disk.y * sinA,
                           disk.x * sinA + disk.y * cosA) * radius;
        float depthRef = depth + dot(dDepth_dUV, offset) - baseBias;
        visibility += CompareShadowManualBilinear(atlasUv + offset, depthRef, atlasRect);
    }
    return visibility * (1.0 / 12.0);
}
