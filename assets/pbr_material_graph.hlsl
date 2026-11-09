#ifdef KROM_MATERIAL_GRAPH
Texture2D graphTexture0 : register(t13);
Texture2D graphTexture1 : register(t9);
Texture2D graphTexture2 : register(t16);
Texture2D graphTexture3 : register(t17);

// Shaders without a PerFrame block declare just the part up to the time.
#ifdef KROM_GRAPH_DECLARE_TIME
cbuffer PerFrame : register(b0)
{
    float4x4 _graphView;
    float4x4 _graphProj;
    float4x4 _graphViewProj;
    float4x4 _graphInvViewProj;
    float4   _graphCameraPosition;
    float4   _graphCameraForward;
    float4   _graphScreenSize;
    float4   timeData;
};
#endif

static float4 gKromGraphValues[16];

float4 KromGraphSample(int slot, float2 uv)
{
    const float2 transformed = uv * graphUV[slot * 2].xy + graphUV[slot * 2].zw + graphUV[slot * 2 + 1].xy * timeData.x;
    if (slot == 0)
        return graphTexture0.Sample(sLinear, transformed);
    if (slot == 1)
        return graphTexture1.Sample(sLinear, transformed);
    if (slot == 2)
        return graphTexture2.Sample(sLinear, transformed);
    return graphTexture3.Sample(sLinear, transformed);
}

// The material compiler orders inputs before the nodes that consume them.
// Constants have alpha 1; two-input math multiplies the alpha of its inputs.
// Mix stores its constant factor in the fraction of the op code.
void EvaluateMaterialGraph(float2 uv, inout float4 baseColor, out float3 emission)
{
    [loop]
    for (int i = 0; i < (int)graphOutputs.z; ++i)
    {
        const float4 node = graphNodes[i];
        const int op = (int)node.w;
        if (op == 0)
            gKromGraphValues[i] = float4(node.rgb, 1.0f);
        else if (op == 1)
            gKromGraphValues[i] = KromGraphSample((int)node.x, uv);
        else if (op == 12)
            gKromGraphValues[i] = float4((timeData.x * node.y).xxx, 1.0f);
        else
        {
            const int inputs = (int)node.x;
            const int a = (inputs & 31) - 1;
            const int b = ((inputs >> 5) & 31) - 1;
            const int c = (inputs >> 10) - 1;
            const float4 inputA = a >= 0 ? gKromGraphValues[max(a, 0)] : 1.0f;
            const float4 inputB = b >= 0 ? gKromGraphValues[max(b, 0)] : 1.0f;
            // Linked inputs are scaled by their factor; alpha stays unscaled.
            const float4 lhs = float4(node.yyy, 1.0f) * inputA;
            const float4 rhs = float4(node.zzz, 1.0f) * inputB;
            float4 result;
            if (op == 2)
                result = float4(lhs.rgb + rhs.rgb, lhs.a * rhs.a);
            else if (op == 3)
                result = float4(lhs.rgb * rhs.rgb, lhs.a * rhs.a);
            else if (op == 4)
            {
                // Red times alpha serves both black-and-white masks and alpha-only masks.
                const float mask = c >= 0 ? gKromGraphValues[max(c, 0)].r * gKromGraphValues[max(c, 0)].a : 1.0f;
                result = lerp(lhs, rhs, saturate(frac(node.w) / 0.999f * mask));
            }
            else if (op == 5)
            {
                const float channel = inputA[(int)node.y];
                result = float4(channel, channel, channel, 1.0f);
            }
            else if (op == 6)
                result = float4(lhs.rgb - rhs.rgb, lhs.a * rhs.a);
            else if (op == 7)
                result = float4(pow(max(lhs.rgb, 0.0f), rhs.rgb), lhs.a * rhs.a);
            else if (op == 8)
                result = float4(1.0f - lhs.rgb, lhs.a);
            else if (op == 9)
                result = float4(saturate(lhs.rgb), lhs.a);
            else if (op == 10)
                result = float4(lerp(node.yyy, node.zzz, inputA.rgb), inputA.a);
            else
                result = float4(0.5f + 0.5f * sin(lhs.rgb * 6.2831853f), lhs.a);
            gKromGraphValues[i] = result;
        }
    }
    if (graphOutputs.x >= 0.0f)
    {
        const float4 result = gKromGraphValues[(int)graphOutputs.x];
        baseColor = float4(result.rgb, result.a * baseColorFactor.a);
    }
    emission = graphOutputs.y >= 0.0f ? gKromGraphValues[max((int)graphOutputs.y, 0)].rgb : 0.0f;
}

// Call after EvaluateMaterialGraph. Without a per-pixel link the fallback stays.
float KromGraphScalar(int index, float fallback)
{
    return index >= 0 ? saturate(gKromGraphValues[index].r) : fallback;
}

float KromGraphMetallic(float fallback)
{
    return KromGraphScalar(((int)graphOutputs.w & 31) - 1, fallback);
}

float KromGraphRoughness(float fallback)
{
    return KromGraphScalar(((int)graphOutputs.w >> 5) - 1, fallback);
}
#endif
