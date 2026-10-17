#include "addons/physics/PhysicsWorld.hpp"
#include "addons/physics/PhysicsComponents.hpp"
#include "core/Debug.hpp"
#include "ecs/Components.hpp"

// Jolt.h muss vor allen anderen Jolt-Headern stehen.
#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace engine::physics {
namespace {

// ─── Jolt-Globalzustand (Factory/Typen) — einmal pro Prozess ─────────────────

std::mutex g_joltMutex;
int        g_joltRefs = 0;

void JoltTrace(const char* fmt, ...)
{
    char buffer[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    Debug::Log("[Jolt] %s", buffer);
}

#ifdef JPH_ENABLE_ASSERTS
bool JoltAssertFailed(const char* expression, const char* message, const char* file, JPH::uint line)
{
    Debug::LogError("[Jolt] Assert: %s (%s) %s:%u",
                    expression, message ? message : "", file, static_cast<unsigned>(line));
    return false;   // nicht in den Debugger springen
}
#endif

void AcquireJolt()
{
    std::lock_guard<std::mutex> lock(g_joltMutex);
    if (g_joltRefs++ > 0)
        return;
    JPH::RegisterDefaultAllocator();
    JPH::Trace = JoltTrace;
    JPH_IF_ENABLE_ASSERTS(JPH::AssertFailed = JoltAssertFailed;)
    JPH::Factory::sInstance = new JPH::Factory();
    JPH::RegisterTypes();
}

void ReleaseJolt()
{
    std::lock_guard<std::mutex> lock(g_joltMutex);
    if (--g_joltRefs > 0)
        return;
    JPH::UnregisterTypes();
    delete JPH::Factory::sInstance;
    JPH::Factory::sInstance = nullptr;
}

// ─── Layer ───────────────────────────────────────────────────────────────────

namespace Layers {
    constexpr JPH::ObjectLayer NON_MOVING = 0;
    constexpr JPH::ObjectLayer MOVING     = 1;
    constexpr JPH::ObjectLayer COUNT      = 2;
}

namespace BroadPhaseLayers {
    constexpr JPH::BroadPhaseLayer NON_MOVING(0);
    constexpr JPH::BroadPhaseLayer MOVING(1);
    constexpr JPH::uint            COUNT = 2;
}

class BroadPhaseLayerMap final : public JPH::BroadPhaseLayerInterface
{
public:
    JPH::uint GetNumBroadPhaseLayers() const override { return BroadPhaseLayers::COUNT; }

    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override
    {
        return layer == Layers::NON_MOVING ? BroadPhaseLayers::NON_MOVING : BroadPhaseLayers::MOVING;
    }

#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override
    {
        return layer == BroadPhaseLayers::NON_MOVING ? "NON_MOVING" : "MOVING";
    }
#endif
};

class ObjectVsBroadPhaseFilter final : public JPH::ObjectVsBroadPhaseLayerFilter
{
public:
    bool ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer bpLayer) const override
    {
        // Statisch gegen statisch wird nie getestet.
        return layer != Layers::NON_MOVING || bpLayer == BroadPhaseLayers::MOVING;
    }
};

class ObjectPairFilter final : public JPH::ObjectLayerPairFilter
{
public:
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override
    {
        return a != Layers::NON_MOVING || b != Layers::NON_MOVING;
    }
};

// ─── Kontakte ────────────────────────────────────────────────────────────────

// Sammelt Kontakt-Beginn/-Ende pro Sub-Shape-Paar. Jolt ruft das aus den
// Physik-Threads auf → nur unter Lock puffern, ausgewertet nach dem Update.
class ContactCollector final : public JPH::ContactListener
{
public:
    struct Raw
    {
        uint32_t body1;
        uint32_t body2;
        bool     added;
    };

    void OnContactAdded(const JPH::Body& body1, const JPH::Body& body2,
                        const JPH::ContactManifold&, JPH::ContactSettings&) override
    {
        Push(body1.GetID(), body2.GetID(), true);
    }

    void OnContactRemoved(const JPH::SubShapeIDPair& pair) override
    {
        Push(pair.GetBody1ID(), pair.GetBody2ID(), false);
    }

    std::vector<Raw> Take()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<Raw> out;
        out.swap(m_raw);
        return out;
    }

    void Clear()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_raw.clear();
    }

private:
    void Push(const JPH::BodyID& a, const JPH::BodyID& b, bool added)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_raw.push_back({ a.GetIndexAndSequenceNumber(), b.GetIndexAndSequenceNumber(), added });
    }

    std::mutex       m_mutex;
    std::vector<Raw> m_raw;
};

// Blendet die Koerper einer Owner-Entity bei Abfragen aus.
class IgnoreOwnerFilter final : public JPH::BodyFilter
{
public:
    IgnoreOwnerFilter(const std::unordered_map<uint32_t, uint32_t>& owners, uint32_t ignoreOwner) noexcept
        : m_owners(owners), m_ignoreOwner(ignoreOwner) {}

    bool ShouldCollide(const JPH::BodyID& id) const override
    {
        if (m_ignoreOwner == 0u)
            return true;
        const auto it = m_owners.find(id.GetIndexAndSequenceNumber());
        return it == m_owners.end() || it->second != m_ignoreOwner;
    }

private:
    const std::unordered_map<uint32_t, uint32_t>& m_owners;
    uint32_t m_ignoreOwner;
};

uint64_t BodyPairKey(uint32_t a, uint32_t b) noexcept
{
    const uint32_t lo = std::min(a, b), hi = std::max(a, b);
    return (static_cast<uint64_t>(hi) << 32) | lo;
}

// ─── Mathe/Transforms ────────────────────────────────────────────────────────

JPH::Vec3 ToJolt(const math::Vec3& v) noexcept { return JPH::Vec3(v.x, v.y, v.z); }
JPH::Quat ToJolt(const math::Quat& q) noexcept
{
    const math::Quat n = q.Normalized();
    return JPH::Quat(n.x, n.y, n.z, n.w);
}
math::Vec3 FromJolt(JPH::Vec3Arg v) noexcept { return { v.GetX(), v.GetY(), v.GetZ() }; }
math::Quat FromJolt(JPH::QuatArg q) noexcept { return { q.GetX(), q.GetY(), q.GetZ(), q.GetW() }; }

math::Vec3 Mul(const math::Vec3& a, const math::Vec3& b) noexcept { return { a.x * b.x, a.y * b.y, a.z * b.z }; }
math::Vec3 Abs(const math::Vec3& v) noexcept { return { std::fabs(v.x), std::fabs(v.y), std::fabs(v.z) }; }
float MaxComponent(const math::Vec3& v) noexcept { return std::max({ v.x, v.y, v.z }); }

struct Pose
{
    math::Vec3 position{ 0.f, 0.f, 0.f };
    math::Quat rotation = math::Quat::Identity();
    math::Vec3 scale{ 1.f, 1.f, 1.f };
};

EntityID ParentOf(const ecs::World& world, EntityID entity)
{
    const auto* parent = world.Get<ParentComponent>(entity);
    return (parent && world.IsAlive(parent->parent)) ? parent->parent : NULL_ENTITY;
}

