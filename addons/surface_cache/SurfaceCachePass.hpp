#pragma once
// =============================================================================
// KROM Engine - addons/surface_cache/SurfaceCachePass.hpp
//
// Passes of the surface cache add-on (package 2):
//   SurfaceCacheComputePass  (BeforeOpaque)    trace of the debug view, the
//                            measuring dispatches and the GPU/CPU check, all on
//                            one compute pipeline (shaders/surface_cache_trace.hlsl)
//   SurfaceCacheOverlayPass  (AfterPostProcess) shows the debug view over the
//                            finished image, blended by SurfaceCacheSettings::overlay
//
// Results go to structured buffers, not textures: the Vulkan backend has no
// storage images (doc/PLAN_GI.md, finding 2). The compute passes write 1x1
// placeholder targets in a chain the overlay reads, so the frame graph keeps them.
// =============================================================================
#include "SurfaceCacheTypes.hpp"
#include "renderer/RenderFramePassInterfaces.hpp"
#include <memory>
#include <string_view>

namespace engine::renderer::addons::surface_cache {

// Shader sources (also compiled by the tests).
extern const char* const kTraceCsHlsl;
extern const char* const kOverlayPsHlsl;
extern const char* const kFullscreenVsHlsl;

// GPU layout of the per-dispatch constants (b3, kTraceCsHlsl).
struct TraceDispatchConstants
{
    uint32_t width = 0u;
    uint32_t height = 0u;
    uint32_t tlasNodeCount = 0u;
    uint32_t mode = 0u;          // SurfaceCacheRayKind
    float    tMax = 0.0f;        // 0 = unbounded
    uint32_t frame = 0u;
    uint32_t primaryBase = 0u;   // first element in the primary result buffer
    uint32_t statsSlot = 0u;     // hit and dispatch counters: stats[2 + 2 * slot], stats[3 + 2 * slot]
};
static_assert(sizeof(TraceDispatchConstants) == 32u, "TraceDispatchConstants layout");

// The measuring passes: primary, diffuse and occlusion rays at 1920x1080 and
// 960x540, the secondary ones unbounded, 15 m and 5 m.
std::vector<SurfaceCacheBenchPass> MakeBenchPasses();

// Creates shaders and pipelines (once, lazily). False if the backend cannot.
bool EnsureSurfaceCachePrograms(IDevice& device, SurfaceCacheGpuResources& res);
// Destroys programs, buffers and the per-world BVH data.
void ReleaseSurfaceCacheGpu(IDevice& device, SurfaceCacheGpuResources& res);
// Makes sure a structured buffer holds `count` elements; grows with reserve.
bool EnsureStructuredBuffer(IDevice& device, BufferHandle& buffer, uint64_t& capacity, uint64_t count,
                            uint32_t stride, bool unorderedAccess, const char* debugName);

class SurfaceCacheComputePass final : public IFramePass
{
public:
    explicit SurfaceCacheComputePass(std::shared_ptr<SurfaceCacheGpuResources> resources)
        : m_resources(std::move(resources)) {}

    uint32_t  GetContributorId() const noexcept override { return 0x5CAC0001u; }
    PassPhase GetPhase()         const noexcept override { return PassPhase::BeforeOpaque; }
    bool      DeclaresResource(std::string_view name) const noexcept override;
    uint64_t  GetStructureRevision() const noexcept override;
    void      BuildPass(const PassBuildContext& ctx) const override;

private:
    std::shared_ptr<SurfaceCacheGpuResources> m_resources;
};

class SurfaceCacheOverlayPass final : public IFramePass
{
public:
    explicit SurfaceCacheOverlayPass(std::shared_ptr<SurfaceCacheGpuResources> resources)
        : m_resources(std::move(resources)) {}

    uint32_t  GetContributorId() const noexcept override { return 0x5CAC0002u; }
    PassPhase GetPhase()         const noexcept override { return PassPhase::AfterPostProcess; }
    uint64_t  GetStructureRevision() const noexcept override;
    void      BuildPass(const PassBuildContext& ctx) const override;

private:
    std::shared_ptr<SurfaceCacheGpuResources> m_resources;
};

// Structure revision shared by both passes (enabled, measuring, check).
[[nodiscard]] uint64_t SurfaceCacheStructureRevision(const SurfaceCacheGpuResources& res) noexcept;

} // namespace engine::renderer::addons::surface_cache
