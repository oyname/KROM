// =============================================================================
// KROM Engine - addons/forward/ForwardSmaa.cpp
// SMAA 1x passes, ported from SMAA.hlsl of the official SMAA repository
// (github.com/iryoku/smaa):
//   Copyright (C) 2013 Jorge Jimenez, Jose I. Echevarria, Belen Masia,
//   Fernando Navarro, Diego Gutierrez. MIT license, see licenses/SMAA.txt.
// Preset "high" (threshold 0.1, 16 search steps, diagonal search 8 steps, corner
// rounding 25), luma edge detection, no predication or reprojection. Texture
// coordinates come from SV_Position, so DX11 and Vulkan use the same origin
// (top left). The tonemapped input is an sRGB texture: edge detection encodes
// the samples back to gamma space as SMAA expects, blending stays linear.
// =============================================================================
#include "ForwardSmaa.hpp"

namespace engine::renderer::addons::forward::smaa {

namespace {
#include "SmaaLookupTextures.inl"
} // namespace

const char* const kEdgeDetectionPsHlsl = R"(
Texture2D    uColor : register(t8);
SamplerState uPoint : register(s2);

struct PSInput
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

static const float kThreshold = 0.1f;
static const float kLocalContrastAdaptation = 2.0f;

float3 LinearToGamma(float3 c)
{
    c = saturate(c);
    return lerp(c * 12.92f, 1.055f * pow(c, 1.0f / 2.4f) - 0.055f, step(0.0031308f, c));
}

float Luma(float2 uv)
{
    return dot(LinearToGamma(uColor.SampleLevel(uPoint, uv, 0.0f).rgb), float3(0.2126f, 0.7152f, 0.0722f));
}

float4 main(PSInput IN) : SV_TARGET
{
    float w, h;
    uColor.GetDimensions(w, h);
    const float2 px = 1.0f / float2(w, h);
    const float2 uv = IN.pos.xy * px;

    const float L     = Luma(uv);
    const float Lleft = Luma(uv + float2(-px.x, 0.0f));
    const float Ltop  = Luma(uv + float2(0.0f, -px.y));

    float4 delta;
    delta.xy = abs(L - float2(Lleft, Ltop));
    float2 edges = step(kThreshold, delta.xy);
    // No edge: keep the cleared value (0).
    if (dot(edges, float2(1.0f, 1.0f)) == 0.0f)
        discard;

    const float Lright  = Luma(uv + float2(px.x, 0.0f));
    const float Lbottom = Luma(uv + float2(0.0f, px.y));
    delta.zw = abs(L - float2(Lright, Lbottom));
    float2 maxDelta = max(delta.xy, delta.zw);

    const float Lleftleft = Luma(uv + float2(-2.0f * px.x, 0.0f));
    const float Ltoptop   = Luma(uv + float2(0.0f, -2.0f * px.y));
    delta.zw = abs(float2(Lleft, Ltop) - float2(Lleftleft, Ltoptop));
    maxDelta = max(maxDelta.xy, delta.zw);
    const float finalDelta = max(maxDelta.x, maxDelta.y);

    // Local contrast adaptation: drop edges next to a much stronger one.
    edges *= step(finalDelta, kLocalContrastAdaptation * delta.xy);
    return float4(edges, 0.0f, 0.0f);
})";

const char* const kBlendingWeightPsHlsl = R"(
Texture2D    uEdges  : register(t8);
Texture2D    uArea   : register(t9);
Texture2D    uSearch : register(t10);
SamplerState uLinear : register(s1);
SamplerState uPoint  : register(s2);

struct PSInput
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

#define SMAA_MAX_SEARCH_STEPS 16
#define SMAA_MAX_SEARCH_STEPS_DIAG 8
#define SMAA_CORNER_ROUNDING 25
#define SMAA_AREATEX_MAX_DISTANCE 16
#define SMAA_AREATEX_MAX_DISTANCE_DIAG 20
// The engine's area texture holds only subsample 0 (top 80 of 560 rows).
#define SMAA_AREATEX_PIXEL_SIZE (1.0 / float2(160.0, 80.0))
#define SMAA_SEARCHTEX_SIZE float2(66.0, 33.0)
#define SMAA_SEARCHTEX_PACKED_SIZE float2(64.0, 16.0)
#define SMAA_CORNER_ROUNDING_NORM (float(SMAA_CORNER_ROUNDING) / 100.0)

