#include "ContactShadowPass.hpp"
#include "addons/forward/StandardFramePipeline.hpp"
#include "assets/AssetRegistry.hpp"
#include "renderer/IDevice.hpp"
#include "renderer/RenderPipelineRecipe.hpp"
#include "renderer/ShaderBindingModel.hpp"
#include "renderer/ShaderCompiler.hpp"
#include "renderer/RenderRuntimeFrameBindings.hpp"
#include "rendergraph/CompiledFrame.hpp"
#include "core/Debug.hpp"
#include <string>

namespace engine::renderer::addons::contact_shadow {

static constexpr std::string_view kResContactShadowRaw    = "ContactShadowRaw";
// Read by the opaque pass on TexSlots::ContactShadow (FrameRecipe::opaqueInputs).
static constexpr std::string_view kResContactShadowOutput = "ContactShadowOutput";
static constexpr std::string_view kExecTrace              = "frame.contact_shadow_trace";
static constexpr std::string_view kExecBlur               = "frame.contact_shadow_blur";

// ─── HLSL Shaders ────────────────────────────────────────────────────────────

static constexpr const char* kFullscreenVsHlsl = R"(
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

// Same layout as FrameConstants (RenderFrameConstants.hpp).
static constexpr const char* kPerFramePrefixHlsl = R"(
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
    float4       featureParams[8]; // add-on parameter blocks (FrameConstants::featureParams)
};

// Contact shadow block (krom-contact-shadow, featureParams slots 3 and 4,
// see kContactShadowFrameParameterBlock).
#define csLightDirWS        (featureParams[3].xyz)
#define csLightIndexPlusOne (asuint(featureParams[3].w))
#define csLength            (featureParams[4].x)
#define csThickness         (featureParams[4].y)
#define csIntensity         (featureParams[4].z)
#define csSteps             (asuint(featureParams[4].w))
)";

// Ray march from the visible surface towards the main light through the depth of
// this frame's prepass. Output: x = visibility (1 = lit), y = linear depth for the blur.
static const std::string kTracePsHlsl = std::string(kPerFramePrefixHlsl) + R"(
#ifdef KROM_CONTACT_SHADOW_MSAA
Texture2DMS<float> uSceneDepth : register(t8);
#else
Texture2D<float>   uSceneDepth : register(t8);
#endif

