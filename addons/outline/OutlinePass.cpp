#include "OutlinePass.hpp"

#include "addons/forward/StandardFramePipeline.hpp"
#include "renderer/IDevice.hpp"
#include "renderer/RenderPipelineRecipe.hpp"
#include "renderer/RenderRuntimeFrameBindings.hpp"
#include "renderer/ShaderBindingModel.hpp"
#include "renderer/ShaderCompiler.hpp"
#include "renderer/RenderWorldViews.hpp"
#include "rendergraph/CompiledFrame.hpp"
#include "core/Debug.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>

namespace engine::renderer::addons::outline {

static constexpr std::string_view kResOutlineMask  = "OutlineMask";
static constexpr std::string_view kExecMask        = "frame.outline_mask";
static constexpr std::string_view kExecDilate      = "frame.outline_dilate";

// ─── GLSL Shaders (OpenGL fallback) ──────────────────────────────────────────

// Fullscreen triangle: same Y-inversion as kSkyFullscreenVsGlsl in ForwardFeature —
// OpenGL scene geometry is Y-flipped into the HDR FBO via GetClipSpaceAdjustment,
// so a fullscreen VS that bypasses the projection matrix must apply the same flip.

// OpenGL (GLSL 410): PerObject als UBO
static constexpr const char* kMaskVsGlsl = R"(#version 410 core

struct GpuLightData { vec4 positionWS; vec4 directionWS; vec4 colorIntensity; vec4 params; };
layout(std140) uniform PerFrame {
    mat4 viewMatrix; mat4 projMatrix; mat4 viewProjMatrix; mat4 invViewProjMatrix;
    vec4 cameraPositionWS; vec4 cameraForwardWS; vec4 screenSize; vec4 timeData; vec4 ambientColor;
    uint lightCount; uint shadowCascadeCount; float nearPlane; float farPlane;
    GpuLightData lights[7];
    mat4 shadowViewProj;
    float iblPrefilterLevels; float shadowBias; float shadowNormalBias; float shadowStrength;
    float shadowTexelSize; uint debugFlags; vec2 _shadowPad;
};
layout(std140) uniform PerObject {
    mat4 worldMatrix;
    mat4 worldMatrixInvT;
    vec4 entityId;
};
layout(location = 0) in vec3 aPosition;
void main() { gl_Position = viewProjMatrix * worldMatrix * vec4(aPosition, 1.0); }
)";

// Vulkan (SPIR-V via GLSL 450): PerObject als Push Constants (3 Zeilen je Matrix)
static constexpr const char* kMaskVsGlslVulkan = R"(#version 450

struct GpuLightData { vec4 positionWS; vec4 directionWS; vec4 colorIntensity; vec4 params; };
layout(std140, binding = 0, set = 0) uniform PerFrame {
    mat4 viewMatrix; mat4 projMatrix; mat4 viewProjMatrix; mat4 invViewProjMatrix;
    vec4 cameraPositionWS; vec4 cameraForwardWS; vec4 screenSize; vec4 timeData; vec4 ambientColor;
    uint lightCount; uint shadowCascadeCount; float nearPlane; float farPlane;
    GpuLightData lights[7];
    mat4 shadowViewProj;
    float iblPrefilterLevels; float shadowBias; float shadowNormalBias; float shadowStrength;
    float shadowTexelSize; uint debugFlags; vec2 _shadowPad;
};
layout(push_constant) uniform PerObject {
    vec4 worldRow0;
    vec4 worldRow1;
    vec4 worldRow2;
    vec4 worldInvTRow0;
    vec4 worldInvTRow1;
    vec4 worldInvTRow2;
    vec4 entityId;
} pc;
layout(location = 0) in vec3 aPosition;
void main() {
    // Zeilen-Vektoren → Spaltenmajorige mat4 rekonstruieren
    mat4 world = transpose(mat4(pc.worldRow0, pc.worldRow1, pc.worldRow2, vec4(0.0, 0.0, 0.0, 1.0)));
    gl_Position = viewProjMatrix * world * vec4(aPosition, 1.0);
}
)";

static constexpr const char* kMaskPsGlsl = R"(#version 410 core
layout(location = 0) out float fragColor;
void main() { fragColor = 1.0; }
)";

