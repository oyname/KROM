#include "addons/particles/ParticleRenderer.hpp"

#include "assets/AssetRegistry.hpp"
#include "core/Debug.hpp"
#include "renderer/ShaderCompiler.hpp"
#include <algorithm>
#include <cstddef>
#include <utility>

namespace engine::particles {
namespace {

constexpr const char* kParticleVsHlsl = R"(
cbuffer ParticleFrame : register(b0)
{
    float4x4 uViewProjection;
};

// Vulkan: Locations = VertexSemantic-Wert (Position 0, TexCoord0 4, Color0 6).
#ifdef __spirv__
#define VK_LOC(n) [[vk::location(n)]]
#else
#define VK_LOC(n)
#endif

struct VSIn
{
    VK_LOC(0) float3 position : POSITION;
    VK_LOC(4) float2 uv       : TEXCOORD0;
    VK_LOC(6) float4 color    : COLOR0;
};

struct VSOut
{
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD0;
    float4 color    : COLOR0;
};

VSOut main(VSIn input)
{
    VSOut output;
    output.position = mul(uViewProjection, float4(input.position, 1.0));
    output.uv = input.uv;
    output.color = input.color;
    return output;
}
)";

// Weiche Scheibe: voll in der Mitte, quadratisch zum Rand hin auslaufend.
// Additiv (SrcAlpha/One) ergibt das rgb * Abdeckung, Alpha-Blend ueberdeckt.
constexpr const char* kParticlePsHlsl = R"(
struct PSIn
{
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD0;
    float4 color    : COLOR0;
};

float4 main(PSIn input) : SV_TARGET
{
    float2 p = input.uv * 2.0 - 1.0;
    float coverage = saturate(1.0 - dot(p, p));
    coverage *= coverage;
    return float4(input.color.rgb, input.color.a * coverage);
}
)";

constexpr const char* kParticleVsGlsl = R"(#version 450 core
layout(std140, binding = 0) uniform ParticleFrame
{
    mat4 uViewProjection;
};
layout(location = 0) in vec3 inPosition;
layout(location = 4) in vec2 inUv;
layout(location = 6) in vec4 inColor;
layout(location = 0) out vec2 vUv;
layout(location = 1) out vec4 vColor;

void main()
{
    gl_Position = uViewProjection * vec4(inPosition, 1.0);
    vUv = inUv;
    vColor = inColor;
}
)";

constexpr const char* kParticleFsGlsl = R"(#version 450 core
layout(location = 0) in vec2 vUv;
layout(location = 1) in vec4 vColor;
layout(location = 0) out vec4 fragColor;

void main()
{
    vec2 p = vUv * 2.0 - 1.0;
    float coverage = clamp(1.0 - dot(p, p), 0.0, 1.0);
    coverage *= coverage;
    fragColor = vec4(vColor.rgb, vColor.a * coverage);
}
)";

constexpr uint32_t kInitialVertexCapacity = 6u * 256u;

[[nodiscard]] uint32_t NextCapacity(uint32_t required) noexcept
{
    uint32_t capacity = kInitialVertexCapacity;
    while (capacity < required)
        capacity <<= 1u;
    return capacity;
}

[[nodiscard]] ShaderHandle CreateParticleShader(renderer::IDevice& device,
                                                assets::ShaderTargetProfile target,
                                                assets::ShaderStage stage,
                                                assets::ShaderSourceLanguage language,
                                                const char* source,
                                                const char* debugName)
{
    assets::ShaderAsset shader{};
    shader.debugName = debugName;
    shader.stage = stage;
    shader.sourceLanguage = language;
    shader.entryPoint = "main";
    shader.sourceCode = source;

    const renderer::ShaderStageMask stageMask = stage == assets::ShaderStage::Vertex
        ? renderer::ShaderStageMask::Vertex
        : renderer::ShaderStageMask::Fragment;

    if (target != assets::ShaderTargetProfile::OpenGL_GLSL450)
    {
        assets::CompiledShaderArtifact compiled{};
        std::string error;
        if (!renderer::ShaderCompiler::CompileForTarget(shader, target, compiled, &error))
        {
            Debug::LogError("Particles: failed to compile shader '%s': %s", debugName, error.c_str());
            return ShaderHandle::Invalid();
        }

        if (!compiled.bytecode.empty())
        {
            return device.CreateShaderFromBytecode(compiled.bytecode.data(),
                                                   compiled.bytecode.size(),
                                                   stageMask,
                                                   debugName);
        }

        if (target == assets::ShaderTargetProfile::Vulkan_SPIRV)
        {
            Debug::LogError("Particles: shader '%s' produced no SPIR-V bytecode", debugName);
            return ShaderHandle::Invalid();
        }

        return device.CreateShaderFromSource(compiled.sourceText.empty() ? source : compiled.sourceText,
                                             stageMask,
                                             compiled.entryPoint.empty() ? "main" : compiled.entryPoint,
                                             debugName);
    }

    return device.CreateShaderFromSource(source, stageMask, "main", debugName);
}

} // namespace