// Aktuelle Weltpose aus den lokalen Transforms (wie TransformSystem) — damit
// sind Aenderungen aus Scripts im selben Frame schon sichtbar. Entities ohne
// TransformComponent fallen auf WorldTransformComponent zurueck.
Pose ComputeWorldPose(const ecs::World& world, EntityID entity, uint32_t depth = 0u)
{
    const auto* local = world.Get<TransformComponent>(entity);
    if (!local)
    {
        Pose pose{};
        if (const auto* wt = world.Get<WorldTransformComponent>(entity))
            pose = { wt->position, wt->rotation, wt->scale };
        return pose;
    }

    const EntityID parent = ParentOf(world, entity);
    if (!parent.IsValid() || depth > 1024u)
        return { local->localPosition, local->localRotation.Normalized(), local->localScale };

    const Pose p = ComputeWorldPose(world, parent, depth + 1u);
    Pose out{};
    const math::Vec3 offset = local->inheritParentScale ? Mul(p.scale, local->localPosition)
                                                        : local->localPosition;
    out.position = p.position + p.rotation.Rotate(offset);
    out.rotation = (p.rotation * local->localRotation).Normalized();
    out.scale    = local->inheritParentScale ? Mul(p.scale, local->localScale) : local->localScale;
    return out;
}

// Pose eines Colliders im Koerperraum des Owners (Owner-Skalierung eingerechnet,
// Owner-Position/-Rotation nicht). Rein aus lokalen Transforms — daher stabil,
// auch waehrend sich der Owner bewegt. Rechnet wie TransformSystem von oben nach
// unten, damit inheritParentScale = false die Parent-Skalierung abschneidet.
Pose ComputePoseInOwner(const ecs::World& world, EntityID collider, EntityID owner,
                        const math::Vec3& ownerScale)
{
    std::vector<EntityID> chain;   // collider ... (Kind des Owners)
    EntityID cursor = collider;
    for (uint32_t depth = 0u; cursor.IsValid() && cursor != owner && depth < 1024u; ++depth)
    {
        chain.push_back(cursor);
        cursor = ParentOf(world, cursor);
    }

    Pose pose{};
    pose.scale = ownerScale;   // Owner-Frame: Ursprung, keine Rotation, Weltskalierung
    for (auto it = chain.rbegin(); it != chain.rend(); ++it)
    {
        const auto* local = world.Get<TransformComponent>(*it);
        if (!local)
            continue;
        const math::Vec3 offset = local->inheritParentScale ? Mul(pose.scale, local->localPosition)
                                                            : local->localPosition;
        pose.position = pose.position + pose.rotation.Rotate(offset);
        pose.rotation = (pose.rotation * local->localRotation).Normalized();
        pose.scale    = local->inheritParentScale ? Mul(pose.scale, local->localScale) : local->localScale;
    }
    return pose;
}

// ─── Beschreibung eines Koerpers, wie er aus dem ECS gelesen wird ────────────

enum class ColliderKind : uint8_t { Box, Sphere, Capsule };

struct ColliderDesc
{
    ColliderKind kind = ColliderKind::Box;
    math::Vec3   position{ 0.f, 0.f, 0.f };     // im Koerperraum
    math::Quat   rotation = math::Quat::Identity();
    math::Vec3   halfExtents{ 0.5f, 0.5f, 0.5f }; // Box
    float        radius     = 0.5f;               // Kugel/Kapsel
    float        halfHeight = 0.f;                // Kapsel: halbe Zylinderhoehe
};

struct BodyDesc
{
    EntityID                  owner = NULL_ENTITY;
    bool                      hasRigidBody = false;
    RigidBodyComponent        rigidBody{};       // nur gueltig mit hasRigidBody
    Pose                      pose{};            // Weltpose des Owners
    std::vector<ColliderDesc> colliders;
    std::vector<EntityID>     colliderEntities;
};

RigidBodyType EffectiveType(const BodyDesc& desc) noexcept
{
    return desc.hasRigidBody ? desc.rigidBody.type : RigidBodyType::Static;
}

// Fingerabdruck aller Form- und Material-Parameter. Aendert er sich, wird der
// Koerper neu erzeugt (Pose zaehlt nicht dazu).
uint64_t ComputeShapeKey(const BodyDesc& desc)
{
    uint64_t hash = 1469598103934665603ull;
    auto mixBytes = [&hash](const void* data, size_t size) {
        const auto* bytes = static_cast<const unsigned char*>(data);
        for (size_t i = 0; i < size; ++i) { hash ^= bytes[i]; hash *= 1099511628211ull; }
    };
    auto mixFloat = [&](float f) { mixBytes(&f, sizeof(f)); };
    auto mixVec3  = [&](const math::Vec3& v) { mixFloat(v.x); mixFloat(v.y); mixFloat(v.z); };

    const auto type = static_cast<uint8_t>(EffectiveType(desc));
    mixBytes(&type, 1);
    if (desc.hasRigidBody)
    {
        const RigidBodyComponent& rb = desc.rigidBody;
        mixFloat(rb.mass); mixFloat(rb.friction); mixFloat(rb.restitution);
        mixFloat(rb.linearDamping); mixFloat(rb.angularDamping); mixFloat(rb.gravityScale);
        const uint8_t locks = static_cast<uint8_t>((rb.lockRotationX ? 1 : 0) |
                                                   (rb.lockRotationY ? 2 : 0) |
                                                   (rb.lockRotationZ ? 4 : 0));
        mixBytes(&locks, 1);
    }
    for (const ColliderDesc& c : desc.colliders)
    {
        const auto kind = static_cast<uint8_t>(c.kind);
        mixBytes(&kind, 1);
        mixVec3(c.position);
        mixFloat(c.rotation.x); mixFloat(c.rotation.y); mixFloat(c.rotation.z); mixFloat(c.rotation.w);
        mixVec3(c.halfExtents);
        mixFloat(c.radius);
        mixFloat(c.halfHeight);
    }
    return hash;
}

JPH::RefConst<JPH::Shape> CreateColliderShape(const ColliderDesc& c)
{
    JPH::ShapeSettings::ShapeResult result;
    switch (c.kind)
    {
        case ColliderKind::Box:
        {
            const float minHalf = std::min({ c.halfExtents.x, c.halfExtents.y, c.halfExtents.z });
            const float convexRadius = std::min(JPH::cDefaultConvexRadius, minHalf * 0.5f);
            result = JPH::BoxShapeSettings(ToJolt(c.halfExtents), convexRadius).Create();
            break;
        }
        case ColliderKind::Sphere:
            result = JPH::SphereShapeSettings(c.radius).Create();
            break;
        case ColliderKind::Capsule:
            result = (c.halfHeight > 1e-4f)
                ? JPH::CapsuleShapeSettings(c.halfHeight, c.radius).Create()
                : JPH::SphereShapeSettings(c.radius).Create();
            break;
    }
    if (result.HasError())
    {
        Debug::LogWarning("PhysicsWorld: Collider-Form ungueltig: %s", result.GetError().c_str());
        return nullptr;
    }
    return result.Get();
}

bool IsIdentity(const ColliderDesc& c) noexcept
{
    return c.position.x == 0.f && c.position.y == 0.f && c.position.z == 0.f &&
           c.rotation.x == 0.f && c.rotation.y == 0.f && c.rotation.z == 0.f;
}

