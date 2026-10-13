#pragma once
// =============================================================================
// KROM Engine - ecs/Components.hpp
// Core ECS components only. Addon-owned components live in their addon headers.
// =============================================================================
#include "core/Math.hpp"
#include "core/Types.hpp"
#include "ecs/ComponentMeta.hpp"

#include <string>
#include <vector>

namespace engine {

using math::Mat4;
using math::Quat;
using math::Vec2;
using math::Vec3;
using math::Vec4;

// =============================================================================
// Scene / Transform
// =============================================================================

struct TransformComponent
{
    Vec3 localPosition{ 0.f, 0.f, 0.f };
    Quat localRotation = Quat::Identity();
    Vec3 localScale{ 1.f, 1.f, 1.f };
    bool inheritParentScale = true;

    bool     dirty = true;
    uint32_t localVersion = 1u;
    uint32_t worldVersion = 0u;
    uint32_t parentWorldVersion = 0u;

    void SetEulerDeg(float pitch, float yaw, float roll) noexcept
    {
        localRotation = Quat::FromEulerDeg(pitch, yaw, roll);
        dirty = true;
    }

    void RotateLocalEulerDeg(float pitch, float yaw, float roll) noexcept
    {
        localRotation = (localRotation * Quat::FromEulerDeg(pitch, yaw, roll)).Normalized();
        dirty = true;
    }

    void RotateWorldEulerDeg(float pitch, float yaw, float roll) noexcept
    {
        localRotation = (Quat::FromEulerDeg(pitch, yaw, roll) * localRotation).Normalized();
        dirty = true;
    }
};

struct WorldTransformComponent
{
    Vec3 position{ 0.f, 0.f, 0.f };
    Quat rotation = Quat::Identity();
    Vec3 scale{ 1.f, 1.f, 1.f };
    Mat4 matrix = Mat4::Identity();
    Mat4 inverse = Mat4::Identity();
};

struct ParentComponent
{
    EntityID parent = NULL_ENTITY;
};

struct ChildrenComponent
{
    std::vector<EntityID> children;

    void Add(EntityID child)
    {
        for (EntityID e : children)
            if (e == child) return;
        children.push_back(child);
    }

    void Remove(EntityID child)
    {
        for (auto it = children.begin(); it != children.end(); ++it)
        {
            if (*it == child)
            {
                *it = children.back();
                children.pop_back();
                return;
            }
        }
    }
};

struct NameComponent
{
    std::string name;

    NameComponent() = default;
    explicit NameComponent(std::string n) : name(std::move(n)) {}
};

struct GuidComponent
{
    std::string guid;

    GuidComponent() = default;
    explicit GuidComponent(std::string value) : guid(std::move(value)) {}
};

struct BoundsComponent
{
    Vec3     centerLocal{ 0.f, 0.f, 0.f };
    Vec3     extentsLocal{ 1.f, 1.f, 1.f };
    Vec3     centerWorld{ 0.f, 0.f, 0.f };
    Vec3     extentsWorld{ 1.f, 1.f, 1.f };
    float    boundingSphere = 1.f;
    uint32_t lastTransformVersion = 0u;
    bool     localDirty = true;
};

// Oriented Bounding Box — Vorläufer der Kollisions-Pipeline (AABB → Sphere → OBB → BVH).
// centerOffset und halfExtents liegen im Entity-lokalen Raum.
// orientation dreht die OBB relativ zur Entity.
struct OBBComponent
{
    Vec3 centerOffset{ 0.f, 0.f, 0.f };
    Vec3 halfExtents{ 0.5f, 0.5f, 0.5f };
    Quat orientation = Quat::Identity();
    bool showInEditor = false;
};

// =============================================================================
// Script / Behaviour
// =============================================================================

struct ActiveComponent
{
    bool active = true;
};

// =============================================================================
// Tag — frei waehlbares String-Label fuer Script- und Spiellogik
// =============================================================================

struct TagComponent
{
    std::string tag;
};

// =============================================================================
// Registration
// =============================================================================

inline void RegisterCoreComponents(ecs::ComponentMetaRegistry& registry)
{
    using namespace ecs;
    RegisterComponent<TransformComponent>(registry, "TransformComponent");
    RegisterComponent<WorldTransformComponent>(registry, "WorldTransformComponent");
    RegisterComponent<ParentComponent>(registry, "ParentComponent");
    RegisterComponent<ChildrenComponent>(registry, "ChildrenComponent");
    RegisterComponent<NameComponent>(registry, "NameComponent");
    RegisterComponent<GuidComponent>(registry, "GuidComponent");
    RegisterComponent<BoundsComponent>(registry, "BoundsComponent");
    RegisterComponent<OBBComponent>(registry, "OBBComponent");
    RegisterComponent<ActiveComponent>(registry, "ActiveComponent");
    RegisterComponent<TagComponent>(registry, "TagComponent");
}

} // namespace engine
