#include "addons/mesh_renderer/MeshRendererExtraction.hpp"

#include "addons/animation/AnimationComponents.hpp"
#include "addons/mesh_renderer/MeshRendererComponents.hpp"
#include "assets/AssetRegistry.hpp"
#include "ecs/Components.hpp"
#include "core/Debug.hpp"
#include "jobs/JobSystem.hpp"

// SkinComponent::gpuBuffer is set by SkinningSystem::Upload() each frame.

namespace engine::addons::mesh_renderer {
namespace {

[[nodiscard]] bool IsEntityActive(const ecs::World& world, EntityID entity) noexcept
{
    const auto* active = world.Get<ActiveComponent>(entity);
    return active == nullptr || active->active;
}

// Füllt gemeinsame Proxy-Felder aus Transform, MeshComponent und optionaler Skin.
void FillProxyBase(renderer::RenderProxy&      p,
                   EntityID                    id,
                   const WorldTransformComponent& wt,
                   const MeshComponent&        mesh,
                   const ecs::World&           world)
{
    p.entity          = id;
    p.mesh            = mesh.mesh;
    p.worldMatrix     = wt.matrix;
    p.worldMatrixInvT = wt.inverse.Transposed();
    p.layerMask       = mesh.layerMask;
    p.castShadows     = mesh.castShadows;
    p.visible         = true;

    p.boundsCenter  = math::Vec3(wt.matrix.m[3][0], wt.matrix.m[3][1], wt.matrix.m[3][2]);
    p.boundsExtents = math::Vec3(1.f, 1.f, 1.f);
    p.boundsRadius  = 0.f;

    if (const auto* b = world.Get<BoundsComponent>(id))
    {
        p.boundsCenter  = b->centerWorld;
        p.boundsExtents = b->extentsWorld;
        p.boundsRadius  = b->boundingSphere;
    }

    if (const auto* skin = world.Get<SkinComponent>(id))
        p.boneBuffer = skin->gpuBuffer;
}

// Schreibt einen Proxy pro Submesh, aufgelöst aus MeshAsset::materialHandles.
// Fallback-Kette: materialHandles[sub.materialIndex] → matFallback → ungültig.
void EmitSubmeshProxies(std::vector<renderer::RenderProxy>& proxies,
                        EntityID                            id,
                        const WorldTransformComponent&      wt,
                        const MeshComponent&                mesh,
                        const assets::MeshAsset&            meshAsset,
                        const MaterialComponent*            materialOverride,
                        const ecs::World&                   world)
{
    const auto& submeshes      = meshAsset.submeshes;
    const auto& matHandles     = meshAsset.materialHandles;
    const uint32_t subCount    = static_cast<uint32_t>(submeshes.size());

    for (uint32_t si = 0u; si < subCount; ++si)
    {
        MaterialHandle mat = MaterialHandle::Invalid();
        if (materialOverride)
        {
            if (const MaterialComponent::SlotOverride* slot = materialOverride->FindSlotOverride(si))
                mat = slot->material;
            if (!mat.IsValid())
                mat = materialOverride->material;
        }
        // mesh->materialHandles nur für Entities ohne entity-level Override (z.B. reine glTF-Imports).
        // Hat die Entity ein MaterialComponent, gewinnt dieses — sonst würden Edits an Objekt A
        // die mesh->materialHandles überschreiben und bei Objekt B (gleiche Geometrie) sichtbar werden.
        if (!mat.IsValid())
        {
            const uint32_t matIdx = submeshes[si].materialIndex;
            if (matIdx < matHandles.size() && matHandles[matIdx].IsValid())
                mat = matHandles[matIdx];
        }

        renderer::RenderProxy& p = proxies.emplace_back();
        FillProxyBase(p, id, wt, mesh, world);
        p.submeshIndex = si;
        p.material     = mat;
    }
}

// Serieller Collect-Pass — unterstützt multi-submesh wenn assetReg vorhanden.
[[nodiscard]] std::vector<renderer::RenderProxy>
CollectProxies(const ecs::World& world, const assets::AssetRegistry* assetReg)
{
    std::vector<renderer::RenderProxy> proxies;

    world.View<WorldTransformComponent, MeshComponent>(
        [&](EntityID id,
            const WorldTransformComponent& wt,
            const MeshComponent& mesh)
        {
            if (!IsEntityActive(world, id)) return;
            if (!mesh.mesh.IsValid())       return;
            if (!mesh.visible)              return;

            const auto* mat = world.Get<MaterialComponent>(id);
            // Wenn AssetRegistry verfügbar: pro Submesh einen Proxy mit dem
            // zugeordneten Material aus materialHandles emittieren.
            if (assetReg)
            {
                const assets::MeshAsset* meshAsset = assetReg->meshes.Get(mesh.mesh);
                if (meshAsset && !meshAsset->submeshes.empty())
                {
                    EmitSubmeshProxies(proxies, id, wt, mesh, *meshAsset, mat, world);
                    return;
                }
            }

            // Fallback: ein Proxy, Submesh 0, Material aus MaterialComponent.
            renderer::RenderProxy& p = proxies.emplace_back();
            FillProxyBase(p, id, wt, mesh, world);
            p.submeshIndex = 0u;
            p.material     = mat ? mat->material : MaterialHandle{};
        });

    return proxies;
}

void ExtractToContext(const renderer::SceneExtractionContext& context)
{
    context.SubmitRenderables(CollectProxies(context.world, context.assetRegistry));
}

// Zweistufiger paralleler Pass:
// Pass 1 (seriell): Entity-IDs und Submesh-Zahl sammeln.
// Pass 2 (parallel): Proxy-Slots parallel befüllen.
//
// Multi-Submesh (assetReg vorhanden) → seriell, da variable Proxy-Anzahl pro
// Entity eine Vorallokation ohne Pass 1 unmöglich macht.
void ExtractToContextParallel(const renderer::SceneExtractionContext& context,
                              jobs::JobSystem& js)
{
    const ecs::World& world = context.world;

    // Wenn AssetRegistry vorhanden, nutzen wir den seriellen Pfad um multi-submesh
    // Expansion korrekt zu unterstützen.
    if (context.assetRegistry)
    {
        context.SubmitRenderables(CollectProxies(world, context.assetRegistry));
        return;
    }

    // Pass 1 (seriell): gültige Entity-IDs sammeln.
    std::vector<EntityID> entities;
    world.View<WorldTransformComponent, MeshComponent>(
        [&](EntityID id, const WorldTransformComponent&, const MeshComponent& mesh)
        {
            if (!IsEntityActive(world, id)) return;
            if (!mesh.mesh.IsValid())       return;
            entities.push_back(id);
        });

    if (entities.empty())
        return;

    // Pass 2 (parallel): disjunkte Proxy-Slots befüllen. Kein AssetRegistry →
    // immer Submesh 0, Material aus MaterialComponent.
    std::vector<renderer::RenderProxy> proxies(entities.size());
    const auto result = js.ParallelFor(entities.size(), [&](size_t begin, size_t end)
    {
        for (size_t i = begin; i < end; ++i)
        {
            const EntityID id    = entities[i];
            const auto*    wt   = world.Get<WorldTransformComponent>(id);
            const auto*    mesh = world.Get<MeshComponent>(id);
            if (!wt || !mesh) continue;

            const auto* mat = world.Get<MaterialComponent>(id);

            renderer::RenderProxy& p = proxies[i];
            FillProxyBase(p, id, *wt, *mesh, world);
            p.submeshIndex = 0u;
            p.material     = MaterialHandle{};
            if (mat)
            {
                if (const MaterialComponent::SlotOverride* slot = mat->FindSlotOverride(0u))
                    p.material = slot->material;
                if (!p.material.IsValid())
                    p.material = mat->material;
            }
        }
    });
    if (!result.Succeeded())
    {
        Debug::LogError("MeshRendererExtraction: parallel pass failed");
        return;
    }

    context.SubmitRenderables(std::move(proxies));
}

} // namespace

void ExtractRenderables(const ecs::World& world, renderer::RenderExtractionView extraction)
{
    renderer::SceneExtractionContext context(world, extraction);
    ExtractToContext(context);
}

void ExtractRenderables(const renderer::SceneExtractionContext& context)
{
    if (context.jobSystem)
        ExtractToContextParallel(context, *context.jobSystem);
    else
        ExtractToContext(context);
}

void ExtractRenderables(const ecs::World& world,
                        renderer::RenderExtractionView extraction,
                        jobs::JobSystem* jobSystem)
{
    renderer::SceneExtractionContext context(world, extraction, nullptr, jobSystem);
    ExtractRenderables(context);
}

} // namespace engine::addons::mesh_renderer
