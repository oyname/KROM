float2 KromPbrUV(float2 meshUV, float3 scaledLocalPosition, float tileSize)
{
    if (tileSize <= 0.0f)
        return meshUV;

    const float3 axis = abs(cross(ddx(scaledLocalPosition), ddy(scaledLocalPosition)));
    const float3 p = scaledLocalPosition / tileSize;
    if (axis.y >= max(axis.x, axis.z))
        return p.xz;
    if (axis.x >= axis.z)
        return p.zy;
    return p.xy;
}
