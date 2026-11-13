#include "addons/probes/ProbeBake.hpp"

#include "addons/forward/StandardFramePipeline.hpp"
#include "addons/probes/ProbeData.hpp"
#include "assets/AssetRegistry.hpp"
#include "core/Debug.hpp"
#include "renderer/IBLBaker.hpp"
#include "renderer/IBLTypes.hpp"
#include "renderer/RenderLayers.hpp"
#include "renderer/ShaderBindingModel.hpp"
#include "renderer/ShaderCompiler.hpp"
#include "rendergraph/CompiledFrame.hpp"
#include "rendergraph/RenderGraph.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

namespace engine::addons::probes {

using namespace engine::renderer;

namespace {

// Same fullscreen triangle and UV handling as the tonemap pass
// (assets/fullscreen.vs.hlsl, fullscreen.opengl.vs.glsl, passthrough.*), so
// the capture ends up in the target with the orientation the tonemap gives.
constexpr const char* kEncodeVsHlsl = R"(struct VSOutput
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

VSOutput main(uint id : SV_VertexID)
{
    VSOutput o;
    if (id == 0u)      { o.pos = float4(-1.0f,  1.0f, 0.0f, 1.0f); o.uv = float2(0.0f, 0.0f); }
    else if (id == 1u) { o.pos = float4(-1.0f, -3.0f, 0.0f, 1.0f); o.uv = float2(0.0f, 2.0f); }
    else               { o.pos = float4( 3.0f,  1.0f, 0.0f, 1.0f); o.uv = float2(2.0f, 0.0f); }
    return o;
}
)";

// Linear HDR -> RGBE in RGBA8: rgb = mantissa in (0.5, 1], a = (exponent + 127) / 255.
constexpr const char* kEncodePsHlsl = R"(Texture2D    uHDRInput : register(t8);
SamplerState uSampler  : register(s1);

struct PSInput
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

float4 main(PSInput IN) : SV_TARGET
{
    float3 c = uHDRInput.Sample(uSampler, IN.uv).rgb;
    c = float3(isnan(c.r) ? 0.0f : c.r, isnan(c.g) ? 0.0f : c.g, isnan(c.b) ? 0.0f : c.b);
    c = clamp(c, 0.0f, 65504.0f);
    float m = max(c.r, max(c.g, c.b));
    if (m < 1e-30f)
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    float e = clamp(ceil(log2(m)), -126.0f, 127.0f);
    return float4(saturate(c / exp2(e)), (e + 127.0f) / 255.0f);
}
)";

constexpr const char* kEncodeVsGlsl = R"(#version 410 core

out vec2 vTexCoord;

void main()
{
    const vec2 pos[3] = vec2[3](
        vec2(-1.0,  1.0),
        vec2(-1.0, -3.0),
        vec2( 3.0,  1.0)
    );
    const vec2 uv[3] = vec2[3](
        vec2(0.0, 0.0),
        vec2(0.0, 2.0),
        vec2(2.0, 0.0)
    );

    gl_Position = vec4(pos[gl_VertexID], 0.0, 1.0);
    vTexCoord = vec2(uv[gl_VertexID].x, 1.0 - uv[gl_VertexID].y);
}
)";

constexpr const char* kEncodePsGlsl = R"(#version 410 core

uniform sampler2D uHDRInput;

in vec2 vTexCoord;
layout(location = 0) out vec4 fragColor;

void main()
{
    vec2 uv = vec2(vTexCoord.x, 1.0 - vTexCoord.y);
    vec3 c = texture(uHDRInput, uv).rgb;
    c = vec3(isnan(c.r) ? 0.0 : c.r, isnan(c.g) ? 0.0 : c.g, isnan(c.b) ? 0.0 : c.b);
    c = clamp(c, vec3(0.0), vec3(65504.0));
    float m = max(c.r, max(c.g, c.b));
    if (m < 1e-30)
    {
        fragColor = vec4(0.0);
        return;
    }
    float e = clamp(ceil(log2(m)), -126.0, 127.0);
    fragColor = vec4(clamp(c / exp2(e), vec3(0.0), vec3(1.0)), (e + 127.0) / 255.0);
}
)";