JPH::RefConst<JPH::Shape> CreateBodyShape(const std::vector<ColliderDesc>& colliders)
{
    if (colliders.size() == 1u)
    {
        JPH::RefConst<JPH::Shape> shape = CreateColliderShape(colliders[0]);
        if (!shape || IsIdentity(colliders[0]))
            return shape;
        auto result = JPH::RotatedTranslatedShapeSettings(
            ToJolt(colliders[0].position), ToJolt(colliders[0].rotation), shape).Create();
        return result.HasError() ? nullptr : result.Get();
    }

    JPH::StaticCompoundShapeSettings compound;
    uint32_t added = 0u;
    for (const ColliderDesc& c : colliders)
    {
        JPH::RefConst<JPH::Shape> shape = CreateColliderShape(c);
        if (!shape)
            continue;
        compound.AddShape(ToJolt(c.position), ToJolt(c.rotation), shape);
        ++added;
    }
    if (added == 0u)
        return nullptr;
    auto result = compound.Create();
    if (result.HasError())
    {
        Debug::LogWarning("PhysicsWorld: Compound-Form ungueltig: %s", result.GetError().c_str());
        return nullptr;
    }
    return result.Get();
}

bool NearlyEqual(const math::Vec3& a, const math::Vec3& b, float eps = 1e-5f) noexcept
{
    return std::fabs(a.x - b.x) <= eps && std::fabs(a.y - b.y) <= eps && std::fabs(a.z - b.z) <= eps;
}

bool NearlyEqual(const math::Quat& a, const math::Quat& b, float eps = 1e-5f) noexcept
{
    // q und -q beschreiben dieselbe Rotation.
    const float dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    return std::fabs(std::fabs(dot) - 1.f) <= eps;
}

// Schreibt eine Weltpose in die TransformComponent (lokal, relativ zum Parent)
// und markiert sie dirty. rotation == nullptr laesst die Rotation unveraendert.
// Liefert die neue localVersion (0 ohne TransformComponent).
uint32_t WriteBackPose(ecs::World& world, EntityID owner, const math::Vec3& position,
                       const math::Quat* rotation)
{
    if (auto* tc = world.Get<TransformComponent>(owner))
    {
        const EntityID parent = ParentOf(world, owner);
        if (parent.IsValid())
        {
            const Pose p = ComputeWorldPose(world, parent);
            const math::Quat inv = p.rotation.Conjugate();
            math::Vec3 local = inv.Rotate(position - p.position);
            if (tc->inheritParentScale)
            {
                local.x = std::fabs(p.scale.x) > 1e-6f ? local.x / p.scale.x : local.x;
                local.y = std::fabs(p.scale.y) > 1e-6f ? local.y / p.scale.y : local.y;
                local.z = std::fabs(p.scale.z) > 1e-6f ? local.z / p.scale.z : local.z;
            }
            tc->localPosition = local;
            if (rotation)
                tc->localRotation = (inv * *rotation).Normalized();
        }
        else
        {
            tc->localPosition = position;
            if (rotation)
                tc->localRotation = *rotation;
        }
        tc->dirty = true;
        ++tc->localVersion;
        return tc->localVersion;
    }
    if (auto* wt = world.Get<WorldTransformComponent>(owner))
    {
        wt->position = position;
        if (rotation)
            wt->rotation = *rotation;
    }
    return 0u;
}

} // namespace

// ─── Impl ────────────────────────────────────────────────────────────────────

struct PhysicsWorld::Impl
{
    struct BodyRecord
    {
        JPH::BodyID   id;
        uint32_t      owner = 0u;     // EntityID-Wert der Koerper-Entity
        RigidBodyType type = RigidBodyType::Static;
        uint64_t      shapeKey = 0u;
        math::Vec3    lastPosition{ 0.f, 0.f, 0.f };  // zuletzt synchronisierte/geschriebene Weltpose
        math::Quat    lastRotation = math::Quat::Identity();
        uint32_t      writtenLocalVersion = 0u;
        bool          hasWritten = false;
        // Kinematik: MoveKinematic setzt in Jolt eine Geschwindigkeit, die erhalten
        // bleibt. Nach einer Bewegung wird daher ein weiterer Schritt zum (dann
        // unveraenderten) Ziel ausgefuehrt, der sie wieder auf null setzt.
        bool          kinematicPending = false;   // neues Ziel seit dem letzten Schritt
        bool          kinematicMoving  = false;   // letzter MoveKinematic hatte Bewegung
        math::Vec3    kinematicPosition{ 0.f, 0.f, 0.f };
        math::Quat    kinematicRotation = math::Quat::Identity();
    };

    PhysicsSettings settings{};
    bool            initialized = false;
    float           accumulator = 0.f;

    BroadPhaseLayerMap       broadPhaseLayers;
    ObjectVsBroadPhaseFilter objectVsBroadPhase;
    ObjectPairFilter         objectPairs;
    ContactCollector         contacts;      // muss das PhysicsSystem ueberleben

    std::unique_ptr<JPH::TempAllocatorImpl>    tempAllocator;
    std::unique_ptr<JPH::JobSystemThreadPool>  jobSystem;
    std::unique_ptr<JPH::PhysicsSystem>        system;

    std::unordered_map<uint32_t, BodyRecord> bodies;        // Owner-Entity → Koerper
    std::unordered_map<uint32_t, uint32_t>   colliderOwner; // Collider-Entity → Owner-Entity
    std::unordered_set<uint32_t>             warnedNested;

    // Kollisions-Events: Sub-Shape-Kontakte pro Koerperpaar; Enter, wenn das
    // Paar die erste Beruehrung bekommt, Exit, wenn die letzte endet.
    // Schlaeft ein Koerper ein, meldet Jolt seine Kontakte als entfernt und
    // beim Aufwachen neu — diese gelten solange als "ruhend" (dormant) und
    // erzeugen keine Events. bodyOwners loest Jolt-BodyIDs auf Entities auf.
    struct PairState
    {
        int active  = 0;
        int dormant = 0;
    };
    std::unordered_map<uint32_t, uint32_t>   bodyOwners;   // BodyID-Wert → Owner-Entity
    std::unordered_map<uint64_t, PairState>  pairContacts;
    std::vector<CollisionEvent>              collisionEvents;

    void EmitPair(uint64_t key, bool enter)
    {
        const auto a = bodyOwners.find(static_cast<uint32_t>(key & 0xffffffffu));
        const auto b = bodyOwners.find(static_cast<uint32_t>(key >> 32));
        if (a != bodyOwners.end() && b != bodyOwners.end())
            collisionEvents.push_back({ EntityID(a->second), EntityID(b->second), enter });
    }

    // Beide Koerper existieren und kein nicht-statischer ist wach.
    bool PairAsleep(uint64_t key)
    {
        JPH::BodyInterface& bi = Bodies();
        for (const uint32_t value : { static_cast<uint32_t>(key & 0xffffffffu), static_cast<uint32_t>(key >> 32) })
        {
            const JPH::BodyID id(value);
            if (!bi.IsAdded(id))
                return false;
            if (bi.GetMotionType(id) != JPH::EMotionType::Static && bi.IsActive(id))
                return false;
        }
        return true;
    }

