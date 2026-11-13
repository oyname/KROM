// =============================================================================
// KROM Engine - addons/gameplay/GameSimulation.cpp
// Uebernommen aus der Runtime-Schleife (src/app/KromWrapper.cpp, Run()).
// =============================================================================
#include "addons/gameplay/GameSimulation.hpp"
#include "addons/animation/AnimationSystem.hpp"
#include "addons/animation/AnimationScriptService.hpp"
#include "scene/BoundsSystem.hpp"
#include "scene/TransformSystem.hpp"
#include "addons/particles/ParticleScriptService.hpp"
#include "addons/script/ITextService.hpp"
#include "ecs/World.hpp"

#ifndef KROM_GAMEPLAY_HAS_PHYSICS
#define KROM_GAMEPLAY_HAS_PHYSICS 0
#endif
#ifndef KROM_GAMEPLAY_HAS_AUDIO
#define KROM_GAMEPLAY_HAS_AUDIO 0
#endif

#if KROM_GAMEPLAY_HAS_PHYSICS
#include "addons/physics/PhysicsWorld.hpp"
#include "addons/script/IPhysicsService.hpp"
#endif
#if KROM_GAMEPLAY_HAS_AUDIO
#include "addons/audio/AudioSystem.hpp"
#include "addons/script/IAudioService.hpp"
#endif

namespace engine::gameplay {

namespace {

// Verbindet die Script-Schnittstelle mit dem Text-Overlay.
class TextScriptService final : public script::ITextService
{
public:
    explicit TextScriptService(addons::text::TextRenderer& text) noexcept : m_text(text) {}

    void Draw(std::string_view text, float x, float y, float size, const math::Vec4& color) override
    { m_text.Draw(text, x, y, size, color); }
    math::Vec2 Measure(std::string_view text, float size) override { return m_text.Measure(text, size); }
    math::Vec2 ScreenSize() const override { return m_text.ViewportSize(); }

private:
    addons::text::TextRenderer& m_text;
};

#if KROM_GAMEPLAY_HAS_AUDIO
// Verbindet die Script-Schnittstelle mit der Wiedergabe — engine_script kennt
// das Audio-Addon nicht.
class AudioScriptService final : public script::IAudioService
{
public:
    explicit AudioScriptService(audio::AudioSystem& audio) noexcept : m_audio(audio) {}

    bool Play(EntityID entity) override { return m_audio.Play(entity); }
    bool Stop(EntityID entity) override { return m_audio.Stop(entity); }
    bool IsPlaying(EntityID entity) const override { return m_audio.IsPlaying(entity); }
    bool PlayOneShot(std::string_view clipPath, float volume) override
    { return m_audio.PlayOneShot(clipPath, volume); }
    bool PlayOneShotAt(std::string_view clipPath, const math::Vec3& position, float volume) override
    { return m_audio.PlayOneShotAt(clipPath, position, volume); }

private:
    audio::AudioSystem& m_audio;
};
#endif

#if KROM_GAMEPLAY_HAS_PHYSICS
// Verbindet die Script-Schnittstelle mit der Simulation — engine_script kennt
// das Physics-Addon nicht.
class PhysicsScriptService final : public script::IPhysicsService
{
public:
    explicit PhysicsScriptService(physics::PhysicsWorld& physics) noexcept : m_physics(physics) {}

