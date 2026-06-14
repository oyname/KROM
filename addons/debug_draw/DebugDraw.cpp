#include "addons/debug_draw/DebugDraw.hpp"

#include "assets/AssetRegistry.hpp"
#include "core/Debug.hpp"
#include "renderer/ShaderCompiler.hpp"
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <utility>

namespace engine::addons::debug_draw {
namespace {

constexpr const char* kDebugLineVsHlsl = R"(
cbuffer DebugDrawFrame : register(b0)
{
    float4x4 uViewProjection;
};

struct VSIn
{
    float3 position : POSITION;
    float4 color    : COLOR0;
};

struct VSOut
{
    float4 position : SV_POSITION;
    float4 color    : COLOR0;
};

VSOut main(VSIn input)
{
    VSOut output;
    output.position = mul(uViewProjection, float4(input.position, 1.0));
    output.color = input.color;
    return output;
}
)";

constexpr const char* kDebugLinePsHlsl = R"(
struct PSIn
{
    float4 position : SV_POSITION;
    float4 color    : COLOR0;
};

float4 main(PSIn input) : SV_TARGET
{
    return input.color;
}
)";

constexpr const char* kDebugLineVsGlsl = R"(#version 450 core
layout(std140, binding = 0) uniform DebugDrawFrame
{
    mat4 uViewProjection;
};
layout(location = 0) in vec3 inPosition;
layout(location = 6) in vec4 inColor;
layout(location = 0) out vec4 vColor;

void main()
{
    gl_Position = uViewProjection * vec4(inPosition, 1.0);
    vColor = inColor;
}
)";

constexpr const char* kDebugLineFsGlsl = R"(#version 450 core
layout(location = 0) in vec4 vColor;
layout(location = 0) out vec4 fragColor;

void main()
{
    fragColor = vColor;
}
)";

[[nodiscard]] uint32_t NextCapacity(uint32_t required) noexcept
{
    uint32_t capacity = 1u;
    while (capacity < required)
        capacity <<= 1u;
    return capacity;
}

[[nodiscard]] ShaderHandle CreateDebugShader(renderer::IDevice& device,
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
            Debug::LogError("DebugDraw: failed to compile shader '%s': %s",
                                      debugName,
                                      error.c_str());
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
            Debug::LogError("DebugDraw: shader '%s' produced no SPIR-V bytecode", debugName);
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

DebugDrawRenderer::DebugDrawRenderer(DebugDrawConfig config)
    : m_config(config)
{
    m_vertices.reserve(std::max(2u, config.initialVertexCapacity));
}

DebugDrawRenderer::~DebugDrawRenderer()
{
    OnDeviceShutdown();
}

void DebugDrawRenderer::SetViewProjection(const math::Mat4& viewProjection) noexcept
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_viewProjection = viewProjection;
}

void DebugDrawRenderer::Clear() noexcept
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_vertices.clear();
}

void DebugDrawRenderer::Line(const math::Vec3& a, const math::Vec3& b, const math::Vec4& color)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_vertices.push_back(DebugLineVertex{a, color});
    m_vertices.push_back(DebugLineVertex{b, color});
}

void DebugDrawRenderer::AABB(const DebugAABB& bounds, const math::Vec4& color)
{
    const std::array<math::Vec3, 8> c = {
        math::Vec3{bounds.min.x, bounds.min.y, bounds.min.z},
        math::Vec3{bounds.max.x, bounds.min.y, bounds.min.z},
        math::Vec3{bounds.max.x, bounds.max.y, bounds.min.z},
        math::Vec3{bounds.min.x, bounds.max.y, bounds.min.z},
        math::Vec3{bounds.min.x, bounds.min.y, bounds.max.z},
        math::Vec3{bounds.max.x, bounds.min.y, bounds.max.z},
        math::Vec3{bounds.max.x, bounds.max.y, bounds.max.z},
        math::Vec3{bounds.min.x, bounds.max.y, bounds.max.z},
    };
    Box(c, color);
}

void DebugDrawRenderer::Box(const std::array<math::Vec3, 8>& c, const math::Vec4& color)
{
    const uint8_t edges[24] = {
        0, 1, 1, 2, 2, 3, 3, 0,
        4, 5, 5, 6, 6, 7, 7, 4,
        0, 4, 1, 5, 2, 6, 3, 7,
    };

    std::lock_guard<std::mutex> lock(m_mutex);
    for (uint8_t index : edges)
        m_vertices.push_back(DebugLineVertex{c[index], color});
}

void DebugDrawRenderer::Frustum(const std::array<math::Vec3, 8>& c, const math::Vec4& color)
{
    Box(c, color);
}

