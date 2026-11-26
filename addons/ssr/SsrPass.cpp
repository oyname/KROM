#include "SsrPass.hpp"
#include "addons/forward/StandardFramePipeline.hpp"
#include "assets/AssetRegistry.hpp"
#include "renderer/IDevice.hpp"
#include "renderer/RenderPipelineRecipe.hpp"
#include "renderer/RenderRuntimeFrameBindings.hpp"
#include "renderer/ShaderBindingModel.hpp"
#include "renderer/ShaderCompiler.hpp"
#include "rendergraph/CompiledFrame.hpp"
#include "core/Debug.hpp"
#include <algorithm>
#include <string>

namespace engine::renderer::addons::ssr {

namespace {

constexpr std::string_view kResNormalRoughness   = "SsrNormalRoughness";
constexpr std::string_view kResSpecularWeight    = "SsrSpecularWeight";
constexpr std::string_view kResSpecularLight     = "SsrSpecularLight";
constexpr std::string_view kResNormalRoughnessMS = "SsrNormalRoughnessMS";
constexpr std::string_view kResSpecularWeightMS  = "SsrSpecularWeightMS";
constexpr std::string_view kResSpecularLightMS   = "SsrSpecularLightMS";
constexpr std::string_view kResReflection        = "SsrReflection";
constexpr std::string_view kExecResolve          = "frame.ssr_resolve";
constexpr std::string_view kExecTrace            = "frame.ssr_trace";
constexpr std::string_view kExecComposite        = "frame.ssr_composite";
constexpr std::string_view kExecCopy             = "frame.ssr_copy";
constexpr Format kInputFormat = Format::RGBA16_FLOAT;

// ─── HLSL ────────────────────────────────────────────────────────────────────

constexpr const char* kFullscreenVsHlsl = R"(
struct VSOutput { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
VSOutput main(uint id : SV_VertexID)
{
    VSOutput o;
    if      (id == 0u) { o.pos = float4(-1.0f,  1.0f, 0.0f, 1.0f); o.uv = float2(0.0f, 0.0f); }
    else if (id == 1u) { o.pos = float4(-1.0f, -3.0f, 0.0f, 1.0f); o.uv = float2(0.0f, 2.0f); }
    else               { o.pos = float4( 3.0f,  1.0f, 0.0f, 1.0f); o.uv = float2(2.0f, 0.0f); }
    return o;
}
)";

// PerFrame layout (FrameConstants) plus the SSR block and shared helpers.
constexpr const char* kCommonHlsl = R"(
struct GpuLightData
{
    float4 positionWS;
    float4 directionWS;
    float4 colorIntensity;
    float4 params;
};

cbuffer PerFrame : register(b0)
{
    float4x4     viewMatrix;
    float4x4     projMatrix;
    float4x4     viewProjMatrix;
    float4x4     invViewProjMatrix;
    float4       cameraPositionWS;
    float4       cameraForwardWS;
    float4       screenSize;
    float4       timeData;
    float4       ambientColor;
    uint         lightCount;
    uint         shadowCascadeCount;
    float        nearPlane;
    float        farPlane;
    GpuLightData lights[7];
    float4x4     shadowViewProj;
    float        iblPrefilterLevels;
    float        shadowBias;
    float        shadowNormalBias;
    float        shadowStrength;
    float        shadowTexelSize;
    uint         debugFlags;
    uint         shadowFilterMode;
    float        _shadowPad;
    float4       shadowLightMeta[4];
    float4       shadowLightExtra[4];
    float4       shadowViewRect[16];
    float4x4     shadowViewProjArray[16];
    uint         shadowLightCount;
    uint         shadowViewCount;
    float2       _shadowArrayPad;
    float4       featureParams[9]; // add-on parameter blocks (FrameConstants::featureParams)
};

// SSR add-on block (krom-ssr, featureParams slot 7, see kSsrFrameParameterBlock).
#define ssrMaxDistance  (featureParams[7].x)
#define ssrThickness    (featureParams[7].y)
#define ssrMaxRoughness (featureParams[7].z)
#define ssrIntensity    (featureParams[7].w)

struct PSInput { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

// Inverse of KromOctEncode (shaders/pbr_ssr_output.hlsl).
float3 OctDecode(float2 e)
{
    float3 n = float3(e, 1.0f - abs(e.x) - abs(e.y));
    const float t = saturate(-n.z);
    n.xy += float2(n.x >= 0.0f ? -t : t, n.y >= 0.0f ? -t : t);
    return normalize(n);
}

float3 WorldFromDepth(float2 uv, float depth)
{
    const float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    const float4 h = mul(invViewProjMatrix, float4(ndc, depth, 1.0f));
    return h.xyz / max(abs(h.w), 1e-6f);
}

// Distance along the camera forward axis.
float ViewDepth(float3 positionWS)
{
    return dot(positionWS - cameraPositionWS.xyz, normalize(cameraForwardWS.xyz));
}
)";

// MSAA: the opaque pass wrote multisampled inputs. Normal and roughness come
// from the first sample with data; weight and light are averaged, so an edge
// pixel gets the replacement in proportion to its covered samples.
const std::string kResolvePsHlsl = std::string(kCommonHlsl) + R"(
Texture2DMS<float4> tNormalRoughnessMS : register(t8);
Texture2DMS<float4> tSpecularWeightMS  : register(t9);
Texture2DMS<float4> tSpecularLightMS   : register(t10);

struct PSOutput
{
    float4 normalRoughness : SV_Target0;
    float4 specularWeight  : SV_Target1;
    float4 specularLight   : SV_Target2;
};

PSOutput main(PSInput IN)
{
    uint width, height, samples;
    tNormalRoughnessMS.GetDimensions(width, height, samples);
    const int2 pixel = int2(IN.pos.xy);

    PSOutput o;
    o.normalRoughness = float4(0.0f, 0.0f, 0.0f, 0.0f);
    float4 weight = 0.0f;
    float4 light = 0.0f;
    [loop]
    for (uint i = 0u; i < samples; ++i)
    {
        const float4 nr = tNormalRoughnessMS.Load(pixel, i);
        if (o.normalRoughness.w < 0.5f && nr.w > 0.5f)
            o.normalRoughness = nr;
        weight += tSpecularWeightMS.Load(pixel, i);
        light  += tSpecularLightMS.Load(pixel, i);
    }
    const float inv = 1.0f / max(float(samples), 1.0f);
    o.specularWeight = weight * inv;
    o.specularLight  = light * inv;
    return o;
}
)";

// Marches the reflected view ray in world space and tests it against the
// scene depth in screen space. rgb = scene colour at the hit, a = confidence.
const std::string kTracePsHlsl = std::string(kCommonHlsl) + R"(
Texture2D<float>  tSceneDepth      : register(t8);
Texture2D<float4> tSceneColor      : register(t9);
Texture2D<float4> tNormalRoughness : register(t10);
SamplerState      sLinear          : register(s1);
SamplerState      sPoint           : register(s2);

static const int kSteps  = 48;
static const int kRefine = 6;

bool Project(float3 positionWS, out float2 uv)
{
    const float4 clip = mul(viewProjMatrix, float4(positionWS, 1.0f));
    uv = float2(0.0f, 0.0f);
    if (clip.w <= 1e-4f)
        return false;
    const float2 ndc = clip.xy / clip.w;
    uv = float2(ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f);
    return all(uv >= 0.0f.xx) && all(uv <= 1.0f.xx);
}

// View depth of the scene surface at uv; sky = very far.
float SceneViewDepth(float2 uv)
{
    const float d = tSceneDepth.SampleLevel(sPoint, uv, 0);
    if (d >= 0.999999f)
        return 1e30f;
    return ViewDepth(WorldFromDepth(uv, d));
}

// Interleaved gradient noise: start offset per pixel against banding.
float StepJitter(float2 pixel)
{
    return frac(52.9829189f * frac(dot(pixel, float2(0.06711056f, 0.00583715f))));
}

float3 PositionAt(int2 pixel)
{
    const float2 uv = (float2(pixel) + 0.5f) / max(screenSize.xy, float2(1.0f, 1.0f));
    return WorldFromDepth(uv, tSceneDepth.Load(int3(pixel, 0)));
}

// Geometric normal from the depth buffer (the stored normal includes the
// normal map). Per axis the neighbour closer in depth is used, so silhouette
// edges do not tilt it.
float3 GeometricNormal(int2 pixel, float3 P, float3 toCamera)
{
    const int2 maxPixel = int2(screenSize.xy) - 1;
    const float3 px = PositionAt(min(pixel + int2(1, 0), maxPixel));
    const float3 nx = PositionAt(max(pixel - int2(1, 0), int2(0, 0)));
    const float3 py = PositionAt(min(pixel + int2(0, 1), maxPixel));
    const float3 ny = PositionAt(max(pixel - int2(0, 1), int2(0, 0)));
    const float3 dx = abs(ViewDepth(px) - ViewDepth(P)) < abs(ViewDepth(nx) - ViewDepth(P)) ? px - P : P - nx;
    const float3 dy = abs(ViewDepth(py) - ViewDepth(P)) < abs(ViewDepth(ny) - ViewDepth(P)) ? py - P : P - ny;
    float3 n = cross(dy, dx);
    const float len = length(n);
    if (len < 1e-12f)
        return float3(0.0f, 0.0f, 0.0f);
    n /= len;
    return dot(n, toCamera) < 0.0f ? -n : n;
}

float4 main(PSInput IN) : SV_TARGET
{
    const int2 pixel = int2(IN.pos.xy);
    const float4 nr = tNormalRoughness.Load(int3(pixel, 0));
    if (nr.w < 0.5f || ssrIntensity <= 0.0f || nr.z >= ssrMaxRoughness)
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    const float depth = tSceneDepth.Load(int3(pixel, 0));
    if (depth >= 0.999999f)
        return float4(0.0f, 0.0f, 0.0f, 0.0f);

    const float2 uv = (float2(pixel) + 0.5f) / max(screenSize.xy, float2(1.0f, 1.0f));
    const float3 P = WorldFromDepth(uv, depth);
    const float3 V = normalize(P - cameraPositionWS.xyz);
    const float3 N = OctDecode(nr.xy);
    const float3 R = reflect(V, N);
    float3 Ng = GeometricNormal(pixel, P, -V);
    if (dot(Ng, Ng) < 0.5f)
        Ng = N;
    // A normal map can tilt the reflection below the actual surface: such a
    // ray would only find the surface itself, so the probe keeps that pixel.
    const float rayRise = dot(R, Ng);
    if (rayRise <= 0.02f)
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    const float bias = max(0.01f, ViewDepth(P) * 0.003f);
    const float3 origin = P + Ng * bias;
    const float jitter = StepJitter(IN.pos.xy);

    // Steps grow with distance (quadratic): fine near the surface, where
    // contact reflections need them, coarse far away.
    // frontT is the last point that was visibly in front of the scene. While
    // the ray runs hidden behind a surface (occluded), the point where it
    // meets the next surface is not in the screen data: a "hit" found then
    // lies on some other, visible surface (e.g. the floor in front of a
    // sphere instead of the floor under it) and is rejected.
    float frontT = 0.0f;
    float hitT = -1.0f;
    bool occluded = false;
    [loop]
    for (int i = 1; i <= kSteps; ++i)
    {
        float f = (float(i) - 1.0f + jitter) / float(kSteps);
        const float t = max(f * f * ssrMaxDistance, 1e-3f);
        const float3 Q = origin + R * t;
        float2 quv;
        if (!Project(Q, quv))
            break;
        const float sceneDepth = SceneViewDepth(quv);
        const float diff = ViewDepth(Q) - sceneDepth;
        if (diff <= 0.0f)
        {
            frontT = t;
            occluded = false;
            continue;
        }
        // The depth buffer at a grazing angle steps in whole pixels, so the
        // start surface itself can appear in front of the ray. A real surface
        // lies above the start plane, about as high as the ray point; one on
        // the start plane is that artefact and counts as "in front"; one
        // clearly below it (a curved start surface, an object in front)
        // really hides the ray.
        const float3 S = WorldFromDepth(quv, tSceneDepth.SampleLevel(sPoint, quv, 0));
        const float sceneHeight = dot(S - P, Ng);
        const float rayHeight = dot(Q - P, Ng);
        if (sceneHeight > 0.5f * rayHeight)
        {
            if (!occluded && diff < ssrThickness + t * 0.02f)
            {
                hitT = t;
                break;
            }
            occluded = true;
        }
        else if (sceneHeight > -bias)
        {
            if (!occluded)
                frontT = t;
        }
        else
        {
            occluded = true;
        }
    }
    if (hitT < 0.0f)
        return float4(0.0f, 0.0f, 0.0f, 0.0f);

    // Binary search between the last point in front and the first behind,
    // with the same notion of "behind a real surface" as the march.
    float lo = frontT;
    float hi = hitT;
    [unroll]
    for (int k = 0; k < kRefine; ++k)
    {
        const float mid = 0.5f * (lo + hi);
        const float3 M = origin + R * mid;
        float2 muv;
        Project(M, muv);
        const float3 S = WorldFromDepth(muv, tSceneDepth.SampleLevel(sPoint, muv, 0));
        const bool behind = ViewDepth(M) - SceneViewDepth(muv) > 0.0f &&
                            dot(S - P, Ng) > 0.5f * dot(M - P, Ng);
        if (behind)
            hi = mid;
        else
            lo = mid;
    }
    float2 hitUV;
    const float3 H = origin + R * hi;
    if (!Project(H, hitUV))
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    // The refined point must still be a valid hit.
    {
        const float hitDiff = ViewDepth(H) - SceneViewDepth(hitUV);
        const float3 S = WorldFromDepth(hitUV, tSceneDepth.SampleLevel(sPoint, hitUV, 0));
        if (hitDiff < 0.0f || hitDiff > ssrThickness + hi * 0.02f ||
            dot(S - P, Ng) <= 0.5f * dot(H - P, Ng))
            return float4(0.0f, 0.0f, 0.0f, 0.0f);
    }

    float confidence = 1.0f;
    // The ray met the back of a surface: that is not what it would see.
    const float4 hitNR = tNormalRoughness.SampleLevel(sPoint, hitUV, 0);
    if (hitNR.w > 0.5f && dot(OctDecode(hitNR.xy), R) > 0.25f)
        confidence = 0.0f;
    // Fade near the screen border, at the end of the range, towards rough
    // surfaces and for rays coming back to the camera (little screen data).
    const float2 border = saturate(min(hitUV, 1.0f.xx - hitUV) * 12.0f);
    confidence *= border.x * border.y;
    confidence *= 1.0f - smoothstep(0.7f, 1.0f, hi / ssrMaxDistance);
    confidence *= 1.0f - smoothstep(ssrMaxRoughness * 0.6f, ssrMaxRoughness, nr.z);
    confidence *= saturate(1.0f + dot(R, normalize(cameraForwardWS.xyz)) * 2.5f);

    const float3 color = tSceneColor.SampleLevel(sLinear, hitUV, 0).rgb;
    return float4(max(color, 0.0f), saturate(confidence));
}
)";

