#pragma once
// =============================================================================
// KROM Engine - scene/Scene.hpp
// Scene-System: Deklaration. Implementierung: src/scene/Scene.cpp
// =============================================================================
#include "ecs/World.hpp"
#include "ecs/Components.hpp"
#include "scene/TransformSystem.hpp"
#include <vector>
#include <string_view>

namespace engine {

class Scene
{
public:
    explicit Scene(ecs::World& world) : m_world(world) {}

    // --- Entity-Erzeugung ---
    [[nodiscard]] EntityID CreateEntity(std::string_view name = "Entity");

    // --- Hierarchie ---
    void SetParent(EntityID child, EntityID parent);
    void DetachFromParent(EntityID child);

    // --- Transforms ---
    void PropagateTransforms();
    void SetLocalPosition(EntityID id, const math::Vec3& pos)   noexcept;
    void SetLocalRotation(EntityID id, const math::Quat& rot)   noexcept;
    void SetLocalScale   (EntityID id, const math::Vec3& scale) noexcept;

    // --- Destroy ---
    void DestroyEntity(EntityID id);

    // --- Suche ---
    [[nodiscard]] EntityID FindByName(std::string_view name) const;

    // --- Zugriff ---
    [[nodiscard]] ecs::World&              GetWorld()  noexcept       { return m_world; }
    [[nodiscard]] const ecs::World&        GetWorld()  const noexcept { return m_world; }
    [[nodiscard]] const std::vector<EntityID>& GetRoots() const noexcept { return m_rootEntities; }
    [[nodiscard]] TransformSystem&         GetTransformSystem() noexcept { return m_transformSys; }

private:
    ecs::World&           m_world;
    std::vector<EntityID> m_rootEntities;
    TransformSystem       m_transformSys;

    void RemoveFromRoots(EntityID id);
    void PropagateTransformRecursive(EntityID id,
                                     const math::Mat4* parentWorld,
                                     bool parentDirty);
};

} // namespace engine
