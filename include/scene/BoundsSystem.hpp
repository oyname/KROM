#pragma once
// =============================================================================
// KROM Engine - scene/BoundsSystem.hpp
// Berechnet Welt-AABB aus lokalen Mesh-Bounds + WorldTransformComponent.
// Aktualisiert BoundsComponent.centerWorld / extentsWorld / boundingSphere.
//
// Muss nach TransformSystem::Update() und nach GPU-Mesh-Upload laufen,
// damit WorldTransformComponent aktuell und Mesh-Bounds bekannt sind.
// =============================================================================
#include "ecs/World.hpp"
#include "ecs/Components.hpp"
#include "assets/AssetRegistry.hpp"

namespace engine {

class BoundsSystem
{
public:
    // Vollständiger Update: berechnet Welt-Bounds für alle Entities mit
    // MeshComponent + WorldTransformComponent + BoundsComponent.
    //
    // meshRegistry: für Mesh-CPU-Daten (lokale AABB aus Vertex-Positionen).
    // Falls kein MeshAsset verfügbar, bleibt localBounds wie gesetzt.
    void Update(ecs::World& world,
                const assets::AssetRegistry& registry);

    // Einzel-Entity-Update (für dirty-only Pfad oder nach SetParent)
    static void ComputeBoundsForEntity(
        ecs::World& world,
        EntityID    id,
        const assets::AssetRegistry& registry) noexcept;

private:
    // Transformiert lokale AABB in Welt-AABB (konservative OBB→AABB-Approximation)
    static void TransformAABB(
        const math::Vec3& localCenter,
        const math::Vec3& localExtents,
        const math::Mat4& worldMatrix,
        math::Vec3& outWorldCenter,
        math::Vec3& outWorldExtents) noexcept;
};

} // namespace engine
