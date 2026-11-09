// =============================================================================
// KROM Engine - tests/test_particles.cpp
// Tests fuer das Partikel-Addon (CPU-Simulation, Serialisierung, Pass, Shader).
// =============================================================================
#include "TestFramework.hpp"

#include "addons/forward/StandardFramePipeline.hpp"
#include "addons/particles/ParticleComponents.hpp"
#include "addons/particles/ParticleRenderer.hpp"
#include "addons/particles/ParticleScriptService.hpp"
#include "addons/particles/ParticleSerialization.hpp"
#include "addons/particles/ParticleSystem.hpp"
#include "addons/script/ScriptList.hpp"
#include "assets/AssetRegistry.hpp"
#include "core/Debug.hpp"
#include "ecs/Components.hpp"
#include "ecs/World.hpp"
#include "renderer/ShaderCompiler.hpp"
#include "serialization/SceneSerializer.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_set>
#include <vector>
#include <limits>

namespace engine::particles {
struct ParticleSystemTestAccess
{
    static ParticleID ExhaustGeneration(ParticleSystem& system, ParticleID id)
    {
        auto& slot = system.m_slots[id.slot];
        slot.generation = std::numeric_limits<uint32_t>::max();
        auto& particle = slot.pool->particles[slot.index];
        particle.id.generation = slot.generation;
        return particle.id;
    }
};
}

using namespace engine;
using namespace engine::particles;