bool CompileParticleShaders(assets::ShaderTargetProfile target, std::string* error)
{
    const std::pair<assets::ShaderStage, const char*> stages[] = {
        {assets::ShaderStage::Vertex,   kParticleVsHlsl},
        {assets::ShaderStage::Fragment, kParticlePsHlsl},
    };
    for (const auto& [stage, source] : stages)
    {
        assets::ShaderAsset shader{};
        shader.debugName = stage == assets::ShaderStage::Vertex ? "ParticleVS" : "ParticlePS";
        shader.stage = stage;
        shader.sourceLanguage = assets::ShaderSourceLanguage::HLSL;
        shader.entryPoint = "main";
        shader.sourceCode = source;
        assets::CompiledShaderArtifact compiled{};
        if (!renderer::ShaderCompiler::CompileForTarget(shader, target, compiled, error))
            return false;
    }
    return true;
}

ParticleRenderer::~ParticleRenderer()
{
    OnDeviceShutdown();
}

void ParticleRenderer::SetCamera(const math::Mat4& viewProjection, const math::Mat4& view) noexcept
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_viewProjection = viewProjection;
    // Zeilen der View-Rotation = Kamera-Achsen im Weltraum (m[Spalte][Zeile]).
    m_cameraRight = math::Vec3{view.m[0][0], view.m[1][0], view.m[2][0]}.Normalized();
    m_cameraUp    = math::Vec3{view.m[0][1], view.m[1][1], view.m[2][1]}.Normalized();
}

void ParticleRenderer::AppendQuads(std::span<const ParticleBillboard> billboards)
{
    for (const ParticleBillboard& b : billboards)
    {
        const float half = b.size * 0.5f;
        const math::Vec3 r = m_cameraRight * half;
        const math::Vec3 u = m_cameraUp * half;
        const ParticleVertex v00{b.position - r - u, 0.f, 1.f, b.color};
        const ParticleVertex v10{b.position + r - u, 1.f, 1.f, b.color};
        const ParticleVertex v11{b.position + r + u, 1.f, 0.f, b.color};
        const ParticleVertex v01{b.position - r + u, 0.f, 0.f, b.color};
        m_vertices.push_back(v00);
        m_vertices.push_back(v10);
        m_vertices.push_back(v11);
        m_vertices.push_back(v00);
        m_vertices.push_back(v11);
        m_vertices.push_back(v01);
    }
}

void ParticleRenderer::Submit(std::span<const ParticleBillboard> alpha,
                              std::span<const ParticleBillboard> additive)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_vertices.clear();
    m_vertices.reserve((alpha.size() + additive.size()) * 6u);
    AppendQuads(alpha);
    m_alphaVertexCount = static_cast<uint32_t>(m_vertices.size());
    AppendQuads(additive);
}

void ParticleRenderer::Clear() noexcept
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_vertices.clear();
    m_alphaVertexCount = 0u;
}

uint32_t ParticleRenderer::PendingParticleCount() const noexcept
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return static_cast<uint32_t>(m_vertices.size() / 6u);
}

renderer::FramePipelinePassCallback ParticleRenderer::MakeFrameCallback()
{
    return [this](const renderer::RGExecContext& execCtx) {
        Flush(execCtx);
    };
}