// Rougher surfaces gather the hits over a wider footprint (normal- and
// depth-aware), then the hit replaces the probe/IBL reflection:
//   colour += intensity * confidence * (hit * weight - specular light)
// Written with additive blending into the scene composite.
const std::string kCompositePsHlsl = std::string(kCommonHlsl) + R"(
Texture2D<float4> tReflection      : register(t8);
Texture2D<float4> tNormalRoughness : register(t9);
Texture2D<float4> tSpecularWeight  : register(t10);
Texture2D<float4> tSpecularLight   : register(t11);
Texture2D<float>  tSceneDepth      : register(t12);

static const int kTaps = 13;
static const float2 kTapOffsets[kTaps] = {
    float2( 0.0f,  0.0f),
    float2( 0.5f,  0.0f), float2(-0.5f,  0.0f), float2( 0.0f,  0.5f), float2( 0.0f, -0.5f),
    float2( 0.71f, 0.71f), float2(-0.71f, 0.71f), float2( 0.71f, -0.71f), float2(-0.71f, -0.71f),
    float2( 1.0f,  0.0f), float2(-1.0f,  0.0f), float2( 0.0f,  1.0f), float2( 0.0f, -1.0f)
};

float LinearDepthAt(int2 pixel)
{
    const float d = tSceneDepth.Load(int3(pixel, 0));
    const float2 uv = (float2(pixel) + 0.5f) / max(screenSize.xy, float2(1.0f, 1.0f));
    return ViewDepth(WorldFromDepth(uv, d));
}