// (1/width, 1/height, width, height) of the edges texture.
static float4 gRt;

void SMAAMovc(bool2 cond, inout float2 variable, float2 value)
{
    [flatten] if (cond.x) variable.x = value.x;
    [flatten] if (cond.y) variable.y = value.y;
}

// --- Diagonal search -------------------------------------------------------

float2 SMAADecodeDiagBilinearAccess(float2 e)
{
    e.r = e.r * abs(5.0 * e.r - 5.0 * 0.75);
    return round(e);
}

float4 SMAADecodeDiagBilinearAccess(float4 e)
{
    e.rb = e.rb * abs(5.0 * e.rb - 5.0 * 0.75);
    return round(e);
}

float2 SMAASearchDiag1(float2 texcoord, float2 dir, out float2 e)
{
    e = float2(0.0, 0.0);
    float4 coord = float4(texcoord, -1.0, 1.0);
    const float3 t = float3(gRt.xy, 1.0);
    [loop]
    while (coord.z < float(SMAA_MAX_SEARCH_STEPS_DIAG - 1) && coord.w > 0.9)
    {
        coord.xyz = mad(t, float3(dir, 1.0), coord.xyz);
        e = uEdges.SampleLevel(uLinear, coord.xy, 0.0).rg;
        coord.w = dot(e, float2(0.5, 0.5));
    }
    return coord.zw;
}

float2 SMAASearchDiag2(float2 texcoord, float2 dir, out float2 e)
{
    e = float2(0.0, 0.0);
    float4 coord = float4(texcoord, -1.0, 1.0);
    coord.x += 0.25 * gRt.x;
    const float3 t = float3(gRt.xy, 1.0);
    [loop]
    while (coord.z < float(SMAA_MAX_SEARCH_STEPS_DIAG - 1) && coord.w > 0.9)
    {
        coord.xyz = mad(t, float3(dir, 1.0), coord.xyz);
        e = uEdges.SampleLevel(uLinear, coord.xy, 0.0).rg;
        e = SMAADecodeDiagBilinearAccess(e);
        coord.w = dot(e, float2(0.5, 0.5));
    }
    return coord.zw;
}

float2 SMAAAreaDiag(float2 dist, float2 e)
{
    float2 texcoord = mad(float2(SMAA_AREATEX_MAX_DISTANCE_DIAG, SMAA_AREATEX_MAX_DISTANCE_DIAG), e, dist);
    texcoord = mad(SMAA_AREATEX_PIXEL_SIZE, texcoord, 0.5 * SMAA_AREATEX_PIXEL_SIZE);
    texcoord.x += 0.5;   // diagonal areas: right half
    return uArea.SampleLevel(uLinear, texcoord, 0.0).rg;
}

float2 SMAACalculateDiagWeights(float2 texcoord, float2 e)
{
    float2 weights = float2(0.0, 0.0);

    float4 d;
    float2 end;
    if (e.r > 0.0)
    {
        d.xz = SMAASearchDiag1(texcoord, float2(-1.0, 1.0), end);
        d.x += float(end.y > 0.9);
    }
    else
    {
        d.xz = float2(0.0, 0.0);
    }
    d.yw = SMAASearchDiag1(texcoord, float2(1.0, -1.0), end);

    [branch]
    if (d.x + d.y > 2.0)
    {
        const float4 coords = mad(float4(-d.x + 0.25, d.x, d.y, -d.y - 0.25), gRt.xyxy, texcoord.xyxy);
        float4 c;
        c.xy = uEdges.SampleLevel(uLinear, coords.xy, 0.0, int2(-1, 0)).rg;
        c.zw = uEdges.SampleLevel(uLinear, coords.zw, 0.0, int2( 1, 0)).rg;
        c.yxwz = SMAADecodeDiagBilinearAccess(c.xyzw);
        float2 cc = mad(float2(2.0, 2.0), c.xz, c.yw);
        SMAAMovc(bool2(step(0.9, d.zw)), cc, float2(0.0, 0.0));
        weights += SMAAAreaDiag(d.xy, cc);
    }

    d.xz = SMAASearchDiag2(texcoord, float2(-1.0, -1.0), end);
    if (uEdges.SampleLevel(uLinear, texcoord, 0.0, int2(1, 0)).r > 0.0)
    {
        d.yw = SMAASearchDiag2(texcoord, float2(1.0, 1.0), end);
        d.y += float(end.y > 0.9);
    }
    else
    {
        d.yw = float2(0.0, 0.0);
    }

    [branch]
    if (d.x + d.y > 2.0)
    {
        const float4 coords = mad(float4(-d.x, -d.x, d.y, d.y), gRt.xyxy, texcoord.xyxy);
        float4 c;
        c.x  = uEdges.SampleLevel(uLinear, coords.xy, 0.0, int2(-1,  0)).g;
        c.y  = uEdges.SampleLevel(uLinear, coords.xy, 0.0, int2( 0, -1)).r;
        c.zw = uEdges.SampleLevel(uLinear, coords.zw, 0.0, int2( 1,  0)).gr;
        float2 cc = mad(float2(2.0, 2.0), c.xz, c.yw);
        SMAAMovc(bool2(step(0.9, d.zw)), cc, float2(0.0, 0.0));
        weights += SMAAAreaDiag(d.xy, cc).gr;
    }

    return weights;
}