struct PSInput { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

// Contact shadows only up close: full strength up to 40 m, gone at 50 m.
static const float kFadeStart = 40.0f;
static const float kFadeEnd   = 50.0f;

static int2 gSize;

float LoadDepth(int2 pixel)
{
    const int2 p = clamp(pixel, int2(0, 0), gSize - 1);
#ifdef KROM_CONTACT_SHADOW_MSAA
    return uSceneDepth.Load(p, 0);
#else
    return uSceneDepth.Load(int3(p, 0));
#endif
}

float3 WorldPosition(float2 uv, float depth)
{
    const float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    const float4 h = mul(invViewProjMatrix, float4(ndc, depth, 1.0f));
    return h.xyz / max(abs(h.w), 1e-6f);
}

float LinearDepth(float3 positionWS)
{
    return dot(positionWS - cameraPositionWS.xyz, normalize(cameraForwardWS.xyz));
}

// Interleaved gradient noise (Jimenez 2014).
float InterleavedGradientNoise(float2 pixel)
{
    return frac(52.9829189f * frac(dot(pixel, float2(0.06711056f, 0.00583715f))));
}

float2 main(PSInput IN) : SV_TARGET
{
#ifdef KROM_CONTACT_SHADOW_MSAA
    uint w, h, samples;
    uSceneDepth.GetDimensions(w, h, samples);
#else
    uint w, h;
    uSceneDepth.GetDimensions(w, h);
#endif
    gSize = int2(w, h);

    const int2   pixel = int2(IN.pos.xy);
    const float  depth = LoadDepth(pixel);
    const float2 size  = float2(gSize);
    const float3 P = WorldPosition((float2(pixel) + 0.5f) / size, depth);
    // Face normal from the depth, before any branch (derivatives need the whole quad).
    float3 Ng = cross(ddy(P), ddx(P));
    const float ngLen = length(Ng);
    Ng = (ngLen > 1e-12f) ? Ng / ngLen : float3(0.0f, 1.0f, 0.0f);
    if (dot(Ng, cameraPositionWS.xyz - P) < 0.0f)
        Ng = -Ng;

    if (depth >= 0.999999f)
        return float2(1.0f, farPlane);

    const float  z = LinearDepth(P);
    const float  distanceFade = saturate((kFadeEnd - z) / (kFadeEnd - kFadeStart));
    if (csLightIndexPlusOne == 0u || distanceFade <= 0.0f || csIntensity <= 0.0f)
        return float2(1.0f, z);

    const float3 L        = normalize(csLightDirWS);
    // Surfaces facing away from the light get no direct light anyway (NoL); a ray from
    // them would run into their own object.
    if (dot(Ng, L) <= 0.02f)
        return float2(1.0f, z);
    const uint   steps    = clamp(csSteps, 4u, 32u);
    const float  rayLen   = max(csLength, 1e-3f);
    const float  stepLen  = rayLen / float(steps);
    const float  thickness = max(csThickness, 1e-4f);
    // Depth tolerance against self-shadowing from depth precision; grows with distance.
    const float  depthBias = 0.002f + z * 0.0008f;
    const float  jitter   = InterleavedGradientNoise(float2(pixel));
    // Start slightly above the surface against self-shadowing on flat ground.
    const float  normalOffset = 0.002f + z * 0.0005f;

    float occlusion = 0.0f;
    [loop] for (uint i = 0u; i < steps; ++i)
    {
        const float  t = (float(i) + jitter) * stepLen + stepLen * 0.25f;
        const float3 S = P + Ng * normalOffset + L * t;
        const float4 clip = mul(viewProjMatrix, float4(S, 1.0f));
        if (clip.w <= 1e-5f)
            break;
        const float2 ndc = clip.xy / clip.w;
        const float2 suv = float2(ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f);
        if (any(suv < 0.0f) || any(suv > 1.0f))
            break;

        const int2  sp = int2(suv * size);
        const float sd = LoadDepth(sp);
        if (sd >= 0.999999f)
            continue;
        const float sceneZ = LinearDepth(WorldPosition((float2(sp) + 0.5f) / size, sd));
        const float delta  = LinearDepth(S) - sceneZ;
        // The assumed occluder depth grows along the ray (like a cone), so thick
        // objects cast a closed shadow instead of thin crescents along their edges.
        if (delta > depthBias && delta < thickness + t * 0.5f)
        {
            // Darkest at the contact, fading out towards the ray end: a hit is a
            // soft value instead of a hard 0/1 edge. Soft at the screen border.
            const float distanceToHit = saturate(t / rayLen);
            const float rayFade  = 1.0f - distanceToHit * distanceToHit * (3.0f - 2.0f * distanceToHit);
            const float2 border  = min(suv, 1.0f - suv);
            const float edgeFade = saturate(min(border.x, border.y) * 20.0f);
            occlusion = rayFade * edgeFade;
            break;
        }
    }
    // Surfaces almost parallel to the light: fade out instead of a hard seam.
    occlusion *= saturate((dot(Ng, L) - 0.02f) * 8.0f);

    return float2(1.0f - saturate(csIntensity) * occlusion * distanceFade, z);
}
)";

// 3x3 depth-aware blur against the step noise of the trace.
static const std::string kBlurPsHlsl = std::string(kPerFramePrefixHlsl) + R"(
Texture2D<float2> uRaw : register(t8);