float4 main(PSInput IN) : SV_TARGET
{
    const int2 pixel = int2(IN.pos.xy);
    const float4 nr = tNormalRoughness.Load(int3(pixel, 0));
    if (nr.w < 0.5f || ssrIntensity <= 0.0f || nr.z >= ssrMaxRoughness)
        return float4(0.0f, 0.0f, 0.0f, 0.0f);

    const float3 N = OctDecode(nr.xy);
    const float centerZ = LinearDepthAt(pixel);
    const float roughness01 = saturate(nr.z / max(ssrMaxRoughness, 1e-3f));
    // At least ~2 px even for mirrors: the per-pixel step jitter makes hit and
    // miss alternate pixel by pixel where the ray only sometimes finds a
    // visible crossing (contact areas); gathering turns that dither into a
    // smooth confidence. Normal and depth weights keep edges.
    const float radius = max(roughness01 * 8.0f * max(screenSize.y / 1080.0f, 0.5f), 1.5f);
    const int2 maxPixel = int2(screenSize.xy) - 1;

    float3 sum = 0.0f;
    float  confidenceSum = 0.0f;
    float  weightSum = 0.0f;
    const int taps = kTaps;
    [loop]
    for (int i = 0; i < taps; ++i)
    {
        const int2 p = clamp(pixel + int2(round(kTapOffsets[i] * radius)), int2(0, 0), maxPixel);
        const float4 n2 = tNormalRoughness.Load(int3(p, 0));
        if (n2.w < 0.5f)
            continue;
        const float normalW = pow(saturate(dot(N, OctDecode(n2.xy))), 8.0f);
        const float dz = (LinearDepthAt(p) - centerZ) / max(centerZ * 0.05f, 0.02f);
        const float w = normalW * exp(-dz * dz);
        const float4 r = tReflection.Load(int3(p, 0));
        sum += r.rgb * r.a * w;
        confidenceSum += r.a * w;
        weightSum += w;
    }
    if (confidenceSum <= 1e-5f || weightSum <= 1e-5f)
        return float4(0.0f, 0.0f, 0.0f, 0.0f);

    const float3 hit = sum / confidenceSum;
    const float confidence = saturate(confidenceSum / weightSum);
    const float3 weight = tSpecularWeight.Load(int3(pixel, 0)).rgb;
    const float3 light  = tSpecularLight.Load(int3(pixel, 0)).rgb;
    return float4(ssrIntensity * confidence * (hit * weight - light), 0.0f);
}
)";