bool ParticleRenderer::EnsureGpuResources(renderer::IDevice& device, uint32_t requiredVertices)
{
    if (m_device != nullptr && m_device != &device)
        DestroyGpuResources(*m_device);
    m_device = &device;

    if (!m_gpu.vertexBuffer.IsValid() || m_gpu.vertexCapacity < requiredVertices)
    {
        if (m_gpu.vertexBuffer.IsValid())
            device.DestroyBuffer(m_gpu.vertexBuffer);
        const uint32_t capacity = NextCapacity(requiredVertices);
        renderer::BufferDesc desc{};
        desc.byteSize = static_cast<uint64_t>(capacity) * sizeof(ParticleVertex);
        desc.stride = sizeof(ParticleVertex);
        desc.type = renderer::BufferType::Vertex;
        desc.usage = renderer::ResourceUsage::VertexBuffer;
        desc.access = renderer::MemoryAccess::CpuWrite;
        desc.initialState = renderer::ResourceState::VertexBuffer;
        desc.debugName = "ParticleVB";
        m_gpu.vertexBuffer = device.CreateBuffer(desc);
        m_gpu.vertexCapacity = m_gpu.vertexBuffer.IsValid() ? capacity : 0u;
        if (!m_gpu.vertexBuffer.IsValid())
            return false;
    }

    if (!m_gpu.constantsBuffer.IsValid())
    {
        renderer::BufferDesc cb{};
        cb.byteSize = renderer::kConstantBufferAlignment;
        cb.stride = renderer::kConstantBufferAlignment;
        cb.type = renderer::BufferType::Constant;
        cb.usage = renderer::ResourceUsage::ConstantBuffer;
        cb.access = renderer::MemoryAccess::CpuWrite;
        cb.initialState = renderer::ResourceState::ConstantBuffer;
        cb.debugName = "ParticleFrameCB";
        m_gpu.constantsBuffer = device.CreateBuffer(cb);
        if (!m_gpu.constantsBuffer.IsValid())
            return false;
    }

    if (m_gpu.initialized)
        return true;
    if (m_gpu.shadersFailed)
        return false;

    const assets::ShaderTargetProfile target = renderer::ShaderCompiler::ResolveTargetProfile(device);
    const bool useOpenGlShaders = target == assets::ShaderTargetProfile::OpenGL_GLSL450;
    const auto language = useOpenGlShaders ? assets::ShaderSourceLanguage::GLSL : assets::ShaderSourceLanguage::HLSL;
    m_gpu.vertexShader = CreateParticleShader(device, target, assets::ShaderStage::Vertex, language,
                                              useOpenGlShaders ? kParticleVsGlsl : kParticleVsHlsl,
                                              useOpenGlShaders ? "ParticleVS_GL" : "ParticleVS");
    m_gpu.fragmentShader = CreateParticleShader(device, target, assets::ShaderStage::Fragment, language,
                                                useOpenGlShaders ? kParticleFsGlsl : kParticlePsHlsl,
                                                useOpenGlShaders ? "ParticleFS_GL" : "ParticlePS");
    if (!m_gpu.vertexShader.IsValid() || !m_gpu.fragmentShader.IsValid())
    {
        // Nicht jeden Frame neu versuchen (und neu loggen).
        m_gpu.shadersFailed = true;
        return false;
    }

    renderer::PipelineDesc pso{};
    pso.shaderStages.push_back(renderer::ShaderStageDesc{m_gpu.vertexShader, renderer::ShaderStageMask::Vertex, "main"});
    pso.shaderStages.push_back(renderer::ShaderStageDesc{m_gpu.fragmentShader, renderer::ShaderStageMask::Fragment, "main"});
    pso.vertexLayout.bindings.push_back(renderer::VertexBinding{0u, sizeof(ParticleVertex), renderer::VertexInputRate::PerVertex});
    pso.vertexLayout.attributes.push_back(renderer::VertexAttribute{renderer::VertexSemantic::Position, renderer::Format::RGB32_FLOAT, 0u, 0u});
    pso.vertexLayout.attributes.push_back(renderer::VertexAttribute{renderer::VertexSemantic::TexCoord0, renderer::Format::RG32_FLOAT, 0u, static_cast<uint32_t>(offsetof(ParticleVertex, u))});
    pso.vertexLayout.attributes.push_back(renderer::VertexAttribute{renderer::VertexSemantic::Color0, renderer::Format::RGBA32_FLOAT, 0u, static_cast<uint32_t>(offsetof(ParticleVertex, color))});
    pso.rasterizer.cullMode = renderer::CullMode::None;
    // Gegen die implizite D24S8-Tiefe von HDRSceneColor testen, nicht schreiben.
    pso.depthStencil.depthEnable = true;
    pso.depthStencil.depthWrite  = false;
    pso.depthStencil.depthFunc   = renderer::DepthFunc::LessEqual;
    pso.topology = renderer::PrimitiveTopology::TriangleList;
    pso.colorFormat = renderer::Format::RGBA16_FLOAT;
    pso.depthFormat = renderer::Format::D24_UNORM_S8_UINT;

    renderer::PipelineDesc alphaPso = pso;
    alphaPso.blendStates[0].blendEnable   = true;
    alphaPso.blendStates[0].srcBlend      = renderer::BlendFactor::SrcAlpha;
    alphaPso.blendStates[0].dstBlend      = renderer::BlendFactor::InvSrcAlpha;
    alphaPso.blendStates[0].blendOp       = renderer::BlendOp::Add;
    alphaPso.blendStates[0].srcBlendAlpha = renderer::BlendFactor::One;
    alphaPso.blendStates[0].dstBlendAlpha = renderer::BlendFactor::InvSrcAlpha;
    alphaPso.blendStates[0].blendOpAlpha  = renderer::BlendOp::Add;
    alphaPso.debugName = "ParticleAlphaPipeline";
    m_gpu.pipelineAlpha = device.CreatePipeline(alphaPso);

    renderer::PipelineDesc additivePso = pso;
    additivePso.blendStates[0].blendEnable   = true;
    additivePso.blendStates[0].srcBlend      = renderer::BlendFactor::SrcAlpha;
    additivePso.blendStates[0].dstBlend      = renderer::BlendFactor::One;
    additivePso.blendStates[0].blendOp       = renderer::BlendOp::Add;
    additivePso.blendStates[0].srcBlendAlpha = renderer::BlendFactor::Zero;
    additivePso.blendStates[0].dstBlendAlpha = renderer::BlendFactor::One;
    additivePso.blendStates[0].blendOpAlpha  = renderer::BlendOp::Add;
    additivePso.debugName = "ParticleAdditivePipeline";
    m_gpu.pipelineAdditive = device.CreatePipeline(additivePso);

    m_gpu.initialized = m_gpu.pipelineAlpha.IsValid() && m_gpu.pipelineAdditive.IsValid();
    if (!m_gpu.initialized)
        m_gpu.shadersFailed = true;
    return m_gpu.initialized;
}

