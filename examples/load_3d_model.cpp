#include "krom.h"
#include "core/Debug.hpp"
#include <algorithm>
#include <cmath>

// =============================================================================
// Eigene Komponenten
// =============================================================================

struct Spin  { float speedDeg = 60.f; };
struct Pulse { float time = 0.f; float amp = 0.3f; float base = 1.f; };

// =============================================================================
// Systeme
// =============================================================================

void SpinSystem(engine::ecs::World& world, float dt)
{
    world.View<Spin>([dt](LPENTITY id, Spin& s)
    {
        Krom::TurnEntity(id, 0.f, 0.f, s.speedDeg * dt);
    });
}

void PulseSystem(engine::ecs::World& world, float dt)
{
    world.View<Pulse>([dt](LPENTITY id, Pulse& p)
    {
        p.time += dt;
        const float scale = p.base + std::sin(p.time * 2.f) * p.amp;
        Krom::SetScale(id, scale, scale, scale);
    });
}

// =============================================================================
// Einstiegspunkt
// =============================================================================

int main()
{
    engine::Debug::ResetMinLevelForBuild();

    Krom::SetAssetRoot("assets");

    Krom::RegisterComponent<Spin>("Spin");
    Krom::RegisterComponent<Pulse>("Pulse");

    if (!Krom::Graphics(Krom::Renderer::OpenGL, 1280, 720, "Load 3D Model"))
        return 1;

    // ── Kamera (Pivot + Child fuer sauberes FPS-Feeling) ─────────────────────

    float yaw   = 0.f;
    float pitch = -5.f;

    LPENTITY camPivot = Krom::CreateEntity("CamPivot");

    // keepWorldTransform=false: Kamera bekommt lokalen Offset (0,0,0) relativ
    // zum Pivot. Pivot danach positionieren, damit Weltposition stimmt.
    LPENTITY camera = Krom::CreateCamera("MainCamera", {0.f, 0.f, 0.f}, 60.f, true);
    Krom::SetParent(camera, camPivot, false);
    Krom::SetRotationEulerDeg(camera, pitch, 0.f, 0.f);

    Krom::SetPosition(camPivot, 0.f, 0.4f, 110.f);
    Krom::SetRotationEulerDeg(camPivot, 0.f, yaw, 0.f);

    // ── Licht ────────────────────────────────────────────────────────────────

    LPENTITY light = Krom::CreateDirectionalLight();
    Krom::SetRotationEulerDeg(light, -50.f, -30.f, 0.f);

    // ── Drachen ───────────────────────────────────────────────────────────────

    LPENTITY dragon = Krom::CreateEntityFromAsset("Dragon/drache.glb");
    Krom::SetName(dragon, "Dragon");
    Krom::SetRotationEulerDeg(dragon, -90.f, -90.f, 0.f);
    Krom::SetPBR(dragon, Krom::PBR{
        .color     = Krom::Texture("Dragon/dragon.png"),
        .roughness = 0.8f,
        .metallic  = 0.f,
    });

    LPENTITY floor = Krom::GetChild(dragon, 0);
    if (Krom::IsAlive(floor))
    {
        Krom::SetPBR(floor, Krom::PBR{
            .color          = Krom::Texture("cobblestone_floor_09_diff_2k.png"),
            .normal         = "cobblestone_floor_09_nor_dx_2k.png",
            .normalStrength = 2.f,
            .roughness      = 0.8f,
            .metallic       = 0.f,
            .uvScale        = {10.f, 10.f},
        });
        Krom::SetPosition(floor, 0.f, 0.f, -25.f);
        Krom::SetScale(floor, 100.f, 100.f, 1.f);
    }

    Krom::AddComponent<Spin>(dragon, Spin{.speedDeg = 35.f});

    // ── Systeme ───────────────────────────────────────────────────────────────

    Krom::RegisterSystem(SpinSystem);

    // ── Haupt-Loop ────────────────────────────────────────────────────────────

    return Krom::Run([=](float dt) mutable
    {
        const float move = 90.f * dt;
        const float turn = 90.f * dt;

        if (Krom::KeyDown(Krom::Key::W)) Krom::MoveEntity(camPivot,  0.f,  0.f, -move);
        if (Krom::KeyDown(Krom::Key::S)) Krom::MoveEntity(camPivot,  0.f,  0.f,  move);
        if (Krom::KeyDown(Krom::Key::A)) Krom::MoveEntity(camPivot, -move, 0.f,  0.f);
        if (Krom::KeyDown(Krom::Key::D)) Krom::MoveEntity(camPivot,  move, 0.f,  0.f);
        if (Krom::KeyDown(Krom::Key::Q)) Krom::MoveEntity(camPivot,  0.f, -move, 0.f);
        if (Krom::KeyDown(Krom::Key::E)) Krom::MoveEntity(camPivot,  0.f,  move, 0.f);

        if (Krom::KeyDown(Krom::Key::Left))  { yaw += turn; Krom::SetRotationEulerDeg(camPivot, 0.f, yaw, 0.f); }
        if (Krom::KeyDown(Krom::Key::Right)) { yaw -= turn; Krom::SetRotationEulerDeg(camPivot, 0.f, yaw, 0.f); }
        if (Krom::KeyDown(Krom::Key::Up))    { pitch = std::min(pitch + turn,  89.f); Krom::SetRotationEulerDeg(camera, pitch, 0.f, 0.f); }
        if (Krom::KeyDown(Krom::Key::Down))  { pitch = std::max(pitch - turn, -89.f); Krom::SetRotationEulerDeg(camera, pitch, 0.f, 0.f); }

        if (Krom::KeyHit(Krom::Key::Escape)) Krom::Quit();
    });
}
