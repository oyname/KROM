vec2 KromPbrUV(vec2 meshUV, vec3 scaledLocalPosition, float tileSize)
{
    if (tileSize <= 0.0)
        return meshUV;

    vec3 axis = abs(cross(dFdx(scaledLocalPosition), dFdy(scaledLocalPosition)));
    vec3 p = scaledLocalPosition / tileSize;
    if (axis.y >= max(axis.x, axis.z))
        return p.xz;
    if (axis.x >= axis.z)
        return p.zy;
    return p.xy;
}