void ParticleRenderer::Flush(const renderer::RGExecContext& execCtx)
{
    if (!execCtx.device || !execCtx.cmd)
        return;

    std::vector<ParticleVertex> vertices;
    uint32_t alphaCount = 0u;
    math::Mat4 viewProjection;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_vertices.empty())
            return;
        vertices.swap(m_vertices);
        alphaCount = m_alphaVertexCount;
        m_alphaVertexCount = 0u;
        viewProjection = m_viewProjection;
    }

    const uint32_t vertexCount = static_cast<uint32_t>(vertices.size());
    const uint32_t additiveCount = vertexCount - alphaCount;

    renderer::IDevice& device = *execCtx.device;
    if (!EnsureGpuResources(device, vertexCount))
        return;

    device.UploadBufferData(m_gpu.vertexBuffer, vertices.data(),
                            static_cast<size_t>(vertexCount) * sizeof(ParticleVertex));
    device.UploadBufferData(m_gpu.constantsBuffer, &viewProjection, sizeof(math::Mat4));

    const renderer::BufferBinding frameCb{m_gpu.constantsBuffer, 0u, static_cast<uint32_t>(sizeof(math::Mat4))};
    if (alphaCount > 0u)
    {
        execCtx.cmd->SetPipeline(m_gpu.pipelineAlpha);
        execCtx.cmd->SetConstantBufferRange(0u, frameCb, renderer::ShaderStageMask::Vertex);
        execCtx.cmd->SetVertexBuffer(0u, m_gpu.vertexBuffer, 0u);
        execCtx.cmd->Draw(alphaCount, 1u, 0u, 0u);
    }
    if (additiveCount > 0u)
    {
        execCtx.cmd->SetPipeline(m_gpu.pipelineAdditive);
        execCtx.cmd->SetConstantBufferRange(0u, frameCb, renderer::ShaderStageMask::Vertex);
        execCtx.cmd->SetVertexBuffer(0u, m_gpu.vertexBuffer, 0u);
        execCtx.cmd->Draw(additiveCount, 1u, alphaCount, 0u);
    }
}

void ParticleRenderer::DestroyGpuResources(renderer::IDevice& device) noexcept
{
    if (m_gpu.vertexBuffer.IsValid())
        device.DestroyBuffer(m_gpu.vertexBuffer);
    if (m_gpu.constantsBuffer.IsValid())
        device.DestroyBuffer(m_gpu.constantsBuffer);
    if (m_gpu.pipelineAlpha.IsValid())
        device.DestroyPipeline(m_gpu.pipelineAlpha);
    if (m_gpu.pipelineAdditive.IsValid())
        device.DestroyPipeline(m_gpu.pipelineAdditive);
    if (m_gpu.fragmentShader.IsValid())
        device.DestroyShader(m_gpu.fragmentShader);
    if (m_gpu.vertexShader.IsValid())
        device.DestroyShader(m_gpu.vertexShader);
    m_gpu = {};
}

void ParticleRenderer::OnDeviceShutdown() noexcept
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_device)
    {
        DestroyGpuResources(*m_device);
        m_device = nullptr;
    }
}

} // namespace engine::particles