static constexpr const char* kFullscreenVsGlsl = R"(#version 410 core
out vec2 vTexCoord;
void main()
{
    const vec2 pos[3] = vec2[3](vec2(-1.0, -1.0), vec2(-1.0, 3.0), vec2( 3.0, -1.0));
    const vec2 uv[3]  = vec2[3](vec2( 0.0,  0.0), vec2( 0.0, 2.0), vec2( 2.0,  0.0));
    gl_Position = vec4(pos[gl_VertexID], 0.0, 1.0);
    vTexCoord   = uv[gl_VertexID];
}
)";

static constexpr const char* kDilatePsGlsl = R"(#version 410 core

struct GpuLightData { vec4 positionWS; vec4 directionWS; vec4 colorIntensity; vec4 params; };
layout(std140) uniform PerFrame {
    mat4 viewMatrix; mat4 projMatrix; mat4 viewProjMatrix; mat4 invViewProjMatrix;
    vec4 cameraPositionWS; vec4 cameraForwardWS; vec4 screenSize; vec4 timeData; vec4 ambientColor;
    uint lightCount; uint shadowCascadeCount; float nearPlane; float farPlane;
    GpuLightData lights[7];
    mat4 shadowViewProj;
    float iblPrefilterLevels; float shadowBias; float shadowNormalBias; float shadowStrength;
    float shadowTexelSize; uint debugFlags; vec2 _shadowPad;
};

uniform sampler2D uMask;

in  vec2 vTexCoord;
layout(location = 0) out vec4 fragColor;

const vec4  kOutlineColor = vec4(1.0, 0.55, 0.0, 1.0);
const float kThickness    = 2.5;

void main()
{
    vec2  texelSize   = screenSize.zw;
    float center      = texture(uMask, vTexCoord).r;
    float maxNeighbor = 0.0;
    for (int dy = -1; dy <= 1; ++dy)
    for (int dx = -1; dx <= 1; ++dx)
    {
        if (dx == 0 && dy == 0) continue;
        vec2 off = vec2(float(dx), float(dy)) * texelSize * kThickness;
        maxNeighbor = max(maxNeighbor, texture(uMask, vTexCoord + off).r);
    }
    float outline = maxNeighbor * (1.0 - center);
    fragColor = vec4(kOutlineColor.rgb, kOutlineColor.a * outline);
}
)";

// ─── HLSL Shaders ─────────────────────────────────────────────────────────────

static constexpr const char* kMaskVsHlsl = R"(
struct GpuLightData { float4 positionWS; float4 directionWS; float4 colorIntensity; float4 params; };
cbuffer PerFrame : register(b0)
{
    float4x4 viewMatrix;
    float4x4 projMatrix;
    float4x4 viewProjMatrix;
    float4x4 invViewProjMatrix;
    float4   cameraPositionWS;
    float4   cameraForwardWS;
    float4   screenSize;
    float4   timeData;
    float4   ambientColor;
    uint     lightCount;
    uint     shadowCascadeCount;
    float    nearPlane;
    float    farPlane;
    GpuLightData lights[7];
    float4x4 shadowViewProj;
    float    iblPrefilterLevels;
    float    shadowBias;
    float    shadowNormalBias;
    float    shadowStrength;
    float    shadowTexelSize;
    uint     debugFlags;
    float2   _shadowPad;
};

cbuffer PerObject : register(b1)
{
    float4x4 worldMatrix;
    float4x4 worldMatrixInvT;
    float4   entityId;
};

float4 main(float3 pos : POSITION) : SV_POSITION
{
    return mul(viewProjMatrix, mul(worldMatrix, float4(pos, 1.0f)));
}
)";

static constexpr const char* kMaskPsHlsl = R"(
float main() : SV_TARGET
{
    return 1.0f;
}
)";

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

static constexpr const char* kDilatePsHlsl = R"(
Texture2D<float> uMask    : register(t8);
SamplerState     uSampler : register(s2);

// PerFrame is bound automatically by the engine for every pass.
// screenSize.zw = (1/viewportWidth, 1/viewportHeight).
struct GpuLightData { float4 positionWS; float4 directionWS; float4 colorIntensity; float4 params; };
cbuffer PerFrame : register(b0)
{
    float4x4 viewMatrix;
    float4x4 projMatrix;
    float4x4 viewProjMatrix;
    float4x4 invViewProjMatrix;
    float4   cameraPositionWS;
    float4   cameraForwardWS;
    float4   screenSize;
    float4   timeData;
    float4   ambientColor;
    uint     lightCount;
    uint     shadowCascadeCount;
    float    nearPlane;
    float    farPlane;
    GpuLightData lights[7];
    float4x4 shadowViewProj;
    float    iblPrefilterLevels;
    float    shadowBias;
    float    shadowNormalBias;
    float    shadowStrength;
    float    shadowTexelSize;
    uint     debugFlags;
    float2   _shadowPad;
};

