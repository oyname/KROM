#ifndef KROM_SHADOW_RECEIVER_HLSL
#define KROM_SHADOW_RECEIVER_HLSL

// Shared by Lit, PBR and Forward+. UV axes need not match screen axes.
float2 KromShadowDepthGradient(float depth, float2 uv)
{
    const float2 dx = ddx(uv);
    const float2 dy = ddy(uv);
    const float2 dz = float2(ddx(depth), ddy(depth));
    const float det = dx.x * dy.y - dx.y * dy.x;
    const float scale = max(length(dx) * length(dy), 1e-20f);
    if (abs(det) <= 1e-5f * scale)
        return float2(0.f, 0.f);
    return float2(dy.y * dz.x - dx.y * dz.y,
                  dx.x * dz.y - dy.x * dz.x) / det;
}

float KromShadowReceiverBias(float biasValue, float4x4 shadowVP)
{
    // Orthographic (directional) bias is in world units, independent of the
    // cascade depth span. Perspective lights retain their depth-buffer bias.
    const float3 wRow = float3(shadowVP[3][0], shadowVP[3][1], shadowVP[3][2]);
    if (dot(wRow, wRow) < 1e-12f)
    {
        const float3 zRow = float3(shadowVP[2][0], shadowVP[2][1], shadowVP[2][2]);
        return max(biasValue * length(zRow), 1e-7f);
    }
    return max(biasValue, 0.00005f);
}

// Largest shift of a filter tap's reference depth along the receiver plane, in
// shadow-map depth: kKromShadowPlaneLimitTexels texels (world size at the
// receiver) along the light axis. The receiver-plane term grows with
// 1 / tan(angle between surface and light); without a limit a wall almost
// parallel to the light moves the reference depth further than the occluder in
// front of it is thick, and its filter taps read past that occluder (light
// leak). SampleShadowAtlas sets the limit per shadow view before filtering.
static const float kKromShadowPlaneLimitTexels = 2.0f;
static float gKromShadowPlaneLimit = 1e30f;

float KromShadowPlaneLimit(float4x4 shadowVP, float tileTexels, float depthNdc)
{
    const float3 xRow = float3(shadowVP[0][0], shadowVP[0][1], shadowVP[0][2]);
    const float3 zRow = float3(shadowVP[2][0], shadowVP[2][1], shadowVP[2][2]);
    const float3 wRow = float3(shadowVP[3][0], shadowVP[3][1], shadowVP[3][2]);
    const float wLen = length(wRow);
    // Depth per world unit along the light axis times the world size of one
    // texel; for perspective views the distance to the light cancels out.
    const float depthPerTexel = wLen < 1e-6f
        ? length(zRow)
        : abs(dot(zRow, wRow) / wLen - depthNdc * wLen);
    return kKromShadowPlaneLimitTexels * 2.0f * depthPerTexel / max(length(xRow) * tileTexels, 1e-6f);
}

// Receiver-plane depth change of a filter tap at uvOffset, limited.
float KromShadowPlaneTerm(float2 dDepth_dUV, float2 uvOffset)
{
    return clamp(dot(dDepth_dUV, uvOffset), -gKromShadowPlaneLimit, gKromShadowPlaneLimit);
}

#endif // KROM_SHADOW_RECEIVER_HLSL