// HDRSceneColor -> scene composite (only while GTAO, which does it otherwise,
// is off).
const std::string kCopyPsHlsl = std::string(kCommonHlsl) + R"(
Texture2D<float4> tHDRColor : register(t8);
SamplerState      sLinear   : register(s1);

float4 main(PSInput IN) : SV_TARGET
{
    return tHDRColor.SampleLevel(sLinear, IN.uv, 0);
}
)";

// ─── Helpers ─────────────────────────────────────────────────────────────────

rendergraph::RGResourceID FindResourceId(const std::vector<rendergraph::CompiledResourceSnapshot>& snaps,
                                         std::string_view name)
{
    for (const auto& s : snaps)
        if (s.debugName == name)
            return s.id;
    return rendergraph::RG_INVALID_RESOURCE;
}

ShaderHandle CompileSsrShader(IDevice& device, assets::ShaderTargetProfile target,
                              assets::ShaderStage stage, const char* source, const char* debugName)
{
    assets::ShaderAsset asset{};
    asset.debugName      = debugName;
    asset.stage          = stage;
    asset.sourceLanguage = assets::ShaderSourceLanguage::HLSL;
    asset.entryPoint     = "main";
    asset.sourceCode     = source;

    const ShaderStageMask stageMask = stage == assets::ShaderStage::Vertex
        ? ShaderStageMask::Vertex : ShaderStageMask::Fragment;

    assets::CompiledShaderArtifact compiled{};
    std::string error;
    if (!ShaderCompiler::CompileForTarget(asset, target, compiled, &error))
    {
        Debug::LogError("SsrPass: shader compile failed '%s': %s", debugName, error.c_str());
        return ShaderHandle::Invalid();
    }
    if (!compiled.bytecode.empty())
        return device.CreateShaderFromBytecode(compiled.bytecode.data(), compiled.bytecode.size(), stageMask, debugName);
    return device.CreateShaderFromSource(compiled.sourceText.empty() ? source : compiled.sourceText,
                                         stageMask,
                                         compiled.entryPoint.empty() ? "main" : compiled.entryPoint.c_str(),
                                         debugName);
}

