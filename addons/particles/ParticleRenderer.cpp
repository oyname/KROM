#include "addons/particles/ParticleRenderer.hpp"

#include "assets/AssetPipeline.hpp"
#include "assets/AssetRegistry.hpp"
#include "core/Debug.hpp"
#include "renderer/ShaderBindingModel.hpp"
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

// Textur (eigenes Bild oder weiche Scheibe) mal Partikelfarbe. Additiv
// (SrcAlpha/One) ergibt rgb * Abdeckung, Alpha-Blend ueberdeckt.
constexpr const char* kParticlePsHlsl = R"(
Texture2D    uParticleTex : register(t8);
SamplerState uSampler     : register(s1);

struct PSIn
{
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD0;
    float4 color    : COLOR0;
};

float4 main(PSIn input) : SV_TARGET
{
    const float4 tex = uParticleTex.Sample(uSampler, input.uv);
    return float4(input.color.rgb * tex.rgb, input.color.a * tex.a);
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
layout(binding = 8) uniform sampler2D uParticleTex;
layout(location = 0) in vec2 vUv;
layout(location = 1) in vec4 vColor;
layout(location = 0) out vec4 fragColor;

void main()
{
    vec4 tex = texture(uParticleTex, vUv);
    fragColor = vec4(vColor.rgb * tex.rgb, vColor.a * tex.a);
}
)";

constexpr uint32_t kInitialVertexCapacity = 6u * 256u;
constexpr uint32_t kDefaultTextureSize = 64u;

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

[[nodiscard]] TextureHandle CreateParticleTexture(renderer::IDevice& device, const ParticleImage& image,
                                                  const char* debugName)
{
    if (image.width == 0u || image.height == 0u ||
        image.rgba.size() < static_cast<size_t>(image.width) * image.height * 4u)
    {
        return TextureHandle::Invalid();
    }
    renderer::TextureDesc desc{};
    desc.width = image.width;
    desc.height = image.height;
    desc.format = image.srgb ? renderer::Format::RGBA8_UNORM_SRGB : renderer::Format::RGBA8_UNORM;
    desc.usage = renderer::ResourceUsage::ShaderResource | renderer::ResourceUsage::CopyDest;
    desc.initialState = renderer::ResourceState::ShaderRead;
    desc.debugName = debugName;
    const TextureHandle texture = device.CreateTexture(desc);
    if (texture.IsValid())
        device.UploadTextureData(texture, image.rgba.data(), image.rgba.size(), 0u, 0u);
    return texture;
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

bool LoadParticleImage(assets::AssetPipeline& pipeline, const assets::AssetRegistry& registry,
                       const std::string& path, ParticleImage& out)
{
    const TextureHandle handle = pipeline.LoadTexture(path);
    const assets::TextureAsset* asset = handle.IsValid() ? registry.textures.Get(handle) : nullptr;
    if (!asset)
        return false;
    const bool rgba8 = asset->format == assets::TextureFormat::RGBA8_UNORM ||
                       asset->format == assets::TextureFormat::RGBA8_SRGB;
    const size_t bytes = static_cast<size_t>(asset->width) * asset->height * 4u;
    if (!rgba8 || asset->width == 0u || asset->height == 0u || asset->pixelData.size() < bytes)
        return false;
    out.width = asset->width;
    out.height = asset->height;
    out.rgba.assign(asset->pixelData.begin(), asset->pixelData.begin() + static_cast<std::ptrdiff_t>(bytes));
    out.srgb = asset->format == assets::TextureFormat::RGBA8_SRGB ||
               asset->metadata.colorSpace == assets::ColorSpace::SRGB;
    return true;
}

ParticleImage MakeSoftDiscImage(uint32_t size)
{
    // Weiss, Alpha voll in der Mitte und quadratisch zum Rand hin auslaufend.
    ParticleImage image{};
    image.width = size;
    image.height = size;
    image.srgb = false;
    image.rgba.resize(static_cast<size_t>(size) * size * 4u);
    for (uint32_t y = 0u; y < size; ++y)
    {
        for (uint32_t x = 0u; x < size; ++x)
        {
            const float px = (static_cast<float>(x) + 0.5f) / static_cast<float>(size) * 2.f - 1.f;
            const float py = (static_cast<float>(y) + 0.5f) / static_cast<float>(size) * 2.f - 1.f;
            float coverage = std::clamp(1.f - (px * px + py * py), 0.f, 1.f);
            coverage *= coverage;
            uint8_t* p = &image.rgba[(static_cast<size_t>(y) * size + x) * 4u];
            p[0] = p[1] = p[2] = 255u;
            p[3] = static_cast<uint8_t>(coverage * 255.f + 0.5f);
        }
    }
    return image;
}

ParticleRenderer::~ParticleRenderer()
{
    OnDeviceShutdown();
}

void ParticleRenderer::SetTextureLoader(ParticleTextureLoader loader)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_loader = std::move(loader);
}

