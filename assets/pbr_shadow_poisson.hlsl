// =============================================================================
// KROM Engine - assets/pbr_shadow_poisson.hlsl
// Shadow filter modes 3 and 4. Mode 3 (ShadowFilter::Poisson): 12-tap Poisson disc, rotated
// per pixel by interleaved gradient noise. Include after
// CompareShadowManualBilinear; that function clamps every tap to the atlas
// rect of the shadow view, so the kernel never reads a neighbouring view.
// Mode 4 (ShadowFilter::PCSS) follows below.
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

// -----------------------------------------------------------------------------
// Shadow filter mode 4 (ShadowFilter::PCSS): percentage-closer soft shadows.
// A blocker search estimates how far the occluder is from the receiver; the
// Poisson filter radius grows with that distance, so shadows are sharp at the
// contact and soft further away.
//
// featureParams[6] (kShadowFilterFrameParameterBlock, shadow add-on):
//   x = light size of the first shadow-casting light (ShadowSettings::lightSize)
// The depth-to-distance relation of the shadow view is derived from its
// view-projection (KromPreparePcss), so no near/far planes are needed.
// -----------------------------------------------------------------------------
#define shadowLightSize (featureParams[6].x)

static const float kPcssSearchRadiusTexels = 6.0f;   // blocker search
static const float kPcssMinRadiusTexels    = 1.0f;   // filter radius at the contact
static const float kPcssMaxRadiusTexels    = 12.0f;  // widest penumbra

// Filled by KromPreparePcss for the shadow view sampled next.
static bool  gPcssOrtho         = true;   // orthographic view (directional light)
static float gPcssDepthA        = 0.0f;   // perspective: depth = A + B / w
static float gPcssDepthB        = 0.0f;
static float gPcssReceiverW     = 1.0f;   // perspective: receiver distance along the view axis
static float gPcssDepthPerWorld = 1.0f;   // orthographic: depth change per world unit towards the light
static float gPcssUvPerWorld    = 0.0f;   // atlas UV per world unit at the receiver

// Call before SampleShadowAtlas with the view-projection and atlas rect of the
// shadow view, the receiver position and the direction to the light.
void KromPreparePcss(float4x4 shadowVP, float4 atlasRect, float3 receiverWS, float3 toLightDir)
{
    if (shadowFilterMode != 4u)
        return;

    const float delta = 0.1f;
    const float3 up = abs(toLightDir.y) < 0.9f ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f);
    const float3 side = normalize(cross(toLightDir, up));
    const float4 c0 = mul(shadowVP, float4(receiverWS, 1.0f));
    const float4 c1 = mul(shadowVP, float4(receiverWS + toLightDir * delta, 1.0f));
    const float4 c2 = mul(shadowVP, float4(receiverWS + side * delta, 1.0f));
    if (c0.w <= 1e-6f || c1.w <= 1e-6f || c2.w <= 1e-6f)
    {
        gPcssUvPerWorld = 0.0f;   // behind the view: SampleShadowAtlas returns lit anyway
        return;
    }

    const float z0 = c0.z / c0.w;
    const float z1 = c1.z / c1.w;
    const float invW0 = 1.0f / c0.w;
    const float invW1 = 1.0f / c1.w;
    gPcssReceiverW = c0.w;
    gPcssDepthPerWorld = abs(z0 - z1) / delta;
    gPcssOrtho = abs(invW0 - invW1) <= 1e-5f * abs(invW0);
    if (!gPcssOrtho)
    {
        gPcssDepthB = (z0 - z1) / (invW0 - invW1);
        gPcssDepthA = z0 - gPcssDepthB * invW0;
    }
    const float2 uvStep = (c2.xy / c2.w - c0.xy / c0.w) * 0.5f * atlasRect.zw;
    gPcssUvPerWorld = length(uvStep) / delta;
}

float SampleShadowPcss(float2 atlasUv, float depth, float2 dDepth_dUV, float baseBias, float4 atlasRect)
{
    const float texel = max(shadowTexelSize, 1e-8f);
    const float angle = KromInterleavedGradientNoise(gKromShadowPixelPos) * 6.28318530718f;
    float sinA;
    float cosA;
    sincos(angle, sinA, cosA);
    const float2 minUv = atlasRect.xy + (0.5f * texel).xx;
    const float2 maxUv = atlasRect.xy + atlasRect.zw - (0.5f * texel).xx;

    // 1. Blocker search: mean depth of the occluders around the receiver.
    const float searchRadius = kPcssSearchRadiusTexels * texel;
    float blockerSum = 0.0f;
    float blockerCount = 0.0f;
    [loop]
    for (uint tap = 0u; tap < kShadowPoissonTapCount; ++tap)
    {
        const float2 disk = kShadowPoissonDisk[tap];
        const float2 offset = float2(disk.x * cosA - disk.y * sinA,
                                     disk.x * sinA + disk.y * cosA) * searchRadius;
        const float occluderDepth = shadowMap.SampleLevel(sPoint, clamp(atlasUv + offset, minUv, maxUv), 0).r;
        const float depthRef = depth + dot(dDepth_dUV, offset) - baseBias;
        if (occluderDepth < depthRef)
        {
            blockerSum += occluderDepth;
            blockerCount += 1.0f;
        }
    }
    if (blockerCount < 0.5f)
        return 1.0f;

    // 2. Penumbra width in world units, then in shadow map texels.
    const float blockerDepth = blockerSum / blockerCount;
    float penumbraWorld = 0.0f;
    if (gPcssOrtho)
    {
        // Directional light: lightSize is the tangent of the light's angular size.
        penumbraWorld = max(depth - blockerDepth, 0.0f) / max(gPcssDepthPerWorld, 1e-8f) * shadowLightSize;
    }
    else
    {
        // Spot / point light: lightSize is the size of the source in world units.
        const float denom = blockerDepth - gPcssDepthA;
        float blockerW = gPcssDepthB / (abs(denom) > 1e-7f ? denom : -1e-7f);
        blockerW = clamp(blockerW, 1e-3f, gPcssReceiverW);
        penumbraWorld = (gPcssReceiverW - blockerW) * shadowLightSize / blockerW;
    }
    const float radiusTexels = clamp(penumbraWorld * gPcssUvPerWorld / texel,
                                     kPcssMinRadiusTexels, kPcssMaxRadiusTexels);

    // 3. Filter with that radius.
    const float radius = radiusTexels * texel;
    float visibility = 0.0f;
    [loop]
    for (uint filterTap = 0u; filterTap < kShadowPoissonTapCount; ++filterTap)
    {
        const float2 disk = kShadowPoissonDisk[filterTap];
        const float2 offset = float2(disk.x * cosA - disk.y * sinA,
                                     disk.x * sinA + disk.y * cosA) * radius;
        const float depthRef = depth + dot(dDepth_dUV, offset) - baseBias;
        visibility += CompareShadowManualBilinear(atlasUv + offset, depthRef, atlasRect);
    }
    return visibility * (1.0f / 12.0f);
}