PipelineDesc FullscreenPipelineDesc(ShaderHandle vs, ShaderHandle ps, Format colorFormat,
                                    Format depthFormat, const char* debugName)
{
    PipelineDesc desc{};
    desc.shaderStages.push_back({ vs, ShaderStageMask::Vertex });
    desc.shaderStages.push_back({ ps, ShaderStageMask::Fragment });
    desc.topology                 = PrimitiveTopology::TriangleList;
    desc.colorFormat              = colorFormat;
    desc.depthFormat              = depthFormat;
    desc.sampleCount              = 1u;
    desc.rasterizer.cullMode      = CullMode::None;
    desc.depthStencil.depthEnable = false;
    desc.depthStencil.depthWrite  = false;
    desc.debugName                = debugName;
    return desc;
}

void BindPerFrame(const rendergraph::RGExecContext& ec, const std::shared_ptr<FrameGraphRuntimeBindings>& bindings)
{
    if (bindings && bindings->resources.perFrameCB.IsValid())
        ec.cmd->SetConstantBuffer(CBSlots::PerFrame, bindings->resources.perFrameCB,
                                  ShaderStageMask::Vertex | ShaderStageMask::Fragment);
}

// Depth of the (resolved) scene target; read directly like GTAO does.
TextureHandle SceneDepthTexture(const rendergraph::RGExecContext& ec)
{
    const rendergraph::RGResourceID hdrId = FindResourceId(*ec.resources, StandardFrameResources::HDRSceneColor);
    if (hdrId == rendergraph::RG_INVALID_RESOURCE)
        return TextureHandle::Invalid();
    const RenderTargetHandle hdrRT = ec.GetRenderTarget(hdrId);
    return hdrRT.IsValid() ? ec.device->GetRenderTargetDepthTexture(hdrRT) : TextureHandle::Invalid();
}

FrameRecipeResourceDesc ColorResource(std::string_view name, uint32_t w, uint32_t h, uint32_t samples)
{
    FrameRecipeResourceDesc d{};
    d.name        = std::string(name);
    d.width       = w;
    d.height      = h;
    d.format      = kInputFormat;
    d.kind        = RGResourceKind::ColorTexture;
    d.sampleCount = samples;
    return d;
}

} // namespace

// ─── GPU resources ───────────────────────────────────────────────────────────

bool InitializeSsrResources(IDevice& device, SsrGpuResources& res)
{
    const assets::ShaderTargetProfile target = ShaderCompiler::ResolveTargetProfile(device);
    res.vsFullscreen = CompileSsrShader(device, target, assets::ShaderStage::Vertex,   kFullscreenVsHlsl,         "ssr_fullscreen.vs");
    res.psResolve    = CompileSsrShader(device, target, assets::ShaderStage::Fragment, kResolvePsHlsl.c_str(),    "ssr_resolve.ps");
    res.psTrace      = CompileSsrShader(device, target, assets::ShaderStage::Fragment, kTracePsHlsl.c_str(),      "ssr_trace.ps");
    res.psComposite  = CompileSsrShader(device, target, assets::ShaderStage::Fragment, kCompositePsHlsl.c_str(),  "ssr_composite.ps");
    res.psCopy       = CompileSsrShader(device, target, assets::ShaderStage::Fragment, kCopyPsHlsl.c_str(),       "ssr_copy.ps");
    if (!res.vsFullscreen.IsValid() || !res.psResolve.IsValid() || !res.psTrace.IsValid() ||
        !res.psComposite.IsValid() || !res.psCopy.IsValid())
    {
        Debug::LogError("SsrPass: shader compilation failed");
        DestroySsrResources(device, res);
        return false;
    }

    PipelineDesc resolve = FullscreenPipelineDesc(res.vsFullscreen, res.psResolve, kInputFormat, Format::Unknown, "SsrResolve");
    resolve.extraColorCount      = 2u;
    resolve.extraColorFormats[0] = kInputFormat;
    resolve.extraColorFormats[1] = kInputFormat;
    res.pipelineResolve = device.CreatePipeline(resolve);

    res.pipelineTrace = device.CreatePipeline(
        FullscreenPipelineDesc(res.vsFullscreen, res.psTrace, Format::RGBA16_FLOAT, Format::Unknown, "SsrTrace"));

    // The scene composite is an RGResourceKind::RenderTarget (D24S8 attached);
    // the pipeline depth format must match it (Vulkan VUID-08914).
    PipelineDesc composite = FullscreenPipelineDesc(res.vsFullscreen, res.psComposite, Format::RGBA16_FLOAT,
                                                    Format::D24_UNORM_S8_UINT, "SsrComposite");
    composite.blendStates[0].blendEnable = true;
    composite.blendStates[0].srcBlend    = BlendFactor::One;
    composite.blendStates[0].dstBlend    = BlendFactor::One;
    composite.blendStates[0].blendOp     = BlendOp::Add;
    composite.blendStates[0].writeMask   = 0x7u;   // colour only, alpha untouched
    res.pipelineComposite = device.CreatePipeline(composite);

    res.pipelineCopy = device.CreatePipeline(
        FullscreenPipelineDesc(res.vsFullscreen, res.psCopy, Format::RGBA16_FLOAT, Format::D24_UNORM_S8_UINT, "SsrCopy"));

    if (!res.pipelineResolve.IsValid() || !res.pipelineTrace.IsValid() ||
        !res.pipelineComposite.IsValid() || !res.pipelineCopy.IsValid())
    {
        Debug::LogError("SsrPass: pipeline creation failed");
        DestroySsrResources(device, res);
        return false;
    }
    res.initialized = true;
    return true;
}