namespace {

ecs::ComponentMetaRegistry MakeParticleRegistry()
{
    ecs::ComponentMetaRegistry reg;
    RegisterCoreComponents(reg);
    RegisterParticleComponents(reg);
    return reg;
}

// Emitter ohne Zufall und ohne Schwerkraft: Ergebnisse exakt vorhersagbar.
ParticleEmitterComponent MakeQuietEmitter()
{
    ParticleEmitterComponent e{};
    e.emitRate = 0.f;
    e.lifetimeRandom = 0.f;
    e.velocityRandom = math::Vec3{0.f, 0.f, 0.f};
    e.gravity = math::Vec3{0.f, 0.f, 0.f};
    return e;
}

EntityID AddEmitter(ecs::World& world, const ParticleEmitterComponent& emitter,
                    const math::Vec3& position = math::Vec3{0.f, 0.f, 0.f})
{
    const EntityID e = world.CreateEntity();
    world.Add<ParticleEmitterComponent>(e, emitter);
    WorldTransformComponent transform{};
    transform.position = position;
    world.Add<WorldTransformComponent>(e, transform);
    return e;
}

void TestEmitRateAccumulates(test::TestContext& ctx)
{
    auto reg = MakeParticleRegistry();
    ecs::World world(reg);
    ParticleEmitterComponent emitter = MakeQuietEmitter();
    emitter.emitRate = 10.f;          // 1 Partikel je 0.1 s
    emitter.particleLifetime = 100.f;
    const EntityID e = AddEmitter(world, emitter);

    ParticleSystem system;
    for (int i = 0; i < 10; ++i)
        system.Update(world, 0.05f);  // 0.5 s → 5 Partikel (Bruchteile werden aufgespart)
    CHECK_EQ(ctx, system.EmitterCount(), size_t(1));
    CHECK_EQ(ctx, system.ParticleCount(e), 5u);
}

void TestPrewarmStartsWithSimulatedParticles(test::TestContext& ctx)
{
    auto registry = MakeParticleRegistry();
    ecs::World world(registry);
    auto emitter = MakeQuietEmitter();
    emitter.emitRate = 60.f;
    emitter.duration = 0.1f;
    emitter.particleLifetime = 1.f;
    const auto cold = AddEmitter(world, emitter);
    emitter.prewarm = true;
    const auto warm = AddEmitter(world, emitter);
    emitter.playing = false;
    const auto stopped = AddEmitter(world, emitter);
    emitter.playing = true;
    emitter.looping = false;
    const auto oneShot = AddEmitter(world, emitter);

    ParticleSystem system;
    const auto manual = system.SpawnParticle(world, warm, {});
    system.Update(world, 0.f);
    CHECK_EQ(ctx, system.ParticleCount(cold), 0u);
    CHECK_EQ(ctx, system.ParticleCount(stopped), 0u);
    CHECK_EQ(ctx, system.ParticleCount(oneShot), 0u);
    const auto* particles = system.Particles(warm);
    CHECK(ctx, particles && particles->size() >= 58u && particles->size() <= 61u);
    if (!particles || particles->size() < 2u)
        return;
    CHECK(ctx, particles->front().id == manual && particles->front().age == 0.f);
    const auto previous = (*particles)[1].id;
    const auto count = system.ParticleCount(warm);
    CHECK(ctx, std::any_of(particles->begin(), particles->end(), [](const Particle& particle) {
        return particle.age > 0.9f && particle.position.y > 0.9f;
    }));

    system.Update(world, 0.f);
    CHECK_EQ(ctx, system.ParticleCount(warm), count);
    CHECK(ctx, system.IsParticleAlive(previous));

    system.Restart(warm);
    ParticleScriptService service(system, world);
    CHECK(ctx, service.Play(stopped));
    system.Update(world, 0.f);
    CHECK_EQ(ctx, system.ParticleCount(warm), count - 1u);
    CHECK_EQ(ctx, system.ParticleCount(stopped), count - 1u);
    CHECK(ctx, !system.IsParticleAlive(previous));
}

void TestPrewarmAgesBurstsBeforeFirstFrame(test::TestContext& ctx)
{
    auto registry = MakeParticleRegistry();
    ecs::World world(registry);
    auto emitter = MakeQuietEmitter();
    emitter.prewarm = true;
    emitter.burstCount = 56u;
    emitter.maxParticles = 2000u;
    emitter.duration = 0.1f;
    emitter.particleLifetime = 2.92f;
    const auto dust = AddEmitter(world, emitter);
    emitter.duration = 2.f;
    emitter.particleLifetime = 1.f;
    const auto sparse = AddEmitter(world, emitter);

    ParticleSystem system;
    system.Update(world, 0.1f);
    const auto dustCount = system.ParticleCount(dust);
    CHECK(ctx, dustCount >= 28u * emitter.burstCount);
    CHECK_EQ(ctx, system.ParticleCount(sparse), emitter.burstCount);
    for (const auto id : {dust, sparse})
    {
        const auto* particles = system.Particles(id);
        CHECK(ctx, particles && std::all_of(particles->begin(), particles->end(), [](const Particle& particle) {
            return particle.age >= 0.049f && particle.position.y >= 0.049f;
        }));
    }
    system.Update(world, 0.f);
    CHECK_EQ(ctx, system.ParticleCount(dust), dustCount);
    CHECK_EQ(ctx, system.ParticleCount(sparse), emitter.burstCount);
}

void TestBurstLifetimeAndMovement(test::TestContext& ctx)
{
    auto reg = MakeParticleRegistry();
    ecs::World world(reg);
    ParticleEmitterComponent emitter = MakeQuietEmitter();
    emitter.burstCount = 20u;
    emitter.particleLifetime = 0.5f;
    emitter.initialVelocity = math::Vec3{0.f, 2.f, 0.f};
    const EntityID e = AddEmitter(world, emitter, math::Vec3{1.f, 0.f, 0.f});

    ParticleSystem system;
    system.Update(world, 0.05f);      // Burst entsteht nach dem Bewegen: Alter 0
    CHECK_EQ(ctx, system.ParticleCount(e), 20u);
    system.Update(world, 0.05f);
    const auto* particles = system.Particles(e);
    CHECK(ctx, particles != nullptr && !particles->empty());
    if (particles && !particles->empty())
    {
        const Particle& p = particles->front();
        CHECK(ctx, std::fabs(p.position.x - 1.f) < 1e-5f);
        CHECK(ctx, std::fabs(p.position.y - 0.1f) < 1e-5f);   // 2 m/s * 0.05 s
    }

    for (int i = 0; i < 10; ++i)
        system.Update(world, 0.05f);  // Gesamtalter 0.55 s > 0.5 s
    CHECK_EQ(ctx, system.ParticleCount(e), 0u);
}

void TestSpawnShapes(test::TestContext& ctx)
{
    auto registry = MakeParticleRegistry();
    ecs::World world(registry);
    const math::Vec3 origin{2.f, 3.f, 4.f};
    const auto rotation = math::Quat::FromAxisAngleDeg(math::Vec3::Right(), 35.f);
    const auto inverse = rotation.Conjugate();
    auto emitter = MakeQuietEmitter();
    emitter.burstCount = 256u;
    emitter.emitterRadius = 1.5f;
    emitter.boxSize = {2.f, 4.f, 6.f};
    emitter.torusRadius = 2.f;
    emitter.torusTubeRadius = 0.5f;
    emitter.planeWidth = 2.f;
    emitter.planeDepth = 6.f;

    for (auto shape : {ParticleSpawnShape::Sphere, ParticleSpawnShape::Box,
                       ParticleSpawnShape::Torus, ParticleSpawnShape::Plane})
    {
        emitter.spawnShape = shape;
        const EntityID id = AddEmitter(world, emitter, origin);
        world.Get<WorldTransformComponent>(id)->rotation = rotation;
        ParticleSystem system(123u);
        system.Update(world, 0.01f);
        const auto* particles = system.Particles(id);
        CHECK(ctx, particles && particles->size() == emitter.burstCount);
        if (!particles || particles->empty()) continue;
        bool inside = true;
        bool spread = false;
        for (const auto& particle : *particles)
        {
            const auto local = inverse.Rotate(particle.position - origin);
            spread |= (particle.position - particles->front().position).LengthSq() > 0.01f;
            switch (shape)
            {
            case ParticleSpawnShape::Sphere:
                inside &= local.LengthSq() <= 2.2501f;
                break;
            case ParticleSpawnShape::Box:
                inside &= std::fabs(local.x) <= 1.0001f && std::fabs(local.y) <= 2.0001f && std::fabs(local.z) <= 3.0001f;
                break;
            case ParticleSpawnShape::Torus:
            {
                const float fromRing = std::hypot(local.x, local.z) - emitter.torusRadius;
                inside &= fromRing * fromRing + local.y * local.y <= 0.2501f;
                break;
            }
            case ParticleSpawnShape::Plane:
                inside &= std::fabs(local.x) <= 1.0001f && std::fabs(local.y) <= 0.0001f && std::fabs(local.z) <= 3.0001f;
                break;
            }
        }
        CHECK(ctx, inside);
        CHECK(ctx, spread);
        CHECK_VALID(ctx, system.SpawnParticle(world, id, origin));
        CHECK(ctx, particles->back().position == origin);
    }
}

void TestGravityAndDrag(test::TestContext& ctx)
{
    auto reg = MakeParticleRegistry();
    ecs::World world(reg);
    ParticleEmitterComponent emitter = MakeQuietEmitter();
    emitter.burstCount = 1u;
    emitter.particleLifetime = 10.f;
    emitter.initialVelocity = math::Vec3{0.f, 0.f, 0.f};
    emitter.gravity = math::Vec3{0.f, -10.f, 0.f};
    const EntityID e = AddEmitter(world, emitter);

    ParticleSystem system;
    system.Update(world, 0.1f);
    system.Update(world, 0.1f);
    const auto* particles = system.Particles(e);
    CHECK(ctx, particles && particles->size() == 1u);
    if (particles && particles->size() == 1u)
        CHECK(ctx, std::fabs((*particles)[0].velocity.y + 1.f) < 1e-5f);

    // Mit Luftwiderstand faellt die Geschwindigkeit kleiner aus.
    auto reg2 = MakeParticleRegistry();
    ecs::World world2(reg2);
    emitter.drag = 2.f;
    const EntityID e2 = AddEmitter(world2, emitter);
    ParticleSystem system2;
    system2.Update(world2, 0.1f);
    system2.Update(world2, 0.1f);
    const auto* dragged = system2.Particles(e2);
    CHECK(ctx, dragged && dragged->size() == 1u);
    if (dragged && dragged->size() == 1u)
        CHECK(ctx, (*dragged)[0].velocity.y > -1.f && (*dragged)[0].velocity.y < 0.f);
}

void TestMaxParticlesAndDuration(test::TestContext& ctx)
{
    auto reg = MakeParticleRegistry();
    ecs::World world(reg);
    ParticleEmitterComponent capped = MakeQuietEmitter();
    capped.emitRate = 1000.f;
    capped.maxParticles = 7u;
    capped.particleLifetime = 100.f;
    const EntityID a = AddEmitter(world, capped);

    ParticleEmitterComponent oneShot = MakeQuietEmitter();
    oneShot.emitRate = 10.f;
    oneShot.looping = false;
    oneShot.duration = 0.3f;
    oneShot.particleLifetime = 100.f;
    const EntityID b = AddEmitter(world, oneShot);

    ParticleSystem system;
    for (int i = 0; i < 20; ++i)
        system.Update(world, 0.05f);  // 1 s, Emission von b endet nach 0.3 s
    CHECK_EQ(ctx, system.ParticleCount(a), 7u);
    CHECK_EQ(ctx, system.ParticleCount(b), 3u);
    CHECK_EQ(ctx, system.ParticleCount(), 10u);

    // Restart: Partikel weg, die Emissionsdauer laeuft erneut.
    system.Restart(b);
    CHECK_EQ(ctx, system.ParticleCount(b), 0u);
    for (int i = 0; i < 20; ++i)
        system.Update(world, 0.05f);
    CHECK_EQ(ctx, system.ParticleCount(b), 3u);
}

void TestPausedAndRemovedEmitters(test::TestContext& ctx)
{
    auto reg = MakeParticleRegistry();
    ecs::World world(reg);
    ParticleEmitterComponent emitter = MakeQuietEmitter();
    emitter.emitRate = 100.f;
    emitter.burstCount = 4u;
    emitter.particleLifetime = 100.f;
    emitter.playing = false;
    const EntityID e = AddEmitter(world, emitter);

    ParticleSystem system;
    system.Update(world, 0.1f);
    CHECK_EQ(ctx, system.ParticleCount(e), 0u);   // pausiert: kein Burst, keine Rate

    world.Get<ParticleEmitterComponent>(e)->playing = true;
    system.Update(world, 0.1f);
    CHECK_EQ(ctx, system.ParticleCount(e), 14u);  // Burst 4 + 10

    world.DestroyEntity(e);
    system.Update(world, 0.1f);
    CHECK_EQ(ctx, system.EmitterCount(), size_t(0));
    CHECK_EQ(ctx, system.ParticleCount(), 0u);
}

void TestSelectionFilter(test::TestContext& ctx)
{
    auto reg = MakeParticleRegistry();
    ecs::World world(reg);
    ParticleEmitterComponent emitter = MakeQuietEmitter();
    emitter.emitRate = 10.f;
    emitter.particleLifetime = 100.f;
    const EntityID a = AddEmitter(world, emitter);
    const EntityID b = AddEmitter(world, emitter);

    ParticleSystem system;
    const std::unordered_set<EntityID> onlyA{a};
    for (int i = 0; i < 5; ++i)
        system.Update(world, 0.1f, &onlyA);
    CHECK_EQ(ctx, system.ParticleCount(a), 5u);
    CHECK_EQ(ctx, system.ParticleCount(b), 0u);   // nicht ausgewaehlt: laeuft nicht
    CHECK_EQ(ctx, system.EmitterCount(), size_t(1));

    // Auswahl wechselt: a wird verworfen, b startet von vorn.
    const std::unordered_set<EntityID> onlyB{b};
    system.Update(world, 0.1f, &onlyB);
    CHECK_EQ(ctx, system.ParticleCount(a), 0u);
    CHECK_EQ(ctx, system.ParticleCount(b), 1u);

    // Ohne Filter laufen alle.
    system.Update(world, 0.1f);
    CHECK_EQ(ctx, system.ParticleCount(a), 1u);
    CHECK_EQ(ctx, system.ParticleCount(b), 2u);
}

void TestScriptBurstAndEmitAt(test::TestContext& ctx)
{
    auto reg = MakeParticleRegistry();
    ecs::World world(reg);
    ParticleEmitterComponent emitter = MakeQuietEmitter();
    emitter.particleLifetime = 0.25f;
    emitter.playing = false;               // Vorlage: spielt selbst nicht
    emitter.initialVelocity = math::Vec3{0.f, 0.f, 0.f};
    const EntityID e = AddEmitter(world, emitter, math::Vec3{1.f, 2.f, 3.f});

    ParticleSystem system;
    ParticleScriptService service(system, world);

    // Burst wirkt auch auf einen pausierten Emitter, im naechsten Update.
    CHECK(ctx, service.Burst(e, 4u));
    system.Update(world, 0.05f);
    CHECK_EQ(ctx, system.ParticleCount(e), 4u);

    // EmitAt: eigener Effekt an anderer Stelle, verschwindet nach der Lebensdauer.
    CHECK(ctx, service.EmitAt(e, math::Vec3{10.f, 0.f, 0.f}, 6u));
    CHECK_EQ(ctx, system.DetachedEffectCount(), size_t(1));
    CHECK_EQ(ctx, system.ParticleCount(), 10u);
    std::vector<ParticleBillboard> alpha;
    std::vector<ParticleBillboard> additive;
    system.CollectBillboards(math::Vec3{0.f, 0.f, 0.f}, alpha, additive);
    int atTen = 0;
    for (const auto& b : alpha)
        atTen += std::fabs(b.position.x - 10.f) < 1e-4f ? 1 : 0;
    CHECK_EQ(ctx, atTen, 6);
    for (int i = 0; i < 5; ++i)
        system.Update(world, 0.1f);
    CHECK_EQ(ctx, system.DetachedEffectCount(), size_t(0));
    CHECK_EQ(ctx, system.ParticleCount(), 0u);

    // Ohne Emitter an der Entity: false, nichts passiert.
    const EntityID plain = world.CreateEntity();
    CHECK(ctx, !service.Burst(plain, 3u));
    CHECK(ctx, !service.EmitAt(plain, math::Vec3{}, 3u));
    CHECK(ctx, !service.Play(plain));
}

void TestScriptPlayStop(test::TestContext& ctx)
{
    auto reg = MakeParticleRegistry();
    ecs::World world(reg);
    ParticleEmitterComponent emitter = MakeQuietEmitter();
    emitter.emitRate = 10.f;
    emitter.burstCount = 2u;
    emitter.particleLifetime = 100.f;
    emitter.playing = false;
    const EntityID e = AddEmitter(world, emitter);

    ParticleSystem system;
    ParticleScriptService service(system, world);
    system.Update(world, 0.1f);
    CHECK_EQ(ctx, system.ParticleCount(e), 0u);
    CHECK(ctx, !service.IsPlaying(e));

    CHECK(ctx, service.Play(e));
    CHECK(ctx, service.IsPlaying(e));
    system.Update(world, 0.1f);
    CHECK_EQ(ctx, system.ParticleCount(e), 3u);   // Burst 2 + 1

    // Weicher Stopp: Partikel bleiben; harter Stopp: sofort weg.
    CHECK(ctx, service.Stop(e, false));
    system.Update(world, 0.1f);
    CHECK_EQ(ctx, system.ParticleCount(e), 3u);
    CHECK(ctx, service.Stop(e, true));
    CHECK_EQ(ctx, system.ParticleCount(e), 0u);
    CHECK(ctx, !world.Get<ParticleEmitterComponent>(e)->playing);

    // Play startet von vorn: Burst erneut.
    CHECK(ctx, service.Play(e));
    system.Update(world, 0.1f);
    CHECK_EQ(ctx, system.ParticleCount(e), 3u);
}

void TestFlipbookAndTexture(test::TestContext& ctx)
{
    auto reg = MakeParticleRegistry();
    ecs::World world(reg);
    ParticleEmitterComponent emitter = MakeQuietEmitter();
    emitter.burstCount = 1u;
    emitter.particleLifetime = 1.f;
    emitter.texturePath = "Textures/feuer.png";
    emitter.flipbookColumns = 2u;
    emitter.flipbookRows = 2u;
    AddEmitter(world, emitter);

    ParticleSystem system;
    system.Update(world, 0.f);
    std::vector<ParticleBillboard> alpha;
    std::vector<ParticleBillboard> additive;
    system.CollectBillboards(math::Vec3{}, alpha, additive);
    CHECK_EQ(ctx, alpha.size(), size_t(1));
    if (alpha.size() == 1u)
    {
        CHECK(ctx, alpha[0].texture != nullptr && *alpha[0].texture == "Textures/feuer.png");
        // Alter 0: erstes Bild oben links.
        CHECK(ctx, alpha[0].uvRect.x == 0.f && alpha[0].uvRect.y == 0.f);
        CHECK(ctx, std::fabs(alpha[0].uvRect.z - 0.5f) < 1e-6f && std::fabs(alpha[0].uvRect.w - 0.5f) < 1e-6f);
    }

    // Nach 80 % der Lebensdauer: Bild 3 (unten rechts) bei 4 Bildern.
    for (int i = 0; i < 8; ++i)
        system.Update(world, 0.1f);
    system.CollectBillboards(math::Vec3{}, alpha, additive);
    CHECK_EQ(ctx, alpha.size(), size_t(1));
    if (alpha.size() == 1u)
        CHECK(ctx, std::fabs(alpha[0].uvRect.x - 0.5f) < 1e-6f && std::fabs(alpha[0].uvRect.y - 0.5f) < 1e-6f);
}

void TestRendererBatchesAndSoftDisc(test::TestContext& ctx)
{
    const std::string fire = "Textures/feuer.png";
    const std::string smoke = "Textures/rauch.png";
    std::vector<ParticleBillboard> alpha(4);
    alpha[0].texture = &smoke;
    alpha[1].texture = &smoke;
    alpha[2].texture = nullptr;
    alpha[3].texture = &smoke;
    std::vector<ParticleBillboard> additive(2);
    additive[0].texture = &fire;
    additive[1].texture = &fire;

    ParticleRenderer renderer;
    renderer.Submit(alpha, additive);
    const auto batches = renderer.PendingBatches();
    // Sortierte Alpha-Reihenfolge bleibt: rauch | Scheibe | rauch, dann additiv feuer.
    CHECK_EQ(ctx, batches.size(), size_t(4));
    if (batches.size() == 4u)
    {
        CHECK(ctx, !batches[0].additive && batches[0].texture == smoke && batches[0].vertexCount == 12u);
        CHECK(ctx, batches[1].texture.empty() && batches[1].firstVertex == 12u);
        CHECK(ctx, batches[2].texture == smoke && batches[2].vertexCount == 6u);
        CHECK(ctx, batches[3].additive && batches[3].texture == fire && batches[3].firstVertex == 24u);
    }

    const ParticleImage disc = MakeSoftDiscImage(16u);
    CHECK_EQ(ctx, disc.rgba.size(), size_t(16u * 16u * 4u));
    const uint8_t centerAlpha = disc.rgba[(8u * 16u + 8u) * 4u + 3u];
    const uint8_t cornerAlpha = disc.rgba[3u];
    CHECK(ctx, centerAlpha > 240u && cornerAlpha == 0u);
}

void TestScriptContextParticleHelpers(test::TestContext& ctx)
{
    auto reg = MakeParticleRegistry();
    ecs::World world(reg);
    ParticleEmitterComponent emitter = MakeQuietEmitter();
    emitter.particleLifetime = 100.f;
    const EntityID e = AddEmitter(world, emitter);

    // Ohne Partikel-Dienst: neutral.
    script::ScriptServices none{};
    script::ScriptFrameContext without(0.016f, e, none);
    CHECK(ctx, !without.BurstParticles(3u));
    CHECK_EQ(ctx, without.GetParticleCount(), 0u);

    ParticleSystem system;
    ParticleScriptService service(system, world);
    script::ScriptServices services{};
    services.world = &world;
    services.particles = &service;
    script::ScriptFrameContext context(0.016f, e, services);
    CHECK(ctx, context.BurstParticles(3u));        // eigene Entity
    system.Update(world, 0.05f);
    CHECK_EQ(ctx, context.GetParticleCount(), 3u);
    CHECK(ctx, context.StopParticles(true));
    CHECK_EQ(ctx, context.GetParticleCount(), 0u);
    CHECK(ctx, !context.AreParticlesPlaying());
}

void TestFloorBounceAndRest(test::TestContext& ctx)
{
    auto reg = MakeParticleRegistry();
    ecs::World world(reg);
    ParticleEmitterComponent emitter = MakeQuietEmitter();
    emitter.burstCount = 1u;
    emitter.particleLifetime = 100.f;
    emitter.looping = false;                     // ein einzelner Burst
    emitter.startSize = 0.f;                     // Unterkante = Mittelpunkt
    emitter.endSize = 0.f;
    emitter.initialVelocity = math::Vec3{1.f, -4.f, 0.f};
    emitter.collideWithFloor = true;
    emitter.floorRelative = true;
    emitter.floorHeight = -0.5f;                 // Boden 0.5 m unter dem Emitter
    emitter.bounce = 0.5f;
    emitter.floorFriction = 0.5f;
    const EntityID e = AddEmitter(world, emitter, math::Vec3{0.f, 2.f, 0.f});

    ParticleSystem system;
    system.Update(world, 0.f);
    system.Update(world, 0.1f);                  // y = 1.6
    system.Update(world, 0.1f);                  // y = 1.2 -> unter 1.5: Aufprall
    const auto* particles = system.Particles(e);
    CHECK(ctx, particles && particles->size() == 1u);
    if (particles && particles->size() == 1u)
    {
        const Particle& p = (*particles)[0];
        CHECK(ctx, std::fabs(p.position.y - 1.5f) < 1e-5f);
        CHECK(ctx, std::fabs(p.velocity.y - 2.f) < 1e-5f);     // 4 m/s * 0.5 nach oben
        CHECK(ctx, std::fabs(p.velocity.x - 0.5f) < 1e-5f);    // Reibung halbiert
    }

    // Ohne Schwerkraft nie wieder unter den Boden; mit Schwerkraft kommt es zur Ruhe.
    world.Get<ParticleEmitterComponent>(e)->gravity = math::Vec3{0.f, -9.81f, 0.f};
    for (int i = 0; i < 60; ++i)
        system.Update(world, 0.1f);
    particles = system.Particles(e);
    CHECK(ctx, particles && particles->size() == 1u);
    if (particles && particles->size() == 1u)
    {
        CHECK(ctx, (*particles)[0].position.y >= 1.5f - 1e-5f);
        CHECK(ctx, std::fabs((*particles)[0].position.y - 1.5f) < 1e-3f);
    }
}

void TestFloorDieAndWorldHeight(test::TestContext& ctx)
{
    auto reg = MakeParticleRegistry();
    ecs::World world(reg);
    ParticleEmitterComponent emitter = MakeQuietEmitter();
    emitter.burstCount = 5u;
    emitter.particleLifetime = 100.f;
    emitter.startSize = 0.f;
    emitter.endSize = 0.f;
    emitter.initialVelocity = math::Vec3{0.f, -5.f, 0.f};
    emitter.collideWithFloor = true;
    emitter.floorRelative = false;
    emitter.floorHeight = 1.f;                   // Welt-Y = 1
    emitter.dieOnFloor = true;
    const EntityID e = AddEmitter(world, emitter, math::Vec3{0.f, 3.f, 0.f});

    ParticleSystem system;
    system.Update(world, 0.f);
    CHECK_EQ(ctx, system.ParticleCount(e), 5u);
    for (int i = 0; i < 3; ++i)
        system.Update(world, 0.1f);              // y = 1.5: noch da
    CHECK_EQ(ctx, system.ParticleCount(e), 5u);
    system.Update(world, 0.1f);                  // y = 1.0 -> nicht unter 1
    system.Update(world, 0.1f);                  // y = 0.5 -> verschwunden
    CHECK_EQ(ctx, system.ParticleCount(e), 0u);

    // EmitAt: relativer Boden bezieht sich auf die Emissionsposition.
    ParticleEmitterComponent relative = emitter;
    relative.floorRelative = true;
    relative.floorHeight = -1.f;
    system.EmitAt(relative, math::Vec3{0.f, 10.f, 0.f}, 3u);
    system.Update(world, 0.1f);                  // y = 9.5
    CHECK_EQ(ctx, system.ParticleCount(), 3u);
    system.Update(world, 0.1f);                  // y = 9.0 -> Boden 9: noch da
    system.Update(world, 0.1f);                  // y = 8.5 -> weg
    CHECK_EQ(ctx, system.ParticleCount(), 0u);
}

void TestBillboardsColorAndSort(test::TestContext& ctx)
{
    auto reg = MakeParticleRegistry();
    ecs::World world(reg);
    ParticleEmitterComponent smoke = MakeQuietEmitter();
    smoke.burstCount = 1u;
    smoke.particleLifetime = 1.f;
    smoke.startSize = 1.f;
    smoke.endSize = 3.f;
    smoke.startColor = math::Vec4{1.f, 0.5f, 0.f, 1.f};
    smoke.endColor = math::Vec4{1.f, 0.5f, 0.f, 0.f};
    smoke.intensity = 2.f;
    AddEmitter(world, smoke, math::Vec3{0.f, 0.f, 1.f});    // nah
    AddEmitter(world, smoke, math::Vec3{0.f, 0.f, 10.f});   // fern

    ParticleEmitterComponent sparks = smoke;
    sparks.blendMode = ParticleBlendMode::Additive;
    AddEmitter(world, sparks, math::Vec3{5.f, 0.f, 0.f});

    ParticleSystem system;
    system.Update(world, 0.f);
    for (int i = 0; i < 5; ++i)
        system.Update(world, 0.1f);   // Mitte der Lebensdauer (Schritte max. 0.1 s)

    std::vector<ParticleBillboard> alpha;
    std::vector<ParticleBillboard> additive;
    system.CollectBillboards(math::Vec3{0.f, 0.f, 0.f}, alpha, additive);
    CHECK_EQ(ctx, alpha.size(), size_t(2));
    CHECK_EQ(ctx, additive.size(), size_t(1));
    if (alpha.size() == 2u)
    {
        CHECK(ctx, alpha[0].position.z > alpha[1].position.z);          // hinten zuerst
        CHECK(ctx, std::fabs(alpha[0].size.x - 2.f) < 1e-5f && std::fabs(alpha[0].size.y - 2.f) < 1e-5f);
        CHECK(ctx, std::fabs(alpha[0].color.w - 0.5f) < 1e-5f);
        CHECK(ctx, std::fabs(alpha[0].color.x - 2.f) < 1e-5f);          // 1.0 sRGB = 1.0 linear, * 2
        CHECK(ctx, std::fabs(alpha[0].color.y - SrgbToLinear(0.5f) * 2.f) < 1e-5f);
    }
    CHECK(ctx, std::fabs(SrgbToLinear(0.f)) < 1e-7f && std::fabs(SrgbToLinear(1.f) - 1.f) < 1e-5f);
}

void TestRendererQuads(test::TestContext& ctx)
{
    ParticleRenderer renderer;
    renderer.SetCamera(math::Mat4::Identity(), math::Mat4::Identity());
    std::vector<ParticleBillboard> alpha(3);
    std::vector<ParticleBillboard> additive(2);
    renderer.Submit(alpha, additive);
    CHECK_EQ(ctx, renderer.PendingParticleCount(), 5u);
    renderer.Clear();
    CHECK(ctx, !renderer.HasPending());
}

void TestSerializationRoundTrip(test::TestContext& ctx)
{
    auto reg = MakeParticleRegistry();
    ecs::World world(reg);
    ParticleEmitterComponent emitter{};
    emitter.maxParticles = 321u;
    emitter.emitRate = 12.5f;
    emitter.burstCount = 9u;
    emitter.emitterRadius = 0.75f;
    emitter.spawnShape = ParticleSpawnShape::Torus;
    emitter.boxSize = {2.f, 3.f, 4.f};
    emitter.torusRadius = 2.f;
    emitter.torusTubeRadius = 0.5f;
    emitter.planeWidth = 3.f;
    emitter.planeDepth = 4.f;
    emitter.initialVelocity = math::Vec3{1.f, 2.f, 3.f};
    emitter.gravity = math::Vec3{0.f, -1.f, 0.f};
    emitter.startColor = math::Vec4{0.9f, 0.4f, 0.1f, 0.8f};
    emitter.endColor = math::Vec4{0.2f, 0.2f, 0.2f, 0.f};
    emitter.intensity = 4.f;
    emitter.blendMode = ParticleBlendMode::Additive;
    emitter.looping = false;
    emitter.prewarm = true;
    emitter.duration = 2.5f;
    emitter.texturePath = "Textures/Effekte/rauch.png";
    emitter.flipbookColumns = 4u;
    emitter.flipbookRows = 2u;
    emitter.frameAnimation = ParticleFrameAnimation::Random;
    emitter.collideWithFloor = true;
    emitter.floorHeight = -0.25f;
    emitter.floorRelative = false;
    emitter.bounce = 0.7f;
    emitter.floorFriction = 0.1f;
    emitter.dieOnFloor = true;
    const EntityID e = world.CreateEntity();
    world.Add<ParticleEmitterComponent>(e, emitter);

    serialization::SceneSerializer ser(world);
    ser.RegisterDefaultHandlers();
    RegisterParticleSerializationHandlers(ser);
    const std::string json = ser.SerializeToJson("Test");

    auto reg2 = MakeParticleRegistry();
    ecs::World world2(reg2);
    serialization::SceneDeserializer deser(world2);
    deser.RegisterDefaultHandlers();
    RegisterParticleDeserializationHandlers(deser);
    CHECK(ctx, deser.DeserializeFromJson(json).success);

    int found = 0;
    world2.ForEachAlive([&](EntityID id) {
        const auto* c = world2.Get<ParticleEmitterComponent>(id);
        if (!c) return;
        ++found;
        CHECK_EQ(ctx, c->maxParticles, 321u);
        CHECK_EQ(ctx, c->burstCount, 9u);
        CHECK(ctx, std::fabs(c->emitRate - 12.5f) < 1e-5f);
        CHECK(ctx, std::fabs(c->emitterRadius - 0.75f) < 1e-5f);
        CHECK(ctx, c->spawnShape == emitter.spawnShape && c->boxSize == emitter.boxSize);
        CHECK(ctx, c->torusRadius == emitter.torusRadius && c->torusTubeRadius == emitter.torusTubeRadius);
        CHECK(ctx, c->planeWidth == emitter.planeWidth && c->planeDepth == emitter.planeDepth);
        CHECK(ctx, std::fabs(c->initialVelocity.z - 3.f) < 1e-5f);
        CHECK(ctx, std::fabs(c->startColor.y - 0.4f) < 1e-5f && std::fabs(c->startColor.w - 0.8f) < 1e-5f);
        CHECK(ctx, std::fabs(c->endColor.w) < 1e-5f);
        CHECK(ctx, std::fabs(c->intensity - 4.f) < 1e-5f);
        CHECK(ctx, c->blendMode == ParticleBlendMode::Additive);
        CHECK(ctx, !c->looping && std::fabs(c->duration - 2.5f) < 1e-5f);
        CHECK(ctx, c->prewarm);
        CHECK_EQ(ctx, c->texturePath, std::string("Textures/Effekte/rauch.png"));
        CHECK(ctx, c->flipbookColumns == 4u && c->flipbookRows == 2u && c->frameAnimation == ParticleFrameAnimation::Random);
        CHECK(ctx, c->collideWithFloor && !c->floorRelative && c->dieOnFloor);
        CHECK(ctx, std::fabs(c->floorHeight + 0.25f) < 1e-5f && std::fabs(c->bounce - 0.7f) < 1e-5f &&
                   std::fabs(c->floorFriction - 0.1f) < 1e-5f);
    });
    CHECK_EQ(ctx, found, 1);

    std::string error;
    const auto legacy = serialization::JsonParser::Parse(R"({"emitterRadius":0.75})", error);
    ParticleEmitterComponent restored;
    ReadParticleEmitterFields(legacy, restored);
    CHECK(ctx, error.empty() && restored.spawnShape == ParticleSpawnShape::Sphere && restored.emitterRadius == 0.75f);
}

void TestParticlePassInRecipe(test::TestContext& ctx)
{
    using namespace engine::renderer;
    StandardFrameRecipeBuilder::BuildParams params{};
    params.transparentEnabled = true;
    params.debugDrawEnabled = true;

    const FrameRecipe without = StandardFrameRecipeBuilder::BuildRecipe(params);
    for (const auto& pass : without.passes)
        CHECK(ctx, pass.name != "ParticlePass");

    params.particlesEnabled = true;
    const FrameRecipe recipe = StandardFrameRecipeBuilder::BuildRecipe(params);
    CHECK_EQ(ctx, recipe.passes.size(), without.passes.size() + 1u);
    int transparent = -1, particles = -1, debugDraw = -1;
    for (size_t i = 0; i < recipe.passes.size(); ++i)
    {
        const auto& pass = recipe.passes[i];
        if (pass.name == "TransparentPass") transparent = static_cast<int>(i);
        if (pass.name == "DebugDrawPass")   debugDraw = static_cast<int>(i);
        if (pass.name == "ParticlePass")
        {
            particles = static_cast<int>(i);
            CHECK_EQ(ctx, pass.executorName, std::string(kParticleFrameExecutor));
            CHECK_EQ(ctx, pass.renderPass.targetResourceName, std::string("HDRSceneColor"));
            CHECK(ctx, !pass.renderPass.clearColor && !pass.renderPass.clearDepth);
        }
    }
    CHECK(ctx, transparent >= 0 && transparent < particles && particles < debugDraw);
    CHECK_EQ(ctx, std::string(StandardFrameExecutors::Particles), std::string(kParticleFrameExecutor));
}

void TestShadersCompile(test::TestContext& ctx)
{
    std::string error;
#ifdef _WIN32
    // D3DCompiler gehoert zu Windows: DX11 muss immer kompilieren.
    CHECK(ctx, CompileParticleShaders(assets::ShaderTargetProfile::DirectX11_SM5, &error));
#endif
    error.clear();
    if (!CompileParticleShaders(assets::ShaderTargetProfile::Vulkan_SPIRV, &error))
    {
        // DXC fehlt auf dieser Maschine: nur protokollieren.
        Debug::LogWarning("Particle shader test: Vulkan nicht kompilierbar (%s), uebersprungen", error.c_str());
    }
}

EntityID AddChildEntity(ecs::World& world, const char* name, EntityID parent)
{
    const EntityID e = world.CreateEntity();
    world.Add<NameComponent>(e, NameComponent{name});
    world.Add<WorldTransformComponent>(e);
    if (parent.IsValid())
    {
        world.Add<ParentComponent>(e, ParentComponent{parent});
        if (auto* children = world.Get<ChildrenComponent>(parent))
            children->Add(e);
        else
            world.Add<ChildrenComponent>(parent).Add(e);
    }
    return e;
}

void TestEffectFinishedAndAutoDestroy(test::TestContext& ctx)
{
    auto reg = MakeParticleRegistry();
    ecs::World world(reg);
    // Effekt: Wurzel ohne Emitter, zwei Kinder mit Einmal-Emittern.
    const EntityID root = AddChildEntity(world, "Explosion", NULL_ENTITY);
    ParticleEmitterComponent flash = MakeQuietEmitter();
    flash.looping = false;
    flash.duration = 0.f;
    flash.burstCount = 2u;
    flash.particleLifetime = 0.15f;
    const EntityID a = AddChildEntity(world, "Blitz", root);
    world.Add<ParticleEmitterComponent>(a, flash);
    ParticleEmitterComponent smoke = flash;
    smoke.particleLifetime = 0.35f;
    const EntityID b = AddChildEntity(world, "Rauch", root);
    world.Add<ParticleEmitterComponent>(b, smoke);
    world.Add<ParticleEffectAutoDestroyComponent>(root, ParticleEffectAutoDestroyComponent{});

    ParticleSystem system;
    std::vector<EntityID> finished;
    system.CollectFinishedEffects(world, finished);
    CHECK(ctx, finished.empty());                  // noch nicht simuliert
    CHECK(ctx, !system.IsFinished(a));

    system.Update(world, 0.f);
    CHECK_EQ(ctx, system.ParticleCount(), 4u);
    system.Update(world, 0.1f);
    system.Update(world, 0.1f);                    // Blitz fertig, Rauch lebt noch
    CHECK(ctx, system.IsFinished(a));
    CHECK(ctx, !system.IsFinished(b));
    system.CollectFinishedEffects(world, finished);
    CHECK(ctx, finished.empty());
    for (int i = 0; i < 3; ++i)
        system.Update(world, 0.1f);
    CHECK(ctx, system.IsFinished(b));
    system.CollectFinishedEffects(world, finished);
    CHECK(ctx, finished.size() == 1u && finished.front() == root);

    // Schleife endet nie; "Aktiv" aus ohne Partikel ist fertig.
    ParticleEmitterComponent looping = MakeQuietEmitter();
    looping.burstCount = 1u;
    looping.particleLifetime = 0.05f;
    const EntityID loop = AddChildEntity(world, "Dauerfeuer", NULL_ENTITY);
    world.Add<ParticleEmitterComponent>(loop, looping);
    ParticleEmitterComponent off = MakeQuietEmitter();
    off.playing = false;
    const EntityID idle = AddChildEntity(world, "Aus", NULL_ENTITY);
    world.Add<ParticleEmitterComponent>(idle, off);
    for (int i = 0; i < 5; ++i)
        system.Update(world, 0.1f);
    CHECK(ctx, !system.IsFinished(loop));
    CHECK(ctx, system.IsFinished(idle));
}

void TestScriptSpawnEffectAndFindChild(test::TestContext& ctx)
{
    auto reg = MakeParticleRegistry();
    ecs::World world(reg);
    ParticleSystem system;
    ParticleScriptService service(system, world);

    // Ersatz fuer die Prefab-Instanziierung der Runtime: Wurzel "Staubwolke"
    // mit Kind "Seite" und Enkel "Steinchen".
    math::Vec3 spawnedAt{};
    engine::script::PrefabInstantiateFn instantiate =
        [&](std::string_view path, const math::Vec3& position, const math::Quat&, const math::Vec3&) -> EntityID
    {
        if (path != "Prefabs/Staubwolke.prefab")
            return NULL_ENTITY;
        spawnedAt = position;
        const EntityID r = AddChildEntity(world, "Staubwolke", NULL_ENTITY);
        const EntityID side = AddChildEntity(world, "Seite", r);
        AddChildEntity(world, "Steinchen", side);
        return r;
    };
    const EntityID self = world.CreateEntity();
    script::ScriptServices services{};
    services.world = &world;
    services.particles = &service;
    services.instantiatePrefab = &instantiate;
    EntityID spawnOwner = NULL_ENTITY;
    EntityID spawnedRoot = NULL_ENTITY;
    uint32_t spawnNotifications = 0u;
    const std::function<void(EntityID, EntityID)> onSpawn = [&](EntityID owner, EntityID root) {
        spawnOwner = owner;
        spawnedRoot = root;
        ++spawnNotifications;
        // Die Runtime muss die Szene zuordnen koennen, bevor SpawnEffect
        // weitere Dienste aufruft. Auch Scripts auf Kindern behalten ihre ID.
        CHECK(ctx, world.IsAlive(root));
        CHECK(ctx, !world.Has<ParticleEffectAutoDestroyComponent>(root));
    };
    services.prefabInstantiated = &onSpawn;
    script::ScriptFrameContext context(0.016f, self, services);

    const EntityID fx = context.SpawnEffect("Prefabs/Staubwolke.prefab", math::Vec3{1.f, 2.f, 3.f});
    CHECK(ctx, fx.IsValid());
    CHECK(ctx, std::fabs(spawnedAt.y - 2.f) < 1e-6f);
    CHECK(ctx, world.Has<ParticleEffectAutoDestroyComponent>(fx));   // raeumt sich selbst auf
    CHECK(ctx, spawnOwner == self && spawnedRoot == fx);
    CHECK_EQ(ctx, spawnNotifications, 1u);
    CHECK(ctx, !context.SpawnEffect("Prefabs/Fehlt.prefab", math::Vec3{}).IsValid());
    CHECK_EQ(ctx, spawnNotifications, 1u);  // fehlgeschlagene Spawns nicht registrieren

    // FindChild: Breitensuche unter dem Parent, nicht ausserhalb.
    const EntityID other = AddChildEntity(world, "Steinchen", NULL_ENTITY);
    const EntityID stones = context.FindChild("Steinchen", fx);
    CHECK(ctx, stones.IsValid() && stones != other);
    CHECK(ctx, context.FindChild("Seite", fx).IsValid());
    CHECK(ctx, !context.FindChild("Staubwolke", fx).IsValid());
    CHECK(ctx, !context.FindChild("Seite").IsValid());               // eigene Entity hat keine Kinder

    // Ohne Partikel-Dienst: Prefab trotzdem da, aber ohne Selbst-Aufraeumen.
    script::ScriptServices plain{};
    plain.world = &world;
    plain.instantiatePrefab = &instantiate;
    script::ScriptFrameContext without(0.016f, self, plain);
    const EntityID fx2 = without.SpawnEffect("Prefabs/Staubwolke.prefab", math::Vec3{});
    CHECK(ctx, fx2.IsValid() && !world.Has<ParticleEffectAutoDestroyComponent>(fx2));
}

void TestLoopRepeatsBurst(test::TestContext& ctx)
{
    auto reg = MakeParticleRegistry();
    ecs::World world(reg);
    ParticleEmitterComponent emitter = MakeQuietEmitter();
    emitter.burstCount = 2u;
    emitter.particleLifetime = 100.f;
    emitter.looping = true;
    emitter.duration = 0.5f;
    const EntityID e = AddEmitter(world, emitter);

    ParticleSystem system;
    system.Update(world, 0.f);
    CHECK_EQ(ctx, system.ParticleCount(e), 2u);
    for (int i = 0; i < 3; ++i)
        system.Update(world, 0.1f);                 // 0.3 s: noch erster Durchgang
    CHECK_EQ(ctx, system.ParticleCount(e), 2u);
    for (int i = 0; i < 4; ++i)
        system.Update(world, 0.1f);                 // ueber 0.5 s: neuer Durchgang, Burst erneut
    CHECK_EQ(ctx, system.ParticleCount(e), 4u);

    // Ohne Schleife bleibt es bei einem Burst.
    world.Get<ParticleEmitterComponent>(e)->looping = false;
    system.Restart(e);
    for (int i = 0; i < 12; ++i)
        system.Update(world, 0.1f);
    CHECK_EQ(ctx, system.ParticleCount(e), 2u);
}

void TestSingleParticleHandles(test::TestContext& ctx)
{
    auto registry = MakeParticleRegistry();
    ecs::World world(registry);
    auto emitter = MakeQuietEmitter();
    emitter.playing = false;
    emitter.maxParticles = 2u;
    emitter.emitterRadius = 10.f;
    const auto entity = AddEmitter(world, emitter);
    ParticleSystem system;
    ParticleScriptService service(system, world);
    auto first = service.SpawnParticle(entity, {2.f, 3.f, 4.f});
    auto second = service.SpawnParticle(entity, {});
    CHECK_VALID(ctx, first);
    CHECK(ctx, system.Particles(entity)->front().position == math::Vec3(2.f, 3.f, 4.f));
    CHECK_INVALID(ctx, service.SpawnParticle(entity, {}));
    CHECK(ctx, service.RemoveParticle(first));
    CHECK(ctx, !service.IsParticleAlive(first));
    CHECK(ctx, service.SetParticlePosition(second, {1.f, 2.f, 3.f}));
    CHECK(ctx, system.Particles(entity)->front().position == math::Vec3(1.f, 2.f, 3.f));
    auto reused = service.SpawnParticle(entity, {});
    CHECK(ctx, reused.slot == first.slot && reused.generation != first.generation);
    auto retired = ParticleSystemTestAccess::ExhaustGeneration(system, reused);
    CHECK(ctx, service.RemoveParticle(retired));
    auto afterRetirement = service.SpawnParticle(entity, {});
    CHECK_NE(ctx, retired.slot, afterRetirement.slot);
    CHECK(ctx, !service.SetParticleFrame(retired, 0u));
    system.Restart(entity);
    CHECK(ctx, !service.IsParticleAlive(second) && !service.IsParticleAlive(afterRetirement));
    auto beforeClear = service.SpawnParticle(entity, {});
    system.Clear();
    auto afterClear = service.SpawnParticle(entity, {});
    CHECK(ctx, beforeClear != afterClear && !service.IsParticleAlive(beforeClear));
    ParticleSystem other;
    CHECK(ctx, !other.IsParticleAlive(afterClear));
    CHECK_NE(ctx, afterClear.system, other.SpawnParticle(world, entity, {}).system);
    std::unordered_set<EntityID> selection;
    system.Update(world, 0.f, &selection);
    CHECK(ctx, !service.IsParticleAlive(afterClear));
    auto removed = service.SpawnParticle(entity, {});
    world.Remove<ParticleEmitterComponent>(entity);
    CHECK(ctx, !service.IsParticleAlive(removed));
    system.Update(world, 0.f);
    CHECK(ctx, !system.IsParticleAlive(removed));
}

void TestSingleParticleAnimation(test::TestContext& ctx)
{
    auto registry = MakeParticleRegistry();
    ecs::World world(registry);
    auto emitter = MakeQuietEmitter();
    emitter.playing = false;
    emitter.flipbookColumns = 4u;
    emitter.particleLifetime = 4.f;
    const auto entity = AddEmitter(world, emitter);
    ParticleSystem system;
    ParticleScriptService service(system, world);
    auto particle = service.SpawnParticle(entity, {});
    CHECK(ctx, service.PlayParticleFrames(particle, 3u, 0u, 8.f, true));
    for (uint32_t frame : {2u, 1u, 0u, 3u})
    {
        system.Update(world, 0.0625f);
        system.Update(world, 0.0625f);
        CHECK_EQ(ctx, *service.GetParticleFrame(particle), frame);
    }
    CHECK(ctx, service.PlayParticleFrames(particle, 0u, 3u, 8.f));
    for (int i = 0; i < 12; ++i) system.Update(world, 0.0625f);
    CHECK_EQ(ctx, *service.GetParticleFrame(particle), 3u);
    CHECK(ctx, !service.PlayParticleFrames(particle, 0u, 4u, 8.f));
    CHECK(ctx, !service.PlayParticleFrames(particle, 0u, 3u, std::numeric_limits<float>::infinity()));
    CHECK_EQ(ctx, *service.GetParticleFrame(particle), 3u);
    CHECK(ctx, service.SetParticleFrameProgress(particle, 3u, 0u, 0.5f));
    CHECK_EQ(ctx, *service.GetParticleFrame(particle), 1u);
    CHECK(ctx, !service.SetParticleFrameProgress(particle, 0u, 3u, std::numeric_limits<float>::quiet_NaN()));
    CHECK_EQ(ctx, *service.GetParticleFrame(particle), 1u);
    CHECK(ctx, service.SetParticleFrameProgress(particle, 3u, 0u, -1.f));
    CHECK_EQ(ctx, *service.GetParticleFrame(particle), 3u);
    CHECK(ctx, service.SetParticleFrameProgress(particle, 3u, 0u, 2.f));
    CHECK_EQ(ctx, *service.GetParticleFrame(particle), 0u);
    CHECK(ctx, service.SetParticleFrame(particle, 2u));
    system.Update(world, 0.1f);
    CHECK_EQ(ctx, *service.GetParticleFrame(particle), 2u);
    CHECK(ctx, service.PlayParticleFramesOverLifetime(particle, 0u, 3u));
    CHECK(ctx, service.SetParticleAnimationPaused(particle, true));
    for (int i = 0; i < 10; ++i) system.Update(world, 0.1f);
    CHECK_EQ(ctx, *service.GetParticleFrame(particle), 0u);
    CHECK(ctx, service.SetParticleAnimationPaused(particle, false));
    system.Update(world, 0.1f);
    CHECK_EQ(ctx, *service.GetParticleFrame(particle), 0u);
    for (int i = 0; i < 40; ++i) system.Update(world, 0.1f);
    CHECK(ctx, !service.IsParticleAlive(particle));
    CHECK(ctx, !service.GetParticleFrame(particle).has_value());
}

void TestParticleLookSizeAndMotion(test::TestContext& ctx)
{
    auto registry = MakeParticleRegistry();
    ecs::World world(registry);
    auto emitter = MakeQuietEmitter();
    emitter.playing = false;
    emitter.startSize = emitter.endSize = 1.f;
    emitter.gravity = {0.f, -10.f, 0.f};
    emitter.collideWithFloor = true;
    emitter.floorRelative = false;
    emitter.dieOnFloor = true;
    const auto entity = AddEmitter(world, emitter);
    ParticleSystem system;
    ParticleScriptService service(system, world);
    const std::vector<ParticleFrame> frames{{0u, 0u, 16u, 16u, .5f, 1.f}, {16u, 0u, 32u, 32u, .5f, .5f}};
    CHECK(ctx, service.SetEmitterFrames(entity, frames));
    CHECK_EQ(ctx, service.GetEmitterFrameCount(entity), 2u);
    const auto particle = service.SpawnParticle(entity, {});
    CHECK(ctx, service.SetParticleVelocity(particle, {}));
    CHECK(ctx, service.SetParticleFrame(particle, 1u));
    CHECK(ctx, service.IsParticleAlive(particle));
    CHECK(ctx, std::fabs(system.Particles(entity)->front().position.y - 1.f) < 1e-6f);
    CHECK(ctx, service.SetParticleSize(particle, {2.f, 3.f}));
    CHECK(ctx, std::fabs(system.Particles(entity)->front().position.y - 3.f) < 1e-6f);
    CHECK(ctx, service.SetParticleMotionEnabled(particle, false));
    CHECK(ctx, service.SetParticlePosition(particle, {0.f, -5.f, 0.f}));
    CHECK(ctx, service.SetParticleVelocity(particle, {1.f, 0.f, 0.f}));
    system.Update(world, 0.1f);
    CHECK(ctx, system.Particles(entity)->front().position == math::Vec3(0.f, -5.f, 0.f));
    CHECK(ctx, system.Particles(entity)->front().velocity == math::Vec3(1.f, 0.f, 0.f));
    std::vector<ParticleBillboard> alpha, additive;
    system.CollectBillboards({}, alpha, additive);
    CHECK_EQ(ctx, alpha.size(), size_t(1));
    CHECK(ctx, alpha.front().size.x == 4.f && alpha.front().size.y == 6.f);
    CHECK(ctx, service.SetEmitterTexture(entity, "Textures/new.png"));
    CHECK(ctx, service.SetEmitterAtlasGrid(entity, 2u, 2u));
    CHECK(ctx, !service.SetEmitterAtlasGrid(entity, UINT32_MAX, 2u));
    auto next = service.SpawnParticle(entity, {0.f, 10.f, 0.f});
    CHECK(ctx, service.SetParticleFrame(next, 3u));
    CHECK(ctx, !service.SetParticleFrame(particle, 3u));
    const auto* particles = system.Particles(entity);
    CHECK(ctx, particles->front().look->frames == frames && particles->front().look->texture.empty());
    CHECK_EQ(ctx, particles->back().look->texture, std::string("Textures/new.png"));
    CHECK(ctx, service.SetParticleMotionEnabled(particle, true));
    system.Update(world, 0.1f);
    CHECK(ctx, !service.IsParticleAlive(particle) && service.IsParticleAlive(next));
}

void TestAtlasClippingGeometry(test::TestContext& ctx)
{
    auto look = std::make_shared<ParticleLook>();
    look->frames = {{80u, 10u, 40u, 40u, .5f, 1.f}};
    ParticleBillboard billboard;
    billboard.look = look;
    billboard.size = {4.f, 4.f};
    billboard.pivot = {.5f, 1.f};
    std::array<ParticleVertex, 6> vertices;
    CHECK(ctx, BuildParticleQuad(billboard, 100u, 100u, {1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, vertices));
    CHECK(ctx, vertices[0].position.x == -2.f && vertices[1].position.x == 0.f);
    CHECK(ctx, vertices[0].position.y == 0.f && vertices[2].position.y == 4.f);
    CHECK(ctx, std::fabs(vertices[0].u - .8f) < 1e-6f && vertices[1].u == 1.f);
    CHECK(ctx, !BuildParticleQuad(billboard, 50u, 100u, {1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, vertices));
    CHECK(ctx, !BuildParticleQuad(billboard, 0u, 0u, {1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, vertices));
    look->frames.clear();
    look->columns = look->rows = 1u;
    billboard.size = {1.f, 1.f};
    billboard.gridAspect = true;
    CHECK(ctx, BuildParticleQuad(billboard, 200u, 100u, {1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, vertices));
    CHECK(ctx, vertices[1].position.x - vertices[0].position.x == 2.f);
    CHECK(ctx, vertices[2].position.y - vertices[1].position.y == 1.f);
}

void TestFrameSerializationAndContext(test::TestContext& ctx)
{
    ParticleEmitterComponent emitter;
    emitter.frames = {{3u, 7u, 16u, 32u, -0.2f, 1.2f}};
    serialization::JsonWriter writer;
    writer.BeginObject();
    WriteParticleEmitterFields(writer, emitter);
    writer.EndObject();
    std::string error;
    const auto json = serialization::JsonParser::Parse(writer.GetString(), error);
    ParticleEmitterComponent restored;
    ReadParticleEmitterFields(json, restored);
    CHECK(ctx, restored == emitter);
    const auto legacy = serialization::JsonParser::Parse("{\"flipbookColumns\":2,\"flipbookRows\":2}", error);
    ReadParticleEmitterFields(legacy, restored);
    CHECK(ctx, restored.frames.empty() && restored.flipbookColumns == 2u && restored.flipbookRows == 2u);
    const auto malformed = serialization::JsonParser::Parse("{\"frames\":[{\"x\":-1,\"y\":0,\"width\":32,\"height\":16}]}", error);
    ReadParticleEmitterFields(malformed, restored);
    CHECK(ctx, restored.frames.empty());
    script::ScriptFrameContext without(.016f, NULL_ENTITY);
    CHECK(ctx, !without.SetEmitterTexture(NULL_ENTITY, "x.png"));
    CHECK(ctx, !without.SetEmitterAtlasGrid(NULL_ENTITY, 2u, 2u));
    CHECK(ctx, !without.SetEmitterFrames(NULL_ENTITY, emitter.frames));
    CHECK_EQ(ctx, without.GetEmitterFrameCount(NULL_ENTITY), 0u);
    CHECK_INVALID(ctx, without.SpawnParticle(NULL_ENTITY, {}));
    CHECK(ctx, !without.SetParticlePosition(NULL_PARTICLE, {}));
    CHECK(ctx, !without.SetParticleVelocity(NULL_PARTICLE, {}));
    CHECK(ctx, !without.SetParticleMotionEnabled(NULL_PARTICLE, false));
    CHECK(ctx, !without.SetParticleSize(NULL_PARTICLE, {1.f, 1.f}));
    CHECK(ctx, !without.SetParticleFrame(NULL_PARTICLE, 0u));
    CHECK(ctx, !without.PlayParticleFrames(NULL_PARTICLE, 0u, 1u, 8.f));
    CHECK(ctx, !without.PlayParticleFramesOverLifetime(NULL_PARTICLE, 0u, 1u));
    CHECK(ctx, !without.SetParticleFrameProgress(NULL_PARTICLE, 0u, 1u, .5f));
    CHECK(ctx, !without.SetParticleAnimationPaused(NULL_PARTICLE, true));
    CHECK(ctx, !without.GetParticleFrame(NULL_PARTICLE).has_value());
    CHECK(ctx, !without.IsParticleAlive(NULL_PARTICLE));
    CHECK(ctx, !without.RemoveParticle(NULL_PARTICLE));

    auto registry = MakeParticleRegistry();
    ecs::World world(registry);
    auto params = MakeQuietEmitter();
    params.playing = false;
    const auto entity = AddEmitter(world, params);
    ParticleSystem system;
    ParticleScriptService service(system, world);
    script::ScriptServices services{};
    services.world = &world;
    services.particles = &service;
    script::ScriptFrameContext context(.016f, entity, services);
    CHECK(ctx, context.SetEmitterTexture(entity, "Textures/sheet.png"));
    CHECK(ctx, context.SetEmitterAtlasGrid(entity, 4u, 1u));
    CHECK_EQ(ctx, context.GetEmitterFrameCount(entity), 4u);
    const auto particle = context.SpawnParticle(entity, {});
    CHECK(ctx, context.IsParticleAlive(particle));
    CHECK(ctx, context.SetParticlePosition(particle, {1.f, 2.f, 3.f}));
    CHECK(ctx, context.SetParticleVelocity(particle, {}));
    CHECK(ctx, context.SetParticleMotionEnabled(particle, false));
    CHECK(ctx, context.SetParticleSize(particle, {2.f, 1.f}));
    CHECK(ctx, context.SetParticleFrame(particle, 2u));
    CHECK_EQ(ctx, *context.GetParticleFrame(particle), 2u);
    CHECK(ctx, context.PlayParticleFrames(particle, 3u, 0u, 8.f));
    CHECK(ctx, context.SetParticleAnimationPaused(particle, true));
    CHECK(ctx, context.PlayParticleFramesOverLifetime(particle, 3u, 0u));
    CHECK(ctx, context.SetParticleFrameProgress(particle, 3u, 0u, .5f));
    CHECK_EQ(ctx, *context.GetParticleFrame(particle), 1u);
    CHECK(ctx, context.SetEmitterFrames(entity, emitter.frames));
    CHECK_EQ(ctx, context.GetEmitterFrameCount(entity), 1u);
    CHECK(ctx, context.RemoveParticle(particle));
    CHECK(ctx, !context.IsParticleAlive(particle));
}

void TestEmitterAnimationMigration(test::TestContext& ctx)
{
    std::string error;
    const auto legacy = serialization::JsonParser::Parse("{\"flipbookRandomFrame\":true,\"flipbookColumns\":4}", error);
    ParticleEmitterComponent emitter;
    ReadParticleEmitterFields(legacy, emitter);
    CHECK_EQ(ctx, emitter.frameAnimation, ParticleFrameAnimation::Random);
    CHECK(ctx, !emitter.prewarm);
    const auto both = serialization::JsonParser::Parse("{\"flipbookRandomFrame\":true,\"frameAnimation\":\"fixed\",\"frameFirst\":2}", error);
    ReadParticleEmitterFields(both, emitter);
    CHECK(ctx, emitter.frameAnimation == ParticleFrameAnimation::Fixed && emitter.frameFirst == 2u);
    emitter.frameLast = kLastParticleFrame;
    emitter.frameAnimation = ParticleFrameAnimation::Rate;
    emitter.frameFps = 8.f;
    emitter.frameLoop = false;
    emitter.frameRandomStart = true;
    emitter.clips = {{"Rise", 3u, 0u, ParticleClipTiming::Rate, 16.f, true},
                     {"Fade", 1u, kLastParticleFrame, ParticleClipTiming::OverLifetime, 12.f, false}};
    serialization::JsonWriter writer;
    writer.BeginObject();
    WriteParticleEmitterFields(writer, emitter);
    writer.EndObject();
    CHECK(ctx, writer.GetString().find("flipbookRandomFrame") == std::string::npos);
    ParticleEmitterComponent restored;
    ReadParticleEmitterFields(serialization::JsonParser::Parse(writer.GetString(), error), restored);
    CHECK(ctx, restored == emitter);
    CHECK(ctx, IsParticleClipNameValid("Aufwirbeln"));
    CHECK(ctx, !IsParticleClipNameValid("") && !IsParticleClipNameValid(std::string(65u, 'a')));
}

void TestEmitterAnimationModesAndSnapshots(test::TestContext& ctx)
{
    auto registry = MakeParticleRegistry();
    ecs::World world(registry);
    auto emitter = MakeQuietEmitter();
    emitter.playing = false;
    emitter.flipbookColumns = 4u;
    emitter.particleLifetime = 2.f;
    emitter.frameFirst = 3u;
    emitter.frameLast = 1u;
    const auto entity = AddEmitter(world, emitter);
    ParticleSystem system(77u);
    ParticleScriptService service(system, world);
    const auto overLifetime = service.SpawnParticle(entity, {});
    CHECK_EQ(ctx, *service.GetParticleFrame(overLifetime), 3u);
    for (int i = 0; i < 16; ++i) system.Update(world, .0625f);
    CHECK_EQ(ctx, *service.GetParticleFrame(overLifetime), 2u);
    auto* params = world.Get<ParticleEmitterComponent>(entity);
    params->frameAnimation = ParticleFrameAnimation::Fixed;
    params->frameFirst = kLastParticleFrame;
    const auto fixed = service.SpawnParticle(entity, {});
    CHECK_EQ(ctx, *service.GetParticleFrame(fixed), 3u);
    CHECK(ctx, service.SetEmitterAtlasGrid(entity, 2u, 1u));
    const auto smaller = service.SpawnParticle(entity, {});
    CHECK_EQ(ctx, *service.GetParticleFrame(smaller), 1u);
    CHECK_EQ(ctx, *service.GetParticleFrame(fixed), 3u);
    system.Update(world, .0625f);
    CHECK_EQ(ctx, *service.GetParticleFrame(overLifetime), 2u);
    CHECK_EQ(ctx, *service.GetParticleFrame(fixed), 3u);

    service.SetEmitterAtlasGrid(entity, 4u, 1u);
    params->frameAnimation = ParticleFrameAnimation::Rate;
    params->frameFirst = 3u;
    params->frameLast = 0u;
    params->frameFps = 8.f;
    params->frameLoop = true;
    const auto rate = service.SpawnParticle(entity, {});
    system.Update(world, .0625f);
    system.Update(world, .0625f);
    CHECK_EQ(ctx, *service.GetParticleFrame(rate), 2u);
    CHECK(ctx, service.SetParticleAnimationPaused(rate, true));
    system.Update(world, .0625f);
    system.Update(world, .0625f);
    CHECK_EQ(ctx, *service.GetParticleFrame(rate), 2u);
    params->frameAnimation = ParticleFrameAnimation::Random;
    params->frameFirst = 3u;
    params->frameLast = 1u;
    const auto random = service.SpawnParticle(entity, {});
    const auto held = *service.GetParticleFrame(random);
    CHECK(ctx, held >= 1u && held <= 3u);
    system.Update(world, .0625f);
    CHECK_EQ(ctx, *service.GetParticleFrame(random), held);

    params->frameAnimation = ParticleFrameAnimation::Rate;
    params->frameRandomStart = true;
    ParticleSystem first(123u), second(123u);
    bool varied = false;
    uint32_t initial = 0u;
    for (int i = 0; i < 16; ++i)
    {
        const auto a = first.SpawnParticle(world, entity, {});
        const auto b = second.SpawnParticle(world, entity, {});
        const uint32_t frame = *first.GetParticleFrame(a);
        CHECK_EQ(ctx, frame, *second.GetParticleFrame(b));
        if (i == 0) initial = frame;
        varied |= frame != initial;
    }
    CHECK(ctx, varied);
}

void TestNamedParticleClips(test::TestContext& ctx)
{
    auto registry = MakeParticleRegistry();
    ecs::World world(registry);
    auto emitter = MakeQuietEmitter();
    emitter.playing = false;
    emitter.flipbookColumns = 4u;
    emitter.particleLifetime = 1.f;
    emitter.clips = {{"Reverse", kLastParticleFrame, 0u, ParticleClipTiming::Rate, 8.f, true},
                     {"Fade", 1u, kLastParticleFrame, ParticleClipTiming::OverLifetime, 12.f, false},
                     {"Reverse", 0u, 0u, ParticleClipTiming::Rate, 12.f, false}};
    const auto entity = AddEmitter(world, emitter);
    ParticleSystem system;
    ParticleScriptService service(system, world);
    script::ScriptServices services{};
    services.world = &world;
    services.particles = &service;
    script::ScriptFrameContext context(.016f, entity, services);
    const auto particle = context.SpawnParticle(entity, {});
    CHECK(ctx, context.PlayParticleClip(particle, "Reverse"));
    CHECK_EQ(ctx, *context.GetParticleFrame(particle), 3u);
    system.Update(world, .0625f);
    system.Update(world, .0625f);
    CHECK_EQ(ctx, *context.GetParticleFrame(particle), 2u);
    CHECK(ctx, !context.PlayParticleClip(particle, "reverse"));
    CHECK_EQ(ctx, *context.GetParticleFrame(particle), 2u);
    auto clip = context.FindEmitterClip(entity, "Reverse");
    CHECK(ctx, clip && clip->first == 3u && clip->last == 0u);
    CHECK(ctx, !context.FindEmitterClip(entity, "missing"));
    world.Get<ParticleEmitterComponent>(entity)->clips.front().first = 0u;
    CHECK(ctx, context.FindEmitterClip(entity, "Reverse")->first == 0u);
    CHECK(ctx, context.PlayParticleClip(particle, "Reverse"));
    CHECK_EQ(ctx, *context.GetParticleFrame(particle), 3u);
    const auto newer = context.SpawnParticle(entity, {});
    CHECK(ctx, context.PlayParticleClip(newer, "Reverse"));
    CHECK_EQ(ctx, *context.GetParticleFrame(newer), 0u);
    CHECK(ctx, context.PlayParticleClip(particle, "Fade"));
    CHECK_EQ(ctx, *context.GetParticleFrame(particle), 1u);
    for (int i = 0; i < 8; ++i) system.Update(world, .0625f);
    CHECK_EQ(ctx, *context.GetParticleFrame(particle), 2u);
    script::ScriptFrameContext without(.016f, entity);
    CHECK(ctx, !without.PlayParticleClip(particle, "Reverse"));
    CHECK(ctx, !without.FindEmitterClip(entity, "Reverse"));
}

void TestSharedFrameMapping(test::TestContext& ctx)
{
    CHECK_EQ(ctx, ResolveParticleFrame(ParticleFrameAnimation::Rate, 3u, 0u, 8., true, .5, 1., 4u), 3u);
    CHECK_EQ(ctx, ResolveParticleFrame(ParticleFrameAnimation::Rate, 3u, 0u, 8., false, 1., 1., 4u), 0u);
    CHECK_EQ(ctx, ResolveParticleFrame(ParticleFrameAnimation::OverLifetime, 0u, 3u, 0., false, .5, 1., 4u), 2u);
    CHECK_EQ(ctx, ResolveParticleFrame(ParticleFrameAnimation::Fixed, kLastParticleFrame, 0u, 0., false, 0., 1., 4u), 3u);
    CHECK_EQ(ctx, ResolveParticleFrame(ParticleFrameAnimation::Random, 3u, 1u, 0., false, 0., 1., 4u, 1u), 2u);
    for (const auto mode : {ParticleFrameAnimation::OverLifetime, ParticleFrameAnimation::Rate, ParticleFrameAnimation::Fixed, ParticleFrameAnimation::Random})
    {
        auto registry = MakeParticleRegistry();
        ecs::World world(registry);
        auto emitter = MakeQuietEmitter();
        emitter.playing = false;
        emitter.flipbookColumns = 4u;
        emitter.particleLifetime = 2.f;
        emitter.frameAnimation = mode;
        emitter.frameFirst = 3u;
        emitter.frameLast = 0u;
        emitter.frameFps = 8.f;
        const auto entity = AddEmitter(world, emitter);
        ParticleSystem system(19u);
        const auto particle = system.SpawnParticle(world, entity, {});
        const auto random = system.Particles(entity)->front().randomFrameIndex;
        for (int i = 0; i < 16; ++i)
        {
            system.Update(world, .0625f);
            CHECK_EQ(ctx, *system.GetParticleFrame(particle), ResolveParticleFrame(mode, 3u, 0u, 8., true, (i + 1) * .0625, 2., 4u, random));
        }
    }
}

} // namespace

int RunParticleTests()
{
    test::TestSuite suite("Particles");
    suite
        .Add("Emit rate accumulates",          TestEmitRateAccumulates)
        .Add("Prewarm starts with simulated particles", TestPrewarmStartsWithSimulatedParticles)
        .Add("Prewarm ages bursts before the first frame", TestPrewarmAgesBurstsBeforeFirstFrame)
        .Add("Burst, lifetime and movement",   TestBurstLifetimeAndMovement)
        .Add("Spawn shapes and explicit positions", TestSpawnShapes)
        .Add("Gravity and drag",               TestGravityAndDrag)
        .Add("Max particles and duration",     TestMaxParticlesAndDuration)
        .Add("Loop repeats burst",             TestLoopRepeatsBurst)
        .Add("Paused and removed emitters",    TestPausedAndRemovedEmitters)
        .Add("Selection filter",               TestSelectionFilter)
        .Add("Floor bounce and rest",          TestFloorBounceAndRest)
        .Add("Floor die and world height",     TestFloorDieAndWorldHeight)
        .Add("Script burst and EmitAt",        TestScriptBurstAndEmitAt)
        .Add("Script play and stop",           TestScriptPlayStop)
        .Add("Script context helpers",         TestScriptContextParticleHelpers)
        .Add("Effect finished and auto destroy", TestEffectFinishedAndAutoDestroy)
        .Add("Script SpawnEffect and FindChild", TestScriptSpawnEffectAndFindChild)
        .Add("Flipbook and texture",           TestFlipbookAndTexture)
        .Add("Renderer batches and soft disc", TestRendererBatchesAndSoftDisc)
        .Add("Billboards color and sort",      TestBillboardsColorAndSort)
        .Add("Renderer builds quads",          TestRendererQuads)
        .Add("Serialization round trip",       TestSerializationRoundTrip)
        .Add("Particle pass in recipe",        TestParticlePassInRecipe)
        .Add("Shaders compile",                TestShadersCompile);
    suite.Add("Single particle handles and lifetime", TestSingleParticleHandles)
        .Add("Single particle frame animation", TestSingleParticleAnimation)
        .Add("Look snapshots, size, floor and manual motion", TestParticleLookSizeAndMotion)
        .Add("Atlas clipping preserves geometry and aspect", TestAtlasClippingGeometry)
        .Add("Frames round trip and neutral script helpers", TestFrameSerializationAndContext);
    suite.Add("Emitter animation migration and clips round trip", TestEmitterAnimationMigration)
        .Add("Emitter animation modes, snapshots and random starts", TestEmitterAnimationModesAndSnapshots)
        .Add("Named clips through script helpers", TestNamedParticleClips)
        .Add("Preview and simulation share frame mapping", TestSharedFrameMapping);
    return suite.Run();
}