    void ProcessContacts()
    {
        for (const ContactCollector::Raw& raw : contacts.Take())
        {
            const uint64_t key = BodyPairKey(raw.body1, raw.body2);
            if (raw.added)
            {
                if (!bodyOwners.count(raw.body1) || !bodyOwners.count(raw.body2))
                    continue;
                PairState& pair = pairContacts[key];
                if (pair.dormant > 0)
                {
                    --pair.dormant;          // aufgewacht: Kontakt war nie weg
                    ++pair.active;
                    continue;
                }
                if (pair.active++ == 0)
                    EmitPair(key, true);
            }
            else
            {
                // Paare entfernter Koerper wurden schon in PurgeContacts beendet.
                auto it = pairContacts.find(key);
                if (it == pairContacts.end() || it->second.active <= 0)
                    continue;
                --it->second.active;
                if (PairAsleep(key))
                {
                    ++it->second.dormant;
                    continue;
                }
                if (it->second.active == 0 && it->second.dormant == 0)
                {
                    EmitPair(key, false);
                    pairContacts.erase(it);
                }
            }
        }

        // Ruhende Kontakte wieder wacher Paare, die Jolt nicht neu gemeldet hat,
        // sind wirklich weg (Jolt meldet beim Aufwachen alle Kontakte im ersten Schritt).
        for (auto it = pairContacts.begin(); it != pairContacts.end();)
        {
            PairState& pair = it->second;
            if (pair.dormant == 0 || PairAsleep(it->first))
            {
                ++it;
                continue;
            }
            pair.dormant = 0;
            if (pair.active == 0)
            {
                EmitPair(it->first, false);
                it = pairContacts.erase(it);
                continue;
            }
            ++it;
        }
    }

    // Koerper wird entfernt: seine Beruehrungen enden jetzt (Exit-Events).
    void PurgeContacts(const BodyRecord& record) { PurgeContacts(record.id, record.owner); }

    void PurgeContacts(const JPH::BodyID& bodyId, uint32_t ownerValue)
    {
        const uint32_t id = bodyId.GetIndexAndSequenceNumber();
        for (auto it = pairContacts.begin(); it != pairContacts.end();)
        {
            const uint32_t lo = static_cast<uint32_t>(it->first & 0xffffffffu);
            const uint32_t hi = static_cast<uint32_t>(it->first >> 32);
            if (lo != id && hi != id)
            {
                ++it;
                continue;
            }
            const auto other = bodyOwners.find(lo == id ? hi : lo);
            if (other != bodyOwners.end())
                collisionEvents.push_back({ EntityID(ownerValue), EntityID(other->second), false });
            it = pairContacts.erase(it);
        }
        bodyOwners.erase(id);
    }

    // ── Character Controller ────────────────────────────────────────────────
    struct CharacterRecord
    {
        JPH::Ref<JPH::CharacterVirtual> character;
        uint64_t   settingsKey = 0u;
        float      gravityScale = 1.f;
        float      stepHeight = 0.4f;
        math::Vec3 lastPosition{ 0.f, 0.f, 0.f };
        uint32_t   writtenLocalVersion = 0u;
        bool       hasWritten = false;
    };
    struct CharacterInput
    {
        math::Vec3 move{ 0.f, 0.f, 0.f };
        float      jumpSpeed = 0.f;
    };
    std::unordered_map<uint32_t, CharacterRecord> characters;      // Entity → Figur
    std::unordered_map<uint32_t, CharacterInput>  characterInput;  // auch vor dem Erzeugen

    void DestroyCharacter(CharacterRecord& record, uint32_t ownerValue, bool emitExit)
    {
        if (!record.character)
            return;
        const JPH::BodyID inner = record.character->GetInnerBodyID();
        if (!inner.IsInvalid())
        {
            if (emitExit)
                PurgeContacts(inner, ownerValue);
            else
                bodyOwners.erase(inner.GetIndexAndSequenceNumber());
        }
        record.character = nullptr;   // Destruktor entfernt den inneren Koerper
    }

    bool CreateCharacter(EntityID owner, const CharacterControllerComponent& cc,
                         const math::Vec3& position, uint64_t key, CharacterRecord& outRecord)
    {
        const float radius = std::max(0.01f, cc.radius);
        const float halfCylinder = std::max(0.f, cc.height * 0.5f - radius);
        JPH::ShapeSettings::ShapeResult inner = (halfCylinder > 1e-4f)
            ? JPH::CapsuleShapeSettings(halfCylinder, radius).Create()
            : JPH::SphereShapeSettings(radius).Create();
        if (inner.HasError())
            return false;
        // Ursprung an den Fuessen: Kapsel um ihre halbe Hoehe nach oben versetzen.
        JPH::ShapeSettings::ShapeResult shape = JPH::RotatedTranslatedShapeSettings(
            JPH::Vec3(0.f, halfCylinder + radius, 0.f), JPH::Quat::sIdentity(), inner.Get()).Create();
        if (shape.HasError())
            return false;

        JPH::Ref<JPH::CharacterVirtualSettings> settings = new JPH::CharacterVirtualSettings();
        settings->mShape = shape.Get();
        settings->mInnerBodyShape = shape.Get();
        settings->mInnerBodyLayer = Layers::MOVING;
        settings->mMaxSlopeAngle = JPH::DegreesToRadians(std::clamp(cc.maxSlopeDeg, 0.f, 89.f));
        settings->mMass = std::max(1.f, cc.mass);
        // Nur Kontakte im unteren Halbkugelbereich tragen die Figur.
        settings->mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisY(), -radius);