// --- Horizontal / vertical search -------------------------------------------

float SMAASearchLength(float2 e, float offset)
{
    float2 scale = SMAA_SEARCHTEX_SIZE * float2(0.5, -1.0);
    float2 bias = SMAA_SEARCHTEX_SIZE * float2(offset, 1.0);
    scale += float2(-1.0, 1.0);
    bias  += float2( 0.5, -0.5);
    scale *= 1.0 / SMAA_SEARCHTEX_PACKED_SIZE;
    bias  *= 1.0 / SMAA_SEARCHTEX_PACKED_SIZE;
    return uSearch.SampleLevel(uLinear, mad(scale, e, bias), 0.0).r;
}

float SMAASearchXLeft(float2 texcoord, float end)
{
    float2 e = float2(0.0, 1.0);
    [loop]
    while (texcoord.x > end && e.g > 0.8281 && e.r == 0.0)
    {
        e = uEdges.SampleLevel(uLinear, texcoord, 0.0).rg;
        texcoord = mad(-float2(2.0, 0.0), gRt.xy, texcoord);
    }
    const float offset = mad(-(255.0 / 127.0), SMAASearchLength(e, 0.0), 3.25);
    return mad(gRt.x, offset, texcoord.x);
}

float SMAASearchXRight(float2 texcoord, float end)
{
    float2 e = float2(0.0, 1.0);
    [loop]
    while (texcoord.x < end && e.g > 0.8281 && e.r == 0.0)
    {
        e = uEdges.SampleLevel(uLinear, texcoord, 0.0).rg;
        texcoord = mad(float2(2.0, 0.0), gRt.xy, texcoord);
    }
    const float offset = mad(-(255.0 / 127.0), SMAASearchLength(e, 0.5), 3.25);
    return mad(-gRt.x, offset, texcoord.x);
}

float SMAASearchYUp(float2 texcoord, float end)
{
    float2 e = float2(1.0, 0.0);
    [loop]
    while (texcoord.y > end && e.r > 0.8281 && e.g == 0.0)
    {
        e = uEdges.SampleLevel(uLinear, texcoord, 0.0).rg;
        texcoord = mad(-float2(0.0, 2.0), gRt.xy, texcoord);
    }
    const float offset = mad(-(255.0 / 127.0), SMAASearchLength(e.gr, 0.0), 3.25);
    return mad(gRt.y, offset, texcoord.y);
}

float SMAASearchYDown(float2 texcoord, float end)
{
    float2 e = float2(1.0, 0.0);
    [loop]
    while (texcoord.y < end && e.r > 0.8281 && e.g == 0.0)
    {
        e = uEdges.SampleLevel(uLinear, texcoord, 0.0).rg;
        texcoord = mad(float2(0.0, 2.0), gRt.xy, texcoord);
    }
    const float offset = mad(-(255.0 / 127.0), SMAASearchLength(e.gr, 0.5), 3.25);
    return mad(-gRt.y, offset, texcoord.y);
}

