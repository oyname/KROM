#ifdef KROM_MATERIAL_GRAPH
uniform sampler2D graphTexture0;
uniform sampler2D graphTexture1;
uniform sampler2D graphTexture2;
uniform sampler2D graphTexture3;

// Shaders without a PerFrame block declare just the part up to the time.
#ifdef KROM_GRAPH_DECLARE_TIME
layout(std140) uniform PerFrame
{
    mat4 _graphView;
    mat4 _graphProj;
    mat4 _graphViewProj;
    mat4 _graphInvViewProj;
    vec4 _graphCameraPosition;
    vec4 _graphCameraForward;
    vec4 _graphScreenSize;
    vec4 timeData;
};
#endif

vec4 gKromGraphValues[16];

vec4 KromGraphSample(int slot, vec2 uv)
{
    vec2 transformed = uv * graphUV[slot * 2].xy + graphUV[slot * 2].zw + graphUV[slot * 2 + 1].xy * timeData.x;
    if (slot == 0)
        return texture(graphTexture0, transformed);
    if (slot == 1)
        return texture(graphTexture1, transformed);
    if (slot == 2)
        return texture(graphTexture2, transformed);
    return texture(graphTexture3, transformed);
}

// The material compiler orders inputs before the nodes that consume them.
// Constants have alpha 1; two-input math multiplies the alpha of its inputs.
// Mix stores its constant factor in the fraction of the op code.
void EvaluateMaterialGraph(vec2 uv, inout vec4 baseColor, out vec3 emission)
{
    for (int i = 0; i < int(graphOutputs.z); ++i)
    {
        vec4 node = graphNodes[i];
        int op = int(node.w);
        if (op == 0)
            gKromGraphValues[i] = vec4(node.rgb, 1.0);
        else if (op == 1)
            gKromGraphValues[i] = KromGraphSample(int(node.x), uv);
        else if (op == 12)
            gKromGraphValues[i] = vec4(vec3(timeData.x * node.y), 1.0);
        else
        {
            int inputs = int(node.x);
            int a = (inputs & 31) - 1;
            int b = ((inputs >> 5) & 31) - 1;
            int c = (inputs >> 10) - 1;
            vec4 inputA = a >= 0 ? gKromGraphValues[max(a, 0)] : vec4(1.0);
            vec4 inputB = b >= 0 ? gKromGraphValues[max(b, 0)] : vec4(1.0);
            // Linked inputs are scaled by their factor; alpha stays unscaled.
            vec4 lhs = vec4(vec3(node.y), 1.0) * inputA;
            vec4 rhs = vec4(vec3(node.z), 1.0) * inputB;
            vec4 result;
            if (op == 2)
                result = vec4(lhs.rgb + rhs.rgb, lhs.a * rhs.a);
            else if (op == 3)
                result = vec4(lhs.rgb * rhs.rgb, lhs.a * rhs.a);
            else if (op == 4)
            {
                // Red times alpha serves both black-and-white masks and alpha-only masks.
                float mask = c >= 0 ? gKromGraphValues[max(c, 0)].r * gKromGraphValues[max(c, 0)].a : 1.0;
                result = mix(lhs, rhs, clamp(fract(node.w) / 0.999 * mask, 0.0, 1.0));
            }
            else if (op == 5)
            {
                float channel = inputA[int(node.y)];
                result = vec4(vec3(channel), 1.0);
            }
            else if (op == 6)
                result = vec4(lhs.rgb - rhs.rgb, lhs.a * rhs.a);
            else if (op == 7)
                result = vec4(pow(max(lhs.rgb, vec3(0.0)), rhs.rgb), lhs.a * rhs.a);
            else if (op == 8)
                result = vec4(vec3(1.0) - lhs.rgb, lhs.a);
            else if (op == 9)
                result = vec4(clamp(lhs.rgb, 0.0, 1.0), lhs.a);
            else if (op == 10)
                result = vec4(mix(vec3(node.y), vec3(node.z), inputA.rgb), inputA.a);
            else
                result = vec4(vec3(0.5) + 0.5 * sin(lhs.rgb * 6.2831853), lhs.a);
            gKromGraphValues[i] = result;
        }
    }
    if (graphOutputs.x >= 0.0)
    {
        vec4 result = gKromGraphValues[int(graphOutputs.x)];
        baseColor = vec4(result.rgb, result.a * baseColorFactor.a);
    }
    emission = graphOutputs.y >= 0.0 ? gKromGraphValues[max(int(graphOutputs.y), 0)].rgb : vec3(0.0);
}

// Call after EvaluateMaterialGraph. Without a per-pixel link the fallback stays.
float KromGraphScalar(int index, float fallback)
{
    return index >= 0 ? clamp(gKromGraphValues[index].r, 0.0, 1.0) : fallback;
}

float KromGraphMetallic(float fallback)
{
    return KromGraphScalar((int(graphOutputs.w) & 31) - 1, fallback);
}

float KromGraphRoughness(float fallback)
{
    return KromGraphScalar((int(graphOutputs.w) >> 5) - 1, fallback);
}
#endif
