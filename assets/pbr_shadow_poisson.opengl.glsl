// =============================================================================
// KROM Engine - assets/pbr_shadow_poisson.opengl.glsl
// Shadow filter modes 3 and 4. Mode 3 (ShadowFilter::Poisson): 12-tap Poisson disc, rotated
// per pixel by interleaved gradient noise. Include after
// CompareShadowManualBilinear; that function clamps every tap to the atlas
// rect of the shadow view, so the kernel never reads a neighbouring view.
// Mode 4 (ShadowFilter::PCSS) follows below.
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

const float kPcssSearchRadiusTexels = 6.0;   // blocker search
const float kPcssMinRadiusTexels    = 1.0;   // filter radius at the contact
const float kPcssMaxRadiusTexels    = 12.0;  // widest penumbra

// Filled by KromPreparePcss for the shadow view sampled next.
bool  gPcssOrtho         = true;   // orthographic view (directional light)
float gPcssDepthA        = 0.0;    // perspective: depth = A + B / w
float gPcssDepthB        = 0.0;
float gPcssReceiverW     = 1.0;    // perspective: receiver distance along the view axis
float gPcssDepthPerWorld = 1.0;    // orthographic: depth change per world unit towards the light
float gPcssUvPerWorld    = 0.0;    // atlas UV per world unit at the receiver

// Call before SampleShadowAtlas with the view-projection and atlas rect of the
// shadow view, the receiver position and the direction to the light.
void KromPreparePcss(mat4 shadowVP, vec4 atlasRect, vec3 receiverWS, vec3 toLightDir)
{
    if (shadowFilterMode != 4u)
        return;

    const float delta = 0.1;
    vec3 up = abs(toLightDir.y) < 0.9 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 side = normalize(cross(toLightDir, up));
    vec4 c0 = shadowVP * vec4(receiverWS, 1.0);
    vec4 c1 = shadowVP * vec4(receiverWS + toLightDir * delta, 1.0);
    vec4 c2 = shadowVP * vec4(receiverWS + side * delta, 1.0);
    if (c0.w <= 1e-6 || c1.w <= 1e-6 || c2.w <= 1e-6)
    {
        gPcssUvPerWorld = 0.0;   // behind the view: SampleShadowAtlas returns lit anyway
        return;
    }

    // Same depth range as SampleShadowAtlas (0..1).
    float z0 = c0.z / c0.w * 0.5 + 0.5;
    float z1 = c1.z / c1.w * 0.5 + 0.5;
    float invW0 = 1.0 / c0.w;
    float invW1 = 1.0 / c1.w;
    gPcssReceiverW = c0.w;
    gPcssDepthPerWorld = abs(z0 - z1) / delta;
    gPcssOrtho = abs(invW0 - invW1) <= 1e-5 * abs(invW0);
    if (!gPcssOrtho)
    {
        gPcssDepthB = (z0 - z1) / (invW0 - invW1);
        gPcssDepthA = z0 - gPcssDepthB * invW0;
    }
    vec2 uvStep = (c2.xy / c2.w - c0.xy / c0.w) * 0.5 * atlasRect.zw;
    gPcssUvPerWorld = length(uvStep) / delta;
}

float SampleShadowPcss(vec2 atlasUv, float depth, vec2 dDepth_dUV, float baseBias, vec4 atlasRect)
{
    float texel = max(shadowTexelSize, 1e-8);
    vec2 pixelPos = vec2(gl_FragCoord.x, screenSize.y - gl_FragCoord.y);
    float angle = KromInterleavedGradientNoise(pixelPos) * 6.28318530718;
    float sinA = sin(angle);
    float cosA = cos(angle);
    vec2 minUv = atlasRect.xy + vec2(0.5 * texel);
    vec2 maxUv = atlasRect.xy + atlasRect.zw - vec2(0.5 * texel);

    // 1. Blocker search: mean depth of the occluders around the receiver.
    float searchRadius = kPcssSearchRadiusTexels * texel;
    float blockerSum = 0.0;
    float blockerCount = 0.0;
    for (int tap = 0; tap < kShadowPoissonTapCount; ++tap)
    {
        vec2 disk = kShadowPoissonDisk[tap];
        vec2 offset = vec2(disk.x * cosA - disk.y * sinA,
                           disk.x * sinA + disk.y * cosA) * searchRadius;
        float occluderDepth = textureLod(shadowMapRaw, clamp(atlasUv + offset, minUv, maxUv), 0.0).r;
        float depthRef = depth + dot(dDepth_dUV, offset) - baseBias;
        if (occluderDepth < depthRef)
        {
            blockerSum += occluderDepth;
            blockerCount += 1.0;
        }
    }
    if (blockerCount < 0.5)
        return 1.0;

    // 2. Penumbra width in world units, then in shadow map texels.
    float blockerDepth = blockerSum / blockerCount;
    float penumbraWorld = 0.0;
    if (gPcssOrtho)
    {
        // Directional light: lightSize is the tangent of the light's angular size.
        penumbraWorld = max(depth - blockerDepth, 0.0) / max(gPcssDepthPerWorld, 1e-8) * shadowLightSize;
    }
    else
    {
        // Spot / point light: lightSize is the size of the source in world units.
        float denom = blockerDepth - gPcssDepthA;
        float blockerW = gPcssDepthB / (abs(denom) > 1e-7 ? denom : -1e-7);
        blockerW = clamp(blockerW, 1e-3, gPcssReceiverW);
        penumbraWorld = (gPcssReceiverW - blockerW) * shadowLightSize / blockerW;
    }
    float radiusTexels = clamp(penumbraWorld * gPcssUvPerWorld / texel,
                               kPcssMinRadiusTexels, kPcssMaxRadiusTexels);

    // 3. Filter with that radius.
    float radius = radiusTexels * texel;
    float visibility = 0.0;
    for (int filterTap = 0; filterTap < kShadowPoissonTapCount; ++filterTap)
    {
        vec2 disk = kShadowPoissonDisk[filterTap];
        vec2 offset = vec2(disk.x * cosA - disk.y * sinA,
                           disk.x * sinA + disk.y * cosA) * radius;
        float depthRef = depth + dot(dDepth_dUV, offset) - baseBias;
        visibility += CompareShadowManualBilinear(atlasUv + offset, depthRef, atlasRect);
    }
    return visibility * (1.0 / 12.0);
}
