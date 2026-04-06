#pragma once
// =============================================================================
// KROM Engine - ecs/Components.hpp
// Standardkomponenten - datenorientiert, keine Methoden außer Hilfsfunktionen.
// Registrierung via RegisterAllComponents() vor World-Nutzung aufrufen.
// =============================================================================
#include "core/Math.hpp"
#include "core/Types.hpp"
#include "ecs/ComponentMeta.hpp"
#include <string>
#include <vector>

namespace engine {

using math::Vec2;
using math::Vec3;
using math::Vec4;
using math::Mat4;
using math::Quat;

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

// Eltern-Kind-Hierarchie
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

// =============================================================================
// Rendering
// =============================================================================

struct MeshComponent
{
    MeshHandle     mesh;
    bool           castShadows    = true;
    bool           receiveShadows = true;
    uint32_t       layerMask      = 0xFFFFFFFFu;

    MeshComponent() = default;
    explicit MeshComponent(MeshHandle m) : mesh(m) {}
};

struct MaterialComponent
{
    MaterialHandle material;
    uint32_t       submeshIndex = 0u;

    MaterialComponent() = default;
    explicit MaterialComponent(MaterialHandle m) : material(m) {}
};

// AABB für Frustum-Culling - wird vom MeshBoundsSystem gesetzt
struct BoundsComponent
{
    Vec3     centerLocal  { 0.f, 0.f, 0.f };
    Vec3     extentsLocal { 1.f, 1.f, 1.f };
    Vec3     centerWorld  { 0.f, 0.f, 0.f };
    Vec3     extentsWorld { 1.f, 1.f, 1.f };
    float    boundingSphere          = 1.f;
    uint32_t lastTransformVersion    = 0u;
    bool     localDirty              = true;
};

// =============================================================================
// Camera
// =============================================================================

enum class ProjectionType : uint8_t { Perspective = 0, Orthographic = 1 };

struct CameraComponent
{
    ProjectionType projection   = ProjectionType::Perspective;
    float          fovYDeg      = 60.f;
    float          nearPlane    = 0.1f;
    float          farPlane     = 1000.f;
    float          orthoSize    = 10.f;
    float          aspectRatio  = 16.f / 9.f;
    uint32_t       renderLayer  = 0u;
    bool           isMainCamera = false;
};

// =============================================================================
// Lights
// =============================================================================

enum class LightType : uint8_t { Directional = 0, Point = 1, Spot = 2 };

struct LightComponent
{
    LightType type        = LightType::Point;
    Vec3      color       { 1.f, 1.f, 1.f };
    float     intensity   = 1.f;
    float     range       = 10.f;
    float     spotInnerDeg = 15.f;
    float     spotOuterDeg = 30.f;
    bool      castShadows  = false;
    uint32_t  layerMask    = 0xFFFFFFFFu;
};

// =============================================================================
// Particles
// =============================================================================

struct ParticleEmitterComponent
{
    TextureHandle atlasTexture;
    uint32_t      maxParticles    = 1000u;
    float         emitRate        = 50.f;   // Particles/sec
    float         particleLifetime = 2.f;   // Sekunden
    Vec3          initialVelocity  { 0.f, 1.f, 0.f };
    Vec3          velocityRandom   { 0.5f, 0.5f, 0.5f };
    Vec3          gravity          { 0.f, -9.81f, 0.f };
    float         startSize       = 0.1f;
    float         endSize         = 0.0f;
    Vec4          startColor      { 1.f, 1.f, 1.f, 1.f };
    Vec4          endColor        { 1.f, 1.f, 1.f, 0.f };
    bool          looping         = true;
    bool          playing         = true;
};

// =============================================================================
// Script / Behaviour (Basis für Spiellogik)
// =============================================================================

struct ActiveComponent
{
    bool active = true;
};

// =============================================================================
// Registrierung
// =============================================================================

inline void RegisterAllComponents()
{
    using namespace ecs;
    RegisterComponent<TransformComponent>    ("TransformComponent");
    RegisterComponent<WorldTransformComponent>("WorldTransformComponent");
    RegisterComponent<ParentComponent>       ("ParentComponent");
    RegisterComponent<ChildrenComponent>     ("ChildrenComponent");
    RegisterComponent<NameComponent>         ("NameComponent");
    RegisterComponent<MeshComponent>         ("MeshComponent");
    RegisterComponent<MaterialComponent>     ("MaterialComponent");
    RegisterComponent<BoundsComponent>       ("BoundsComponent");
    RegisterComponent<CameraComponent>       ("CameraComponent");
    RegisterComponent<LightComponent>        ("LightComponent");
    RegisterComponent<ParticleEmitterComponent>("ParticleEmitterComponent");
    RegisterComponent<ActiveComponent>       ("ActiveComponent");
}

} // namespace engine