void ParticleRenderer::SetCamera(const math::Mat4& viewProjection, const math::Mat4& view) noexcept
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_viewProjection = viewProjection;
    // Zeilen der View-Rotation = Kamera-Achsen im Weltraum (m[Spalte][Zeile]).
    m_cameraRight = math::Vec3{view.m[0][0], view.m[1][0], view.m[2][0]}.Normalized();
    m_cameraUp    = math::Vec3{view.m[0][1], view.m[1][1], view.m[2][1]}.Normalized();
}

void ParticleRenderer::AppendQuads(std::span<const ParticleBillboard> billboards, bool additive)
{
    for (const ParticleBillboard& b : billboards)
    {
        // Neuer Batch, wenn Mischung oder Textur wechseln (Alpha bleibt sortiert).
        const std::string_view texture = b.texture ? std::string_view(*b.texture) : std::string_view{};
        if (m_batches.empty() || m_batches.back().additive != additive || m_batches.back().texture != texture)
        {
            ParticleDrawBatch batch{};
            batch.additive = additive;
            batch.texture = std::string(texture);
            batch.firstVertex = static_cast<uint32_t>(m_vertices.size());
            m_batches.push_back(std::move(batch));
        }

        const float half = b.size * 0.5f;
        const math::Vec3 r = m_cameraRight * half;
        const math::Vec3 u = m_cameraUp * half;
        const float u0 = b.uvRect.x, v0 = b.uvRect.y, u1 = b.uvRect.z, v1 = b.uvRect.w;
        const ParticleVertex v00{b.position - r - u, u0, v1, b.color};
        const ParticleVertex v10{b.position + r - u, u1, v1, b.color};
        const ParticleVertex v11{b.position + r + u, u1, v0, b.color};
        const ParticleVertex v01{b.position - r + u, u0, v0, b.color};
        m_vertices.push_back(v00);
        m_vertices.push_back(v10);
        m_vertices.push_back(v11);
        m_vertices.push_back(v00);
        m_vertices.push_back(v11);
        m_vertices.push_back(v01);
        m_batches.back().vertexCount += 6u;
    }
}

void ParticleRenderer::Submit(std::span<const ParticleBillboard> alpha,
                              std::span<const ParticleBillboard> additive)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_vertices.clear();
    m_batches.clear();
    m_vertices.reserve((alpha.size() + additive.size()) * 6u);
    AppendQuads(alpha, false);
    AppendQuads(additive, true);
}

void ParticleRenderer::Clear() noexcept
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_vertices.clear();
    m_batches.clear();
}

uint32_t ParticleRenderer::PendingParticleCount() const noexcept
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return static_cast<uint32_t>(m_vertices.size() / 6u);
}

std::vector<ParticleDrawBatch> ParticleRenderer::PendingBatches() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_batches;
}

renderer::FramePipelinePassCallback ParticleRenderer::MakeFrameCallback()
{
    return [this](const renderer::RGExecContext& execCtx) {
        Flush(execCtx);
    };
}