struct PSInput { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

float main(PSInput IN) : SV_TARGET
{
    uint w, h;
    uRaw.GetDimensions(w, h);
    const int2   size   = int2(w, h);
    const int2   pixel  = int2(IN.pos.xy);
    const float2 center = uRaw.Load(int3(pixel, 0));
    const float  sigma  = max(center.y * 0.03f, 0.02f);

    float sum = 0.0f;
    float wsum = 0.0f;
    [unroll] for (int y = -1; y <= 1; ++y)
    {
        [unroll] for (int x = -1; x <= 1; ++x)
        {
            const int2   p = clamp(pixel + int2(x, y), int2(0, 0), size - 1);
            const float2 s = uRaw.Load(int3(p, 0));
            const float  d = (s.y - center.y) / sigma;
            const float  wgt = exp(-d * d) * ((x == 0 && y == 0) ? 2.0f : 1.0f);
            sum  += s.x * wgt;
            wsum += wgt;
        }
    }
    return (wsum > 1e-5f) ? sum / wsum : center.x;
}
)";

// ─── Resource Helpers ────────────────────────────────────────────────────────

static rendergraph::RGResourceID FindResourceId(
    const std::vector<rendergraph::CompiledResourceSnapshot>& snaps,
    std::string_view name)
{
    for (const auto& s : snaps)
        if (s.debugName == name) return s.id;
    return rendergraph::RG_INVALID_RESOURCE;
}

static PipelineHandle MakeFullscreenPipeline(IDevice& device, ShaderHandle vs, ShaderHandle ps,
                                             Format colorFormat, const char* debugName)
{
    PipelineDesc desc{};
    desc.shaderStages.push_back({ vs, ShaderStageMask::Vertex });
    desc.shaderStages.push_back({ ps, ShaderStageMask::Fragment });
    desc.topology                 = PrimitiveTopology::TriangleList;
    desc.colorFormat              = colorFormat;
    desc.depthFormat              = Format::Unknown;
    desc.sampleCount              = 1u;
    desc.rasterizer.cullMode      = CullMode::None;
    desc.depthStencil.depthEnable = false;
    desc.depthStencil.depthWrite  = false;
    desc.debugName                = debugName;
    return device.CreatePipeline(desc);
}

static ShaderHandle CompileShader(IDevice& device, assets::ShaderTargetProfile target,
                                  assets::ShaderStage stage, const char* source, const char* debugName)
{
    assets::ShaderAsset asset{};
    asset.debugName      = debugName;
    asset.stage          = stage;
    asset.sourceLanguage = assets::ShaderSourceLanguage::HLSL;
    asset.entryPoint     = "main";
    asset.sourceCode     = source;

    const ShaderStageMask stageMask = (stage == assets::ShaderStage::Vertex)
        ? ShaderStageMask::Vertex : ShaderStageMask::Fragment;

    assets::CompiledShaderArtifact compiled{};
    std::string error;
    if (!ShaderCompiler::CompileForTarget(asset, target, compiled, &error))
    {
        Debug::LogError("ContactShadowPass: shader compile failed '%s': %s", debugName, error.c_str());
        return ShaderHandle::Invalid();
    }
    if (!compiled.bytecode.empty())
        return device.CreateShaderFromBytecode(compiled.bytecode.data(), compiled.bytecode.size(), stageMask, debugName);
    return device.CreateShaderFromSource(
        compiled.sourceText.empty() ? source : compiled.sourceText,
        stageMask,
        compiled.entryPoint.empty() ? "main" : compiled.entryPoint.c_str(),
        debugName);
}

