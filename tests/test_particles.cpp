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

#include <cmath>
#include <string>
#include <unordered_set>
#include <vector>

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
        CHECK(ctx, std::fabs(alpha[0].size - 2.f) < 1e-5f);
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
    emitter.initialVelocity = math::Vec3{1.f, 2.f, 3.f};
    emitter.gravity = math::Vec3{0.f, -1.f, 0.f};
    emitter.startColor = math::Vec4{0.9f, 0.4f, 0.1f, 0.8f};
    emitter.endColor = math::Vec4{0.2f, 0.2f, 0.2f, 0.f};
    emitter.intensity = 4.f;
    emitter.blendMode = ParticleBlendMode::Additive;
    emitter.looping = false;
    emitter.duration = 2.5f;
    emitter.texturePath = "Textures/Effekte/rauch.png";
    emitter.flipbookColumns = 4u;
    emitter.flipbookRows = 2u;
    emitter.flipbookRandomFrame = true;
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
        CHECK(ctx, std::fabs(c->initialVelocity.z - 3.f) < 1e-5f);
        CHECK(ctx, std::fabs(c->startColor.y - 0.4f) < 1e-5f && std::fabs(c->startColor.w - 0.8f) < 1e-5f);
        CHECK(ctx, std::fabs(c->endColor.w) < 1e-5f);
        CHECK(ctx, std::fabs(c->intensity - 4.f) < 1e-5f);
        CHECK(ctx, c->blendMode == ParticleBlendMode::Additive);
        CHECK(ctx, !c->looping && std::fabs(c->duration - 2.5f) < 1e-5f);
        CHECK_EQ(ctx, c->texturePath, std::string("Textures/Effekte/rauch.png"));
        CHECK(ctx, c->flipbookColumns == 4u && c->flipbookRows == 2u && c->flipbookRandomFrame);
        CHECK(ctx, c->collideWithFloor && !c->floorRelative && c->dieOnFloor);
        CHECK(ctx, std::fabs(c->floorHeight + 0.25f) < 1e-5f && std::fabs(c->bounce - 0.7f) < 1e-5f &&
                   std::fabs(c->floorFriction - 0.1f) < 1e-5f);
    });
    CHECK_EQ(ctx, found, 1);
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

} // namespace

int RunParticleTests()
{
    test::TestSuite suite("Particles");
    suite
        .Add("Emit rate accumulates",          TestEmitRateAccumulates)
        .Add("Burst, lifetime and movement",   TestBurstLifetimeAndMovement)
        .Add("Gravity and drag",               TestGravityAndDrag)
        .Add("Max particles and duration",     TestMaxParticlesAndDuration)
        .Add("Paused and removed emitters",    TestPausedAndRemovedEmitters)
        .Add("Selection filter",               TestSelectionFilter)
        .Add("Floor bounce and rest",          TestFloorBounceAndRest)
        .Add("Floor die and world height",     TestFloorDieAndWorldHeight)
        .Add("Script burst and EmitAt",        TestScriptBurstAndEmitAt)
        .Add("Script play and stop",           TestScriptPlayStop)
        .Add("Script context helpers",         TestScriptContextParticleHelpers)
        .Add("Flipbook and texture",           TestFlipbookAndTexture)
        .Add("Renderer batches and soft disc", TestRendererBatchesAndSoftDisc)
        .Add("Billboards color and sort",      TestBillboardsColorAndSort)
        .Add("Renderer builds quads",          TestRendererQuads)
        .Add("Serialization round trip",       TestSerializationRoundTrip)
        .Add("Particle pass in recipe",        TestParticlePassInRecipe)
        .Add("Shaders compile",                TestShadersCompile);
    return suite.Run();
}
