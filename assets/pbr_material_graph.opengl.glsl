#ifdef KROM_MATERIAL_GRAPH
uniform sampler2D graphTexture0;
uniform sampler2D graphTexture1;
uniform sampler2D graphTexture2;
uniform sampler2D graphTexture3;

#ifdef KROM_GRAPH_DECLARE_TIME
#include "per_frame.opengl.glsl"
#endif

vec4 gKromGraphValues[16];

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
    for (int i = 0; i < int(graphOutputs.z); ++i)
    {
        vec4 node = graphNodes[i];
        int op = int(node.w);
        if (op == 0)
            gKromGraphValues[i] = vec4(node.rgb, 1.0);
        else if (op == 1)
            gKromGraphValues[i] = KromGraphSample(int(node.x), KromGraphTextureUV(node, uv));
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

vec2 KromGraphNormalUV(vec2 uv)
{
    return KromGraphLinkedUV(KromGraphNormalUVIndex(), uv);
}

bool KromGraphNormalUsesDerivedFrame()
{
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