ShaderHandle CreateShader(IDevice& device, assets::ShaderTargetProfile target, assets::ShaderStage stage,
                          const char* hlsl, const char* glsl, const char* debugName)
{
    const ShaderStageMask stageMask = stage == assets::ShaderStage::Vertex
        ? ShaderStageMask::Vertex : ShaderStageMask::Fragment;
    if (target == assets::ShaderTargetProfile::OpenGL_GLSL450)
        return device.CreateShaderFromSource(glsl, stageMask, "main", debugName);

    assets::ShaderAsset shader{};
    shader.debugName = debugName;
    shader.stage = stage;
    shader.sourceLanguage = assets::ShaderSourceLanguage::HLSL;
    shader.entryPoint = "main";
    shader.sourceCode = hlsl;

    assets::CompiledShaderArtifact compiled{};
    std::string error;
    if (!ShaderCompiler::CompileForTarget(shader, target, compiled, &error))
    {
        Debug::LogError("ProbeBake.cpp: failed to compile '%s': %s", debugName, error.c_str());
        return ShaderHandle::Invalid();
    }
    if (compiled.bytecode.empty())
    {
        if (target == assets::ShaderTargetProfile::Vulkan_SPIRV)
        {
            Debug::LogError("ProbeBake.cpp: '%s' produced no bytecode", debugName);
            return ShaderHandle::Invalid();
        }
        return device.CreateShaderFromSource(compiled.sourceText.empty() ? std::string(hlsl) : compiled.sourceText,
                                             stageMask,
                                             compiled.entryPoint.empty() ? "main" : compiled.entryPoint,
                                             debugName);
    }
    return device.CreateShaderFromBytecode(compiled.bytecode.data(), compiled.bytecode.size(),
                                           stageMask, debugName);
}

// Forward and up per cube face (+X, -X, +Y, -Y, +Z, -Z). With LookAtRH the
// screen's right axis is cross(forward, up), the mirror of the D3D cube
// convention the baker and the shaders use, so read-back rows are mirrored.
struct FaceBasis { math::Vec3 forward; math::Vec3 up; };
constexpr std::array<FaceBasis, 6> kFaces{{
    { {  1.f,  0.f,  0.f }, { 0.f, 1.f,  0.f } },
    { { -1.f,  0.f,  0.f }, { 0.f, 1.f,  0.f } },
    { {  0.f,  1.f,  0.f }, { 0.f, 0.f, -1.f } },
    { {  0.f, -1.f,  0.f }, { 0.f, 0.f,  1.f } },
    { {  0.f,  0.f,  1.f }, { 0.f, 1.f,  0.f } },
    { {  0.f,  0.f, -1.f }, { 0.f, 1.f,  0.f } },
}};

const rendergraph::CompiledResourceSnapshot* FindResource(const rendergraph::RGExecContext& ctx, std::string_view name)
{
    if (!ctx.resources)
        return nullptr;
    for (const rendergraph::CompiledResourceSnapshot& r : *ctx.resources)
        if (r.debugName == name)
            return &r;
    return nullptr;
}

[[maybe_unused]] uint64_t HashBytes(const void* data, size_t size, uint64_t h)
{
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0u; i < size; ++i)
    {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

} // namespace

std::string MakeProbeDataPath(const std::string& sceneNameUtf8, uint64_t probeId)
{
    char id[17]{};
    std::snprintf(id, sizeof(id), "%016llx", static_cast<unsigned long long>(probeId));
    const std::string scene = sceneNameUtf8.empty() ? std::string("Scene") : sceneNameUtf8;
    return "Baked/" + scene + "/" + id + ".kprobe";
}

uint64_t GenerateProbeId()
{
    std::random_device rd;
    std::mt19937_64 rng((static_cast<uint64_t>(rd()) << 32u) ^ rd() ^
        static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()));
    uint64_t id = 0u;
    while (id == 0u)
        id = rng();
    return id;
}

ProbeCapture::~ProbeCapture()
{
    if (m_active)
        Debug::LogWarning("ProbeBake.cpp: capture destroyed without Finish/Cancel, GPU objects leak");
}

