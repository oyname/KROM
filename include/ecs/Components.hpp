#pragma once
// Kamera-, Licht-, Partikel- und MeshRenderer-Komponenten leben bewusst in separaten AddOn-Headern.
// =============================================================================
// KROM Engine - ecs/Components.hpp
// Standardkomponenten - datenorientiert, keine Methoden ausser Hilfsfunktionen.
// Registrierung bevorzugt ueber die granularen Register*Components()-Funktionen.
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
    Vec3 localScale   { 1.f, 1.f, 1.f };

    bool     dirty        = true;
    uint32_t localVersion = 1u;
    uint32_t worldVersion = 0u;

    void SetEulerDeg(float pitch, float yaw, float roll) noexcept
    {
        localRotation = Quat::FromEulerDeg(pitch, yaw, roll);
        dirty = true;
    }
};

struct WorldTransformComponent
{
    Mat4 matrix  = Mat4::Identity();
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
            if (*it == child) { *it = children.back(); children.pop_back(); return; }
        }
    }
};

struct NameComponent
{
    std::string name;

    NameComponent() = default;
    explicit NameComponent(std::string n) : name(std::move(n)) {}
};

struct BoundsComponent
{
    Vec3     centerLocal  { 0.f, 0.f, 0.f };
    Vec3     extentsLocal { 1.f, 1.f, 1.f };
    Vec3     centerWorld  { 0.f, 0.f, 0.f };
    Vec3     extentsWorld { 1.f, 1.f, 1.f };
    float    boundingSphere       = 1.f;
    uint32_t lastTransformVersion = 0u;
    bool     localDirty           = true;
};

// =============================================================================
// Script / Behaviour
// =============================================================================

struct ActiveComponent
{
    bool active = true;
};

// =============================================================================
// Registrierung
// =============================================================================

inline void RegisterCoreComponents()
{
    using namespace ecs;
    RegisterComponent<TransformComponent>("TransformComponent");
    RegisterComponent<WorldTransformComponent>("WorldTransformComponent");
    RegisterComponent<ParentComponent>("ParentComponent");
    RegisterComponent<ChildrenComponent>("ChildrenComponent");
    RegisterComponent<NameComponent>("NameComponent");
    RegisterComponent<BoundsComponent>("BoundsComponent");
    RegisterComponent<ActiveComponent>("ActiveComponent");
}

} // namespace engine
