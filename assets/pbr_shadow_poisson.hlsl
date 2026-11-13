// =============================================================================
// KROM Engine - assets/pbr_shadow_poisson.hlsl
// Shadow filter mode 3 (ShadowFilter::Poisson): 12-tap Poisson disc, rotated
// per pixel by interleaved gradient noise. Include after
// CompareShadowManualBilinear; that function clamps every tap to the atlas
// rect of the shadow view, so the kernel never reads a neighbouring view.
// GLSL twin: pbr_shadow_poisson.opengl.glsl.
// =============================================================================

// Pixel position (SV_Position.xy) of the fragment; main() sets it first.
static float2 gKromShadowPixelPos = float2(0.0f, 0.0f);

// Kernel radius in shadow map texels. Each tap is a bilinear 2x2 compare, so
// the filtered footprint is about one texel wider.
static const float kShadowPoissonRadiusTexels = 2.0f;
static const uint  kShadowPoissonTapCount     = 12u;

static const float2 kShadowPoissonDisk[12] =
{
    float2(-0.326212f, -0.405805f),
    float2(-0.840144f, -0.073580f),
    float2(-0.695914f,  0.457137f),
    float2(-0.203345f,  0.620716f),
    float2( 0.962340f, -0.194983f),
    float2( 0.473434f, -0.480026f),
    float2( 0.519456f,  0.767022f),
    float2( 0.185461f, -0.893124f),
    float2( 0.507431f,  0.064425f),
    float2( 0.896420f,  0.412458f),
    float2(-0.321940f, -0.932615f),
    float2(-0.791559f, -0.597705f)
};

// Jimenez 2014, "Next Generation Post Processing in Call of Duty".
float KromInterleavedGradientNoise(float2 pixelPos)
{
    return frac(52.9829189f * frac(dot(pixelPos, float2(0.06711056f, 0.00583715f))));
}

float SampleShadowPoisson(float2 atlasUv, float depth, float2 dDepth_dUV, float baseBias, float4 atlasRect)
{
    const float angle = KromInterleavedGradientNoise(gKromShadowPixelPos) * 6.28318530718f;
    float sinA;
    float cosA;
    sincos(angle, sinA, cosA);
    const float radius = kShadowPoissonRadiusTexels * shadowTexelSize;

    float visibility = 0.0f;
    [loop]
    for (uint tap = 0u; tap < kShadowPoissonTapCount; ++tap)
    {
        const float2 disk = kShadowPoissonDisk[tap];
        const float2 offset = float2(disk.x * cosA - disk.y * sinA,
                                     disk.x * sinA + disk.y * cosA) * radius;
        const float depthRef = depth + dot(dDepth_dUV, offset) - baseBias;
        visibility += CompareShadowManualBilinear(atlasUv + offset, depthRef, atlasRect);
    }
    return visibility * (1.0f / 12.0f);
}