bool InitializeContactShadowResources(IDevice& device, ContactShadowGpuResources& res)
{
    const assets::ShaderTargetProfile target = ShaderCompiler::ResolveTargetProfile(device);
    const std::string traceMS = "#define KROM_CONTACT_SHADOW_MSAA 1\n" + kTracePsHlsl;

    res.vsFullscreen = CompileShader(device, target, assets::ShaderStage::Vertex,   kFullscreenVsHlsl,    "contact_shadow_fullscreen.vs");
    res.psTrace      = CompileShader(device, target, assets::ShaderStage::Fragment, kTracePsHlsl.c_str(), "contact_shadow_trace.ps");
    res.psTraceMS    = CompileShader(device, target, assets::ShaderStage::Fragment, traceMS.c_str(),      "contact_shadow_trace_ms.ps");
    res.psBlur       = CompileShader(device, target, assets::ShaderStage::Fragment, kBlurPsHlsl.c_str(),  "contact_shadow_blur.ps");
    if (!res.vsFullscreen.IsValid() || !res.psTrace.IsValid() || !res.psTraceMS.IsValid() || !res.psBlur.IsValid())
    {
        Debug::LogError("ContactShadowPass: shader compilation failed");
        DestroyContactShadowResources(device, res);
        return false;
    }

    res.pipelineTrace   = MakeFullscreenPipeline(device, res.vsFullscreen, res.psTrace,   Format::RG16_FLOAT, "ContactShadowTrace");
    res.pipelineTraceMS = MakeFullscreenPipeline(device, res.vsFullscreen, res.psTraceMS, Format::RG16_FLOAT, "ContactShadowTraceMS");
    res.pipelineBlur    = MakeFullscreenPipeline(device, res.vsFullscreen, res.psBlur,    Format::R8_UNORM,   "ContactShadowBlur");
    if (!res.pipelineTrace.IsValid() || !res.pipelineTraceMS.IsValid() || !res.pipelineBlur.IsValid())
    {
        Debug::LogError("ContactShadowPass: pipeline creation failed");
        DestroyContactShadowResources(device, res);
        return false;
    }

    res.initialized = true;
    return true;
}

void DestroyContactShadowResources(IDevice& device, ContactShadowGpuResources& res)
{
    if (res.pipelineBlur.IsValid())    { device.DestroyPipeline(res.pipelineBlur);    res.pipelineBlur    = {}; }
    if (res.pipelineTraceMS.IsValid()) { device.DestroyPipeline(res.pipelineTraceMS); res.pipelineTraceMS = {}; }
    if (res.pipelineTrace.IsValid())   { device.DestroyPipeline(res.pipelineTrace);   res.pipelineTrace   = {}; }
    if (res.psBlur.IsValid())          { device.DestroyShader(res.psBlur);            res.psBlur          = {}; }
    if (res.psTraceMS.IsValid())       { device.DestroyShader(res.psTraceMS);         res.psTraceMS       = {}; }
    if (res.psTrace.IsValid())         { device.DestroyShader(res.psTrace);           res.psTrace         = {}; }
    if (res.vsFullscreen.IsValid())    { device.DestroyShader(res.vsFullscreen);      res.vsFullscreen    = {}; }
    res.initialized = false;
}

// ─── ContactShadowPass ───────────────────────────────────────────────────────

ContactShadowPass::ContactShadowPass(uint32_t id, std::shared_ptr<ContactShadowGpuResources> resources)
    : m_id(id)
    , m_resources(std::move(resources))
{}

bool ContactShadowPass::Active() const noexcept
{
    return m_resources && m_resources->initialized && m_resources->settings.enabled;
}

uint64_t ContactShadowPass::GetStructureRevision() const noexcept
{
    return Active() ? 1u : 0u;
}

bool ContactShadowPass::DeclaresResource(std::string_view name) const noexcept
{
    return Active() && (name == kResContactShadowRaw || name == kResContactShadowOutput);
}

