#pragma once
#include "core/Math.hpp"
#include "core/Types.hpp"
#include "ecs/Components.hpp"
#include "ecs/World.hpp"
#include "platform/IInput.hpp"
#include <string_view>
#include <utility>

namespace engine::script {

// Bezugsraum fuer Translate().
enum class Space : uint8_t { Local, World };

// Kontext, den jedes Script in OnStart/OnUpdate/OnDestroy bekommt.
//
// Alle Entity-Parameter sind optional: NULL_ENTITY (Default) meint die eigene Entity.
// Transform-Helfer arbeiten auf TransformComponent (lokaler Raum) und markieren
// die Komponente selbst als dirty — Scripts muessen das nicht mehr von Hand tun.
//
// In OnDestroy (und in Tests ohne Runtime) koennen World und Input fehlen;
// die Helfer liefern dann neutrale Werte (nullptr, false, Null-Vektor).
class IScriptContext
{
public:
    virtual ~IScriptContext() = default;

    // ── Frame ────────────────────────────────────────────────────────────────
    [[nodiscard]] virtual float    DeltaTime() const noexcept = 0;
    [[nodiscard]] virtual EntityID GetEntity() const noexcept = 0;

    // ── Dienste ──────────────────────────────────────────────────────────────
    [[nodiscard]] virtual ecs::World*             GetWorld() noexcept { return nullptr; }
    [[nodiscard]] virtual const platform::IInput* GetInput() const noexcept { return nullptr; }

    [[nodiscard]] virtual EntityID InstantiatePrefab(
        std::string_view prefabAssetPath,
        const math::Vec3& position = {0.f, 0.f, 0.f},
        const math::Quat& rotation = math::Quat::Identity(),
        const math::Vec3& scale = {1.f, 1.f, 1.f}) = 0;

    // Zerstoert die Entity (inkl. Kinder) am Ende des Script-Updates dieses Frames.
    // Sicher aufrufbar waehrend OnUpdate, auch fuer die eigene Entity.
    virtual void Destroy(EntityID entity = NULL_ENTITY) { (void)entity; }

    // ── Komponenten ──────────────────────────────────────────────────────────
    template<typename T>
    [[nodiscard]] T* Get(EntityID entity = NULL_ENTITY) noexcept
    {
        ecs::World* world = GetWorld();
        const EntityID id = Resolve(entity);
        return (world && world->IsAlive(id)) ? world->Get<T>(id) : nullptr;
    }

    template<typename T>
    [[nodiscard]] bool Has(EntityID entity = NULL_ENTITY) noexcept
    {
        ecs::World* world = GetWorld();
        const EntityID id = Resolve(entity);
        return world && world->IsAlive(id) && world->Has<T>(id);
    }

    // Fuegt die Komponente hinzu oder gibt die vorhandene zurueck.
    // nullptr nur, wenn keine World verfuegbar oder die Entity tot ist.
    template<typename T, typename... Args>
    T* Add(EntityID entity, Args&&... args)
    {
        ecs::World* world = GetWorld();
        const EntityID id = Resolve(entity);
        if (!world || !world->IsAlive(id))
            return nullptr;
        if (T* existing = world->Get<T>(id))
            return existing;
        return &world->Add<T>(id, std::forward<Args>(args)...);
    }

    // Erste lebende Entity mit diesem NameComponent-Namen, sonst NULL_ENTITY.
    [[nodiscard]] EntityID FindEntity(std::string_view name);

    // ── Transform (lokaler Raum) ─────────────────────────────────────────────
    [[nodiscard]] math::Vec3 GetPosition(EntityID entity = NULL_ENTITY);
    void SetPosition(const math::Vec3& position, EntityID entity = NULL_ENTITY);
    // Space::Local verschiebt entlang der eigenen Achsen (z.B. Forward()).
    void Translate(const math::Vec3& delta, Space space = Space::World, EntityID entity = NULL_ENTITY);

    [[nodiscard]] math::Quat GetRotation(EntityID entity = NULL_ENTITY);
    void SetRotation(const math::Quat& rotation, EntityID entity = NULL_ENTITY);
    void SetRotationEulerDeg(float pitch, float yaw, float roll, EntityID entity = NULL_ENTITY);
    // Dreht relativ zur aktuellen Ausrichtung — um die eigenen Achsen (Local)
    // oder um die Weltachsen (World, z.B. Yaw um die globale Y-Achse).
    void RotateEulerDeg(float pitch, float yaw, float roll,
                        Space space = Space::Local, EntityID entity = NULL_ENTITY);

    [[nodiscard]] math::Vec3 GetScale(EntityID entity = NULL_ENTITY);
    void SetScale(const math::Vec3& scale, EntityID entity = NULL_ENTITY);

    // Richtungsvektoren aus der lokalen Rotation (Forward = -Z).
    [[nodiscard]] math::Vec3 Forward(EntityID entity = NULL_ENTITY);
    [[nodiscard]] math::Vec3 Right(EntityID entity = NULL_ENTITY);
    [[nodiscard]] math::Vec3 Up(EntityID entity = NULL_ENTITY);

    // Weltposition aus dem letzten TransformSystem-Update (ein Frame alt,
    // falls die Entity in diesem Frame bewegt wurde).
    [[nodiscard]] math::Vec3 GetWorldPosition(EntityID entity = NULL_ENTITY);

    // ── Input ────────────────────────────────────────────────────────────────
    [[nodiscard]] bool KeyDown(platform::Key key) const;
    [[nodiscard]] bool KeyHit(platform::Key key) const;
    [[nodiscard]] bool KeyReleased(platform::Key key) const;
    [[nodiscard]] bool MouseButtonDown(platform::MouseButton button) const;
    [[nodiscard]] bool MouseButtonHit(platform::MouseButton button) const;
    [[nodiscard]] math::Vec2 MouseDelta() const;
    [[nodiscard]] float MouseScroll() const;
    // -1 / 0 / +1 aus zwei Tasten, z.B. Axis(Key::S, Key::W).
    [[nodiscard]] float Axis(platform::Key negative, platform::Key positive) const;

private:
    [[nodiscard]] EntityID Resolve(EntityID entity) const noexcept
    {
        return entity.IsValid() ? entity : GetEntity();
    }

    TransformComponent* EditTransform(EntityID entity);
};

} // namespace engine::script
