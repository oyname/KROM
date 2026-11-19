#ifdef KROM_MATERIAL_GRAPH
uniform sampler2D graphTexture0;
uniform sampler2D graphTexture1;
uniform sampler2D graphTexture2;
uniform sampler2D graphTexture3;

#ifdef KROM_GRAPH_DECLARE_TIME
#include "per_frame.opengl.glsl"
#endif

vec4 gKromGraphValues[16];
vec2 gKromGraphMeshUV;

vec3 KromGraphSafeNormal(vec3 n)
{
    float len2 = dot(n, n);
    return len2 > 1e-12 ? n * inversesqrt(len2) : vec3(0.0, 0.0, 1.0);
}

vec3 KromGraphNormalStrength(vec3 n, float strength)
{
    n = KromGraphSafeNormal(n);
    // Slope scaling preserves the direction at strength one, including steep RNM results.
    return KromGraphSafeNormal(vec3(n.xy * strength, max(n.z, 1e-5)));
}

// Build two UV frames on the same geometric plane. This rotates/mirrors each
// normal sample into the mesh UV frame before layers with different UVs combine.
bool KromGraphUVFrame(vec2 uv, vec3 dp1, vec3 dp2, vec3 n, out vec3 t, out vec3 b)
{
    vec2 duv1 = dFdx(uv), duv2 = dFdy(uv);
    vec3 p2 = cross(dp2, n), p1 = cross(n, dp1);
    vec3 rawT = p2 * duv1.x + p1 * duv2.x;
    vec3 rawB = p2 * duv1.y + p1 * duv2.y;
    float refLen2 = max(dot(dp1, dp1), dot(dp2, dp2)) * max(dot(duv1, duv1), dot(duv2, duv2));
    t = KromGraphSafeNormal(rawT);
    b = cross(n, t) * (dot(cross(n, t), rawB) < 0.0 ? -1.0 : 1.0);
    return refLen2 > 0.0 && dot(rawT, rawT) > refLen2 * 1e-6;
}

vec3 KromGraphReorientSample(vec3 mapN, vec2 sampleUV, vec2 meshUV, vec3 positionWS)
{
    vec3 dp1 = dFdx(positionWS), dp2 = dFdy(positionWS);
    vec3 n = KromGraphSafeNormal(cross(dp1, dp2));
    vec3 meshT, meshB, sampleT, sampleB;
    bool meshValid = KromGraphUVFrame(meshUV, dp1, dp2, n, meshT, meshB);
    bool sampleValid = KromGraphUVFrame(sampleUV, dp1, dp2, n, sampleT, sampleB);
    if (!meshValid || !sampleValid)
        return mapN;
    vec3 worldXY = sampleT * mapN.x + sampleB * mapN.y;
    return KromGraphSafeNormal(vec3(dot(worldXY, meshT), dot(worldXY, meshB), mapN.z));
}

// Inputs and output use encoded RGB (0..1), so Mix and Math can also feed Normal.
vec3 KromGraphBlendNormals(vec3 encodedA, vec3 encodedB, float strengthA, float strengthB)
{
    vec3 a = KromGraphNormalStrength(encodedA * 2.0 - 1.0, strengthA);
    vec3 b = KromGraphNormalStrength(encodedB * 2.0 - 1.0, strengthB);
    vec3 t = a + vec3(0.0, 0.0, 1.0);
    vec3 u = b * vec3(-1.0, -1.0, 1.0);
    vec3 blended = KromGraphSafeNormal(t * dot(t, u) / max(t.z, 1e-5) - u);
    return blended * 0.5 + 0.5;
}


// Linked indices are stored as index + 1; zero selects the local value.
vec2 KromGraphUVValue(int link, vec2 fallback)
{
    return link > 0 ? gKromGraphValues[max(link - 1, 0)].xy : fallback;
}

vec2 KromGraphTextureUV(vec4 node, vec2 meshUV)
{
    int slot = int(node.x);
    int links = int(node.z);
    vec4 local = graphUV[slot * 2];
    vec2 uv = KromGraphUVValue(int(node.y), meshUV);
    vec2 scale = KromGraphUVValue(links & 31, local.xy);
    vec2 offset = KromGraphUVValue((links >> 5) & 31, local.zw);
    vec2 scroll = KromGraphUVValue((links >> 10) & 31, graphUV[slot * 2 + 1].xy);
    return uv * scale + offset + scroll * timeData.x;
}

