#include "GtaoPass.hpp"
#include "addons/forward/StandardFramePipeline.hpp"
#include "assets/AssetRegistry.hpp"
#include "renderer/IDevice.hpp"
#include "renderer/RenderPipelineRecipe.hpp"
#include "renderer/ShaderBindingModel.hpp"
#include "renderer/ShaderCompiler.hpp"
#include "renderer/RenderRuntimeFrameBindings.hpp"
#include "rendergraph/CompiledFrame.hpp"
#include "core/Debug.hpp"
#include <algorithm>
#include <string>

namespace engine::renderer::addons::gtao {

static constexpr std::string_view kResSceneLinearDepth = "SceneLinearDepth";
static constexpr std::string_view kResGTAORaw          = "GTAORaw";
static constexpr std::string_view kResGTAOOutput       = StandardFrameRecipeBuilder::kGTAOOutputResource;
static constexpr std::string_view kResGTAOComposite    = "GTAOComposite";
static constexpr std::string_view kExecLinearize       = "frame.gtao_linearize";
static constexpr std::string_view kExecGtaoMain        = "frame.gtao_main";
static constexpr std::string_view kExecBlur            = "frame.gtao_blur";
static constexpr std::string_view kExecComposite       = "frame.gtao_composite";

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

// GTAO add-on block (krom-gtao, featureParams slot 0, see kGtaoFrameParameterBlock):
// x = radius, y = bias, z = intensity, w = enabled (uint bits)
#define gtaoRadius    (featureParams[0].x)
#define gtaoBias      (featureParams[0].y)
#define gtaoIntensity (featureParams[0].z)
#define gtaoEnabled   (asuint(featureParams[0].w))
)";

// Reconstruct hardware depth to camera-forward linear depth in world units.
static const std::string kLinearizeDepthPsHlsl = std::string(kPerFramePrefixHlsl) + R"(
#ifdef KROM_GTAO_MSAA
Texture2DMS<float> uSceneDepth : register(t8);
#else
Texture2D<float> uSceneDepth : register(t8);
#endif

struct PSInput { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

float3 ReconstructWorldPosition(float2 uv, float depth)
{
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float4 worldH = mul(invViewProjMatrix, float4(ndc, depth, 1.0f));
    return worldH.xyz / max(abs(worldH.w), 1e-6f);
}

float main(PSInput IN) : SV_TARGET
{
    const int2 pixel = int2(IN.pos.xy);
#ifdef KROM_GTAO_MSAA
    uint width, height, sampleCount;
    uSceneDepth.GetDimensions(width, height, sampleCount);
    float depth = 1.0f;
    for (uint sampleIndex = 0u; sampleIndex < sampleCount; ++sampleIndex)
        depth = min(depth, uSceneDepth.Load(pixel, sampleIndex));
#else
    const float depth = uSceneDepth.Load(int3(pixel, 0));
#endif
    if (depth >= 0.999999f)
        return farPlane;

    float3 worldPos = ReconstructWorldPosition(IN.uv, depth);
    float linearZ = dot(worldPos - cameraPositionWS.xyz, normalize(cameraForwardWS.xyz));
    return clamp(linearZ, 0.0f, farPlane);
}
)";

// Horizon-based AO (HBAO style), 4 directions x 4 steps.
static const std::string kGtaoMainPsHlsl = std::string(kPerFramePrefixHlsl) + R"(
Texture2D<float> uLinearDepth : register(t8);
SamplerState     uSampler     : register(s2);