float2 SMAAArea(float2 dist, float e1, float e2)
{
    // Rounding prevents precision errors of bilinear filtering.
    float2 texcoord = mad(float2(SMAA_AREATEX_MAX_DISTANCE, SMAA_AREATEX_MAX_DISTANCE), round(4.0 * float2(e1, e2)), dist);
    texcoord = mad(SMAA_AREATEX_PIXEL_SIZE, texcoord, 0.5 * SMAA_AREATEX_PIXEL_SIZE);
    return uArea.SampleLevel(uLinear, texcoord, 0.0).rg;
}

// --- Corner detection --------------------------------------------------------

void SMAADetectHorizontalCornerPattern(inout float2 weights, float4 texcoord, float2 d)
{
    const float2 leftRight = step(d.xy, d.yx);
    float2 rounding = (1.0 - SMAA_CORNER_ROUNDING_NORM) * leftRight;
    rounding /= leftRight.x + leftRight.y;

    float2 factor = float2(1.0, 1.0);
    factor.x -= rounding.x * uEdges.SampleLevel(uLinear, texcoord.xy, 0.0, int2(0,  1)).r;
    factor.x -= rounding.y * uEdges.SampleLevel(uLinear, texcoord.zw, 0.0, int2(1,  1)).r;
    factor.y -= rounding.x * uEdges.SampleLevel(uLinear, texcoord.xy, 0.0, int2(0, -2)).r;
    factor.y -= rounding.y * uEdges.SampleLevel(uLinear, texcoord.zw, 0.0, int2(1, -2)).r;
    weights *= saturate(factor);
}

void SMAADetectVerticalCornerPattern(inout float2 weights, float4 texcoord, float2 d)
{
    const float2 leftRight = step(d.xy, d.yx);
    float2 rounding = (1.0 - SMAA_CORNER_ROUNDING_NORM) * leftRight;
    rounding /= leftRight.x + leftRight.y;

    float2 factor = float2(1.0, 1.0);
    factor.x -= rounding.x * uEdges.SampleLevel(uLinear, texcoord.xy, 0.0, int2( 1, 0)).g;
    factor.x -= rounding.y * uEdges.SampleLevel(uLinear, texcoord.zw, 0.0, int2( 1, 1)).g;
    factor.y -= rounding.x * uEdges.SampleLevel(uLinear, texcoord.xy, 0.0, int2(-2, 0)).g;
    factor.y -= rounding.y * uEdges.SampleLevel(uLinear, texcoord.zw, 0.0, int2(-2, 1)).g;
    weights *= saturate(factor);
}

float4 main(PSInput IN) : SV_TARGET
{
    float w, h;
    uEdges.GetDimensions(w, h);
    gRt = float4(1.0 / w, 1.0 / h, w, h);
    const float2 texcoord = IN.pos.xy * gRt.xy;
    const float2 pixcoord = texcoord * gRt.zw;

    float4 offset[3];
    offset[0] = mad(gRt.xyxy, float4(-0.25, -0.125,  1.25, -0.125), texcoord.xyxy);
    offset[1] = mad(gRt.xyxy, float4(-0.125, -0.25, -0.125,  1.25), texcoord.xyxy);
    offset[2] = mad(gRt.xxyy, float4(-2.0, 2.0, -2.0, 2.0) * float(SMAA_MAX_SEARCH_STEPS),
                    float4(offset[0].xz, offset[1].yw));

    float4 weights = float4(0.0, 0.0, 0.0, 0.0);
    float2 e = uEdges.SampleLevel(uPoint, texcoord, 0.0).rg;

    [branch]
    if (e.g > 0.0)   // edge at north
    {
        // Diagonals have priority; with one found, skip horizontal/vertical.
        weights.rg = SMAACalculateDiagWeights(texcoord, e);

        [branch]
        if (weights.r == -weights.g)
        {
            float2 d;
            float3 coords;
            coords.x = SMAASearchXLeft(offset[0].xy, offset[2].x);
            coords.y = offset[1].y;
            d.x = coords.x;
            const float e1 = uEdges.SampleLevel(uLinear, coords.xy, 0.0).r;

            coords.z = SMAASearchXRight(offset[0].zw, offset[2].y);
            d.y = coords.z;

            d = abs(round(mad(gRt.zz, d, -pixcoord.xx)));
            const float2 sqrt_d = sqrt(d);
            const float e2 = uEdges.SampleLevel(uLinear, coords.zy, 0.0, int2(1, 0)).r;

            weights.rg = SMAAArea(sqrt_d, e1, e2);
            coords.y = texcoord.y;
            SMAADetectHorizontalCornerPattern(weights.rg, coords.xyzy, d);
        }
        else
        {
            e.r = 0.0;   // skip vertical processing
        }
    }

    [branch]
    if (e.r > 0.0)   // edge at west
    {
        float2 d;
        float3 coords;
        coords.y = SMAASearchYUp(offset[1].xy, offset[2].z);
        coords.x = offset[0].x;
        d.x = coords.y;
        const float e1 = uEdges.SampleLevel(uLinear, coords.xy, 0.0).g;

        coords.z = SMAASearchYDown(offset[1].zw, offset[2].w);
        d.y = coords.z;

        d = abs(round(mad(gRt.ww, d, -pixcoord.yy)));
        const float2 sqrt_d = sqrt(d);
        const float e2 = uEdges.SampleLevel(uLinear, coords.xz, 0.0, int2(0, 1)).g;

        weights.ba = SMAAArea(sqrt_d, e1, e2);
        coords.x = texcoord.x;
        SMAADetectVerticalCornerPattern(weights.ba, coords.xyxz, d);
    }

    return weights;
})";

