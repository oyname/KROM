#pragma once
// =============================================================================
// KROM Engine - addons/gameplay/GameSimulation.hpp
// Gemeinsame Spielschleife ohne Fenster und Rendering: Spielzeit, Scripts,
// Events, Trigger, Physik (feste Schritte), Audio und Partikel. Die Runtime
// (krom.h) und Play im Editor benutzen dieselbe Instanz-Logik, damit sich ein
// Spiel im Editor wie im Export verhaelt.
//
// Frame order (single source of truth: RunFrame):
//   BeginFrame(dt)            advance game time, clear screen text
//   steps.beforeScripts       caller hook (runtime: tick callback, user systems)
//   UpdateScripts(world, ...) OnUpdate, events, triggers, collisions,
//                             OnFixedUpdate + physics, Destroy/LoadScene
//   AnimationSystem           node/skeleton animation (optional)
//   steps.beforeTransforms    caller hook (local bounds, editor sync)
//   TransformSystem, BoundsSystem
//   UpdateLate(world, ...)    audio and particles at the current pose
// Runtime (krom.h) and editor play both call RunFrame, so the order cannot drift.
// =============================================================================
#include "addons/gameplay/MaterialParameterInstances.hpp"
#include "addons/particles/ParticleSystem.hpp"
#include "addons/script/ScriptList.hpp"
#include "addons/script/ScriptTriggers.hpp"
#include "addons/text/TextRenderer.hpp"
#include "collision/SceneQueries.hpp"
#include "core/FixedTimestep.hpp"
#include "core/ProjectSettings.hpp"
#include <filesystem>
#include <functional>
#include <memory>
#include <vector>

namespace engine::platform { class IInput; }
namespace engine { class TransformSystem; class BoundsSystem; }
namespace engine::addons::animation { class AnimationSystem; }

namespace engine::gameplay {

// Systems and caller-specific steps for RunFrame. RunFrame fixes where each
// step runs; the caller only decides what happens inside the hooks.
struct GameFrameSteps
{
    TransformSystem* transforms = nullptr;                    // required
    BoundsSystem*    bounds     = nullptr;                    // required
    addons::animation::AnimationSystem* animation = nullptr;  // optional
    // After BeginFrame, before scripts. Receives the scaled delta.
    std::function<void(float)> beforeScripts;
    // After scripts, physics and animation, before the TransformSystem.
    std::function<void()> beforeTransforms;
};

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
    // Fuer SetMaterialParameter der Scripts; null = nicht verfuegbar.
    renderer::MaterialSystem* materials = nullptr;
    // Worker pool of the engine for IScriptContext::GetJobSystem(); null = not available.
    jobs::JobSystem* jobs = nullptr;
    // For IScriptContext::GetRenderSettings(); null = not available.
    script::IRenderSettingsService* renderSettings = nullptr;
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
    void SetProjectSettings(const ProjectSettings& settings);
    [[nodiscard]] bool ActionDown(std::string_view name) const;
    [[nodiscard]] bool ActionHit(std::string_view name) const;

    // One full simulation frame in the fixed order described at the top of this
    // file. Returns the scaled delta. Prefer this over calling the steps below
    // one by one.
    float RunFrame(ecs::World& world, float unscaledDeltaSeconds, const platform::IInput* input,
                   const GameSimulationHooks& hooks, const GameFrameSteps& steps);

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
    ProjectSettings                       m_projectSettings;
    struct ActionState { bool down = false; bool hit = false; };
    std::vector<ActionState>              m_actionStates;
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
    MaterialParameterInstances            m_materialInstances;
    // Animation system of the current frame (RunFrame); scripts reach controllers through it.
    addons::animation::AnimationSystem*   m_animation = nullptr;
};

} // namespace engine::gameplay