static const float4 kOutlineColor = float4(1.0f, 0.55f, 0.0f, 1.0f);
static const float  kThickness    = 2.5f;

struct PSInput { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

float4 main(PSInput IN) : SV_TARGET
{
    float2 texelSize = screenSize.zw;
    float center = uMask.SampleLevel(uSampler, IN.uv, 0).r;

    float maxNeighbor = 0.0f;
    for (int dy = -1; dy <= 1; ++dy)
    for (int dx = -1; dx <= 1; ++dx)
    {
        if (dx == 0 && dy == 0) continue;
        float2 off = float2(float(dx), float(dy)) * texelSize * kThickness;
        maxNeighbor = max(maxNeighbor, uMask.SampleLevel(uSampler, IN.uv + off, 0).r);
    }

    float outline = maxNeighbor * (1.0f - center);
    return float4(kOutlineColor.rgb, kOutlineColor.a * outline);
}
)";

// ─── Helpers ──────────────────────────────────────────────────────────────────

static rendergraph::RGResourceID FindResourceId(
    const std::vector<rendergraph::CompiledResourceSnapshot>& snaps,
    std::string_view name)
{
    for (const auto& s : snaps)
        if (s.debugName == name)
            return s.id;
    return rendergraph::RG_INVALID_RESOURCE;
}

static ShaderHandle CompileOutlineShader(IDevice& device,
    assets::ShaderTargetProfile target,
    assets::ShaderStage stage,
    const char* hlslSource,
    const char* glslSource,       // OpenGL
    const char* debugName,
    const char* glslVulkanSource = nullptr)  // Vulkan-SPIR-V (optional, GLSL mit push_constant)
{
    const ShaderStageMask stageMask = (stage == assets::ShaderStage::Vertex)
        ? ShaderStageMask::Vertex
        : ShaderStageMask::Fragment;

    if (target == assets::ShaderTargetProfile::OpenGL_GLSL450)
        return device.CreateShaderFromSource(glslSource, stageMask, "main", debugName);

    // Vulkan: GLSL mit push_constant direkt kompilieren wenn vorhanden
    if (target == assets::ShaderTargetProfile::Vulkan_SPIRV && glslVulkanSource)
    {
        assets::ShaderAsset asset{};
        asset.debugName      = debugName;
        asset.stage          = stage;
        asset.sourceLanguage = assets::ShaderSourceLanguage::GLSL;
        asset.entryPoint     = "main";
        asset.sourceCode     = glslVulkanSource;

        assets::CompiledShaderArtifact compiled{};
        std::string error;
        if (!ShaderCompiler::CompileForTarget(asset, target, compiled, &error))
        {
            Debug::LogError("OutlinePass: Vulkan GLSL compile failed '%s': %s", debugName, error.c_str());
            return ShaderHandle::Invalid();
        }
        if (!compiled.bytecode.empty())
            return device.CreateShaderFromBytecode(compiled.bytecode.data(), compiled.bytecode.size(), stageMask, debugName);
        return device.CreateShaderFromSource(
            compiled.sourceText.empty() ? glslVulkanSource : compiled.sourceText,
            stageMask, "main", debugName);
    }

    assets::ShaderAsset asset{};
    asset.debugName      = debugName;
    asset.stage          = stage;
    asset.sourceLanguage = assets::ShaderSourceLanguage::HLSL;
    asset.entryPoint     = "main";
    asset.sourceCode     = hlslSource;

    assets::CompiledShaderArtifact compiled{};
    std::string error;
    if (!ShaderCompiler::CompileForTarget(asset, target, compiled, &error))
    {
        Debug::LogError("OutlinePass: failed to compile shader '%s': %s", debugName, error.c_str());
        return ShaderHandle::Invalid();
    }

    if (!compiled.bytecode.empty())
        return device.CreateShaderFromBytecode(compiled.bytecode.data(), compiled.bytecode.size(), stageMask, debugName);

    return device.CreateShaderFromSource(
        compiled.sourceText.empty() ? hlslSource : compiled.sourceText,
        stageMask,
        compiled.entryPoint.empty() ? "main" : compiled.entryPoint.c_str(),
        debugName);
}