void DestroySsrResources(IDevice& device, SsrGpuResources& res)
{
    for (PipelineHandle* p : { &res.pipelineResolve, &res.pipelineTrace, &res.pipelineComposite, &res.pipelineCopy })
        if (p->IsValid()) { device.DestroyPipeline(*p); *p = {}; }
    for (ShaderHandle* s : { &res.psResolve, &res.psTrace, &res.psComposite, &res.psCopy, &res.vsFullscreen })
        if (s->IsValid()) { device.DestroyShader(*s); *s = {}; }
    res.initialized = false;
}

// ─── SsrPass ─────────────────────────────────────────────────────────────────

SsrPass::SsrPass(uint32_t id, std::shared_ptr<SsrGpuResources> resources)
    : m_id(id)
    , m_resources(std::move(resources))
{}

bool SsrPass::Active() const noexcept
{
    return m_resources && m_resources->initialized && m_resources->settings.enabled;
}

bool SsrPass::DeclaresResource(std::string_view name) const noexcept
{
    if (!Active())
        return false;
    return name == kResNormalRoughness || name == kResSpecularWeight ||
           name == kResSpecularLight || name == kResReflection;
}

uint64_t SsrPass::GetStructureRevision() const noexcept
{
    return Active() ? 1u : 0u;
}