        outRecord = CharacterRecord{};
        outRecord.character = new JPH::CharacterVirtual(settings, ToJolt(position), JPH::Quat::sIdentity(),
                                                        owner.value, system.get());
        outRecord.settingsKey  = key;
        outRecord.gravityScale = cc.gravityScale;
        outRecord.stepHeight   = std::max(0.f, cc.stepHeight);
        outRecord.lastPosition = position;
        const JPH::BodyID innerId = outRecord.character->GetInnerBodyID();
        if (!innerId.IsInvalid())
            bodyOwners[innerId.GetIndexAndSequenceNumber()] = owner.value;
        return true;
    }

    void SyncCharacters(ecs::World& world)
    {
        std::unordered_set<uint32_t> seen;
        world.View<CharacterControllerComponent>([&](EntityID id, const CharacterControllerComponent& cc) {
            if (!cc.enabled)
                return;
            for (EntityID cursor = id; cursor.IsValid(); cursor = ParentOf(world, cursor))
            {
                if (const auto* active = world.Get<ActiveComponent>(cursor); active && !active->active)
                    return;
            }
            seen.insert(id.value);

            uint64_t key = 1469598103934665603ull;
            for (const float f : { cc.radius, cc.height, cc.maxSlopeDeg, cc.mass })
            {
                uint32_t bits = 0u;
                std::memcpy(&bits, &f, sizeof(bits));
                key = (key ^ bits) * 1099511628211ull;
            }

            const Pose pose = ComputeWorldPose(world, id);
            auto it = characters.find(id.value);
            if (it == characters.end() || it->second.settingsKey != key)
            {
                JPH::Vec3 velocity = JPH::Vec3::sZero();
                if (it != characters.end())
                {
                    velocity = it->second.character->GetLinearVelocity();
                    DestroyCharacter(it->second, id.value, true);
                    characters.erase(it);
                }
                CharacterRecord record{};
                if (!CreateCharacter(id, cc, pose.position, key, record))
                {
                    Debug::LogWarning("PhysicsWorld: Figur fuer Entity %u konnte nicht erzeugt werden.", id.value);
                    return;
                }
                record.character->SetLinearVelocity(velocity);
                if (const auto* tc = world.Get<TransformComponent>(id))
                    record.writtenLocalVersion = tc->localVersion;
                characters.emplace(id.value, std::move(record));
                return;
            }

            CharacterRecord& record = it->second;
            record.gravityScale = cc.gravityScale;
            record.stepHeight   = std::max(0.f, cc.stepHeight);
            // Von aussen gesetzte Position (Script/Editor) → Figur dorthin setzen.
            const auto* tc = world.Get<TransformComponent>(id);
            const bool touched = !tc || tc->dirty || tc->localVersion != record.writtenLocalVersion;
            if (touched && !NearlyEqual(pose.position, record.lastPosition))
            {
                record.character->SetPosition(ToJolt(pose.position));
                record.lastPosition = pose.position;
                if (tc)
                    record.writtenLocalVersion = tc->localVersion;
            }
        });

        for (auto it = characters.begin(); it != characters.end();)
        {
            if (seen.count(it->first))
            {
                ++it;
                continue;
            }
            DestroyCharacter(it->second, it->first, true);
            it = characters.erase(it);
        }
    }

    void StepCharacters(float dt)
    {
        const JPH::Vec3 up = JPH::Vec3::sAxisY();
        for (auto& [ownerValue, record] : characters)
        {
            JPH::CharacterVirtual& ch = *record.character;
            CharacterInput* input = nullptr;
            if (auto it = characterInput.find(ownerValue); it != characterInput.end())
                input = &it->second;

            ch.UpdateGroundVelocity();
            const JPH::Vec3 groundVelocity = ch.GetGroundVelocity();
            const JPH::Vec3 currentVertical = up * up.Dot(ch.GetLinearVelocity());
            const bool towardsGround = (currentVertical.GetY() - groundVelocity.GetY()) < 0.1f;
            const JPH::Vec3 gravity = system->GetGravity() * record.gravityScale;

            JPH::Vec3 velocity;
            if (ch.GetGroundState() == JPH::CharacterBase::EGroundState::OnGround && towardsGround)
            {
                velocity = groundVelocity;
                if (input && input->jumpSpeed > 0.f)
                {
                    velocity += up * input->jumpSpeed;
                    input->jumpSpeed = 0.f;
                }
            }
            else
            {
                velocity = currentVertical;
            }
            velocity += gravity * dt;
            if (input)
                velocity += JPH::Vec3(input->move.x, 0.f, input->move.z);
            ch.SetLinearVelocity(velocity);

            JPH::CharacterVirtual::ExtendedUpdateSettings update;
            update.mWalkStairsStepUp = JPH::Vec3(0.f, record.stepHeight, 0.f);
            update.mStickToFloorStepDown = JPH::Vec3(0.f, -std::max(0.5f, record.stepHeight), 0.f);
            ch.ExtendedUpdate(dt, gravity, update,
                              system->GetDefaultBroadPhaseLayerFilter(Layers::MOVING),
                              system->GetDefaultLayerFilter(Layers::MOVING),
                              JPH::BodyFilter{}, JPH::ShapeFilter{}, *tempAllocator);
        }
    }

    // Befehle aus Scripts fuer Koerper, die erst beim naechsten Sync entstehen
    // (Scripts laufen vor der Physik, z.B. AddImpulse in OnStart eines Prefabs).
    enum class CommandKind : uint8_t { Force, Impulse, Velocity };
    struct PendingCommand
    {
        EntityID    entity;
        CommandKind kind;
        math::Vec3  value;
    };
    std::vector<PendingCommand> pendingCommands;

    void ApplyCommand(const BodyRecord& record, CommandKind kind, const math::Vec3& value)
    {
        switch (kind)
        {
            case CommandKind::Force:    Bodies().AddForce(record.id, ToJolt(value)); break;
            case CommandKind::Impulse:  Bodies().AddImpulse(record.id, ToJolt(value)); break;
            case CommandKind::Velocity: Bodies().SetLinearVelocity(record.id, ToJolt(value)); break;
        }
    }

    bool SubmitCommand(EntityID entity, CommandKind kind, const math::Vec3& value)
    {
        if (!initialized || !entity.IsValid())
            return false;
        if (const BodyRecord* record = FindRecordFor(entity))
        {
            if (record->type != RigidBodyType::Dynamic)
                return false;
            ApplyCommand(*record, kind, value);
            return true;
        }
        pendingCommands.push_back({ entity, kind, value });
        return true;
    }

    // Nach dem Sync: vorgemerkte Befehle anwenden, Rest (kein dynamischer
    // Koerper entstanden) verwerfen.
    void FlushPendingCommands()
    {
        for (const PendingCommand& command : pendingCommands)
        {
            if (const BodyRecord* record = FindDynamicRecordFor(command.entity))
                ApplyCommand(*record, command.kind, command.value);
        }
        pendingCommands.clear();
    }

    JPH::BodyInterface& Bodies() { return system->GetBodyInterface(); }

    const BodyRecord* FindRecordFor(EntityID entity) const
    {
        uint32_t owner = entity.value;
        if (auto it = colliderOwner.find(entity.value); it != colliderOwner.end())
            owner = it->second;
        auto it = bodies.find(owner);
        return it != bodies.end() ? &it->second : nullptr;
    }

    const BodyRecord* FindDynamicRecordFor(EntityID entity) const
    {
        const BodyRecord* record = FindRecordFor(entity);
        return (record && record->type == RigidBodyType::Dynamic) ? record : nullptr;
    }

    void DestroyBody(const BodyRecord& record)
    {
        PurgeContacts(record);
        Bodies().RemoveBody(record.id);
        Bodies().DestroyBody(record.id);
    }

    // Ohne Exit-Events (Szenenwechsel/Shutdown).
    void DestroyAllBodies()
    {
        if (!system)
            return;
        for (auto& [owner, record] : characters)
            DestroyCharacter(record, owner, false);
        characters.clear();
        for (const auto& [owner, record] : bodies)
        {
            Bodies().RemoveBody(record.id);
            Bodies().DestroyBody(record.id);
        }
        bodies.clear();
        colliderOwner.clear();
        bodyOwners.clear();
        pairContacts.clear();
        collisionEvents.clear();
        contacts.Clear();
    }

    // Bestimmt den Owner eines Colliders. false = Collider nimmt nicht teil
    // (inaktiv oder unter einem deaktivierten Rigidbody).
    bool ResolveOwner(const ecs::World& world, EntityID collider, BodyDesc& outDesc)
    {
        EntityID topRigidBody = NULL_ENTITY;
        uint32_t rigidBodyCount = 0u;
        EntityID cursor = collider;
        for (uint32_t depth = 0u; cursor.IsValid() && depth < 1024u; ++depth)
        {
            if (const auto* active = world.Get<ActiveComponent>(cursor); active && !active->active)
                return false;
            if (world.Has<CharacterControllerComponent>(cursor))
                return false;   // Figur hat ihre eigene Form
            if (world.Has<RigidBodyComponent>(cursor))
            {
                topRigidBody = cursor;
                ++rigidBodyCount;
            }
            cursor = ParentOf(world, cursor);
        }

        if (rigidBodyCount > 1u && warnedNested.insert(collider.value).second)
            Debug::LogWarning("PhysicsWorld: verschachtelter Rigidbody unter Entity %u wird ignoriert "
                              "(Collider gehoert zum obersten Rigidbody).", collider.value);

        if (topRigidBody.IsValid())
        {
            const auto* rb = world.Get<RigidBodyComponent>(topRigidBody);
            if (!rb->enabled)
                return false;
            outDesc.owner        = topRigidBody;
            outDesc.hasRigidBody = true;
            outDesc.rigidBody    = *rb;
        }
        else
        {
            outDesc.owner        = collider;
            outDesc.hasRigidBody = false;
        }
        return true;
    }

    void CollectBodies(const ecs::World& world, std::unordered_map<uint32_t, BodyDesc>& outBodies)
    {
        auto addCollider = [&](EntityID entity, auto&& buildCollider) {
            BodyDesc probe{};
            if (!ResolveOwner(world, entity, probe))
                return;
            auto [it, inserted] = outBodies.try_emplace(probe.owner.value, std::move(probe));
            BodyDesc& desc = it->second;
            if (inserted)
                desc.pose = ComputeWorldPose(world, desc.owner);
            const Pose local = ComputePoseInOwner(world, entity, desc.owner, desc.pose.scale);
            ColliderDesc collider{};
            buildCollider(local, collider);
            desc.colliders.push_back(collider);
            desc.colliderEntities.push_back(entity);
        };

        world.View<OBBComponent>([&](EntityID id, const OBBComponent& obb) {
            if (obb.isTrigger)
                return;
            addCollider(id, [&](const Pose& local, ColliderDesc& c) {
                c.kind = ColliderKind::Box;
                c.position = local.position + local.rotation.Rotate(Mul(local.scale, obb.centerOffset));
                c.rotation = (local.rotation * obb.orientation).Normalized();
                const math::Vec3 half = Abs(Mul(local.scale, obb.halfExtents));
                c.halfExtents = { std::max(half.x, 0.001f), std::max(half.y, 0.001f), std::max(half.z, 0.001f) };
            });
        });

        world.View<SphereColliderComponent>([&](EntityID id, const SphereColliderComponent& sphere) {
            addCollider(id, [&](const Pose& local, ColliderDesc& c) {
                c.kind = ColliderKind::Sphere;
                c.position = local.position + local.rotation.Rotate(Mul(local.scale, sphere.centerOffset));
                c.rotation = local.rotation;
                c.radius = std::max(0.001f, sphere.radius * MaxComponent(Abs(local.scale)));
            });
        });

        world.View<CapsuleColliderComponent>([&](EntityID id, const CapsuleColliderComponent& capsule) {
            addCollider(id, [&](const Pose& local, ColliderDesc& c) {
                const math::Vec3 s = Abs(local.scale);
                float axisScale = s.y, radialScale = std::max(s.x, s.z);
                math::Quat axisRotation = math::Quat::Identity();   // Jolt-Kapsel liegt auf Y
                if (capsule.axis == CapsuleAxis::X)
                {
                    axisScale = s.x; radialScale = std::max(s.y, s.z);
                    axisRotation = math::Quat::FromAxisAngleDeg({ 0.f, 0.f, 1.f }, -90.f);
                }
                else if (capsule.axis == CapsuleAxis::Z)
                {
                    axisScale = s.z; radialScale = std::max(s.x, s.y);
                    axisRotation = math::Quat::FromAxisAngleDeg({ 1.f, 0.f, 0.f }, 90.f);
                }
                c.kind = ColliderKind::Capsule;
                c.position = local.position + local.rotation.Rotate(Mul(local.scale, capsule.centerOffset));
                c.rotation = (local.rotation * axisRotation).Normalized();
                c.radius = std::max(0.001f, capsule.radius * radialScale);
                c.halfHeight = std::max(0.f, capsule.height * axisScale * 0.5f - c.radius);
            });
        });
    }

    bool CreateBody(const BodyDesc& desc, uint64_t shapeKey, BodyRecord& outRecord)
    {
        JPH::RefConst<JPH::Shape> shape = CreateBodyShape(desc.colliders);
        if (!shape)
            return false;

        const RigidBodyType type = EffectiveType(desc);
        const JPH::EMotionType motion =
            type == RigidBodyType::Dynamic   ? JPH::EMotionType::Dynamic :
            type == RigidBodyType::Kinematic ? JPH::EMotionType::Kinematic :
                                               JPH::EMotionType::Static;
        const JPH::ObjectLayer layer = type == RigidBodyType::Static ? Layers::NON_MOVING : Layers::MOVING;

        JPH::BodyCreationSettings bcs(shape, ToJolt(desc.pose.position), ToJolt(desc.pose.rotation),
                                      motion, layer);
        bcs.mUserData = desc.owner.value;
        if (desc.hasRigidBody)
        {
            const RigidBodyComponent& rb = desc.rigidBody;
            bcs.mFriction       = std::max(0.f, rb.friction);
            bcs.mRestitution    = std::clamp(rb.restitution, 0.f, 1.f);
            bcs.mLinearDamping  = std::max(0.f, rb.linearDamping);
            bcs.mAngularDamping = std::max(0.f, rb.angularDamping);
            bcs.mGravityFactor  = rb.gravityScale;
            if (type == RigidBodyType::Dynamic)
            {
                bcs.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
                bcs.mMassPropertiesOverride.mMass = std::max(0.001f, rb.mass);
                JPH::EAllowedDOFs dofs = JPH::EAllowedDOFs::All;
                if (rb.lockRotationX) dofs = dofs & ~JPH::EAllowedDOFs::RotationX;
                if (rb.lockRotationY) dofs = dofs & ~JPH::EAllowedDOFs::RotationY;
                if (rb.lockRotationZ) dofs = dofs & ~JPH::EAllowedDOFs::RotationZ;
                bcs.mAllowedDOFs = dofs;
            }
        }

        const JPH::EActivation activation =
            type == RigidBodyType::Static ? JPH::EActivation::DontActivate : JPH::EActivation::Activate;
        const JPH::BodyID id = Bodies().CreateAndAddBody(bcs, activation);
        if (id.IsInvalid())
        {
            Debug::LogWarning("PhysicsWorld: Koerper fuer Entity %u konnte nicht erzeugt werden "
                              "(maxBodies erreicht?).", desc.owner.value);
            return false;
        }

        outRecord = BodyRecord{};
        outRecord.id           = id;
        outRecord.owner        = desc.owner.value;
        bodyOwners[id.GetIndexAndSequenceNumber()] = desc.owner.value;
        outRecord.type         = type;
        outRecord.shapeKey     = shapeKey;
        outRecord.lastPosition = desc.pose.position;
        outRecord.lastRotation = desc.pose.rotation;
        return true;
    }
};