static PipelineHandle MakeMaskPipeline(IDevice& device,
    ShaderHandle vs,
    ShaderHandle ps,
    uint32_t vertexStride)
{
    PipelineDesc desc{};
    desc.shaderStages.push_back({ vs, ShaderStageMask::Vertex });
    desc.shaderStages.push_back({ ps, ShaderStageMask::Fragment });
    desc.vertexLayout.bindings.push_back(VertexBinding{ 0u, vertexStride, VertexInputRate::PerVertex });
    desc.vertexLayout.attributes.push_back(VertexAttribute{
        VertexSemantic::Position, Format::RGB32_FLOAT, 0u, 0u });
    desc.topology                 = PrimitiveTopology::TriangleList;
    desc.colorFormat              = Format::R8_UNORM;
    desc.depthFormat              = Format::Unknown;
    desc.sampleCount              = 1u;
    desc.rasterizer.cullMode      = CullMode::None;
    desc.depthStencil.depthEnable = false;
    desc.depthStencil.depthWrite  = false;
    char name[64];
    std::snprintf(name, sizeof(name), "OutlineMaskPipeline_s%u", vertexStride);
    desc.debugName = name;
    return device.CreatePipeline(desc);
}

static PipelineHandle MakeDilatePipeline(IDevice& device, ShaderHandle vs, ShaderHandle ps)
{
    PipelineDesc desc{};
    desc.shaderStages.push_back({ vs, ShaderStageMask::Vertex });
    desc.shaderStages.push_back({ ps, ShaderStageMask::Fragment });
    desc.topology                      = PrimitiveTopology::TriangleList;
    desc.colorFormat                   = Format::RGBA16_FLOAT;
    desc.depthFormat                   = Format::Unknown;
    desc.sampleCount                   = 1u;
    desc.rasterizer.cullMode           = CullMode::None;
    desc.depthStencil.depthEnable      = false;
    desc.depthStencil.depthWrite       = false;
    desc.blendStates[0].blendEnable    = true;
    desc.blendStates[0].srcBlend       = BlendFactor::SrcAlpha;
    desc.blendStates[0].dstBlend       = BlendFactor::InvSrcAlpha;
    desc.blendStates[0].blendOp        = BlendOp::Add;
    desc.blendStates[0].srcBlendAlpha  = BlendFactor::One;
    desc.blendStates[0].dstBlendAlpha  = BlendFactor::InvSrcAlpha;
    desc.blendStates[0].blendOpAlpha   = BlendOp::Add;
    desc.debugName                     = "OutlineDilatePipeline";
    return device.CreatePipeline(desc);
}

// ─── Init / Destroy ───────────────────────────────────────────────────────────

bool InitializeOutlineResources(IDevice& device, OutlineGpuResources& res)
{
    const assets::ShaderTargetProfile target = ShaderCompiler::ResolveTargetProfile(device);

    // Mask-VS: Vulkan bekommt GLSL mit push_constant statt HLSL-UBO
    res.vsMask       = CompileOutlineShader(device, target, assets::ShaderStage::Vertex,   kMaskVsHlsl,       kMaskVsGlsl,       "outline_mask.vs",       kMaskVsGlslVulkan);
    res.psMask       = CompileOutlineShader(device, target, assets::ShaderStage::Fragment, kMaskPsHlsl,       kMaskPsGlsl,       "outline_mask.ps");
    res.vsFullscreen = CompileOutlineShader(device, target, assets::ShaderStage::Vertex,   kFullscreenVsHlsl, kFullscreenVsGlsl, "outline_fullscreen.vs");
    res.psDilate     = CompileOutlineShader(device, target, assets::ShaderStage::Fragment, kDilatePsHlsl,     kDilatePsGlsl,     "outline_dilate.ps");

    if (!res.vsMask.IsValid() || !res.psMask.IsValid() ||
        !res.vsFullscreen.IsValid() || !res.psDilate.IsValid())
    {
        Debug::LogError("OutlinePass: shader compilation failed");
        return false;
    }

    res.pipelineDilate = MakeDilatePipeline(device, res.vsFullscreen, res.psDilate);
    if (!res.pipelineDilate.IsValid())
    {
        Debug::LogError("OutlinePass: dilate pipeline creation failed");
        return false;
    }

    // One 256-byte CB for per-object data — uploaded per-draw for backends (Vulkan)
    // that use Push Constants and leave perObjectArena empty.
    {
        BufferDesc cbd{};
        cbd.byteSize   = kConstantBufferAlignment;  // 256 bytes >= sizeof(PerObjectConstants)
        cbd.type       = BufferType::Constant;
        cbd.usage      = ResourceUsage::ConstantBuffer;
        cbd.access     = MemoryAccess::CpuWrite;
        cbd.lifetime   = ResourceLifetimeClass::Persistent;
        cbd.debugName  = "OutlinePerObjectCB";
        res.perObjectCB = device.CreateBuffer(cbd);
        if (!res.perObjectCB.IsValid())
        {
            Debug::LogError("OutlinePass: per-object CB creation failed");
            return false;
        }
    }

    res.initialized = true;
    Debug::Log("OutlinePass: initialized");
    return true;
}

