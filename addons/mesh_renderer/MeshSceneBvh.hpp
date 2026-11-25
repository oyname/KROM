#pragma once
// =============================================================================
// KROM Engine - addons/mesh_renderer/MeshSceneBvh.hpp
//
// Feeds collision::SceneBvh from the scene: one instance per visible submesh of
// every active entity with a MeshComponent, except skinned meshes. The BLAS of
// each (mesh asset, submesh, geometry revision) is built once, in the
// background when a job system is set, and shared by all instances; an instance
// whose BLAS is not ready yet is left out of the TLAS until it is.
//
// Nothing runs unless Update() is called: a project without GI pays nothing.
// Instance keys are (EntityID::value << 32) | submesh; the material id is the
// MaterialHandle value the renderer would use for that submesh.
// =============================================================================
#include "collision/SceneBvh.hpp"
#include "core/Types.hpp"
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace engine::ecs { class World; }
namespace engine::assets { class AssetRegistry; }
namespace engine::jobs { class JobSystem; }

namespace engine::mesh_renderer {

class MeshSceneBvh
{
public:
    // A cached BLAS no instance used for this many updates is dropped.
    static constexpr uint32_t kBlasEvictAfterUpdates = 600u;

    explicit MeshSceneBvh(jobs::JobSystem* jobs = nullptr);
    ~MeshSceneBvh();
    MeshSceneBvh(const MeshSceneBvh&) = delete;
    MeshSceneBvh& operator=(const MeshSceneBvh&) = delete;

    // Worker jobs build BLAS on the CPU only (no device calls).
    void SetJobSystem(jobs::JobSystem* jobs) noexcept { m_jobs = jobs; }

    // Collects the instances and updates the scene BVH. Call once per frame,
    // after TransformSystem and BoundsSystem.
    void Update(const ecs::World& world, const assets::AssetRegistry& registry);

    [[nodiscard]] const collision::SceneBvh& Scene() const noexcept { return m_scene; }
    [[nodiscard]] uint32_t PendingBlasBuilds() const;
    [[nodiscard]] size_t CachedBlasCount() const noexcept { return m_blas.size(); }
    // Blocks until all background BLAS builds have finished (tests, tools).
    void WaitForBlasBuilds();
    void Clear();

    // Key of a cached BLAS: mesh handle and submesh.
    [[nodiscard]] static uint64_t BlasKey(MeshHandle mesh, uint32_t submesh) noexcept
    {
        return (static_cast<uint64_t>(mesh.value) << 32u) | submesh;
    }
    [[nodiscard]] static uint64_t InstanceKey(EntityID entity, uint32_t submesh) noexcept
    {
        return (static_cast<uint64_t>(entity.value) << 32u) | submesh;
    }

private:
    struct BuildResult
    {
        uint64_t key = 0u;
        uint64_t revision = 0u;
        std::shared_ptr<collision::MeshBvh> bvh;
    };
    // Shared with the worker jobs; outlives this object while jobs run.
    struct SharedState
    {
        mutable std::mutex      mutex;
        std::condition_variable idle;
        std::vector<BuildResult> done;
        uint32_t                pending = 0u;
    };
    struct BlasEntry
    {
        uint64_t revision = 0u;
        std::shared_ptr<const collision::MeshBvh> bvh;   // null while building or empty
        bool     building = false;
        bool     empty = false;                          // no usable triangle
        uint32_t unusedUpdates = 0u;
    };

    void CollectFinishedBuilds();
    void ApplyBuild(BuildResult result);
    void RequestBlas(uint64_t key, uint64_t revision, const std::vector<float>& positions,
                     const std::vector<uint32_t>& indices);

    jobs::JobSystem*                         m_jobs = nullptr;
    std::shared_ptr<SharedState>             m_shared;
    std::unordered_map<uint64_t, BlasEntry>  m_blas;
    std::vector<collision::SceneBvhInstanceDesc> m_descs;
    collision::SceneBvh                      m_scene;
    uint64_t                                 m_updateCounter = 0u;
};

} // namespace engine::mesh_renderer