void DebugDrawRenderer::Circle(const math::Vec3& center,
                               const math::Vec3& normal,
                               float             radius,
                               uint32_t          segments,
                               const math::Vec4& color)
{
    if (segments < 3u)
        return;

    const math::Vec3 n = normal.Normalized();
    const math::Vec3 ref = (std::abs(n.x) <= std::abs(n.y) && std::abs(n.x) <= std::abs(n.z))
        ? math::Vec3{1.f, 0.f, 0.f}
        : (std::abs(n.y) <= std::abs(n.z) ? math::Vec3{0.f, 1.f, 0.f} : math::Vec3{0.f, 0.f, 1.f});
    const math::Vec3 tangent   = math::Vec3::Cross(n, ref).Normalized();
    const math::Vec3 bitangent = math::Vec3::Cross(n, tangent);

    const float step = math::TWO_PI / static_cast<float>(segments);

    std::lock_guard<std::mutex> lock(m_mutex);
    for (uint32_t i = 0u; i < segments; ++i)
    {
        const float a0 = step * static_cast<float>(i);
        const float a1 = step * static_cast<float>(i + 1u);
        const math::Vec3 p0 = center + (tangent * std::cos(a0) + bitangent * std::sin(a0)) * radius;
        const math::Vec3 p1 = center + (tangent * std::cos(a1) + bitangent * std::sin(a1)) * radius;
        m_vertices.push_back({p0, color});
        m_vertices.push_back({p1, color});
    }
}

uint32_t DebugDrawRenderer::LineCount() const noexcept
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return static_cast<uint32_t>(m_vertices.size() / 2u);
}

renderer::FramePipelinePassCallback DebugDrawRenderer::MakeFrameCallback()
{
    return [this](const renderer::RGExecContext& execCtx) {
        Flush(execCtx);
    };
}

bool DebugDrawRenderer::EnsureVertexCapacity(renderer::IDevice& device, uint32_t requiredVertices)
{
    requiredVertices = std::max(requiredVertices, 2u);
    if (m_gpu.vertexBuffer.IsValid() && m_gpu.vertexCapacity >= requiredVertices)
        return true;

    if (m_gpu.vertexBuffer.IsValid())
        device.DestroyBuffer(m_gpu.vertexBuffer);

    const uint32_t capacity = NextCapacity(std::max(requiredVertices, m_config.initialVertexCapacity));
    renderer::BufferDesc desc{};
    desc.byteSize = static_cast<uint64_t>(capacity) * sizeof(DebugLineVertex);
    desc.stride = sizeof(DebugLineVertex);
    desc.type = renderer::BufferType::Vertex;
    desc.usage = renderer::ResourceUsage::VertexBuffer;
    desc.access = renderer::MemoryAccess::CpuWrite;
    desc.initialState = renderer::ResourceState::VertexBuffer;
    desc.debugName = "DebugDrawLinesVB";
    m_gpu.vertexBuffer = device.CreateBuffer(desc);
    m_gpu.vertexCapacity = m_gpu.vertexBuffer.IsValid() ? capacity : 0u;
    return m_gpu.vertexBuffer.IsValid();
}

