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
static float2 gKromGraphMeshUV;

float3 KromGraphSafeNormal(float3 n)
{
    float len2 = dot(n, n);
    return len2 > 1e-12f ? n * rsqrt(len2) : float3(0.0f, 0.0f, 1.0f);
}

float3 KromGraphNormalStrength(float3 n, float strength)
{
    n = KromGraphSafeNormal(n);
    // Slope scaling preserves the direction at strength one, including steep RNM results.
    return KromGraphSafeNormal(float3(n.xy * strength, max(n.z, 1e-5f)));
}

// Build two UV frames on the same geometric plane. This rotates/mirrors each
// normal sample into the mesh UV frame before layers with different UVs combine.
bool KromGraphUVFrame(float2 uv, float3 dp1, float3 dp2, float3 n, out float3 t, out float3 b)
{
    float2 duv1 = ddx(uv), duv2 = ddy(uv);
    float3 p2 = cross(dp2, n), p1 = cross(n, dp1);
    float3 rawT = p2 * duv1.x + p1 * duv2.x;
    float3 rawB = p2 * duv1.y + p1 * duv2.y;
    float refLen2 = max(dot(dp1, dp1), dot(dp2, dp2)) * max(dot(duv1, duv1), dot(duv2, duv2));
    t = KromGraphSafeNormal(rawT);
    b = cross(n, t) * (dot(cross(n, t), rawB) < 0.0f ? -1.0f : 1.0f);
    return refLen2 > 0.0f && dot(rawT, rawT) > refLen2 * 1e-6f;
}

float3 KromGraphReorientSample(float3 mapN, float2 sampleUV, float2 meshUV, float3 positionWS)
{
    float3 dp1 = ddx(positionWS), dp2 = ddy(positionWS);
    float3 n = KromGraphSafeNormal(cross(dp1, dp2));
    float3 meshT, meshB, sampleT, sampleB;
    bool meshValid = KromGraphUVFrame(meshUV, dp1, dp2, n, meshT, meshB);
    bool sampleValid = KromGraphUVFrame(sampleUV, dp1, dp2, n, sampleT, sampleB);
    if (!meshValid || !sampleValid)
        return mapN;
    float3 worldXY = sampleT * mapN.x + sampleB * mapN.y;
    return KromGraphSafeNormal(float3(dot(worldXY, meshT), dot(worldXY, meshB), mapN.z));
}

// Inputs and output use encoded RGB (0..1), so Mix and Math can also feed Normal.
float3 KromGraphBlendNormals(float3 encodedA, float3 encodedB, float strengthA, float strengthB)
{
    float3 a = KromGraphNormalStrength(encodedA * 2.0f - 1.0f, strengthA);
    float3 b = KromGraphNormalStrength(encodedB * 2.0f - 1.0f, strengthB);
    float3 t = a + float3(0.0f, 0.0f, 1.0f);
    float3 u = b * float3(-1.0f, -1.0f, 1.0f);
    float3 blended = KromGraphSafeNormal(t * dot(t, u) / max(t.z, 1e-5f) - u);
    return blended * 0.5f + 0.5f;
}

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
    gKromGraphMeshUV = uv;
    [loop]
    for (int i = 0; i < (int)graphOutputs.z; ++i)
    {
        const float4 node = graphNodes[i];
        const int op = (int)node.w;
        if (op == 0)
            gKromGraphValues[i] = float4(node.rgb, 1.0f);
        else if (op == 1)
            gKromGraphValues[i] = KromGraphSample((int)(node.x), KromGraphTextureUV(node, uv));
        else if (op == 17)
        {
            float2 sampleUV = KromGraphTextureUV(node, uv);
            float2 xy = KromGraphSample((int)(node.x), sampleUV).rg * 2.0f - 1.0f;
            // Both RGB and BC5 normal textures describe the upper tangent hemisphere.
            float3 mapN = KromGraphSafeNormal(float3(xy, sqrt(saturate(1.0f - dot(xy, xy)))));
            mapN = KromGraphReorientSample(mapN, sampleUV, uv, positionWS);
            gKromGraphValues[i] = float4(mapN * 0.5f + 0.5f, 1.0f);
        }
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
            if (op == 16)
            {
                float3 normalA = a >= 0 ? inputA.rgb : float3(0.5f, 0.5f, 1.0f);
                float3 normalB = b >= 0 ? inputB.rgb : float3(0.5f, 0.5f, 1.0f);
                result = float4(KromGraphBlendNormals(normalA, normalB, node.y, node.z), 1.0f);
            }
            else if (op == 2)
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

bool KromGraphHasNormal()
{
    return graphUV[7].z > 0.0f;
}

float3 KromGraphNormal()
{
    int index = max((int)graphUV[7].z - 1, 0);
    return KromGraphSafeNormal(gKromGraphValues[index].rgb * 2.0f - 1.0f);
}

float2 KromGraphNormalUV(float2 uv)
{
    if (KromGraphHasNormal())
        return gKromGraphMeshUV;
    return KromGraphLinkedUV(KromGraphNormalUVIndex(), uv);
}

bool KromGraphNormalUsesDerivedFrame()
{
    if (KromGraphHasNormal())
        return false; // Each graph sample is already expressed in the mesh UV frame.
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
float KromGraphSpecular(float fallback)
{
    return KromGraphScalar((int)graphUV[7].w - 1, fallback);
}
#endif