void DestroyOutlineResources(IDevice& device, OutlineGpuResources& res)
{
    for (auto& [stride, pipeline] : res.maskPipelines)
        if (pipeline.IsValid()) device.DestroyPipeline(pipeline);
    res.maskPipelines.clear();

    if (res.pipelineDilate.IsValid()) { device.DestroyPipeline(res.pipelineDilate); res.pipelineDilate = {}; }
    if (res.perObjectCB.IsValid())    { device.DestroyBuffer(res.perObjectCB);      res.perObjectCB    = {}; }
    if (res.psDilate.IsValid())       { device.DestroyShader(res.psDilate);         res.psDilate       = {}; }
    if (res.vsFullscreen.IsValid())   { device.DestroyShader(res.vsFullscreen);     res.vsFullscreen   = {}; }
    if (res.psMask.IsValid())         { device.DestroyShader(res.psMask);           res.psMask         = {}; }
    if (res.vsMask.IsValid())         { device.DestroyShader(res.vsMask);           res.vsMask         = {}; }
    res.initialized = false;
}

// ─── OutlinePass ──────────────────────────────────────────────────────────────

OutlinePass::OutlinePass(uint32_t id,
                         std::shared_ptr<OutlineGpuResources> resources,
                         std::function<EntityID()> getSelected,
                         std::function<bool(EntityID)> shouldOutlineEntity)
    : m_id(id)
    , m_resources(std::move(resources))
    , m_getSelected(std::move(getSelected))
    , m_shouldOutlineEntity(std::move(shouldOutlineEntity))
{}

bool OutlinePass::DeclaresResource(std::string_view name) const noexcept
{
    if (!m_resources || !m_resources->initialized)
        return false;
    return name == kResOutlineMask;
}