bool ProbeCapture::Begin(IDevice& device, ProbeRuntime& runtime, const RenderView& baseView,
                         const ProbeBakeRequest& request, std::string* outError)
{
    if (m_active)
    {
        if (outError) *outError = "capture already running";
        return false;
    }
    if (request.relativePath.empty())
    {
        if (outError) *outError = "no target path";
        return false;
    }

    m_request = request;
    m_request.resolution = std::clamp(request.resolution, 16u, 512u);
    m_captureSize = m_request.resolution * 2u;

    const assets::ShaderTargetProfile target = ShaderCompiler::ResolveTargetProfile(device);
    m_vs = CreateShader(device, target, assets::ShaderStage::Vertex, kEncodeVsHlsl, kEncodeVsGlsl, "ProbeCaptureEncodeVS");
    m_ps = CreateShader(device, target, assets::ShaderStage::Fragment, kEncodePsHlsl, kEncodePsGlsl, "ProbeCaptureEncodePS");
    if (m_vs.IsValid() && m_ps.IsValid())
    {
        PipelineDesc desc{};
        desc.shaderStages.push_back({ m_vs, ShaderStageMask::Vertex });
        desc.shaderStages.push_back({ m_ps, ShaderStageMask::Fragment });
        desc.topology = PrimitiveTopology::TriangleList;
        desc.colorFormat = Format::RGBA8_UNORM;
        desc.depthFormat = Format::Unknown;
        desc.sampleCount = 1u;
        desc.rasterizer.cullMode = CullMode::None;
        desc.depthStencil.depthEnable = false;
        desc.depthStencil.depthWrite = false;
        desc.debugName = "ProbeCaptureEncode_Pipeline";
        m_pipeline = device.CreatePipeline(desc);
    }

    bool ok = m_pipeline.IsValid();
    for (uint32_t f = 0u; ok && f < 6u; ++f)
    {
        RenderTargetDesc rt{};
        rt.width = m_captureSize;
        rt.height = m_captureSize;
        rt.colorFormat = Format::RGBA8_UNORM;
        rt.hasDepth = false;
        rt.debugName = "ProbeCaptureFace";
        m_targets[f] = device.CreateRenderTarget(rt);
        ok = m_targets[f].IsValid() && device.GetRenderTargetColorTexture(m_targets[f]).IsValid();
    }
    if (!ok)
    {
        if (outError) *outError = "could not create capture targets or pipeline";
        m_active = true;   // so Release cleans up
        Release(device, runtime);
        return false;
    }

    const PipelineHandle pipeline = m_pipeline;
    m_callbacks = FramePipelineCallbacks{};
    m_callbacks.Register(StandardFrameExecutors::Tonemap,
        [pipeline](const rendergraph::RGExecContext& ctx)
        {
            if (!ctx.cmd)
                return;
            const rendergraph::CompiledResourceSnapshot* hdr = FindResource(ctx, StandardFrameResources::HDRSceneColor);
            if (!hdr || !hdr->texture.IsValid())
            {
                Debug::LogError("ProbeBake.cpp: HDR scene colour not found in the capture frame");
                return;
            }
            ctx.cmd->SetPipeline(pipeline);
            ctx.cmd->SetShaderResource(TexSlots::PassSRV0, hdr->texture, ShaderStageMask::Fragment);
            ctx.cmd->SetSampler(SamplerSlots::LinearClamp, SamplerSlots::LinearClamp, ShaderStageMask::Fragment);
            ctx.cmd->Draw(3u, 1u, 0u, 0u);
        });

    const math::Mat4 projection = math::Mat4::PerspectiveFovRH(
        90.f * math::DEG_TO_RAD, 1.f, m_request.nearPlane, m_request.farPlane);
    std::vector<const RenderView*> suppressed;
    for (uint32_t f = 0u; f < 6u; ++f)
    {
        RenderView view = baseView;
        view.view = math::Mat4::LookAtRH(m_request.position, m_request.position + kFaces[f].forward, kFaces[f].up);
        view.projection = projection;
        view.cameraPosition = m_request.position;
        view.cameraForward = kFaces[f].forward;
        view.nearPlane = m_request.nearPlane;
        view.farPlane = m_request.farPlane;
        view.debugFlags = 0u;
        view.visibilityLayerMask &= ~(LAYER_EDITOR_GIZMO | LAYER_EDITOR_GIZMO_DEPTH | LAYER_EDITOR_PREVIEW_MASK | LAYER_UI);
        view.enableBloom = false;
        view.enableAmbientOcclusion = false;
        view.enableShadows = true;
        view.enableSelectionOutline = false;
        view.sceneOutputRT = RenderTargetHandle::Invalid();
        view.sceneOutputTex = TextureHandle::Invalid();
        view.sceneOutputWidth = 0u;
        view.sceneOutputHeight = 0u;
        m_views[f] = view;

        OffscreenRenderRequest& req = m_requests[f];
        req.view = &m_views[f];
        req.outputRT = m_targets[f];
        req.outputTex = device.GetRenderTargetColorTexture(m_targets[f]);
        req.viewportWidth = m_captureSize;
        req.viewportHeight = m_captureSize;
        req.callbacks = &m_callbacks;
        suppressed.push_back(&m_views[f]);
    }
    // First pass: no probe lights the capture (global IBL only). Second pass:
    // the probes of the first pass light it.
    runtime.SetSuppressedViews(m_request.lightWithProbes ? std::vector<const RenderView*>{} : std::move(suppressed));
    m_active = true;
    return true;
}

std::span<const OffscreenRenderRequest> ProbeCapture::GetRequests() const noexcept
{
    if (!m_active)
        return {};
    return std::span<const OffscreenRenderRequest>(m_requests.data(), m_requests.size());
}

