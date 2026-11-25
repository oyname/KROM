#include "addons/mesh_renderer/MeshSceneBvh.hpp"

#include "addons/animation/AnimationComponents.hpp"
#include "addons/mesh_renderer/MeshRendererComponents.hpp"
#include "assets/AssetRegistry.hpp"
#include "core/Debug.hpp"
#include "ecs/Components.hpp"
#include "ecs/HierarchyState.hpp"
#include "ecs/World.hpp"
#include "jobs/JobSystem.hpp"

namespace engine::mesh_renderer {

namespace {

[[nodiscard]] bool IsMeshShown(const ecs::World& world, EntityID entity) noexcept
{
    // Same rule as the mesh extraction: active, visible, and no hidden mesh above.
    return IsEntityActiveInHierarchy(world, entity) &&
           IsEntityVisibleInHierarchy(world, entity) &&
           AllHierarchyAncestors(world, entity, [&](EntityID id) {
               const auto* mesh = world.Get<MeshComponent>(id);
               return !mesh || mesh->visible;
           });
}

// Material the renderer uses for a submesh (see MeshRendererExtraction.cpp).
[[nodiscard]] MaterialHandle ResolveSubmeshMaterial(const assets::MeshAsset& asset, uint32_t submesh,
                                                    const MaterialComponent* component) noexcept
{
    MaterialHandle mat = MaterialHandle::Invalid();
    if (component)
    {
        if (const MaterialComponent::SlotOverride* slot = component->FindSlotOverride(submesh))
            mat = slot->material;
        if (!mat.IsValid())
            mat = component->material;
    }
    if (!mat.IsValid())
    {
        const uint32_t index = asset.submeshes[submesh].materialIndex;
        if (index < asset.materialHandles.size() && asset.materialHandles[index].IsValid())
            mat = asset.materialHandles[index];
    }
    return mat;
}

} // namespace

MeshSceneBvh::MeshSceneBvh(jobs::JobSystem* jobs)
    : m_jobs(jobs)
    , m_shared(std::make_shared<SharedState>())
{}

MeshSceneBvh::~MeshSceneBvh() = default;   // running jobs keep m_shared alive

uint32_t MeshSceneBvh::PendingBlasBuilds() const
{
    std::lock_guard<std::mutex> lock(m_shared->mutex);
    return m_shared->pending;
}

void MeshSceneBvh::WaitForBlasBuilds()
{
    std::unique_lock<std::mutex> lock(m_shared->mutex);
    m_shared->idle.wait(lock, [&] { return m_shared->pending == 0u; });
}

void MeshSceneBvh::Clear()
{
    m_blas.clear();
    m_descs.clear();
    m_scene.Clear();
    std::lock_guard<std::mutex> lock(m_shared->mutex);
    m_shared->done.clear();
}

void MeshSceneBvh::RequestBlas(uint64_t key, uint64_t revision, const std::vector<float>& positions,
                               const std::vector<uint32_t>& indices)
{
    BlasEntry& entry = m_blas[key];
    entry.revision = revision;
    entry.bvh.reset();
    entry.building = true;
    entry.empty = false;
    entry.unusedUpdates = 0u;

    if (m_jobs)
    {
        // Copies: the asset may change or go away while the job runs.
        auto task = [shared = m_shared, key, revision, positions, indices]()
        {
            auto bvh = std::make_shared<collision::MeshBvh>();
            bvh->Build(positions, indices);
            std::lock_guard<std::mutex> lock(shared->mutex);
            shared->done.push_back(BuildResult{ key, revision, std::move(bvh) });
            --shared->pending;
            shared->idle.notify_all();
        };
        {
            std::lock_guard<std::mutex> lock(m_shared->mutex);
            ++m_shared->pending;
        }
        if (m_jobs->Dispatch(task, jobs::JobPriority::Background))
            return;
        std::lock_guard<std::mutex> lock(m_shared->mutex);   // shutting down: build here
        --m_shared->pending;
    }

    auto bvh = std::make_shared<collision::MeshBvh>();
    bvh->Build(positions, indices);
    ApplyBuild(BuildResult{ key, revision, std::move(bvh) });
}

void MeshSceneBvh::ApplyBuild(BuildResult result)
{
    auto it = m_blas.find(result.key);
    if (it == m_blas.end() || it->second.revision != result.revision)
        return;   // evicted or superseded by a newer revision
    BlasEntry& entry = it->second;
    entry.building = false;
    const collision::MeshBvhBuildStats& stats = result.bvh->BuildStats();
    if (result.bvh->Empty())
    {
        entry.empty = true;
        Debug::LogVerbose("MeshSceneBvh.cpp: BLAS %llx has no usable triangle (%u input, %u degenerate)",
                          static_cast<unsigned long long>(result.key), stats.inputTriangles,
                          stats.degenerateTriangles);
        return;
    }
    Debug::LogVerbose("MeshSceneBvh.cpp: BLAS %llx built: %u triangles (%u degenerate dropped), %u nodes, "
                      "depth %u, %.2f ms",
                      static_cast<unsigned long long>(result.key),
                      stats.inputTriangles - stats.degenerateTriangles, stats.degenerateTriangles,
                      stats.nodes, stats.maxDepth, stats.buildMs);
    entry.bvh = std::move(result.bvh);
}

void MeshSceneBvh::CollectFinishedBuilds()
{
    std::vector<BuildResult> done;
    {
        std::lock_guard<std::mutex> lock(m_shared->mutex);
        done.swap(m_shared->done);
    }
    for (BuildResult& result : done)
        ApplyBuild(std::move(result));
}

void MeshSceneBvh::Update(const ecs::World& world, const assets::AssetRegistry& registry)
{
    ++m_updateCounter;
    CollectFinishedBuilds();
    for (auto& [key, entry] : m_blas)
        ++entry.unusedUpdates;

    m_descs.clear();
    world.View<WorldTransformComponent, MeshComponent>(
        [&](EntityID id, const WorldTransformComponent& wt, const MeshComponent& mesh)
        {
            if (!mesh.mesh.IsValid() || !IsMeshShown(world, id))
                return;
            if (world.Get<SkinComponent>(id))
                return;   // skinned meshes stay out (doc/PLAN_GI.md)
            const assets::MeshAsset* asset = registry.meshes.Get(mesh.mesh);
            if (!asset || asset->state != assets::AssetState::Loaded || asset->HasSkinning())
                return;

            const auto* transform = world.Get<TransformComponent>(id);
            // Without a TransformComponent nothing tracks changes: compare the matrix every update.
            const uint64_t transformVersion = transform ? transform->worldVersion : m_updateCounter;
            const auto* material = world.Get<MaterialComponent>(id);

            for (uint32_t sub = 0u; sub < asset->submeshes.size(); ++sub)
            {
                const uint64_t blasKey = BlasKey(mesh.mesh, sub);
                auto it = m_blas.find(blasKey);
                if (it == m_blas.end() || it->second.revision != asset->geometryRevision)
                {
                    const assets::SubMeshData& data = asset->submeshes[sub];
                    RequestBlas(blasKey, asset->geometryRevision, data.positions, data.indices);
                    it = m_blas.find(blasKey);
                }
                BlasEntry& entry = it->second;
                entry.unusedUpdates = 0u;
                if (!entry.bvh)
                    continue;   // building, or empty

                collision::SceneBvhInstanceDesc& desc = m_descs.emplace_back();
                desc.key = InstanceKey(id, sub);
                desc.entity = id.value;
                desc.submesh = sub;
                desc.materialId = ResolveSubmeshMaterial(*asset, sub, material).value;
                desc.blas = entry.bvh;
                desc.objectToWorld = wt.matrix;
                desc.transformVersion = transformVersion;
            }
        });

    for (auto it = m_blas.begin(); it != m_blas.end();)
    {
        if (!it->second.building && it->second.unusedUpdates > kBlasEvictAfterUpdates)
            it = m_blas.erase(it);
        else
            ++it;
    }

    m_scene.Update(m_descs);
}

} // namespace engine::mesh_renderer