struct PSInput { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

static const int   kNumDir  = 4;
static const int   kNumStep = 4;
static const float kPi      = 3.14159265f;

float3 ViewPos(float2 uv, float linearZ)
{
    float2 ndc    = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float2 viewXY = ndc * float2(1.0f / projMatrix[0][0], 1.0f / projMatrix[1][1]) * linearZ;
    return float3(viewXY, -linearZ);
}

float main(PSInput IN) : SV_TARGET
{
    float centerDepth = uLinearDepth.SampleLevel(uSampler, IN.uv, 0).r;
    if (farPlane >= 1.0f && centerDepth >= farPlane * 0.99f)
        return 1.0f;

    float radius = max(gtaoRadius, 0.01f);
    float bias   = max(gtaoBias,   0.0f);

    float3 P         = ViewPos(IN.uv, centerDepth);
    float  focalY    = abs(projMatrix[1][1]);
    float  radiusPx  = clamp(radius * focalY / max(centerDepth, 0.001f) * screenSize.y * 0.5f, 4.0f, 64.0f);
    float  stepSizePx = radiusPx / float(kNumStep);
    float2 texelSize = 1.0f / screenSize.xy;

    float noise = frac(sin(dot(floor(IN.pos.xy), float2(127.1f, 311.7f))) * 43758.547f);

    float ao = 0.0f;
    [unroll] for (int d = 0; d < kNumDir; d++)
    {
        float  theta    = (float(d) + noise) * (kPi / float(kNumDir));
        float2 dir      = float2(cos(theta), sin(theta));
        float  maxSinH  = sin(bias);

        [unroll] for (int s = 1; s <= kNumStep; s++)
        {
            float2 sampleUV = saturate(IN.uv + dir * (float(s) * stepSizePx) * texelSize);
            float3 H        = ViewPos(sampleUV, uLinearDepth.SampleLevel(uSampler, sampleUV, 0).r) - P;
            float  lenH     = length(H);
            if (lenH < 1e-4f) continue;

            float falloff     = saturate(1.0f - lenH / (radius * 2.0f));
            float weightedSin = lerp(maxSinH, H.z / lenH, falloff);
            maxSinH           = max(maxSinH, weightedSin);
        }

        ao += 1.0f - max(0.0f, maxSinH);
    }

    return saturate(ao / float(kNumDir));
}
)";

// Depth-aware (bilateral) blur to denoise the raw GTAO before compositing.
// The main pass rotates its sampling per pixel, which leaves a fixed interleave
// pattern; a small cross-bilateral kernel removes it while preserving depth edges.
static const std::string kBlurPsHlsl = std::string(kPerFramePrefixHlsl) + R"(
Texture2D<float> uAOMap    : register(t8);
Texture2D<float> uLinearZ  : register(t9);
SamplerState     uSampler  : register(s2);

struct PSInput { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

float main(PSInput IN) : SV_TARGET
{
    const float2 texel   = 1.0f / max(screenSize.xy, float2(1.0f, 1.0f));
    const float  centerZ = uLinearZ.SampleLevel(uSampler, IN.uv, 0).r;
    // Tolerate ~5% relative depth difference before rejecting a neighbour.
    const float  depthSigma = max(centerZ * 0.05f, 0.02f);

    float sum  = 0.0f;
    float wsum = 0.0f;
    const int R = 2; // 5x5 kernel
    [unroll] for (int y = -R; y <= R; ++y)
    {
        [unroll] for (int x = -R; x <= R; ++x)
        {
            const float2 o  = float2(x, y) * texel;
            const float  a  = uAOMap.SampleLevel(uSampler, IN.uv + o, 0).r;
            const float  z  = uLinearZ.SampleLevel(uSampler, IN.uv + o, 0).r;
            const float  dz = (z - centerZ) / depthSigma;
            const float  w  = exp(-dz * dz);
            sum  += a * w;
            wsum += w;
        }
    }

    return (wsum > 1e-5f) ? (sum / wsum) : uAOMap.SampleLevel(uSampler, IN.uv, 0).r;
}
)";

// The lit pass applies GTAO. The composite is a scene-colour copy for SSR,
// transparent draws, overlays and tonemapping.
static const std::string kCompositePsHlsl = std::string(kPerFramePrefixHlsl) + R"(
Texture2D<float4> uHDRColor : register(t8);
SamplerState      uSampler  : register(s1);

