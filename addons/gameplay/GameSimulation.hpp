#pragma once
// =============================================================================
// KROM Engine - addons/gameplay/GameSimulation.hpp
// Gemeinsame Spielschleife ohne Fenster und Rendering: Spielzeit, Scripts,
// Events, Trigger, Physik (feste Schritte), Audio und Partikel. Die Runtime
// (krom.h) und Play im Editor benutzen dieselbe Instanz-Logik, damit sich ein
// Spiel im Editor wie im Export verhaelt.
//
// Ablauf pro Frame:
//   BeginFrame(dt)            Spielzeit fortschreiben, Bildschirmtext leeren
//   UpdateScripts(world, ...) OnUpdate, Events, Trigger, Kollisionen,
//                             OnFixedUpdate + Physik, Destroy/LoadScene
//   (Aufrufer: Animation, TransformSystem, BoundsSystem)
//   UpdateLate(world, ...)    Audio und Partikel an der aktuellen Pose
// =============================================================================
#include "addons/particles/ParticleSystem.hpp"
#include "addons/script/ScriptList.hpp"
#include "addons/script/ScriptTriggers.hpp"
#include "addons/text/TextRenderer.hpp"
#include "collision/SceneQueries.hpp"
#include "core/FixedTimestep.hpp"
#include <filesystem>
#include <functional>
#include <memory>
#include <vector>

namespace engine::platform { class IInput; }

namespace engine::gameplay {

// Was je nach Aufrufer verschieden ist (Szenenverwaltung der Runtime bzw. des Editors).
struct GameSimulationHooks
{
    // Prefab-Spawn aus Scripts; null = nicht verfuegbar.
    const script::PrefabInstantiateFn* instantiatePrefab = nullptr;
    // Nach erfolgreichem Spawn (Zuordnung der neuen Root zur Szene des Aufrufers).
    const std::function<void(EntityID owner, EntityID root)>* prefabInstantiated = nullptr;
    // Zerstoert ein Objekt samt Kindern (Destroy aus Scripts, fertige Effekte).
    std::function<void(EntityID)> destroyEntity;
    // Szenenwechsel aus Scripts (LoadScene); leer = ignoriert.
    std::function<void(const script::SceneLoadRequest&)> loadScene;
};

class GameSimulation
{
public:
    GameSimulation();
    ~GameSimulation();
    GameSimulation(const GameSimulation&) = delete;
    GameSimulation& operator=(const GameSimulation&) = delete;

    // Vor dem ersten Frame: Basis fuer Audio-Clips, Ausgabe an/aus.
    void SetAssetRoot(const std::filesystem::path& assetRoot);
    void SetAudioDevice(bool enabled) noexcept { m_audioDevice = enabled; }

    // Spielzeit um das ungeskalierte Delta fortschreiben (TimeScale aus dem
    // letzten Frame) und den Bildschirmtext leeren. Liefert das skalierte Delta.
    float BeginFrame(float unscaledDeltaSeconds);

    void UpdateScripts(ecs::World& world, const platform::IInput* input, const GameSimulationHooks& hooks);

    // Nach dem TransformSystem: Audio-Quellen/Listener und Partikel an der aktuellen Pose.
    void UpdateLate(ecs::World& world, const GameSimulationHooks& hooks);

    // Nicht-additiver Szenenwechsel: Trigger, Events, Physikkoerper, Klaenge und
    // Partikel der alten Szene verwerfen.
    void ResetForSceneChange();

    // Physik und Audio beenden, Spielzeit zuruecksetzen (Ende von Play im Editor).
    void Shutdown();

    [[nodiscard]] script::ScriptClock& Clock() noexcept { return m_clock; }
    [[nodiscard]] addons::text::TextRenderer& Text() noexcept { return m_text; }
    [[nodiscard]] particles::ParticleSystem& Particles() noexcept { return m_particles; }
    [[nodiscard]] float DeltaSeconds() const noexcept { return m_deltaSeconds; }

private:
    struct Backends;   // Physik/Audio (optional gebaut, siehe CMake)

    void ApplyScriptRequests(ecs::World& world, const GameSimulationHooks& hooks);

    std::unique_ptr<Backends>             m_backends;
    script::ScriptClock                   m_clock;
    FixedTimestep                         m_fixedStep;
    bool                                  m_fixedStepConfigured = false;
    bool                                  m_audioDevice = true;
    std::filesystem::path                 m_assetRoot;
    float                                 m_deltaSeconds = 0.f;
    collision::SceneQueries               m_sceneQueries;
    script::ScriptTriggerSystem           m_triggers;
    std::vector<script::ScriptEvent>      m_pendingEvents;
    std::vector<EntityID>                 m_pendingDestroy;
    std::optional<script::SceneLoadRequest> m_pendingSceneLoad;
    std::vector<EntityID>                 m_scriptEntities;
    addons::text::TextRenderer            m_text;
    particles::ParticleSystem             m_particles;
};

} // namespace engine::gameplay
