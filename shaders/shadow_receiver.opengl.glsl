// GLSL twin of shadow_receiver.hlsl.
vec2 KromShadowDepthGradient(float depth, vec2 uv)
{
    vec2 dx = dFdx(uv);
    vec2 dy = dFdy(uv);
    vec2 dz = vec2(dFdx(depth), dFdy(depth));
    float det = dx.x * dy.y - dx.y * dy.x;
    float scale = max(length(dx) * length(dy), 1e-20);
    if (abs(det) <= 1e-5 * scale)
        return vec2(0.0);
    return vec2(dy.y * dz.x - dx.y * dz.y,
                dx.x * dz.y - dy.x * dz.x) / det;
}

float KromShadowReceiverBias(float biasValue, mat4 shadowVP)
{
    vec3 wRow = vec3(shadowVP[0][3], shadowVP[1][3], shadowVP[2][3]);
    if (dot(wRow, wRow) < 1e-12)
    {
        vec3 zRow = vec3(shadowVP[0][2], shadowVP[1][2], shadowVP[2][2]);
        // OpenGL clip depth is [-1,1], sampled depth is [0,1].
        return max(biasValue * length(zRow) * 0.5, 1e-7);
    }
    return max(biasValue, 0.00005);
}
