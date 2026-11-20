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