vec4 KromGraphSample(int slot, vec2 transformed)
{
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
// Ops 1 and 15 use the same texture UV transform, including optional graph inputs.
// Op 13: mesh UV times tiling. Op 14: world position projected to a plane.
void EvaluateMaterialGraph(vec2 uv, vec3 positionWS)
{
    gKromGraphMeshUV = uv;
    for (int i = 0; i < int(graphOutputs.z); ++i)
    {
        vec4 node = graphNodes[i];
        int op = int(node.w);
        if (op == 0)
            gKromGraphValues[i] = vec4(node.rgb, 1.0);
        else if (op == 1)
            gKromGraphValues[i] = KromGraphSample(int(node.x), KromGraphTextureUV(node, uv));
        else if (op == 17)
        {
            vec2 sampleUV = KromGraphTextureUV(node, uv);
            vec2 xy = KromGraphSample(int(node.x), sampleUV).rg * 2.0 - 1.0;
            vec3 mapN = KromGraphSafeNormal(vec3(xy, sqrt(clamp(1.0 - dot(xy, xy), 0.0, 1.0))));
            mapN = KromGraphReorientSample(mapN, sampleUV, uv, positionWS);
            gKromGraphValues[i] = vec4(mapN * 0.5 + 0.5, 1.0);
        }
        else if (op == 12)
            gKromGraphValues[i] = vec4(vec3(timeData.x * node.y), 1.0);
        else if (op == 13)
            gKromGraphValues[i] = vec4(uv * node.xy, 0.0, 1.0);
        else if (op == 14)
        {
            int plane = int(node.y);
            vec2 projected = plane == 0 ? positionWS.xz : (plane == 1 ? positionWS.xy : positionWS.zy);
            gKromGraphValues[i] = vec4(projected * node.x, 0.0, 1.0);
        }
        else if (op == 15)
            gKromGraphValues[i] = vec4(KromGraphTextureUV(node, uv), 0.0, 1.0);
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
            if (op == 16)
            {
                vec3 normalA = a >= 0 ? inputA.rgb : vec3(0.5, 0.5, 1.0);
                vec3 normalB = b >= 0 ? inputB.rgb : vec3(0.5, 0.5, 1.0);
                result = vec4(KromGraphBlendNormals(normalA, normalB, node.y, node.z), 1.0);
            }
            else if (op == 2)
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
}

void ApplyMaterialGraph(inout vec4 baseColor, out vec3 emission)
{
    if (graphOutputs.x >= 0.0)
    {
        vec4 result = gKromGraphValues[int(graphOutputs.x)];
        baseColor = vec4(result.rgb, result.a * baseColorFactor.a);
    }
    emission = graphOutputs.y >= 0.0 ? gKromGraphValues[max(int(graphOutputs.y), 0)].rgb : vec3(0.0);
}

// Call after graph evaluation. The vertex-transformed UV remains the PBR fallback.
vec2 KromGraphDirectUV(vec2 uv, vec2 meshUV, vec2 scale, vec2 offset)
{
    int scaleLink = int(graphUV[1].z);
    int offsetLink = int(graphUV[1].w);
    if (scaleLink == 0 && offsetLink == 0)
        return uv;
    return meshUV * KromGraphUVValue(scaleLink, scale) + KromGraphUVValue(offsetLink, offset);
}

vec2 KromGraphDirectUV(vec2 meshUV)
{
    return KromGraphDirectUV(meshUV, meshUV, graphUV[3].zw, graphUV[5].zw);
}

// UV for the normal and ORM maps: a UV node linked to their texture node, else the mesh UV.
vec2 KromGraphLinkedUV(int index, vec2 uv)
{
    return index >= 0 ? gKromGraphValues[index].xy : uv;
}

int KromGraphNormalUVIndex()
{
    return ((int(graphOutputs.w) >> 10) & 31) - 1;
}

bool KromGraphHasNormal()
{
    return graphUV[7].z > 0.0;
}

vec3 KromGraphNormal()
{
    int index = max(int(graphUV[7].z) - 1, 0);
    return KromGraphSafeNormal(gKromGraphValues[index].rgb * 2.0 - 1.0);
}

vec2 KromGraphNormalUV(vec2 uv)
{
    if (KromGraphHasNormal())
        return gKromGraphMeshUV;
    return KromGraphLinkedUV(KromGraphNormalUVIndex(), uv);
}

bool KromGraphNormalUsesDerivedFrame()
{
    if (KromGraphHasNormal())
        return false; // Graph samples already use the mesh UV frame.
    int index = KromGraphNormalUVIndex();
    if (index < 0)
        return graphUV[1].z > 0.0 || graphUV[1].w > 0.0;
    vec4 node = graphNodes[index];
    if (int(node.w) != 15 || node.y > 0.0 || node.z > 0.0)
        return true;
    // A constant UV offset or scroll does not change the tangent frame.
    vec2 scale = graphUV[int(node.x) * 2].xy;
    return scale.x <= 0.0 || scale.y <= 0.0;
}

vec2 KromGraphOrmUV(vec2 uv)
{
    return KromGraphLinkedUV(((int(graphOutputs.w) >> 15) & 31) - 1, uv);
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
    return KromGraphScalar(((int(graphOutputs.w) >> 5) & 31) - 1, fallback);
}
#endif
