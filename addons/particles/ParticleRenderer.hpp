#pragma once
// =============================================================================
// KROM Engine - addons/particles/ParticleRenderer.hpp
// GPU-Darstellung des Partikel-Addons. Keine ECS-Abhaengigkeit.
//
// Submit baut aus den Billboards des Frames kameraausgerichtete Quads (CPU,
// 6 Vertices je Partikel). Der Partikel-Pass ("frame.particles") zeichnet sie
// nach Opaque/Transparent in HDRSceneColor: gegen deren implizite Tiefe
// getestet, ohne Tiefe zu schreiben, zuerst alpha-geblendet (von hinten nach
// vorn), dann additiv. Form: weiche Scheibe aus dem Pixelshader, keine Textur.
// =============================================================================

#include "addons/particles/ParticleSystem.hpp"
#include "core/Math.hpp"
#include "renderer/IDevice.hpp"
#include "renderer/RenderPipelineTypes.hpp"

#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace engine::assets {
enum class ShaderTargetProfile : uint8_t;
}

namespace engine::particles {

inline constexpr std::string_view kParticleFrameExecutor = "frame.particles";

struct ParticleVertex
{
    math::Vec3 position;
    float u = 0.f, v = 0.f;
    math::Vec4 color;
};

class ParticleRenderer
{
public:
    ParticleRenderer() = default;
    ~ParticleRenderer();

    ParticleRenderer(const ParticleRenderer&) = delete;
    ParticleRenderer& operator=(const ParticleRenderer&) = delete;

    // viewProjection inkl. Clip-Space-Anpassung des Devices; view fuer die
    // Kamera-Achsen der Billboards.
    void SetCamera(const math::Mat4& viewProjection, const math::Mat4& view) noexcept;
    void Submit(std::span<const ParticleBillboard> alpha,
                std::span<const ParticleBillboard> additive);
    void Clear() noexcept;

    [[nodiscard]] uint32_t PendingParticleCount() const noexcept;
    [[nodiscard]] bool HasPending() const noexcept { return PendingParticleCount() > 0u; }

    void Flush(const renderer::RGExecContext& execCtx);
    void OnDeviceShutdown() noexcept;
    [[nodiscard]] renderer::FramePipelinePassCallback MakeFrameCallback();

private:
    struct GpuResources
    {
        ShaderHandle vertexShader = ShaderHandle::Invalid();
        ShaderHandle fragmentShader = ShaderHandle::Invalid();
        PipelineHandle pipelineAlpha = PipelineHandle::Invalid();
        PipelineHandle pipelineAdditive = PipelineHandle::Invalid();
        BufferHandle vertexBuffer = BufferHandle::Invalid();
        BufferHandle constantsBuffer = BufferHandle::Invalid();
        uint32_t vertexCapacity = 0u;
        bool initialized = false;
        bool shadersFailed = false;
    };

    void AppendQuads(std::span<const ParticleBillboard> billboards);
    bool EnsureGpuResources(renderer::IDevice& device, uint32_t requiredVertices);
    void DestroyGpuResources(renderer::IDevice& device) noexcept;

    math::Mat4 m_viewProjection = math::Mat4::Identity();
    math::Vec3 m_cameraRight{1.f, 0.f, 0.f};
    math::Vec3 m_cameraUp{0.f, 1.f, 0.f};
    std::vector<ParticleVertex> m_vertices;   // [alpha | additiv]
    uint32_t m_alphaVertexCount = 0u;
    GpuResources m_gpu{};
    renderer::IDevice* m_device = nullptr;
    mutable std::mutex m_mutex;
};

// Kompiliert beide HLSL-Stufen fuer ein Ziel ohne Device (fuer Tests).
[[nodiscard]] bool CompileParticleShaders(assets::ShaderTargetProfile target, std::string* error);

// Registriert den Partikel-Pass; ohne Callback baut die Pipeline ihn nicht.
inline void AddParticleCallback(renderer::FramePipelineCallbacks& callbacks, ParticleRenderer& particles)
{
    callbacks.Register(kParticleFrameExecutor, particles.MakeFrameCallback());
}

} // namespace engine::particles