    bool AddForce(EntityID entity, const math::Vec3& force) override
    { return m_physics.AddForce(entity, force); }
    bool AddImpulse(EntityID entity, const math::Vec3& impulse) override
    { return m_physics.AddImpulse(entity, impulse); }
    bool SetLinearVelocity(EntityID entity, const math::Vec3& velocity) override
    { return m_physics.SetLinearVelocity(entity, velocity); }
    bool GetLinearVelocity(EntityID entity, math::Vec3& outVelocity) const override
    { return m_physics.GetLinearVelocity(entity, outVelocity); }
    bool SetCharacterMove(EntityID entity, const math::Vec3& velocity) override
    { return m_physics.SetCharacterMove(entity, velocity); }
    bool CharacterJump(EntityID entity, float speed) override
    { return m_physics.CharacterJump(entity, speed); }
    bool IsCharacterGrounded(EntityID entity) const override
    { return m_physics.IsCharacterGrounded(entity); }
    bool Raycast(const math::Vec3& origin, const math::Vec3& direction, float maxDistance,
                 collision::RaycastHit& outHit, EntityID ignoreEntity) const override
    { return m_physics.Raycast(origin, direction, maxDistance, outHit, ignoreEntity); }

private:
    physics::PhysicsWorld& m_physics;
};
#endif

} // namespace

struct GameSimulation::Backends
{
#if KROM_GAMEPLAY_HAS_PHYSICS
    physics::PhysicsWorld physics;
    // Kollisions-Events des letzten Physikschritts; zugestellt in der naechsten
    // Script-Phase (dort wirken Destroy/LoadScene aus Callbacks).
    std::vector<physics::CollisionEvent> pendingCollisions;
#endif
#if KROM_GAMEPLAY_HAS_AUDIO
    audio::AudioSystem audio;
#endif
};

GameSimulation::GameSimulation()
    : m_backends(std::make_unique<Backends>())
{
}

GameSimulation::~GameSimulation()
{
    Shutdown();
}

void GameSimulation::SetProjectSettings(const ProjectSettings& settings)
{
    m_projectSettings = settings;
    m_actionStates.assign(settings.actions.size(), {});
#if KROM_GAMEPLAY_HAS_PHYSICS
    m_backends->physics.Shutdown();
    m_backends->pendingCollisions.clear();
    m_fixedStepConfigured = false;
#endif
}

bool GameSimulation::ActionDown(std::string_view name) const
{
    for (size_t i = 0; i < m_actionStates.size(); ++i)
        if (m_projectSettings.actions[i].name == name)
            return m_actionStates[i].down;
    return false;
}

bool GameSimulation::ActionHit(std::string_view name) const
{
    for (size_t i = 0; i < m_actionStates.size(); ++i)
        if (m_projectSettings.actions[i].name == name)
            return m_actionStates[i].hit;
    return false;
}

void GameSimulation::SetAssetRoot(const std::filesystem::path& assetRoot)
{
    m_assetRoot = assetRoot;
#if KROM_GAMEPLAY_HAS_AUDIO
    m_backends->audio.SetAssetRoot(assetRoot);
#endif
}

float GameSimulation::BeginFrame(float unscaledDeltaSeconds)
{
    // Spielzeit: alles Gameplay (Scripts, Physik, Animation) laeuft mit dem
    // skalierten Delta; TimeScale aus dem letzten Frame.
    m_deltaSeconds = unscaledDeltaSeconds * m_clock.timeScale;
    m_clock.unscaledDeltaTime = unscaledDeltaSeconds;
    m_clock.deltaTime         = m_deltaSeconds;
    m_clock.time             += m_deltaSeconds;
    m_clock.unscaledTime     += unscaledDeltaSeconds;
    ++m_clock.frameCount;
    // Bildschirmtext gilt pro Frame.
    m_text.Clear();
    return m_deltaSeconds;
}

float GameSimulation::RunFrame(ecs::World& world, float unscaledDeltaSeconds,
                              const platform::IInput* input, const GameSimulationHooks& hooks,
                              const GameFrameSteps& steps)
{
    const float deltaSeconds = BeginFrame(unscaledDeltaSeconds);
    if (steps.beforeScripts)
        steps.beforeScripts(deltaSeconds);

    m_animation = steps.animation;
    UpdateScripts(world, input, hooks);

    // Animation after physics write-back: both write TransformComponent, the
    // animated pose wins for animated nodes.
    if (steps.animation)
    {
        steps.animation->Update(world, deltaSeconds);
        // Clip events reach OnEvent of the animator's scripts with the next script
        // update (after OnUpdate), like collision events.
        for (const addons::animation::FiredAnimationEvent& fired : steps.animation->FiredEvents())
            m_pendingEvents.push_back({fired.event->name, fired.entity, fired.entity, fired.event->value,
                                       math::Vec3{0.f, 0.f, 0.f}, fired.event->text});
    }

    if (steps.beforeTransforms)
        steps.beforeTransforms();

    if (steps.transforms)
        steps.transforms->Update(world);
    if (steps.bounds)
        steps.bounds->Update(world);

    UpdateLate(world, hooks);
    return deltaSeconds;
}

void GameSimulation::UpdateScripts(ecs::World& world, const platform::IInput* input,
                                   const GameSimulationHooks& hooks)
{
    const float deltaSeconds = m_deltaSeconds;

    for (size_t i = 0; i < m_actionStates.size(); ++i)
    {
        auto& state = m_actionStates[i];
        const auto& binding = m_projectSettings.actions[i];
        const bool keyBound = input && binding.key != platform::Key::Unknown;
        const bool down = (keyBound && input->KeyDown(binding.key)) ||
                          (input && binding.button != platform::GamepadButton::None && input->GamepadDown(binding.button));
        state.hit = !state.down && (down || (keyBound && input->KeyHit(binding.key)));
        state.down = down;
    }
    const std::function<bool(std::string_view, bool)> action = [this](std::string_view name, bool hit) {
        return hit ? ActionHit(name) : ActionDown(name);
    };

    script::ScriptServices services{};
    services.world              = &world;
    services.input              = input;
    services.action             = &action;
    services.instantiatePrefab  = hooks.instantiatePrefab;
    services.prefabInstantiated = hooks.prefabInstantiated;
    services.pendingDestroy     = &m_pendingDestroy;
    services.sceneQueries       = &m_sceneQueries;
    services.pendingSceneLoad   = &m_pendingSceneLoad;
    services.pendingEvents      = &m_pendingEvents;
    services.clock              = &m_clock;
    services.jobs               = hooks.jobs;
    TextScriptService textService(m_text);
    services.text               = &textService;
    particles::ParticleScriptService particleService(m_particles, world);
    services.particles          = &particleService;
    std::optional<addons::animation::AnimationScriptService> animationService;
    if (m_animation)
    {
        animationService.emplace(*m_animation, world);
        services.animation = &*animationService;
    }
    m_materialInstances.CollectDestroyed(world);
    std::optional<MaterialScriptService> materialService;
    if (hooks.materials)
    {
        materialService.emplace(m_materialInstances, *hooks.materials, world);
        services.materials = &*materialService;
    }
#if KROM_GAMEPLAY_HAS_PHYSICS
    physics::PhysicsWorld& physicsWorld = m_backends->physics;
    // Vor den Scripts initialisieren, damit AddForce & Co. im ersten Frame greifen.
    if (!physicsWorld.IsInitialized())
    {
        physics::PhysicsSettings settings;
        settings.gravity = m_projectSettings.gravity;
        settings.collisionMask = m_projectSettings.collisionMask;
        physicsWorld.Initialize(settings);
    }
    PhysicsScriptService physicsService(physicsWorld);
    services.physics = &physicsService;
    if (!m_fixedStepConfigured)
        m_fixedStep.Configure(physicsWorld.FixedTimeStep(), physicsWorld.MaxSubSteps());
#endif
    m_fixedStepConfigured = true;
    m_clock.fixedDeltaTime = m_fixedStep.Step();
#if KROM_GAMEPLAY_HAS_AUDIO
    audio::AudioSystem& audioSystem = m_backends->audio;
    if (!audioSystem.IsInitialized())
    {
        audioSystem.SetAssetRoot(m_assetRoot);
        audio::AudioSettings audioSettings{};
        audioSettings.noDevice = !m_audioDevice;
        audioSystem.Initialize(audioSettings);
    }
    AudioScriptService audioService(audioSystem);
    services.audio = &audioService;
#endif

    m_scriptEntities.clear();
    world.ForEachAlive([&](EntityID id)
    {
        if (world.Has<script::ScriptList>(id))
            m_scriptEntities.push_back(id);
    });
    // Kollisionssicht fuer Raycast/Overlap — Stand: Transforms/Bounds des letzten Frames.
    if (!m_scriptEntities.empty())
        m_sceneQueries.BuildColliders(world);
    for (const EntityID id : m_scriptEntities)
    {
        if (!world.IsAlive(id))
            continue;
        if (auto* scripts = world.Get<script::ScriptList>(id))
            scripts->Update(deltaSeconds, id, services);
    }
    // Events aus OnUpdate zustellen (neu entstehende Events: naechster Frame).
    script::DispatchScriptEvents(world, services);
    m_triggers.Update(world, services);
#if KROM_GAMEPLAY_HAS_PHYSICS
    {
        const std::vector<physics::CollisionEvent> collisions = std::move(m_backends->pendingCollisions);
        m_backends->pendingCollisions.clear();
        for (const physics::CollisionEvent& event : collisions)
            script::DispatchCollision(world, event.enter, event.a, event.b, services);
    }
#endif
    // Destroy()/LoadScene() aus Scripts erst nach allen Updates ausfuehren —
    // sonst wuerden ScriptLists zerstoert, waehrend andere Scripts noch auf sie
    // zugreifen. Laeuft nach OnUpdate und nochmals nach OnFixedUpdate.
    ApplyScriptRequests(world, hooks);

    // Feste Schritte: pro Schritt OnFixedUpdate, dann ein Physikschritt.
    // Nach Scripts/Destroy/Szenenwechsel, vor dem TransformSystem: Sync liest
    // die von Scripts gesetzten Transforms, WriteBack markiert sie dirty.
    const uint32_t fixedSteps = m_fixedStep.Advance(deltaSeconds);
#if KROM_GAMEPLAY_HAS_PHYSICS
    physicsWorld.Sync(world);
#endif
    for (uint32_t step = 0; step < fixedSteps; ++step)
    {
        if (!m_scriptEntities.empty())
        {
#if KROM_GAMEPLAY_HAS_PHYSICS
            // OnFixedUpdate sieht die echte Physik-Pose, nicht die interpolierte.
            if (step > 0u || m_clock.physicsInterpolation)
                physicsWorld.WriteBack(world);
#endif
            for (const EntityID id : m_scriptEntities)
            {
                if (!world.IsAlive(id))
                    continue;
                if (auto* scripts = world.Get<script::ScriptList>(id))
                    scripts->FixedUpdate(m_clock.fixedDeltaTime, id, services);
            }
#if KROM_GAMEPLAY_HAS_PHYSICS
            physicsWorld.Sync(world);
#endif
        }
#if KROM_GAMEPLAY_HAS_PHYSICS
        physicsWorld.StepFixed(fixedSteps - step);
#endif
    }
#if KROM_GAMEPLAY_HAS_PHYSICS
    physicsWorld.WriteBack(world, m_clock.physicsInterpolation ? m_fixedStep.Alpha() : 1.f);
    {
        std::vector<physics::CollisionEvent> collisions = physicsWorld.TakeCollisionEvents();
        m_backends->pendingCollisions.insert(m_backends->pendingCollisions.end(),
                                             collisions.begin(), collisions.end());
    }
#endif
    // Events aus OnFixedUpdate kommen mit der naechsten Zustellung (naechster Frame).
    ApplyScriptRequests(world, hooks);
}

void GameSimulation::UpdateLate(ecs::World& world, const GameSimulationHooks& hooks)
{
#if KROM_GAMEPLAY_HAS_AUDIO
    // Nach dem TransformSystem: Quellen und Listener an der aktuellen Pose.
    m_backends->audio.Update(world);
#endif
    // Nach dem TransformSystem: Partikel entstehen an der aktuellen Pose.
    m_particles.Update(world, m_deltaSeconds);
    // Per SpawnEffect gestartete Effekte entfernen, wenn sie fertig sind.
    std::vector<EntityID> finishedEffects;
    m_particles.CollectFinishedEffects(world, finishedEffects);
    for (const EntityID id : finishedEffects)
        if (hooks.destroyEntity)
            hooks.destroyEntity(id);
}

void GameSimulation::ApplyScriptRequests(ecs::World& world, const GameSimulationHooks& hooks)
{
    for (const EntityID id : m_pendingDestroy)
    {
        if (!world.IsAlive(id))
            continue;
        if (hooks.destroyEntity)
            hooks.destroyEntity(id);
    }
    m_pendingDestroy.clear();
    if (m_pendingSceneLoad)
    {
        const script::SceneLoadRequest request = *m_pendingSceneLoad;
        m_pendingSceneLoad.reset();
        if (hooks.loadScene && (!request.path.empty() || request.index > 0))
            hooks.loadScene(request);
    }
}

void GameSimulation::ResetForSceneChange()
{
    m_materialInstances.Release();
    // Trigger-Kontakte der alten Szene ohne Exit-Callbacks verwerfen.
    m_triggers.Reset();
    m_pendingEvents.clear();
#if KROM_GAMEPLAY_HAS_PHYSICS
    // Koerper der alten Szene verwerfen; der naechste Sync baut neu auf.
    m_backends->physics.Reset();
    m_backends->pendingCollisions.clear();
#endif
#if KROM_GAMEPLAY_HAS_AUDIO
    m_backends->audio.StopAll();
#endif
    m_particles.Clear();
}

void GameSimulation::Shutdown()
{
    m_materialInstances.Release();
    m_triggers.Reset();
    m_pendingEvents.clear();
    m_pendingDestroy.clear();
    m_pendingSceneLoad.reset();
    m_scriptEntities.clear();
#if KROM_GAMEPLAY_HAS_PHYSICS
    m_backends->physics.Shutdown();
    m_backends->pendingCollisions.clear();
#endif
#if KROM_GAMEPLAY_HAS_AUDIO
    m_backends->audio.Shutdown();
#endif
    m_particles.Clear();
    m_text.Clear();
    m_fixedStep.Reset();
    m_fixedStepConfigured = false;
    m_clock = script::ScriptClock{};
    m_deltaSeconds = 0.f;
}

} // namespace engine::gameplay