bool ProbeCapture::Finish(IDevice& device, ProbeRuntime& runtime, jobs::JobSystem* jobSystem, std::string* outError)
{
    if (!m_active)
    {
        if (outError) *outError = "no capture running";
        return false;
    }
    device.WaitIdle();

    const uint32_t size = m_captureSize;
    std::vector<float> faces(static_cast<size_t>(6u) * size * size * 3u, 0.f);
    bool ok = true;
    for (uint32_t f = 0u; ok && f < 6u; ++f)
    {
        std::vector<uint8_t> rgba;
        uint32_t w = 0u;
        uint32_t h = 0u;
        if (!device.ReadTexturePixels(device.GetRenderTargetColorTexture(m_targets[f]), rgba, w, h) ||
            w != size || h != size || rgba.size() < static_cast<size_t>(w) * h * 4u)
        {
            if (outError) *outError = "read back of a capture face failed";
            ok = false;
            break;
        }
        for (uint32_t y = 0u; y < size; ++y)
        {
            for (uint32_t x = 0u; x < size; ++x)
            {
                // Mirror horizontally, see kFaces.
                const uint8_t* src = &rgba[(static_cast<size_t>(y) * size + (size - 1u - x)) * 4u];
                float* dst = &faces[((static_cast<size_t>(f) * size + y) * size + x) * 3u];
                if (src[3] == 0u && src[0] == 0u && src[1] == 0u && src[2] == 0u)
                    continue;
                const float scale = std::ldexp(1.f / 255.f, static_cast<int>(src[3]) - 127);
                dst[0] = src[0] * scale;
                dst[1] = src[1] * scale;
                dst[2] = src[2] * scale;
            }
        }
    }

    if (ok)
    {
        IBLBakeParams params{};
        params.irradianceSize = 32u;
        params.irradianceSamples = 64u;
        params.prefilterBaseSize = m_request.resolution;
        params.prefilterMipCount = kIBLPrefilterMipCount;
        params.prefilterSamples = 128u;
        params.jobSystem = jobSystem;
        const auto t0 = std::chrono::steady_clock::now();
        IBLBakedData baked = IBLBaker::BakeFromCubeFaces(faces.data(), size, params);
        const float ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();

        ProbeBakedData data{};
        data.capturePosition = m_request.position;
        data.extents = m_request.extents;
        data.inputHash = m_request.inputHash;
        data.irradianceSize = baked.irradianceSize;
        data.prefilterBaseSize = baked.prefilterBaseSize;
        data.prefilterMipCount = baked.prefilterMipCount;
        data.irradianceData = std::move(baked.irradianceData);
        data.prefilterData = std::move(baked.prefilterData);

        std::string writeError;
        const std::string path = runtime.ResolvePath(m_request.relativePath);
        if (!WriteProbeFile(path, data, &writeError))
        {
            if (outError) *outError = "writing " + path + " failed: " + writeError;
            ok = false;
        }
        else
        {
            Debug::Log("ProbeBake.cpp: baked '%s' (%u px capture, convolution %.0f ms)",
                       m_request.relativePath.c_str(), size, ms);
            runtime.InvalidateFile(device, m_request.relativePath);
        }
    }

    Release(device, runtime);
    return ok;
}

void ProbeCapture::Cancel(IDevice& device, ProbeRuntime& runtime)
{
    if (!m_active)
        return;
    device.WaitIdle();
    Release(device, runtime);
}

void ProbeCapture::Release(IDevice& device, ProbeRuntime& runtime)
{
    runtime.SetSuppressedViews({});
    for (RenderTargetHandle& rt : m_targets)
    {
        if (rt.IsValid())
            device.DestroyRenderTarget(rt);
        rt = RenderTargetHandle::Invalid();
    }
    if (m_pipeline.IsValid()) device.DestroyPipeline(m_pipeline);
    if (m_ps.IsValid()) device.DestroyShader(m_ps);
    if (m_vs.IsValid()) device.DestroyShader(m_vs);
    m_pipeline = PipelineHandle::Invalid();
    m_ps = ShaderHandle::Invalid();
    m_vs = ShaderHandle::Invalid();
    m_requests = {};
    m_callbacks = FramePipelineCallbacks{};
    m_active = false;
}

bool BakeProbeBlocking(RenderSystem& renderSystem, const ecs::World& world, const MaterialSystem& materials,
                       const platform::IPlatformTiming& timing, const RenderView& baseView,
                       ProbeRuntime& runtime, const ProbeBakeRequest& request, std::string* outError)
{
    IDevice* device = renderSystem.GetDevice();
    if (!device)
    {
        if (outError) *outError = "no device";
        return false;
    }
    ProbeCapture capture;
    if (!capture.Begin(*device, runtime, baseView, request, outError))
        return false;
    if (!renderSystem.RenderFrame(world, materials, baseView, timing, {}, capture.GetRequests()))
    {
        capture.Cancel(*device, runtime);
        if (outError) *outError = "capture frame failed";
        return false;
    }
    return capture.Finish(*device, runtime, &renderSystem.GetJobSystem(), outError);
}

} // namespace engine::addons::probes
