// =============================================================================
// KROM Engine - src/scene/BoundsSystem.cpp
// =============================================================================
#include "scene/BoundsSystem.hpp"
#include "core/Debug.hpp"
#include <cmath>
#include <cfloat>

namespace engine {

void BoundsSystem::TransformAABB(
    const math::Vec3& localCenter,
    const math::Vec3& localExtents,
    const math::Mat4& worldMatrix,
    math::Vec3& outWorldCenter,
    math::Vec3& outWorldExtents) noexcept
{
    // Methode: transformiere Mittelpunkt, dann projiziere Extents via
    // |M_col0| * ex + |M_col1| * ey + |M_col2| * ez
    // (konservative AABB-Transformation, exakt für achsen-ausgerichtete Extents)
    outWorldCenter = worldMatrix.TransformPoint(localCenter);

    // Extents: Summe der absoluten Matrix-Spalten * jeweiligen Extent-Anteil
    const math::Mat4& m = worldMatrix;
    outWorldExtents.x =
        std::abs(m.m[0][0]) * localExtents.x +
        std::abs(m.m[1][0]) * localExtents.y +
        std::abs(m.m[2][0]) * localExtents.z;
    outWorldExtents.y =
        std::abs(m.m[0][1]) * localExtents.x +
        std::abs(m.m[1][1]) * localExtents.y +
        std::abs(m.m[2][1]) * localExtents.z;
    outWorldExtents.z =
        std::abs(m.m[0][2]) * localExtents.x +
        std::abs(m.m[1][2]) * localExtents.y +
        std::abs(m.m[2][2]) * localExtents.z;
}

void BoundsSystem::Update(ecs::World& world, const assets::AssetRegistry& registry)
{
    world.View<MeshComponent, WorldTransformComponent, BoundsComponent, TransformComponent>(
        [&](EntityID id,
            MeshComponent& mesh,
            WorldTransformComponent& wtc,
            BoundsComponent& bounds,
            TransformComponent& tc)
        {
            // Bounds nur neu berechnen wenn Transform sich geändert hat
            // (worldVersion der Komponente verfolgt das)
            if (tc.worldVersion == bounds.lastTransformVersion && !bounds.localDirty)
                return;

            // Lokale AABB aus Mesh-Asset holen wenn vorhanden
            if (mesh.mesh.IsValid())
            {
                const assets::MeshAsset* asset = registry.meshes.Get(mesh.mesh);
                if (asset && asset->state == assets::AssetState::Loaded
                    && !asset->submeshes.empty())
                {
                    math::Vec3 minPos, maxPos;
                    asset->ComputeBounds(minPos, maxPos);
                    bounds.centerLocal  = (minPos + maxPos) * 0.5f;
                    bounds.extentsLocal = (maxPos - minPos) * 0.5f;
                    bounds.localDirty   = false;
                }
            }

            // Lokale AABB → Welt-AABB
            TransformAABB(bounds.centerLocal, bounds.extentsLocal,
                          wtc.matrix,
                          bounds.centerWorld, bounds.extentsWorld);

            // Bounding-Sphere-Radius = Länge der World-Extents (konservativ)
            bounds.boundingSphere = bounds.extentsWorld.Length();

            bounds.lastTransformVersion = tc.worldVersion;
        });
}

void BoundsSystem::ComputeBoundsForEntity(
    ecs::World& world,
    EntityID    id,
    const assets::AssetRegistry& registry) noexcept
{
    if (!world.IsAlive(id)) return;
    auto* mesh   = world.Get<MeshComponent>(id);
    auto* wtc    = world.Get<WorldTransformComponent>(id);
    auto* bounds = world.Get<BoundsComponent>(id);
    auto* tc     = world.Get<TransformComponent>(id);
    if (!mesh || !wtc || !bounds || !tc) return;

    if (mesh->mesh.IsValid())
    {
        const assets::MeshAsset* asset = registry.meshes.Get(mesh->mesh);
        if (asset && !asset->submeshes.empty())
        {
            math::Vec3 mn, mx;
            asset->ComputeBounds(mn, mx);
            bounds->centerLocal  = (mn + mx) * 0.5f;
            bounds->extentsLocal = (mx - mn) * 0.5f;
            bounds->localDirty   = false;
        }
    }

    TransformAABB(bounds->centerLocal, bounds->extentsLocal,
                  wtc->matrix, bounds->centerWorld, bounds->extentsWorld);
    bounds->boundingSphere       = bounds->extentsWorld.Length();
    bounds->lastTransformVersion = tc->worldVersion;
}

} // namespace engine