struct PSInput { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

float4 main(PSInput IN) : SV_TARGET
{
    return uHDRColor.SampleLevel(uSampler, IN.uv, 0);
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

// ─── GPU Resource Init / Destroy ─────────────────────────────────────────────

static PipelineHandle MakeFullscreenPipeline(IDevice& device,
    ShaderHandle vs, ShaderHandle ps,
    Format colorFormat, const char* debugName,
    Format depthFormat = Format::Unknown)
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
    return device.CreatePipeline(desc);
}

static ShaderHandle CompileGtaoShader(IDevice& device,
    assets::ShaderTargetProfile target,
    assets::ShaderStage stage,
    const char* source, const char* debugName)
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
        Debug::LogError("GtaoPass: shader compile failed '%s': %s", debugName, error.c_str());
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

bool InitializeGtaoResources(IDevice& device, GtaoGpuResources& res)
{
    const assets::ShaderTargetProfile target = ShaderCompiler::ResolveTargetProfile(device);

    res.vsFullscreen     = CompileGtaoShader(device, target, assets::ShaderStage::Vertex,   kFullscreenVsHlsl,     "gtao_fullscreen.vs");
    res.psLinearizeDepth = CompileGtaoShader(device, target, assets::ShaderStage::Fragment, kLinearizeDepthPsHlsl.c_str(), "gtao_linearize.ps");
    const std::string linearizeMS = "#define KROM_GTAO_MSAA 1\n" + kLinearizeDepthPsHlsl;
    res.psLinearizeDepthMS = CompileGtaoShader(device, target, assets::ShaderStage::Fragment, linearizeMS.c_str(), "gtao_linearize_ms.ps");
    res.psGtaoMain       = CompileGtaoShader(device, target, assets::ShaderStage::Fragment, kGtaoMainPsHlsl.c_str(),       "gtao_main.ps");
    res.psBlur           = CompileGtaoShader(device, target, assets::ShaderStage::Fragment, kBlurPsHlsl.c_str(),           "gtao_blur.ps");
    res.psComposite      = CompileGtaoShader(device, target, assets::ShaderStage::Fragment, kCompositePsHlsl.c_str(),      "gtao_composite.ps");

    if (!res.vsFullscreen.IsValid() || !res.psLinearizeDepth.IsValid() || !res.psLinearizeDepthMS.IsValid() ||
        !res.psGtaoMain.IsValid()   || !res.psBlur.IsValid() || !res.psComposite.IsValid())
    {
        Debug::LogError("GtaoPass: shader compilation failed");
        DestroyGtaoResources(device, res);
        return false;
    }

    res.pipelineLinearize = MakeFullscreenPipeline(device, res.vsFullscreen, res.psLinearizeDepth, Format::R32_FLOAT,    "GtaoLinearize");
    res.pipelineLinearizeMS = MakeFullscreenPipeline(device, res.vsFullscreen, res.psLinearizeDepthMS, Format::R32_FLOAT, "GtaoLinearizeMS");
    res.pipelineGtaoMain  = MakeFullscreenPipeline(device, res.vsFullscreen, res.psGtaoMain,       Format::R16_FLOAT,    "GtaoMain");
    res.pipelineBlur      = MakeFullscreenPipeline(device, res.vsFullscreen, res.psBlur,           Format::R16_FLOAT,    "GtaoBlur");
    // GTAOComposite ist ein RGResourceKind::RenderTarget und bekommt dadurch ein
    // D24S8-Depth-Attachment; das Pipeline-Format muss passen (Vulkan VUID-08914).
    res.pipelineComposite = MakeFullscreenPipeline(device, res.vsFullscreen, res.psComposite,      Format::RGBA16_FLOAT, "GtaoComposite",
                                                   Format::D24_UNORM_S8_UINT);

    if (!res.pipelineLinearize.IsValid() || !res.pipelineLinearizeMS.IsValid() || !res.pipelineGtaoMain.IsValid() || !res.pipelineBlur.IsValid() || !res.pipelineComposite.IsValid())
    {
        Debug::LogError("GtaoPass: pipeline creation failed");
        DestroyGtaoResources(device, res);
        return false;
    }

    res.initialized = true;
    return true;
}

void DestroyGtaoResources(IDevice& device, GtaoGpuResources& res)
{
    if (res.pipelineComposite.IsValid())  { device.DestroyPipeline(res.pipelineComposite);  res.pipelineComposite  = {}; }
    if (res.psComposite.IsValid())        { device.DestroyShader(res.psComposite);           res.psComposite        = {}; }
    if (res.pipelineBlur.IsValid())       { device.DestroyPipeline(res.pipelineBlur);       res.pipelineBlur       = {}; }
    if (res.pipelineGtaoMain.IsValid())   { device.DestroyPipeline(res.pipelineGtaoMain);   res.pipelineGtaoMain   = {}; }
    if (res.pipelineLinearizeMS.IsValid()) { device.DestroyPipeline(res.pipelineLinearizeMS); res.pipelineLinearizeMS = {}; }
    if (res.pipelineLinearize.IsValid())  { device.DestroyPipeline(res.pipelineLinearize);  res.pipelineLinearize  = {}; }
    if (res.psBlur.IsValid())             { device.DestroyShader(res.psBlur);                res.psBlur             = {}; }
    if (res.psGtaoMain.IsValid())         { device.DestroyShader(res.psGtaoMain);            res.psGtaoMain         = {}; }
    if (res.psLinearizeDepthMS.IsValid()) { device.DestroyShader(res.psLinearizeDepthMS); res.psLinearizeDepthMS = {}; }
    if (res.psLinearizeDepth.IsValid())   { device.DestroyShader(res.psLinearizeDepth);      res.psLinearizeDepth   = {}; }
    if (res.vsFullscreen.IsValid())       { device.DestroyShader(res.vsFullscreen);          res.vsFullscreen       = {}; }
    res.initialized = false;
}

// ─── GtaoPass ────────────────────────────────────────────────────────────────

GtaoPass::GtaoPass(uint32_t id, PassPhase phase, std::shared_ptr<GtaoGpuResources> resources)
    : m_id(id)
    , m_phase(phase)
    , m_resources(std::move(resources))
{}

bool GtaoPass::Active() const noexcept
{
    return m_resources && m_resources->initialized && m_resources->settings.enabled;
}

uint64_t GtaoPass::GetStructureRevision() const noexcept
{
    return Active() ? 1u : 0u;
}

bool GtaoPass::DeclaresResource(std::string_view name) const noexcept
{
    if (!Active())
        return false;
    if (m_phase == PassPhase::BeforeTransparent)
        return name == kResGTAOComposite;
    return name == kResSceneLinearDepth || name == kResGTAORaw || name == kResGTAOOutput;
}

void GtaoPass::BuildPass(const PassBuildContext& ctx) const
{
    if (!ctx.frame.enableAmbientOcclusion || !Active())
        return;
    if (m_phase == PassPhase::BeforeTransparent)
    {
        BuildComposite(ctx);
        return;
    }

    const uint32_t w = ctx.frame.viewportWidth;
    const uint32_t h = ctx.frame.viewportHeight;
    const bool msaa = ctx.frame.msaaSamples > 1u;
    ctx.recipe.resources.push_back(FrameRecipeResourceDesc{ std::string(kResSceneLinearDepth), false, w, h, Format::R32_FLOAT, RGResourceKind::ColorTexture });
    ctx.recipe.resources.push_back(FrameRecipeResourceDesc{ std::string(kResGTAORaw),          false, w, h, Format::R16_FLOAT, RGResourceKind::ColorTexture });
    ctx.recipe.resources.push_back(FrameRecipeResourceDesc{ std::string(kResGTAOOutput),       false, w, h, Format::R16_FLOAT, RGResourceKind::ColorTexture });

    // Pass 1: Linearize Depth
    {
        FrameRecipePassDesc pass{};
        pass.name         = "GtaoLinearizeDepth";
        pass.executorName = std::string(kExecLinearize);
        pass.accesses.push_back({ StandardFrameRecipeBuilder::kOpaqueDepthResource, FrameRecipeAccessKind::ReadDepthStencil });
        pass.accesses.push_back({ std::string(kResSceneLinearDepth),                  FrameRecipeAccessKind::WriteRenderTarget });
        pass.renderPass.enabled            = true;
        pass.renderPass.targetResourceName = std::string(kResSceneLinearDepth);
        pass.renderPass.viewportWidth      = w;
        pass.renderPass.viewportHeight     = h;
        ctx.recipe.passes.push_back(std::move(pass));
    }

    // Pass 2: GTAO Main
    {
        FrameRecipePassDesc pass{};
        pass.name         = "GtaoMain";
        pass.executorName = std::string(kExecGtaoMain);
        pass.accesses.push_back({ std::string(kResSceneLinearDepth), FrameRecipeAccessKind::ReadTexture });
        pass.accesses.push_back({ std::string(kResGTAORaw),          FrameRecipeAccessKind::WriteRenderTarget });
        pass.renderPass.enabled            = true;
        pass.renderPass.targetResourceName = std::string(kResGTAORaw);
        pass.renderPass.viewportWidth      = w;
        pass.renderPass.viewportHeight     = h;
        pass.renderPass.clearColor         = true;
        pass.renderPass.clearColorValue    = { 1.f, 0.f, 0.f, 0.f };
        ctx.recipe.passes.push_back(std::move(pass));
    }

    // Pass 3: Bilateral blur (denoise raw AO, preserve depth edges).
    // The fullscreen triangle covers every pixel, so no clear is needed.
    {
        FrameRecipePassDesc pass{};
        pass.name         = "GtaoBlur";
        pass.executorName = std::string(kExecBlur);
        pass.accesses.push_back({ std::string(kResGTAORaw),          FrameRecipeAccessKind::ReadTexture });
        pass.accesses.push_back({ std::string(kResSceneLinearDepth), FrameRecipeAccessKind::ReadTexture });
        pass.accesses.push_back({ std::string(kResGTAOOutput),      FrameRecipeAccessKind::WriteRenderTarget });
        pass.renderPass.enabled            = true;
        pass.renderPass.targetResourceName = std::string(kResGTAOOutput);
        pass.renderPass.viewportWidth      = w;
        pass.renderPass.viewportHeight     = h;
        pass.renderPass.clearColor         = false;
        ctx.recipe.passes.push_back(std::move(pass));
    }

    auto res        = m_resources;
    auto rtBindings = ctx.frame.runtimeBindings;

    // Executor 1: Linearize Depth
    ctx.callbacks.Register(kExecLinearize,
        [res, msaa, rtBindings](const rendergraph::RGExecContext& ec)
        {
            if (!ec.cmd || !ec.resources) return;

            const TextureHandle depthTex = ec.GetTexture(
                FindResourceId(*ec.resources, StandardFrameRecipeBuilder::kOpaqueDepthResource));
            if (!depthTex.IsValid()) return;

            ec.cmd->SetPipeline(msaa ? res->pipelineLinearizeMS : res->pipelineLinearize);
            if (rtBindings && rtBindings->resources.perFrameCB.IsValid())
                ec.cmd->SetConstantBuffer(CBSlots::PerFrame, rtBindings->resources.perFrameCB,
                    ShaderStageMask::Vertex | ShaderStageMask::Fragment);
            ec.cmd->SetShaderResource(TexSlots::PassSRV0, depthTex, ShaderStageMask::Fragment);
            ec.cmd->Draw(3u, 1u, 0u, 0u);
        });

    // Executor 2: GTAO Main
    ctx.callbacks.Register(kExecGtaoMain,
        [res, rtBindings](const rendergraph::RGExecContext& ec)
        {
            if (!ec.cmd || !res || !res->initialized || !ec.resources) return;

            const rendergraph::RGResourceID linearDepthId = FindResourceId(*ec.resources, kResSceneLinearDepth);
            if (linearDepthId == rendergraph::RG_INVALID_RESOURCE) return;
            const TextureHandle linearDepthTex = ec.GetTexture(linearDepthId);
            if (!linearDepthTex.IsValid()) return;

            ec.cmd->SetPipeline(res->pipelineGtaoMain);
            if (rtBindings && rtBindings->resources.perFrameCB.IsValid())
                ec.cmd->SetConstantBuffer(CBSlots::PerFrame, rtBindings->resources.perFrameCB,
                    ShaderStageMask::Vertex | ShaderStageMask::Fragment);
            ec.cmd->SetShaderResource(TexSlots::PassSRV0, linearDepthTex, ShaderStageMask::Fragment);
            ec.cmd->SetSampler(SamplerSlots::PointClamp, SamplerSlots::PointClamp, ShaderStageMask::Fragment);
            ec.cmd->Draw(3u, 1u, 0u, 0u);
        });

    // Executor 3: Bilateral blur (AO denoise)
    ctx.callbacks.Register(kExecBlur,
        [res, rtBindings](const rendergraph::RGExecContext& ec)
        {
            if (!ec.cmd || !res || !res->initialized || !ec.resources) return;

            const rendergraph::RGResourceID aoId = FindResourceId(*ec.resources, kResGTAORaw);
            const rendergraph::RGResourceID zId  = FindResourceId(*ec.resources, kResSceneLinearDepth);
            if (aoId == rendergraph::RG_INVALID_RESOURCE || zId == rendergraph::RG_INVALID_RESOURCE) return;

            const TextureHandle aoTex = ec.GetTexture(aoId);
            const TextureHandle zTex  = ec.GetTexture(zId);
            if (!aoTex.IsValid() || !zTex.IsValid()) return;

            ec.cmd->SetPipeline(res->pipelineBlur);
            if (rtBindings && rtBindings->resources.perFrameCB.IsValid())
                ec.cmd->SetConstantBuffer(CBSlots::PerFrame, rtBindings->resources.perFrameCB,
                    ShaderStageMask::Vertex | ShaderStageMask::Fragment);
            ec.cmd->SetShaderResource(TexSlots::PassSRV0, aoTex, ShaderStageMask::Fragment);
            ec.cmd->SetShaderResource(TexSlots::PassSRV1, zTex,  ShaderStageMask::Fragment);
            ec.cmd->SetSampler(SamplerSlots::PointClamp, SamplerSlots::PointClamp, ShaderStageMask::Fragment);
            ec.cmd->Draw(3u, 1u, 0u, 0u);

        });
}

void GtaoPass::BuildComposite(const PassBuildContext& ctx) const
{
    // SSR can also supply the copy when it was registered first.
    if (std::any_of(ctx.recipe.resources.begin(), ctx.recipe.resources.end(),
                    [](const FrameRecipeResourceDesc& r) { return r.name == kResGTAOComposite; }))
        return;

    const uint32_t w = ctx.frame.viewportWidth;
    const uint32_t h = ctx.frame.viewportHeight;
    ctx.recipe.resources.push_back(FrameRecipeResourceDesc{ std::string(kResGTAOComposite),
        false, w, h, Format::RGBA16_FLOAT, RGResourceKind::RenderTarget });

    // Pass 4: Copy HDRSceneColor → GTAOComposite (the buffer overlays + tonemap use).
    {
        FrameRecipePassDesc pass{};
        pass.name         = "GtaoComposite";
        pass.executorName = std::string(kExecComposite);
        pass.accesses.push_back({ std::string(StandardFrameResources::HDRSceneColor), FrameRecipeAccessKind::ReadTexture });
        pass.accesses.push_back({ std::string(kResGTAOComposite),                     FrameRecipeAccessKind::WriteRenderTarget });
        pass.renderPass.enabled            = true;
        pass.renderPass.targetResourceName = std::string(kResGTAOComposite);
        pass.renderPass.viewportWidth      = w;
        pass.renderPass.viewportHeight     = h;
        pass.renderPass.clearColor         = false;
        ctx.recipe.passes.push_back(std::move(pass));
    }

    auto res = m_resources;
    auto rtBindings = ctx.frame.runtimeBindings;
    ctx.callbacks.Register(kExecComposite,
        [res, rtBindings](const rendergraph::RGExecContext& ec)
        {
            if (!ec.cmd || !res || !res->initialized || !ec.resources) return;

            const rendergraph::RGResourceID hdrId = FindResourceId(*ec.resources, "HDRSceneColor");
            if (hdrId == rendergraph::RG_INVALID_RESOURCE) return;
            const TextureHandle hdrTex = ec.GetTexture(hdrId);
            if (!hdrTex.IsValid()) return;

            ec.cmd->SetPipeline(res->pipelineComposite);
            if (rtBindings && rtBindings->resources.perFrameCB.IsValid())
                ec.cmd->SetConstantBuffer(CBSlots::PerFrame, rtBindings->resources.perFrameCB,
                    ShaderStageMask::Vertex | ShaderStageMask::Fragment);
            ec.cmd->SetShaderResource(TexSlots::PassSRV0, hdrTex, ShaderStageMask::Fragment);
            ec.cmd->SetSampler(SamplerSlots::LinearClamp, SamplerSlots::LinearClamp, ShaderStageMask::Fragment);
            ec.cmd->Draw(3u, 1u, 0u, 0u);
        });
}

} // namespace engine::renderer::addons::gtao