// ─── PhysicsWorld ────────────────────────────────────────────────────────────

PhysicsWorld::PhysicsWorld() : m_impl(std::make_unique<Impl>()) {}

PhysicsWorld::~PhysicsWorld()
{
    Shutdown();
}

bool PhysicsWorld::Initialize(const PhysicsSettings& settings)
{
    if (m_impl->initialized)
        return true;

    AcquireJolt();
    Impl& impl = *m_impl;
    impl.settings = settings;
    impl.settings.fixedTimeStep = std::max(settings.fixedTimeStep, 1.f / 1000.f);
    impl.settings.maxSubSteps   = std::max(settings.maxSubSteps, 1u);

    uint32_t threads = settings.workerThreads;
    if (threads == 0u)
    {
        const uint32_t hw = std::max(1u, std::thread::hardware_concurrency());
        threads = std::clamp(hw > 1u ? hw - 1u : 1u, 1u, 4u);
    }

    impl.tempAllocator = std::make_unique<JPH::TempAllocatorImpl>(10u * 1024u * 1024u);
    impl.jobSystem = std::make_unique<JPH::JobSystemThreadPool>(
        JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, static_cast<int>(threads));
    impl.system = std::make_unique<JPH::PhysicsSystem>();
    impl.system->Init(impl.settings.maxBodies, 0u, 65536u, 10240u,
                      impl.broadPhaseLayers, impl.objectVsBroadPhase, impl.objectPairs);
    impl.system->SetGravity(ToJolt(impl.settings.gravity));
    impl.system->SetContactListener(&impl.contacts);

    impl.accumulator = 0.f;
    impl.initialized = true;
    Debug::Log("PhysicsWorld: Jolt initialisiert (%u Worker-Threads)", threads);
    return true;
}