void SsrPass::BuildPass(const PassBuildContext& ctx) const
{
    if (!Active() || !ctx.frame.enableScreenSpaceReflections || !ctx.frame.mrtSupported)
        return;

    const uint32_t w = ctx.frame.viewportWidth;
    const uint32_t h = ctx.frame.viewportHeight;
    const uint32_t samples = std::max(ctx.frame.msaaSamples, 1u);
    const bool msaa = samples > 1u;
    const std::string hdrName(StandardFrameResources::HDRSceneColor);
    const std::string compositeName(kSceneCompositeResource);

    // Inputs written by the opaque pass (multisampled with MSAA, then resolved).
    ctx.recipe.resources.push_back(ColorResource(kResNormalRoughness, w, h, 1u));
    ctx.recipe.resources.push_back(ColorResource(kResSpecularWeight, w, h, 1u));
    ctx.recipe.resources.push_back(ColorResource(kResSpecularLight, w, h, 1u));
    if (msaa)
    {
        ctx.recipe.resources.push_back(ColorResource(kResNormalRoughnessMS, w, h, samples));
        ctx.recipe.resources.push_back(ColorResource(kResSpecularWeightMS, w, h, samples));
        ctx.recipe.resources.push_back(ColorResource(kResSpecularLightMS, w, h, samples));
    }
    ctx.recipe.sceneOutputs.push_back({ std::string(msaa ? kResNormalRoughnessMS : kResNormalRoughness), kInputFormat });
    ctx.recipe.sceneOutputs.push_back({ std::string(msaa ? kResSpecularWeightMS : kResSpecularWeight), kInputFormat });
    ctx.recipe.sceneOutputs.push_back({ std::string(msaa ? kResSpecularLightMS : kResSpecularLight), kInputFormat });

    ctx.recipe.resources.push_back(ColorResource(kResReflection, w, h, 1u));

    // The scene composite exists when GTAO (an earlier contributor) made it.
    const bool haveComposite = std::any_of(ctx.recipe.resources.begin(), ctx.recipe.resources.end(),
        [&](const FrameRecipeResourceDesc& r) { return r.name == compositeName; });
    if (!haveComposite)
        ctx.recipe.resources.push_back(FrameRecipeResourceDesc{ compositeName, false, w, h,
                                                                Format::RGBA16_FLOAT, RGResourceKind::RenderTarget });

    auto res = m_resources;
    auto rtBindings = ctx.frame.runtimeBindings;

    if (msaa)
    {
        FrameRecipePassDesc pass{};
        pass.name         = "SsrResolve";
        pass.executorName = std::string(kExecResolve);
        pass.accesses.push_back({ std::string(kResNormalRoughnessMS), FrameRecipeAccessKind::ReadTexture });
        pass.accesses.push_back({ std::string(kResSpecularWeightMS),  FrameRecipeAccessKind::ReadTexture });
        pass.accesses.push_back({ std::string(kResSpecularLightMS),   FrameRecipeAccessKind::ReadTexture });
        pass.accesses.push_back({ std::string(kResNormalRoughness),   FrameRecipeAccessKind::WriteRenderTarget });
        pass.accesses.push_back({ std::string(kResSpecularWeight),    FrameRecipeAccessKind::WriteRenderTarget });
        pass.accesses.push_back({ std::string(kResSpecularLight),     FrameRecipeAccessKind::WriteRenderTarget });
        pass.renderPass.enabled            = true;
        pass.renderPass.targetResourceName = std::string(kResNormalRoughness);
        pass.renderPass.viewportWidth      = w;
        pass.renderPass.viewportHeight     = h;
        pass.renderPass.clearColor         = false;   // every pixel is written
        pass.renderPass.clearExtraColors   = false;
        pass.renderPass.extraColorTargets.push_back({ std::string(kResSpecularWeight) });
        pass.renderPass.extraColorTargets.push_back({ std::string(kResSpecularLight) });
        ctx.recipe.passes.push_back(std::move(pass));

        ctx.callbacks.Register(kExecResolve,
            [res, rtBindings](const rendergraph::RGExecContext& ec)
            {
                if (!ec.cmd || !ec.resources || !res || !res->initialized)
                    return;
                const TextureHandle nr = ec.GetTexture(FindResourceId(*ec.resources, kResNormalRoughnessMS));
                const TextureHandle sw = ec.GetTexture(FindResourceId(*ec.resources, kResSpecularWeightMS));
                const TextureHandle sl = ec.GetTexture(FindResourceId(*ec.resources, kResSpecularLightMS));
                if (!nr.IsValid() || !sw.IsValid() || !sl.IsValid())
                    return;
                ec.cmd->SetPipeline(res->pipelineResolve);
                BindPerFrame(ec, rtBindings);
                ec.cmd->SetShaderResource(TexSlots::PassSRV0, nr, ShaderStageMask::Fragment);
                ec.cmd->SetShaderResource(TexSlots::PassSRV1, sw, ShaderStageMask::Fragment);
                ec.cmd->SetShaderResource(TexSlots::PassSRV2, sl, ShaderStageMask::Fragment);
                ec.cmd->Draw(3u, 1u, 0u, 0u);
            });
    }

    // Trace. The executor opens the render pass itself: the scene depth is read
    // directly and its barrier must come before the render pass (Vulkan).
    {
        FrameRecipePassDesc pass{};
        pass.name         = "SsrTrace";
        pass.executorName = std::string(kExecTrace);
        pass.accesses.push_back({ hdrName,                           FrameRecipeAccessKind::ReadTexture });
        pass.accesses.push_back({ std::string(kResNormalRoughness), FrameRecipeAccessKind::ReadTexture });
        pass.accesses.push_back({ std::string(kResReflection),      FrameRecipeAccessKind::WriteRenderTarget });
        pass.renderPass.enabled = false;
        ctx.recipe.passes.push_back(std::move(pass));

        ctx.callbacks.Register(kExecTrace,
            [res, rtBindings, w, h](const rendergraph::RGExecContext& ec)
            {
                if (!ec.cmd || !ec.device || !ec.resources || !res || !res->initialized)
                    return;
                const rendergraph::RGResourceID hdrId = FindResourceId(*ec.resources, StandardFrameResources::HDRSceneColor);
                const TextureHandle hdrColor = ec.GetTexture(hdrId);
                const TextureHandle depth = SceneDepthTexture(ec);
                const TextureHandle nr = ec.GetTexture(FindResourceId(*ec.resources, kResNormalRoughness));
                const RenderTargetHandle out = ec.GetRenderTarget(FindResourceId(*ec.resources, kResReflection));
                if (!hdrColor.IsValid() || !depth.IsValid() || !nr.IsValid() || !out.IsValid())
                    return;

                ec.cmd->TransitionResource(depth, ResourceState::Unknown, ResourceState::ShaderRead);
                ICommandList::RenderPassBeginInfo rp{};
                rp.renderTarget = out;
                rp.clearColor   = false;   // every pixel is written
                rp.clearDepth   = false;
                ec.cmd->BeginRenderPass(rp);
                ec.cmd->SetViewport(0.f, 0.f, static_cast<float>(w), static_cast<float>(h), 0.f, 1.f);
                ec.cmd->SetScissor(0, 0, w, h);
                ec.cmd->SetPipeline(res->pipelineTrace);
                BindPerFrame(ec, rtBindings);
                ec.cmd->SetShaderResource(TexSlots::PassSRV0, depth, ShaderStageMask::Fragment);
                ec.cmd->SetShaderResource(TexSlots::PassSRV1, hdrColor, ShaderStageMask::Fragment);
                ec.cmd->SetShaderResource(TexSlots::PassSRV2, nr, ShaderStageMask::Fragment);
                ec.cmd->SetSampler(SamplerSlots::LinearClamp, SamplerSlots::LinearClamp, ShaderStageMask::Fragment);
                ec.cmd->SetSampler(SamplerSlots::PointClamp, SamplerSlots::PointClamp, ShaderStageMask::Fragment);
                ec.cmd->Draw(3u, 1u, 0u, 0u);
                ec.cmd->EndRenderPass();
                ec.cmd->TransitionResource(depth, ResourceState::Unknown, ResourceState::DepthWrite);
            });
    }

    // Scene composite = HDR copy, when GTAO does not provide it.
    if (!haveComposite)
    {
        FrameRecipePassDesc pass{};
        pass.name         = "SsrSceneCopy";
        pass.executorName = std::string(kExecCopy);
        pass.accesses.push_back({ hdrName,       FrameRecipeAccessKind::ReadTexture });
        pass.accesses.push_back({ compositeName, FrameRecipeAccessKind::WriteRenderTarget });
        pass.renderPass.enabled            = true;
        pass.renderPass.targetResourceName = compositeName;
        pass.renderPass.viewportWidth      = w;
        pass.renderPass.viewportHeight     = h;
        pass.renderPass.clearColor         = false;
        ctx.recipe.passes.push_back(std::move(pass));

        ctx.callbacks.Register(kExecCopy,
            [res, rtBindings](const rendergraph::RGExecContext& ec)
            {
                if (!ec.cmd || !ec.resources || !res || !res->initialized)
                    return;
                const TextureHandle hdr = ec.GetTexture(FindResourceId(*ec.resources, StandardFrameResources::HDRSceneColor));
                if (!hdr.IsValid())
                    return;
                ec.cmd->SetPipeline(res->pipelineCopy);
                BindPerFrame(ec, rtBindings);
                ec.cmd->SetShaderResource(TexSlots::PassSRV0, hdr, ShaderStageMask::Fragment);
                ec.cmd->SetSampler(SamplerSlots::LinearClamp, SamplerSlots::LinearClamp, ShaderStageMask::Fragment);
                ec.cmd->Draw(3u, 1u, 0u, 0u);
            });
    }

    // Composite: additive into the scene composite (after GTAO's copy, before
    // the overlays). Opens its render pass itself for the depth barrier.
    {
        FrameRecipePassDesc pass{};
        pass.name         = "SsrComposite";
        pass.executorName = std::string(kExecComposite);
        pass.accesses.push_back({ std::string(kResReflection),      FrameRecipeAccessKind::ReadTexture });
        pass.accesses.push_back({ std::string(kResNormalRoughness), FrameRecipeAccessKind::ReadTexture });
        pass.accesses.push_back({ std::string(kResSpecularWeight),  FrameRecipeAccessKind::ReadTexture });
        pass.accesses.push_back({ std::string(kResSpecularLight),   FrameRecipeAccessKind::ReadTexture });
        pass.accesses.push_back({ hdrName,                          FrameRecipeAccessKind::ReadTexture });
        pass.accesses.push_back({ compositeName,                    FrameRecipeAccessKind::WriteRenderTarget });
        pass.renderPass.enabled = false;
        ctx.recipe.passes.push_back(std::move(pass));

        ctx.callbacks.Register(kExecComposite,
            [res, rtBindings, w, h](const rendergraph::RGExecContext& ec)
            {
                if (!ec.cmd || !ec.device || !ec.resources || !res || !res->initialized)
                    return;
                const TextureHandle refl = ec.GetTexture(FindResourceId(*ec.resources, kResReflection));
                const TextureHandle nr   = ec.GetTexture(FindResourceId(*ec.resources, kResNormalRoughness));
                const TextureHandle sw   = ec.GetTexture(FindResourceId(*ec.resources, kResSpecularWeight));
                const TextureHandle sl   = ec.GetTexture(FindResourceId(*ec.resources, kResSpecularLight));
                const TextureHandle depth = SceneDepthTexture(ec);
                const RenderTargetHandle out = ec.GetRenderTarget(FindResourceId(*ec.resources, kSceneCompositeResource));
                if (!refl.IsValid() || !nr.IsValid() || !sw.IsValid() || !sl.IsValid() || !depth.IsValid() || !out.IsValid())
                    return;

                ec.cmd->TransitionResource(depth, ResourceState::Unknown, ResourceState::ShaderRead);
                ICommandList::RenderPassBeginInfo rp{};
                rp.renderTarget = out;
                rp.clearColor   = false;
                rp.clearDepth   = false;
                ec.cmd->BeginRenderPass(rp);
                ec.cmd->SetViewport(0.f, 0.f, static_cast<float>(w), static_cast<float>(h), 0.f, 1.f);
                ec.cmd->SetScissor(0, 0, w, h);
                ec.cmd->SetPipeline(res->pipelineComposite);
                BindPerFrame(ec, rtBindings);
                ec.cmd->SetShaderResource(TexSlots::PassSRV0, refl, ShaderStageMask::Fragment);
                ec.cmd->SetShaderResource(TexSlots::PassSRV1, nr, ShaderStageMask::Fragment);
                ec.cmd->SetShaderResource(TexSlots::PassSRV2, sw, ShaderStageMask::Fragment);
                ec.cmd->SetShaderResource(TexSlots::HistoryBuffer, sl, ShaderStageMask::Fragment);
                ec.cmd->SetShaderResource(TexSlots::BloomTexture, depth, ShaderStageMask::Fragment);
                ec.cmd->Draw(3u, 1u, 0u, 0u);
                ec.cmd->EndRenderPass();
                ec.cmd->TransitionResource(depth, ResourceState::Unknown, ResourceState::DepthWrite);
            });
    }
}

} // namespace engine::renderer::addons::ssr