void OutlinePass::BuildPass(const PassBuildContext& ctx) const
{
    if (!m_resources || !m_resources->initialized)
        return;

    if (!m_shouldOutlineEntity)
        return;

    const uint32_t w = ctx.frame.viewportWidth;
    const uint32_t h = ctx.frame.viewportHeight;

    // ── Transient OutlineMask resource ────────────────────────────────────────
    ctx.recipe.resources.push_back(FrameRecipeResourceDesc{
        std::string(kResOutlineMask), false, w, h,
        Format::R8_UNORM, RGResourceKind::ColorTexture
    });

    // ── Pass 1: Render selected mesh(es) into R8 mask ─────────────────────────
    {
        FrameRecipePassDesc pass{};
        pass.name         = "OutlineMaskPass";
        pass.executorName = std::string(kExecMask);
        // No HDRSceneColor read here — that would create a WAR hazard because
        // OutlineDilatePass writes HDRSceneColor later in the same contributor.
        // Ordering after Opaque is guaranteed by recipe position (postPasses slot).
        pass.accesses.push_back({ std::string(kResOutlineMask),
                                   FrameRecipeAccessKind::WriteRenderTarget });
        // Manual Begin/EndRenderPass so the mask RT is cleared to 0 before drawing
        pass.renderPass.enabled = false;
        ctx.recipe.passes.push_back(std::move(pass));
    }

    // ── Pass 2: Dilate mask and alpha-blend orange outline onto scene color ──
    // When GTAO is active, GTAOComposite is the final scene color that Tonemap reads.
    // Writing back to HDRSceneColor after GtaoComposite read it would be a WAR hazard.
    // We detect GTAO presence by checking if GTAOComposite is already declared in the
    // accumulated recipe (GTAO registers before Outline and calls BuildPass first).
    const bool hasGtao = std::any_of(ctx.recipe.resources.begin(), ctx.recipe.resources.end(),
        [](const FrameRecipeResourceDesc& r) { return r.name == "GTAOComposite"; });
    const std::string dilateTarget = hasGtao
        ? "GTAOComposite"
        : std::string(StandardFrameResources::HDRSceneColor);

    {
        FrameRecipePassDesc pass{};
        pass.name         = "OutlineDilatePass";
        pass.executorName = std::string(kExecDilate);
        pass.accesses.push_back({ std::string(kResOutlineMask),
                                   FrameRecipeAccessKind::ReadTexture });
        pass.accesses.push_back({ dilateTarget,
                                   FrameRecipeAccessKind::WriteRenderTarget });
        pass.renderPass.enabled            = true;
        pass.renderPass.targetResourceName = dilateTarget;
        pass.renderPass.viewportWidth      = w;
        pass.renderPass.viewportHeight     = h;
        pass.renderPass.clearColor         = false;  // alpha blend, preserve scene
        pass.renderPass.clearDepth         = false;
        ctx.recipe.passes.push_back(std::move(pass));
    }

    // ── Executors ─────────────────────────────────────────────────────────────
    auto res      = m_resources;
    auto bindings = ctx.frame.runtimeBindings;
    const RenderQueue* renderQueue = ctx.frame.renderQueue;

    // Mask executor: render each DrawItem belonging to the selected entity.
    // Pro Draw wird die World-Matrix in perObjectCB geladen.
    // DX11 Immediate Context garantiert sequentielle Ausführung → kein Overwrite-Problem.
    ctx.callbacks.Register(kExecMask,
        [res, bindings, renderQueue, shouldOutlineEntity = m_shouldOutlineEntity, w, h](const rendergraph::RGExecContext& ec)
        {
            if (!ec.cmd || !ec.device || !res || !res->initialized) return;
            if (!ec.resources) return;

            const rendergraph::RGResourceID maskId =
                FindResourceId(*ec.resources, kResOutlineMask);
            if (maskId == rendergraph::RG_INVALID_RESOURCE) return;

            const RenderTargetHandle maskRt = ec.GetRenderTarget(maskId);
            if (!maskRt.IsValid()) return;

            // Nicht nur die Opaque-Liste verwenden. Parent-/Gruppen-Selektion
            // muss alle sichtbaren DrawItems der Hierarchie markieren, auch wenn
            // einzelne Submeshes durch Material-State in einer anderen DrawList
            // landen.

            // Begin render pass with black clear (no entity drawn = no mask)
            ICommandList::RenderPassBeginInfo rp{};
            rp.renderTarget        = maskRt;
            rp.clearColor          = true;
            rp.colorClear.color[0] = 0.f;
            rp.colorClear.color[1] = 0.f;
            rp.colorClear.color[2] = 0.f;
            rp.colorClear.color[3] = 0.f;
            rp.clearDepth          = false;
            ec.cmd->BeginRenderPass(rp);
            ec.cmd->SetViewport(0.f, 0.f, static_cast<float>(w), static_cast<float>(h), 0.f, 1.f);
            ec.cmd->SetScissor(0, 0, w, h);

            if (renderQueue && bindings && bindings->resources.perFrameCB.IsValid() &&
                res->perObjectCB.IsValid())
            {
                // Bind PerFrame CB once (contains viewProjMatrix)
                ec.cmd->SetConstantBufferRange(CBSlots::PerFrame,
                    BufferBinding{ bindings->resources.perFrameCB, 0u,
                                   static_cast<uint32_t>(sizeof(float) * 16u * 4u + sizeof(float) * 4u * 8u) },
                    ShaderStageMask::Vertex);

                for (const DrawList& list : renderQueue->GetLists())
                {
                    if (list.passId == StandardRenderPasses::Shadow())
                        continue;

                    for (const DrawItem& item : list.items)
                    {
                        if (!item.hasGpuData()) continue;
                        // shouldOutlineEntity liest selectedEntity zum Ausfuehrungs-Zeitpunkt —
                        // kein Build-Zeit/Ausfuehrungs-Zeit-Versatz mehr (DX11-Fix).
                        if (!shouldOutlineEntity(item.entity))
                            continue;

                        // Lazy per-stride pipeline creation
                        const uint32_t stride = item.gpuVertexStride;
                        auto it = res->maskPipelines.find(stride);
                        if (it == res->maskPipelines.end())
                        {
                            PipelineHandle pipe = MakeMaskPipeline(*ec.device, res->vsMask, res->psMask, stride);
                            if (!pipe.IsValid()) continue;
                            it = res->maskPipelines.emplace(stride, pipe).first;
                        }

                        ec.cmd->SetPipeline(it->second);

                        // Per-Object-Daten setzen:
                        // Vulkan → Push Constants (exakt wie der normale Render-Pfad)
                        // DX11   → Upload in perObjectCB, bind als UBO
                        if (item.cbOffset < static_cast<uint32_t>(renderQueue->objectConstants.size()))
                        {
                            const PerObjectConstants& poc = renderQueue->objectConstants[item.cbOffset];

                            const bool isVulkan = ec.device &&
                                ec.device->GetShaderTargetProfile() ==
                                    assets::ShaderTargetProfile::Vulkan_SPIRV;

                            if (isVulkan)
                            {
                                VulkanPerObjectPushConstants packed{};
                                packed.worldRow0[0] = poc.worldMatrix[0u];
                                packed.worldRow0[1] = poc.worldMatrix[4u];
                                packed.worldRow0[2] = poc.worldMatrix[8u];
                                packed.worldRow0[3] = poc.worldMatrix[12u];
                                packed.worldRow1[0] = poc.worldMatrix[1u];
                                packed.worldRow1[1] = poc.worldMatrix[5u];
                                packed.worldRow1[2] = poc.worldMatrix[9u];
                                packed.worldRow1[3] = poc.worldMatrix[13u];
                                packed.worldRow2[0] = poc.worldMatrix[2u];
                                packed.worldRow2[1] = poc.worldMatrix[6u];
                                packed.worldRow2[2] = poc.worldMatrix[10u];
                                packed.worldRow2[3] = poc.worldMatrix[14u];
                                packed.entityId[0]  = poc.entityId[0];
                                packed.entityId[1]  = poc.entityId[1];
                                packed.entityId[2]  = poc.entityId[2];
                                packed.entityId[3]  = poc.entityId[3];
                                ec.cmd->SetPushConstants(0u, &packed,
                                    static_cast<uint32_t>(sizeof(packed)),
                                    ShaderStageMask::Vertex);
                            }
                            else
                            {
                                ec.device->UploadBufferData(res->perObjectCB, &poc,
                                    sizeof(PerObjectConstants), 0u);
                                ec.cmd->SetConstantBufferRange(CBSlots::PerObject,
                                    BufferBinding{ res->perObjectCB, 0u, kConstantBufferAlignment },
                                    ShaderStageMask::Vertex);
                            }
                        }

                        ec.cmd->SetVertexBuffer(0u, item.gpuVertexBuffer, 0u);
                        ec.cmd->SetIndexBuffer(item.gpuIndexBuffer, true, 0u);
                        ec.cmd->DrawIndexed(item.gpuIndexCount, item.instanceCount, 0u, 0, item.firstInstance);
                    }
                }
            }

            ec.cmd->EndRenderPass();
        });

    // Dilate executor: fullscreen alpha-blended orange outline.
    // screenSize.zw (texelSize) is read from PerFrame CB (b0) which the engine binds
    // automatically for every pass — no explicit CB upload needed here.
    ctx.callbacks.Register(kExecDilate,
        [res](const rendergraph::RGExecContext& ec)
        {
            if (!ec.cmd || !res || !res->initialized) return;
            if (!res->pipelineDilate.IsValid()) return;
            if (!ec.resources) return;

            const rendergraph::RGResourceID maskId =
                FindResourceId(*ec.resources, kResOutlineMask);
            if (maskId == rendergraph::RG_INVALID_RESOURCE) return;

            const TextureHandle maskTex = ec.GetTexture(maskId);
            if (!maskTex.IsValid()) return;

            ec.cmd->SetPipeline(res->pipelineDilate);
            ec.cmd->SetShaderResource(TexSlots::PassSRV0, maskTex, ShaderStageMask::Fragment);
            ec.cmd->SetSampler(SamplerSlots::PointClamp, SamplerSlots::PointClamp,
                ShaderStageMask::Fragment);
            ec.cmd->Draw(3u, 1u, 0u, 0u);
        });
}

} // namespace engine::renderer::addons::outline