bool ParticleRenderer::EnsureDefaultTexture(renderer::IDevice& device)
{
    if (!m_gpu.defaultTexture.IsValid())
        m_gpu.defaultTexture = CreateParticleTexture(device, MakeSoftDiscImage(kDefaultTextureSize), "ParticleSoftDisc");
    return m_gpu.defaultTexture.IsValid();
}

void ParticleRenderer::Prepare(renderer::IDevice& device)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_device != nullptr && m_device != &device)
        DestroyGpuResources(*m_device);
    m_device = &device;
    if (m_batches.empty())
        return;

    EnsureDefaultTexture(device);
    for (const ParticleDrawBatch& batch : m_batches)
    {
        if (batch.texture.empty() || m_textures.count(batch.texture) != 0u)
            continue;
        ParticleImage image{};
        TextureHandle texture = TextureHandle::Invalid();
        if (m_loader && m_loader(batch.texture, image))
            texture = CreateParticleTexture(device, image, "ParticleTexture");
        if (!texture.IsValid())
            Debug::LogWarning("Particles: Textur '%s' nicht ladbar - weiche Scheibe als Ersatz", batch.texture.c_str());
        m_textures.emplace(batch.texture, texture);
    }
}

TextureHandle ParticleRenderer::ResolveTexture(const std::string& path) const
{
    if (!path.empty())
    {
        if (const auto it = m_textures.find(path); it != m_textures.end() && it->second.IsValid())
            return it->second;
    }
    return m_gpu.defaultTexture;
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

    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_vertices.empty())
        return;
    std::vector<ParticleVertex> vertices;
    std::vector<ParticleDrawBatch> batches;
    vertices.swap(m_vertices);
    batches.swap(m_batches);

    const uint32_t vertexCount = static_cast<uint32_t>(vertices.size());
    renderer::IDevice& device = *execCtx.device;
    // Ohne Prepare (oder ohne Standard-Textur) nichts zeichnen statt ungebunden.
    if (!EnsureGpuResources(device, vertexCount) || !m_gpu.defaultTexture.IsValid())
        return;

    device.UploadBufferData(m_gpu.vertexBuffer, vertices.data(),
                            static_cast<size_t>(vertexCount) * sizeof(ParticleVertex));
    device.UploadBufferData(m_gpu.constantsBuffer, &m_viewProjection, sizeof(math::Mat4));

    const renderer::BufferBinding frameCb{m_gpu.constantsBuffer, 0u, static_cast<uint32_t>(sizeof(math::Mat4))};
    PipelineHandle bound = PipelineHandle::Invalid();
    for (const ParticleDrawBatch& batch : batches)
    {
        const PipelineHandle pipeline = batch.additive ? m_gpu.pipelineAdditive : m_gpu.pipelineAlpha;
        if (pipeline != bound)
        {
            execCtx.cmd->SetPipeline(pipeline);
            execCtx.cmd->SetConstantBufferRange(0u, frameCb, renderer::ShaderStageMask::Vertex);
            execCtx.cmd->SetVertexBuffer(0u, m_gpu.vertexBuffer, 0u);
            execCtx.cmd->SetSampler(renderer::SamplerSlots::LinearClamp, renderer::SamplerSlots::LinearClamp,
                                    renderer::ShaderStageMask::Fragment);
            bound = pipeline;
        }
        execCtx.cmd->SetShaderResource(renderer::TexSlots::PassSRV0, ResolveTexture(batch.texture),
                                       renderer::ShaderStageMask::Fragment);
        execCtx.cmd->Draw(batch.vertexCount, 1u, batch.firstVertex, 0u);
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
    if (m_gpu.defaultTexture.IsValid())
        device.DestroyTexture(m_gpu.defaultTexture);
    for (const auto& [path, texture] : m_textures)
    {
        if (texture.IsValid())
            device.DestroyTexture(texture);
    }
    m_textures.clear();
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