void PhysicsWorld::Shutdown()
{
    if (!m_impl || !m_impl->initialized)
        return;
    Impl& impl = *m_impl;
    impl.DestroyAllBodies();
    impl.system.reset();
    impl.jobSystem.reset();
    impl.tempAllocator.reset();
    impl.warnedNested.clear();
    impl.pendingCommands.clear();
    impl.initialized = false;
    ReleaseJolt();
}

bool PhysicsWorld::IsInitialized() const noexcept
{
    return m_impl->initialized;
}

void PhysicsWorld::Update(ecs::World& world, float dt)
{
    Sync(world);
    Step(dt);
    WriteBack(world);
}

void PhysicsWorld::Sync(ecs::World& world)
{
    Impl& impl = *m_impl;
    if (!impl.initialized)
        return;

    std::unordered_map<uint32_t, BodyDesc> wanted;
    impl.CollectBodies(world, wanted);

    // Koerper ohne Owner/Collider mehr entfernen.
    for (auto it = impl.bodies.begin(); it != impl.bodies.end();)
    {
        if (wanted.find(it->first) == wanted.end())
        {
            impl.DestroyBody(it->second);
            it = impl.bodies.erase(it);
        }
        else
        {
            ++it;
        }
    }

    impl.colliderOwner.clear();
    uint32_t created = 0u;
    JPH::BodyInterface& bodies = impl.Bodies();

    for (auto& [ownerValue, desc] : wanted)
    {
        for (const EntityID collider : desc.colliderEntities)
            impl.colliderOwner[collider.value] = ownerValue;

        const uint64_t key = ComputeShapeKey(desc);
        auto it = impl.bodies.find(ownerValue);

        // Neu oder Form/Parameter geaendert → (neu) erzeugen.
        if (it == impl.bodies.end() || it->second.shapeKey != key)
        {
            JPH::Vec3 linear = JPH::Vec3::sZero(), angular = JPH::Vec3::sZero();
            bool keepVelocity = false;
            if (it != impl.bodies.end())
            {
                if (it->second.type == RigidBodyType::Dynamic)
                {
                    linear  = bodies.GetLinearVelocity(it->second.id);
                    angular = bodies.GetAngularVelocity(it->second.id);
                    keepVelocity = true;
                }
                impl.DestroyBody(it->second);
                impl.bodies.erase(it);
            }

            Impl::BodyRecord record{};
            if (!impl.CreateBody(desc, key, record))
                continue;
            if (keepVelocity && record.type == RigidBodyType::Dynamic)
                bodies.SetLinearAndAngularVelocity(record.id, linear, angular);
            if (const auto* tc = world.Get<TransformComponent>(desc.owner))
                record.writtenLocalVersion = tc->localVersion;
            impl.bodies.emplace(ownerValue, record);
            ++created;
            continue;
        }

        Impl::BodyRecord& record = it->second;
        const bool moved = !NearlyEqual(desc.pose.position, record.lastPosition) ||
                           !NearlyEqual(desc.pose.rotation, record.lastRotation);
        switch (record.type)
        {
            case RigidBodyType::Static:
                if (moved)
                    bodies.SetPositionAndRotation(record.id, ToJolt(desc.pose.position),
                                                  ToJolt(desc.pose.rotation), JPH::EActivation::DontActivate);
                break;

            case RigidBodyType::Kinematic:
                if (moved)
                {
                    record.kinematicPending  = true;
                    record.kinematicPosition = desc.pose.position;
                    record.kinematicRotation = desc.pose.rotation;
                }
                break;

            case RigidBodyType::Dynamic:
            {
                // Transform wurde von aussen gesetzt (Script/Editor) → teleportieren.
                // Signal ist localVersion ODER dirty: Hilfsfunktionen wie
                // SetEulerDeg() setzen nur dirty. Unser WriteBack setzt dirty zwar
                // auch, das TransformSystem loescht es aber noch im selben Frame.
                const auto* tc = world.Get<TransformComponent>(desc.owner);
                const bool touched = !tc || tc->dirty || tc->localVersion != record.writtenLocalVersion;
                const bool externallyChanged = touched && moved;
                if (externallyChanged)
                {
                    bodies.SetPositionAndRotation(record.id, ToJolt(desc.pose.position),
                                                  ToJolt(desc.pose.rotation), JPH::EActivation::Activate);
                    if (tc)
                        record.writtenLocalVersion = tc->localVersion;
                }
                break;
            }
        }
        record.lastPosition = desc.pose.position;
        record.lastRotation = desc.pose.rotation;
    }

    impl.SyncCharacters(world);
    impl.FlushPendingCommands();

    if (created >= 32u)
        impl.system->OptimizeBroadPhase();
}

uint32_t PhysicsWorld::Step(float dt)
{
    Impl& impl = *m_impl;
    if (!impl.initialized || !(dt > 0.f))
        return 0u;

    const float fixed = impl.settings.fixedTimeStep;
    impl.accumulator += dt;
    uint32_t steps = static_cast<uint32_t>(impl.accumulator / fixed);
    if (steps > impl.settings.maxSubSteps)
    {
        steps = impl.settings.maxSubSteps;
        impl.accumulator = 0.f;   // Rueckstand verwerfen statt Todesspirale
    }
    else
    {
        impl.accumulator -= static_cast<float>(steps) * fixed;
    }
    if (steps == 0u)
        return 0u;

    const float stepTime = static_cast<float>(steps) * fixed;
    JPH::BodyInterface& bodies = impl.Bodies();
    for (auto& [owner, record] : impl.bodies)
    {
        if (record.type != RigidBodyType::Kinematic)
            continue;
        // Auch nach dem Stillstand einmal zum unveraenderten Ziel bewegen → v = 0.
        if (!record.kinematicPending && !record.kinematicMoving)
            continue;
        bodies.MoveKinematic(record.id, ToJolt(record.kinematicPosition),
                             ToJolt(record.kinematicRotation), stepTime);
        record.kinematicMoving  = record.kinematicPending;
        record.kinematicPending = false;
    }

    for (uint32_t i = 0; i < steps; ++i)
        impl.StepCharacters(fixed);
    for (auto& [owner, input] : impl.characterInput)
        input.jumpSpeed = 0.f;   // nicht eingeloester Sprung verfaellt

    const JPH::EPhysicsUpdateError error = impl.system->Update(
        stepTime, static_cast<int>(steps), impl.tempAllocator.get(), impl.jobSystem.get());
    impl.ProcessContacts();
    if (error != JPH::EPhysicsUpdateError::None)
        Debug::LogWarning("PhysicsWorld: Update meldet Fehler 0x%x (Limits erhoehen?)",
                          static_cast<unsigned>(error));
    return steps;
}

