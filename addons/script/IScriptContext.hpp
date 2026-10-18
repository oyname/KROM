#pragma once
#include "addons/script/IAudioService.hpp"
#include "addons/script/IPhysicsService.hpp"
#include "collision/SceneQueries.hpp"
#include "core/Math.hpp"
#include "core/Types.hpp"
#include "ecs/Components.hpp"
#include "ecs/World.hpp"
#include "platform/IInput.hpp"
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace engine::script {

// Bezugsraum fuer Translate().
enum class Space : uint8_t { Local, World };

// Vorgemerkter Szenenwechsel; die Runtime fuehrt ihn nach dem Script-Update aus.
struct SceneLoadRequest
{
    std::string path;          // leer → index verwenden
    int         index = 0;     // 1-basiert (Projekt-Szenenreihenfolge)
    bool        additive = false;
};

// Nachricht zwischen Scripts (SendEvent/BroadcastEvent → ComponentScript::OnEvent).
// Neben dem Namen traegt sie optional eine Zahl, einen Vektor und einen Text.
struct ScriptEvent
{
    std::string name;
    EntityID    sender = NULL_ENTITY;
    EntityID    target = NULL_ENTITY;   // NULL_ENTITY = an alle Scripts (Broadcast)
    float       value  = 0.f;
    math::Vec3  vector{ 0.f, 0.f, 0.f };
    std::string text;
};

// Zeitstand der Runtime, pro Frame vor den Scripts aktualisiert (ScriptServices::clock).
// "Skaliert" heisst: mit timeScale multipliziert (0 = Pause, 0.5 = Zeitlupe).
struct ScriptClock
{
    float    deltaTime         = 0.f;          // skaliertes Frame-Delta
    float    unscaledDeltaTime = 0.f;          // echtes Frame-Delta
    float    fixedDeltaTime    = 1.f / 60.f;   // Dauer eines festen Schritts (OnFixedUpdate)
    double   time              = 0.0;          // skalierte Zeit seit Start
    double   unscaledTime      = 0.0;          // echte Zeit seit Start
    uint64_t frameCount        = 0u;
    // Gesetzt per SetTimeScale(); wirkt ab dem naechsten Frame.
    float    timeScale         = 1.f;
};

// Kontext, den jedes Script in OnStart/OnUpdate/OnFixedUpdate/OnDestroy bekommt.
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
    // In OnFixedUpdate die feste Schrittdauer, sonst das skalierte Frame-Delta.
    [[nodiscard]] virtual float    DeltaTime() const noexcept = 0;
    [[nodiscard]] virtual EntityID GetEntity() const noexcept = 0;
    // true waehrend OnFixedUpdate.
    [[nodiscard]] virtual bool     IsFixedStep() const noexcept { return false; }

    // ── Zeit ─────────────────────────────────────────────────────────────────
    // Ohne Runtime-Uhr (Tests, OnDestroy) neutral: Zeit 0, Skala 1.
    [[nodiscard]] virtual ScriptClock* GetClock() const noexcept { return nullptr; }

    [[nodiscard]] float UnscaledDeltaTime() const noexcept
    {
        const ScriptClock* clock = GetClock();
        return clock ? clock->unscaledDeltaTime : DeltaTime();
    }
    [[nodiscard]] float FixedDeltaTime() const noexcept
    {
        const ScriptClock* clock = GetClock();
        return clock ? clock->fixedDeltaTime : 1.f / 60.f;
    }
    // Sekunden seit Start (auch in OnFixedUpdate die Zeit des aktuellen Frames).
    [[nodiscard]] float Time() const noexcept
    {
        const ScriptClock* clock = GetClock();
        return clock ? static_cast<float>(clock->time) : 0.f;
    }
    [[nodiscard]] float UnscaledTime() const noexcept
    {
        const ScriptClock* clock = GetClock();
        return clock ? static_cast<float>(clock->unscaledTime) : 0.f;
    }
    [[nodiscard]] uint64_t FrameCount() const noexcept
    {
        const ScriptClock* clock = GetClock();
        return clock ? clock->frameCount : 0u;
    }
    [[nodiscard]] float GetTimeScale() const noexcept
    {
        const ScriptClock* clock = GetClock();
        return clock ? clock->timeScale : 1.f;
    }
    // 0 = Pause (OnUpdate laeuft weiter mit DeltaTime 0, Physik und OnFixedUpdate
    // stehen), 1 = normal. Begrenzt auf [0, 100]; wirkt ab dem naechsten Frame.
    void SetTimeScale(float scale) noexcept;

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

    // Kollisionssicht des aktuellen Frames (Stand: Ende des letzten Frames).
    [[nodiscard]] virtual const collision::SceneQueries* GetSceneQueries() const noexcept { return nullptr; }

    // Merkt einen Szenenwechsel vor; der letzte Aufruf im Frame gewinnt.
    virtual void RequestSceneLoad(const SceneLoadRequest& request) { (void)request; }

    // Physik (null, wenn die Runtime ohne Physik laeuft).
    [[nodiscard]] virtual IPhysicsService* GetPhysics() noexcept { return nullptr; }

    // Audio (null, wenn die Runtime ohne Audio laeuft).
    [[nodiscard]] virtual IAudioService* GetAudio() noexcept { return nullptr; }

    // Reiht ein Event zur Zustellung ein (siehe SendEvent/BroadcastEvent).
    virtual bool PostEvent(ScriptEvent event) { (void)event; return false; }

    // ── Events zwischen Scripts ──────────────────────────────────────────────
    // Zustellung im selben Frame nach allen OnUpdate-Aufrufen an OnEvent der
    // Scripts der Ziel-Entity bzw. aller Scripts (Broadcast). Events, die
    // waehrend der Zustellung entstehen, kommen im naechsten Frame an.
    bool SendEvent(EntityID target, std::string_view name, float value = 0.f,
                   const math::Vec3& vector = {0.f, 0.f, 0.f}, std::string_view text = {})
    {
        if (!target.IsValid())
            return false;
        return PostEvent({std::string(name), GetEntity(), target, value, vector, std::string(text)});
    }
    bool BroadcastEvent(std::string_view name, float value = 0.f,
                        const math::Vec3& vector = {0.f, 0.f, 0.f}, std::string_view text = {})
    {
        return PostEvent({std::string(name), GetEntity(), NULL_ENTITY, value, vector, std::string(text)});
    }

    // ── Szenen ───────────────────────────────────────────────────────────────
    // Wechselt nach dem Script-Update dieses Frames die Szene (Pfad relativ zum
    // Asset-Root oder 1-basierter Index aus der Projekt-Szenenreihenfolge).
    void LoadScene(std::string_view path) { RequestSceneLoad({std::string(path), 0, false}); }
    void LoadScene(int index)             { RequestSceneLoad({{}, index, false}); }
    void LoadSceneAdditive(std::string_view path) { RequestSceneLoad({std::string(path), 0, true}); }

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

    // Oberstes Element der Hierarchie (Entity ohne Parent) — "das Objekt",
    // zu dem z.B. ein getroffenes Child-Mesh gehoert.
    [[nodiscard]] EntityID GetRoot(EntityID entity = NULL_ENTITY);

    // ── Kollisionsabfragen ───────────────────────────────────────────────────
    // Arbeiten auf Box-Kollisionen (OBBComponent ohne Trigger) und Mesh-Bounds.
    // Ergebnis-Entities sind Hierarchie-Wurzeln (siehe GetRoot). ignoreSelf
    // blendet die eigene Hierarchie aus.
    bool Raycast(const math::Vec3& origin, const math::Vec3& direction, float maxDistance,
                 collision::RaycastHit& outHit, bool ignoreSelf = true);
    [[nodiscard]] std::vector<EntityID> OverlapSphere(const math::Vec3& center, float radius,
                                                      bool ignoreSelf = true);
    [[nodiscard]] std::vector<EntityID> OverlapBox(const math::Vec3& center, const math::Vec3& halfExtents,
                                                   bool ignoreSelf = true);

    // Wie Raycast, aber ueber die Physik gegen die exakten Kollisionsformen
    // (gedrehte Boxen, Kugeln, Kapseln, Kollisions-Kinder, Figuren). Trifft nur
    // Objekte mit Kollision, keine reinen Meshes. outHit.entity = Koerper-Entity
    // (Rigidbody bzw. statische Kollision). ignoreSelf blendet den Koerper der
    // Hierarchie-Wurzel dieses Scripts aus. false ohne Physik.
    bool PhysicsRaycast(const math::Vec3& origin, const math::Vec3& direction, float maxDistance,
                        collision::RaycastHit& outHit, bool ignoreSelf = true);

    // ── Physik ───────────────────────────────────────────────────────────────
    // Wirken auf den dynamischen Rigidbody der Entity (bzw. ihres Rigidbody-
    // Parents, wenn die Entity ein Collider-Kind ist). Impuls wirkt sofort
    // (N*s, bei 1 kg = Delta m/s). Kraft (N): in OnFixedUpdate ueber genau
    // diesen Physikschritt; ausserhalb (OnUpdate, Callbacks) als Impuls
    // Kraft * Frame-Delta, damit die Wirkung nicht von der Framerate abhaengt.
    // false / Null-Vektor ohne Physik oder ohne dynamischen Koerper.
    bool AddForce(const math::Vec3& force, EntityID entity = NULL_ENTITY);
    bool AddImpulse(const math::Vec3& impulse, EntityID entity = NULL_ENTITY);
    bool SetVelocity(const math::Vec3& velocity, EntityID entity = NULL_ENTITY);
    [[nodiscard]] math::Vec3 GetVelocity(EntityID entity = NULL_ENTITY);

    // Spielfigur (CharacterControllerComponent): horizontale Laufgeschwindigkeit
    // in Weltkoordinaten (m/s, Y ignoriert; gilt bis zum naechsten Aufruf),
    // Sprung mit Startgeschwindigkeit (nur auf dem Boden) und Bodenkontakt.
    bool SetMoveVelocity(const math::Vec3& velocity, EntityID entity = NULL_ENTITY);
    bool Jump(float speed, EntityID entity = NULL_ENTITY);
    [[nodiscard]] bool IsGrounded(EntityID entity = NULL_ENTITY);

    // ── Audio ────────────────────────────────────────────────────────────────
    // PlayAudio/StopAudio wirken auf die AudioSourceComponent der Entity.
    // One-Shots spielen eine Datei einmal ab (Pfad relativ zum Asset-Root),
    // 2D bzw. an einer Weltposition. false ohne Audio oder bei Ladefehler.
    bool PlayAudio(EntityID entity = NULL_ENTITY);
    bool StopAudio(EntityID entity = NULL_ENTITY);
    [[nodiscard]] bool IsAudioPlaying(EntityID entity = NULL_ENTITY);
    bool PlayOneShot(std::string_view clipPath, float volume = 1.f);
    bool PlayOneShotAt(std::string_view clipPath, const math::Vec3& position, float volume = 1.f);

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
    std::vector<EntityID> FilterRoots(const std::vector<EntityID>& entities, bool ignoreSelf);
};

} // namespace engine::script