void ContactShadowPass::BuildPass(const PassBuildContext& ctx) const
{
    // Needs this frame's depth prepass; views without it (previews, bakes) get none.
    if (!ctx.frame.enableDepthPrepass || !Active())
        return;

    const uint32_t w = ctx.frame.viewportWidth;
    const uint32_t h = ctx.frame.viewportHeight;
    const bool msaa = ctx.frame.msaaSamples > 1u;
    ctx.recipe.resources.push_back(FrameRecipeResourceDesc{ std::string(kResContactShadowRaw),    false, w, h, Format::RG16_FLOAT, RGResourceKind::ColorTexture });
    ctx.recipe.resources.push_back(FrameRecipeResourceDesc{ std::string(kResContactShadowOutput), false, w, h, Format::R8_UNORM,   RGResourceKind::ColorTexture });

    {
        FrameRecipePassDesc pass{};
        pass.name         = "ContactShadowTrace";
        pass.executorName = std::string(kExecTrace);
        pass.accesses.push_back({ StandardFrameRecipeBuilder::kOpaqueDepthResource, FrameRecipeAccessKind::ReadDepthStencil });
        pass.accesses.push_back({ std::string(kResContactShadowRaw),               FrameRecipeAccessKind::WriteRenderTarget });
        pass.renderPass.enabled            = true;
        pass.renderPass.targetResourceName = std::string(kResContactShadowRaw);
        pass.renderPass.viewportWidth      = w;
        pass.renderPass.viewportHeight     = h;
        pass.renderPass.clearColor         = false; // the fullscreen triangle covers every pixel
        ctx.recipe.passes.push_back(std::move(pass));
    }
    {
        FrameRecipePassDesc pass{};
        pass.name         = "ContactShadowBlur";
        pass.executorName = std::string(kExecBlur);
        pass.accesses.push_back({ std::string(kResContactShadowRaw),    FrameRecipeAccessKind::ReadTexture });
        pass.accesses.push_back({ std::string(kResContactShadowOutput), FrameRecipeAccessKind::WriteRenderTarget });
        pass.renderPass.enabled            = true;
        pass.renderPass.targetResourceName = std::string(kResContactShadowOutput);
        pass.renderPass.viewportWidth      = w;
        pass.renderPass.viewportHeight     = h;
        pass.renderPass.clearColor         = false;
        ctx.recipe.passes.push_back(std::move(pass));
    }
    ctx.recipe.opaqueInputs.push_back({ std::string(kResContactShadowOutput), TexSlots::ContactShadow });

    auto res        = m_resources;
    auto rtBindings = ctx.frame.runtimeBindings;

    ctx.callbacks.Register(kExecTrace,
        [res, msaa, rtBindings](const rendergraph::RGExecContext& ec)
        {
            if (!ec.cmd || !res || !res->initialized || !ec.resources) return;
            const TextureHandle depthTex = ec.GetTexture(
                FindResourceId(*ec.resources, StandardFrameRecipeBuilder::kOpaqueDepthResource));
            if (!depthTex.IsValid()) return;

            ec.cmd->SetPipeline(msaa ? res->pipelineTraceMS : res->pipelineTrace);
            if (rtBindings && rtBindings->resources.perFrameCB.IsValid())
                ec.cmd->SetConstantBuffer(CBSlots::PerFrame, rtBindings->resources.perFrameCB,
                    ShaderStageMask::Vertex | ShaderStageMask::Fragment);
            ec.cmd->SetShaderResource(TexSlots::PassSRV0, depthTex, ShaderStageMask::Fragment);
            ec.cmd->Draw(3u, 1u, 0u, 0u);
        });

    ctx.callbacks.Register(kExecBlur,
        [res, rtBindings](const rendergraph::RGExecContext& ec)
        {
            if (!ec.cmd || !res || !res->initialized || !ec.resources) return;
            const rendergraph::RGResourceID rawId = FindResourceId(*ec.resources, kResContactShadowRaw);
            if (rawId == rendergraph::RG_INVALID_RESOURCE) return;
            const TextureHandle rawTex = ec.GetTexture(rawId);
            if (!rawTex.IsValid()) return;

            ec.cmd->SetPipeline(res->pipelineBlur);
            if (rtBindings && rtBindings->resources.perFrameCB.IsValid())
                ec.cmd->SetConstantBuffer(CBSlots::PerFrame, rtBindings->resources.perFrameCB,
                    ShaderStageMask::Vertex | ShaderStageMask::Fragment);
            ec.cmd->SetShaderResource(TexSlots::PassSRV0, rawTex, ShaderStageMask::Fragment);
            ec.cmd->Draw(3u, 1u, 0u, 0u);
        });
}

} // namespace engine::renderer::addons::contact_shadow