void PhysicsWorld::WriteBack(ecs::World& world)
{
    Impl& impl = *m_impl;
    if (!impl.initialized)
        return;

    for (auto& [ownerValue, record] : impl.characters)
    {
        const math::Vec3 position = FromJolt(record.character->GetPosition());
        if (record.hasWritten && NearlyEqual(position, record.lastPosition, 0.f))
            continue;
        const EntityID owner(ownerValue);
        if (!world.IsAlive(owner))
            continue;
        if (const uint32_t version = WriteBackPose(world, owner, position, nullptr))
            record.writtenLocalVersion = version;
        record.lastPosition = position;
        record.hasWritten   = true;
    }

    JPH::BodyInterface& bodies = impl.Bodies();
    for (auto& [ownerValue, record] : impl.bodies)
    {
        if (record.type != RigidBodyType::Dynamic)
            continue;

        JPH::RVec3 jPosition;
        JPH::Quat  jRotation;
        bodies.GetPositionAndRotation(record.id, jPosition, jRotation);
        const math::Vec3 position = FromJolt(jPosition);
        const math::Quat rotation = FromJolt(jRotation).Normalized();
        if (record.hasWritten && NearlyEqual(position, record.lastPosition, 0.f) &&
            NearlyEqual(rotation, record.lastRotation, 0.f))
            continue;

        const EntityID owner(ownerValue);
        if (!world.IsAlive(owner))
            continue;

        if (const uint32_t version = WriteBackPose(world, owner, position, &rotation))
            record.writtenLocalVersion = version;

        record.lastPosition = position;
        record.lastRotation = rotation;
        record.hasWritten   = true;
    }
}

void PhysicsWorld::Reset()
{
    Impl& impl = *m_impl;
    impl.DestroyAllBodies();
    impl.warnedNested.clear();
    impl.pendingCommands.clear();
    impl.characterInput.clear();
    impl.accumulator = 0.f;
}

bool PhysicsWorld::AddForce(EntityID entity, const math::Vec3& force)
{
    return m_impl->SubmitCommand(entity, Impl::CommandKind::Force, force);
}

bool PhysicsWorld::AddImpulse(EntityID entity, const math::Vec3& impulse)
{
    return m_impl->SubmitCommand(entity, Impl::CommandKind::Impulse, impulse);
}

bool PhysicsWorld::SetLinearVelocity(EntityID entity, const math::Vec3& velocity)
{
    return m_impl->SubmitCommand(entity, Impl::CommandKind::Velocity, velocity);
}

bool PhysicsWorld::GetLinearVelocity(EntityID entity, math::Vec3& outVelocity) const
{
    if (auto it = m_impl->characters.find(entity.value); it != m_impl->characters.end())
    {
        outVelocity = FromJolt(it->second.character->GetLinearVelocity());
        return true;
    }
    const Impl::BodyRecord* record = m_impl->initialized ? m_impl->FindDynamicRecordFor(entity) : nullptr;
    if (!record)
        return false;
    outVelocity = FromJolt(m_impl->system->GetBodyInterface().GetLinearVelocity(record->id));
    return true;
}

bool PhysicsWorld::GetBodyPose(EntityID entity, math::Vec3& outPosition, math::Quat& outRotation) const
{
    const Impl::BodyRecord* record = m_impl->initialized ? m_impl->FindRecordFor(entity) : nullptr;
    if (!record)
        return false;
    JPH::RVec3 position;
    JPH::Quat  rotation;
    m_impl->system->GetBodyInterface().GetPositionAndRotation(record->id, position, rotation);
    outPosition = FromJolt(position);
    outRotation = FromJolt(rotation);
    return true;
}

std::vector<CollisionEvent> PhysicsWorld::TakeCollisionEvents()
{
    std::vector<CollisionEvent> events;
    events.swap(m_impl->collisionEvents);
    return events;
}

bool PhysicsWorld::Raycast(const math::Vec3& origin, const math::Vec3& direction, float maxDistance,
                           collision::RaycastHit& outHit, EntityID ignoreEntity) const
{
    const Impl& impl = *m_impl;
    if (!impl.initialized || !(maxDistance > 0.f))
        return false;
    const math::Vec3 dir = direction.Normalized();
    if (dir.LengthSq() < 0.5f)
        return false;

    uint32_t ignoreOwner = 0u;
    if (ignoreEntity.IsValid())
    {
        ignoreOwner = ignoreEntity.value;
        if (auto it = impl.colliderOwner.find(ignoreEntity.value); it != impl.colliderOwner.end())
            ignoreOwner = it->second;
    }

    const JPH::RRayCast ray(ToJolt(origin), ToJolt(dir * maxDistance));
    JPH::RayCastResult result;
    const IgnoreOwnerFilter filter(impl.bodyOwners, ignoreOwner);
    if (!impl.system->GetNarrowPhaseQuery().CastRay(ray, result, {}, {}, filter))
        return false;

    const JPH::RVec3 point = ray.GetPointOnRay(result.mFraction);
    outHit = collision::RaycastHit{};
    outHit.distance = result.mFraction * maxDistance;
    outHit.position = FromJolt(point);
    if (auto it = impl.bodyOwners.find(result.mBodyID.GetIndexAndSequenceNumber()); it != impl.bodyOwners.end())
        outHit.entity = EntityID(it->second);

    JPH::BodyLockRead lock(impl.system->GetBodyLockInterface(), result.mBodyID);
    if (lock.Succeeded())
        outHit.normal = FromJolt(lock.GetBody().GetWorldSpaceSurfaceNormal(result.mSubShapeID2, point));
    return true;
}

bool PhysicsWorld::SetCharacterMove(EntityID entity, const math::Vec3& horizontalVelocity)
{
    if (!m_impl->initialized || !entity.IsValid())
        return false;
    m_impl->characterInput[entity.value].move = { horizontalVelocity.x, 0.f, horizontalVelocity.z };
    return true;
}

bool PhysicsWorld::CharacterJump(EntityID entity, float speed)
{
    if (!m_impl->initialized || !entity.IsValid() || !(speed > 0.f))
        return false;
    m_impl->characterInput[entity.value].jumpSpeed = speed;
    return true;
}

bool PhysicsWorld::IsCharacterGrounded(EntityID entity) const
{
    auto it = m_impl->characters.find(entity.value);
    return it != m_impl->characters.end() &&
           it->second.character->GetGroundState() == JPH::CharacterBase::EGroundState::OnGround;
}

bool PhysicsWorld::HasCharacter(EntityID entity) const noexcept
{
    return m_impl->characters.find(entity.value) != m_impl->characters.end();
}

size_t PhysicsWorld::CharacterCount() const noexcept
{
    return m_impl->characters.size();
}

size_t PhysicsWorld::BodyCount() const noexcept
{
    return m_impl->bodies.size();
}

bool PhysicsWorld::HasBody(EntityID entity) const noexcept
{
    return m_impl->FindRecordFor(entity) != nullptr;
}

} // namespace engine::physics
