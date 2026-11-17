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

// Linked indices are stored as index + 1; zero selects the local value.
float2 KromGraphUVValue(int link, float2 fallback)
{
    return link > 0 ? gKromGraphValues[max(link - 1, 0)].xy : fallback;
}

float2 KromGraphTextureUV(float4 node, float2 meshUV)
{
    int slot = (int)(node.x);
    int links = (int)(node.z);
    float4 local = graphUV[slot * 2];
    float2 uv = KromGraphUVValue((int)(node.y), meshUV);
    float2 scale = KromGraphUVValue(links & 31, local.xy);
    float2 offset = KromGraphUVValue((links >> 5) & 31, local.zw);
    float2 scroll = KromGraphUVValue((links >> 10) & 31, graphUV[slot * 2 + 1].xy);
    return uv * scale + offset + scroll * timeData.x;
}

float4 KromGraphSample(int slot, float2 transformed)
{
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
// Ops 1 and 15 use the same texture UV transform, including optional graph inputs.
// Op 13: mesh UV times tiling. Op 14: world position projected to a plane.
void EvaluateMaterialGraph(float2 uv, float3 positionWS)
{
    [loop]
    for (int i = 0; i < (int)graphOutputs.z; ++i)
    {
        const float4 node = graphNodes[i];
        const int op = (int)node.w;
        if (op == 0)
            gKromGraphValues[i] = float4(node.rgb, 1.0f);
        else if (op == 1)
            gKromGraphValues[i] = KromGraphSample((int)(node.x), KromGraphTextureUV(node, uv));
        else if (op == 12)
            gKromGraphValues[i] = float4((timeData.x * node.y).xxx, 1.0f);
        else if (op == 13)
            gKromGraphValues[i] = float4(uv * node.xy, 0.0f, 1.0f);
        else if (op == 14)
        {
            const int plane = (int)node.y;
            const float2 projected = plane == 0 ? positionWS.xz : (plane == 1 ? positionWS.xy : positionWS.zy);
            gKromGraphValues[i] = float4(projected * node.x, 0.0f, 1.0f);
        }
        else if (op == 15)
            gKromGraphValues[i] = float4(KromGraphTextureUV(node, uv), 0.0f, 1.0f);
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
}

void ApplyMaterialGraph(inout float4 baseColor, out float3 emission)
{
    if (graphOutputs.x >= 0.0f)
    {
        const float4 result = gKromGraphValues[(int)graphOutputs.x];
        baseColor = float4(result.rgb, result.a * baseColorFactor.a);
    }
    emission = graphOutputs.y >= 0.0f ? gKromGraphValues[max((int)graphOutputs.y, 0)].rgb : 0.0f;
}

// Call after graph evaluation. The vertex-transformed UV remains the PBR fallback.
float2 KromGraphDirectUV(float2 uv, float2 meshUV, float2 scale, float2 offset)
{
    int scaleLink = (int)(graphUV[1].z);
    int offsetLink = (int)(graphUV[1].w);
    if (scaleLink == 0 && offsetLink == 0)
        return uv;
    return meshUV * KromGraphUVValue(scaleLink, scale) + KromGraphUVValue(offsetLink, offset);
}

float2 KromGraphDirectUV(float2 meshUV)
{
    return KromGraphDirectUV(meshUV, meshUV, graphUV[3].zw, graphUV[5].zw);
}

// UV for the normal and ORM maps: a UV node linked to their texture node, else the mesh UV.
float2 KromGraphLinkedUV(int index, float2 uv)
{
    return index >= 0 ? gKromGraphValues[index].xy : uv;
}

int KromGraphNormalUVIndex()
{
    return (((int)(graphOutputs.w) >> 10) & 31) - 1;
}

float2 KromGraphNormalUV(float2 uv)
{
    return KromGraphLinkedUV(KromGraphNormalUVIndex(), uv);
}

bool KromGraphNormalUsesDerivedFrame()
{
    int index = KromGraphNormalUVIndex();
    if (index < 0)
        return graphUV[1].z > 0.0f || graphUV[1].w > 0.0f;
    float4 node = graphNodes[index];
    if ((int)(node.w) != 15 || node.y > 0.0f || node.z > 0.0f)
        return true;
    // A constant UV offset or scroll does not change the tangent frame.
    float2 scale = graphUV[(int)(node.x) * 2].xy;
    return scale.x <= 0.0f || scale.y <= 0.0f;
}

float2 KromGraphOrmUV(float2 uv)
{
    return KromGraphLinkedUV((((int)graphOutputs.w >> 15) & 31) - 1, uv);
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
    return KromGraphScalar((((int)graphOutputs.w >> 5) & 31) - 1, fallback);
}
#endif