bool DebugDrawRenderer::EnsureGpuResources(renderer::IDevice& device, uint32_t requiredVertices)
{
    if (m_device != nullptr && m_device != &device)
        DestroyGpuResources(*m_device);
    m_device = &device;

    if (!EnsureVertexCapacity(device, requiredVertices))
        return false;

    if (!m_gpu.constantsBuffer.IsValid())
    {
        renderer::BufferDesc cb{};
        cb.byteSize = renderer::kConstantBufferAlignment;
        cb.stride = renderer::kConstantBufferAlignment;
        cb.type = renderer::BufferType::Constant;
        cb.usage = renderer::ResourceUsage::ConstantBuffer;
        cb.access = renderer::MemoryAccess::CpuWrite;
        cb.initialState = renderer::ResourceState::ConstantBuffer;
        cb.debugName = "DebugDrawFrameCB";
        m_gpu.constantsBuffer = device.CreateBuffer(cb);
        if (!m_gpu.constantsBuffer.IsValid())
            return false;
    }

    if (m_gpu.initialized)
        return true;

    const assets::ShaderTargetProfile target = renderer::ShaderCompiler::ResolveTargetProfile(device);
    const bool useOpenGlShaders = target == assets::ShaderTargetProfile::OpenGL_GLSL450;
    m_gpu.vertexShader = CreateDebugShader(device,
                                           target,
                                           assets::ShaderStage::Vertex,
                                           useOpenGlShaders ? assets::ShaderSourceLanguage::GLSL : assets::ShaderSourceLanguage::HLSL,
                                           useOpenGlShaders ? kDebugLineVsGlsl : kDebugLineVsHlsl,
                                           useOpenGlShaders ? "DebugDrawLineVS_GL" : "DebugDrawLineVS");
    m_gpu.fragmentShader = CreateDebugShader(device,
                                             target,
                                             assets::ShaderStage::Fragment,
                                             useOpenGlShaders ? assets::ShaderSourceLanguage::GLSL : assets::ShaderSourceLanguage::HLSL,
                                             useOpenGlShaders ? kDebugLineFsGlsl : kDebugLinePsHlsl,
                                             useOpenGlShaders ? "DebugDrawLineFS_GL" : "DebugDrawLinePS");
    if (!m_gpu.vertexShader.IsValid() || !m_gpu.fragmentShader.IsValid())
        return false;

    renderer::PipelineDesc pso{};
    pso.shaderStages.push_back(renderer::ShaderStageDesc{m_gpu.vertexShader, renderer::ShaderStageMask::Vertex, "main"});
    pso.shaderStages.push_back(renderer::ShaderStageDesc{m_gpu.fragmentShader, renderer::ShaderStageMask::Fragment, "main"});
    pso.vertexLayout.bindings.push_back(renderer::VertexBinding{0u, sizeof(DebugLineVertex), renderer::VertexInputRate::PerVertex});
    pso.vertexLayout.attributes.push_back(renderer::VertexAttribute{renderer::VertexSemantic::Position, renderer::Format::RGB32_FLOAT, 0u, 0u});
    pso.vertexLayout.attributes.push_back(renderer::VertexAttribute{renderer::VertexSemantic::Color0, renderer::Format::RGBA32_FLOAT, 0u, static_cast<uint32_t>(offsetof(DebugLineVertex, color))});
    pso.rasterizer.cullMode = renderer::CullMode::None;
    pso.depthStencil.depthEnable = false;
    pso.depthStencil.depthWrite = false;
    pso.topology = renderer::PrimitiveTopology::LineList;
    pso.colorFormat = renderer::Format::RGBA16_FLOAT;
    pso.depthFormat = renderer::Format::Unknown;
    pso.debugName = "DebugDrawLinePipeline";

    m_gpu.pipeline = device.CreatePipeline(pso);
    m_gpu.initialized = m_gpu.pipeline.IsValid();
    return m_gpu.initialized;
}

void DebugDrawRenderer::Flush(const renderer::RGExecContext& execCtx)
{
    if (!execCtx.device || !execCtx.cmd)
        return;

    std::vector<DebugLineVertex> vertices;
    math::Mat4 viewProjection;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_vertices.empty())
            return;
        vertices.swap(m_vertices);
        viewProjection = m_viewProjection;
    }

    const uint32_t vertexCount = static_cast<uint32_t>(vertices.size());
    if (vertexCount < 2u)
        return;

    renderer::IDevice& device = *execCtx.device;
    if (!EnsureGpuResources(device, vertexCount))
        return;

    device.UploadBufferData(m_gpu.vertexBuffer,
                            vertices.data(),
                            static_cast<size_t>(vertexCount) * sizeof(DebugLineVertex));
    device.UploadBufferData(m_gpu.constantsBuffer,
                            &viewProjection,
                            sizeof(math::Mat4));

    execCtx.cmd->SetPipeline(m_gpu.pipeline);
    execCtx.cmd->SetConstantBufferRange(0u,
                                        renderer::BufferBinding{m_gpu.constantsBuffer, 0u, static_cast<uint32_t>(sizeof(math::Mat4))},
                                        renderer::ShaderStageMask::Vertex);
    execCtx.cmd->SetVertexBuffer(0u, m_gpu.vertexBuffer, 0u);
    execCtx.cmd->Draw(vertexCount);
}

void DebugDrawRenderer::DestroyGpuResources(renderer::IDevice& device) noexcept
{
    if (m_gpu.vertexBuffer.IsValid())
        device.DestroyBuffer(m_gpu.vertexBuffer);
    if (m_gpu.constantsBuffer.IsValid())
        device.DestroyBuffer(m_gpu.constantsBuffer);
    if (m_gpu.pipeline.IsValid())
        device.DestroyPipeline(m_gpu.pipeline);
    if (m_gpu.fragmentShader.IsValid())
        device.DestroyShader(m_gpu.fragmentShader);
    if (m_gpu.vertexShader.IsValid())
        device.DestroyShader(m_gpu.vertexShader);
    m_gpu = {};
}

void DebugDrawRenderer::OnDeviceShutdown() noexcept
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_device)
    {
        DestroyGpuResources(*m_device);
        m_device = nullptr;
    }
}

} // namespace engine::addons::debug_draw