const char* const kNeighborhoodBlendingPsHlsl = R"(
Texture2D    uColor   : register(t8);
Texture2D    uWeights : register(t9);
SamplerState uLinear  : register(s1);

struct PSInput
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

float4 main(PSInput IN) : SV_TARGET
{
    float w, h;
    uColor.GetDimensions(w, h);
    const float4 rt = float4(1.0f / w, 1.0f / h, w, h);
    const float2 texcoord = IN.pos.xy * rt.xy;
    const float4 offset = mad(rt.xyxy, float4(1.0f, 0.0f, 0.0f, 1.0f), texcoord.xyxy);

    // Weights of this pixel: right and bottom neighbours write ours.
    float4 a;
    a.x  = uWeights.SampleLevel(uLinear, offset.xy, 0.0f).a;   // right
    a.y  = uWeights.SampleLevel(uLinear, offset.zw, 0.0f).g;   // bottom
    a.wz = uWeights.SampleLevel(uLinear, texcoord, 0.0f).xz;   // top / left

    [branch]
    if (dot(a, float4(1.0f, 1.0f, 1.0f, 1.0f)) < 1e-5f)
        return uColor.SampleLevel(uLinear, texcoord, 0.0f);

    const bool horizontal = max(a.x, a.z) > max(a.y, a.w);
    const float4 blendingOffset = horizontal ? float4(a.x, 0.0f, a.z, 0.0f) : float4(0.0f, a.y, 0.0f, a.w);
    float2 blendingWeight = horizontal ? a.xz : a.yw;
    blendingWeight /= dot(blendingWeight, float2(1.0f, 1.0f));

    // Bilinear filtering mixes the pixel with the chosen neighbour (linear, sRGB view).
    const float4 blendingCoord = mad(blendingOffset, float4(rt.xy, -rt.xy), texcoord.xyxy);
    float4 color = blendingWeight.x * uColor.SampleLevel(uLinear, blendingCoord.xy, 0.0f);
    color += blendingWeight.y * uColor.SampleLevel(uLinear, blendingCoord.zw, 0.0f);
    return color;
})";

const uint8_t* AreaTexData() noexcept { return kSmaaAreaTexBytes; }
size_t         AreaTexSize() noexcept { return sizeof(kSmaaAreaTexBytes); }
const uint8_t* SearchTexData() noexcept { return kSmaaSearchTexBytes; }
size_t         SearchTexSize() noexcept { return sizeof(kSmaaSearchTexBytes); }

static_assert(sizeof(kSmaaAreaTexBytes) == kAreaTexWidth * kAreaTexHeight * 2u, "area texture size");
static_assert(sizeof(kSmaaSearchTexBytes) == kSearchTexWidth * kSearchTexHeight, "search texture size");

} // namespace engine::renderer::addons::forward::smaa
