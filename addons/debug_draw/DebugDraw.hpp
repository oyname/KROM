#pragma once
// =============================================================================
// KROM Engine - addons/debug_draw/DebugDraw.hpp
// Optionales DebugDraw-Addon. Keine Core-/ECS-Abhaengigkeit.
// Zeichnet transiente LineList-Primitives ueber einen Forward-Pipeline-Callback.
// =============================================================================

#include "core/Math.hpp"
#include "renderer/IDevice.hpp"
#include "renderer/RenderPipelineTypes.hpp"
#include <array>
#include <cstdint>
#include <mutex>
#include <vector>

namespace engine::addons::debug_draw {

struct DebugLineVertex
{
    math::Vec3 position;
    math::Vec4 color;
};

struct DebugAABB
{
    math::Vec3 min;
    math::Vec3 max;
};

struct DebugDrawConfig
{
    uint32_t initialVertexCapacity = 4096u;
};

class DebugDrawRenderer
{
public:
    explicit DebugDrawRenderer(DebugDrawConfig config = {});
    ~DebugDrawRenderer();

    DebugDrawRenderer(const DebugDrawRenderer&) = delete;
    DebugDrawRenderer& operator=(const DebugDrawRenderer&) = delete;

    void SetViewProjection(const math::Mat4& viewProjection) noexcept;
    void Clear() noexcept;

    void Line(const math::Vec3& a,
              const math::Vec3& b,
              const math::Vec4& color = math::Vec4{1.f, 1.f, 1.f, 1.f});

    void AABB(const DebugAABB& bounds,
              const math::Vec4& color = math::Vec4{0.f, 1.f, 0.f, 1.f});

    void Box(const std::array<math::Vec3, 8>& corners,
             const math::Vec4& color = math::Vec4{0.f, 1.f, 0.f, 1.f});

    // Erwartet die 8 World-Space-Frustum-Ecken in dieser Reihenfolge:
    // near: 0..3, far: 4..7; jeweils umlaufend.
    void Frustum(const std::array<math::Vec3, 8>& corners,
                 const math::Vec4& color = math::Vec4{1.f, 1.f, 0.f, 1.f});

    // Kreis aus Liniensegmenten. normal definiert die Kreisebene.
    void Circle(const math::Vec3& center,
                const math::Vec3& normal,
                float             radius,
                uint32_t          segments,
                const math::Vec4& color = math::Vec4{1.f, 1.f, 1.f, 1.f});

    void Flush(const renderer::RGExecContext& execCtx);
    void OnDeviceShutdown() noexcept;

    [[nodiscard]] renderer::FramePipelinePassCallback MakeFrameCallback();
    [[nodiscard]] uint32_t LineCount() const noexcept;

private:
    struct GpuResources
    {
        ShaderHandle vertexShader = ShaderHandle::Invalid();
        ShaderHandle fragmentShader = ShaderHandle::Invalid();
        PipelineHandle pipeline = PipelineHandle::Invalid();
        BufferHandle vertexBuffer = BufferHandle::Invalid();
        BufferHandle constantsBuffer = BufferHandle::Invalid();
        uint32_t vertexCapacity = 0u;
        bool initialized = false;
    };

    bool EnsureGpuResources(renderer::IDevice& device, uint32_t requiredVertices);
    bool EnsureVertexCapacity(renderer::IDevice& device, uint32_t requiredVertices);
    void DestroyGpuResources(renderer::IDevice& device) noexcept;

    DebugDrawConfig m_config{};
    math::Mat4 m_viewProjection = math::Mat4::Identity();
    std::vector<DebugLineVertex> m_vertices;
    GpuResources m_gpu{};
    renderer::IDevice* m_device = nullptr;
    mutable std::mutex m_mutex;
};

inline void AddDebugDrawCallback(renderer::FramePipelineCallbacks& callbacks,
                                 DebugDrawRenderer& debugDraw)
{
    callbacks.Register("frame.debug_draw", debugDraw.MakeFrameCallback());
}

} // namespace engine::addons::debug_draw
