#include "krom.h"
#include "core/Debug.hpp"
#include <algorithm>
#include <cmath>

static constexpr float PI = 3.14159265358979323846f;

int main()
{
    engine::Debug::ResetMinLevelForBuild();
    Krom::SetAssetRoot("assets");

    if (!Krom::Graphics(Krom::Renderer::DX11, 1280, 720, "IBL Sphere"))
        return 1;

    // ── IBL Umgebung ──────────────────────────────────────────────────────────

    auto env = Krom::CreateEnvironmentFromHDR("autumn_field_puresky_2k.hdr", 1.f, true);
    Krom::SetActiveEnvironment(env);

    // ── Kamera ────────────────────────────────────────────────────────────────

    float yaw   = 0.f;
    float pitch = 0.f;

    LPENTITY camPivot = Krom::CreateEntity("CamPivot");
    LPENTITY camera   = Krom::CreateCamera("MainCamera", {0.f, 0.f, 0.f}, 60.f, true);
    Krom::SetParent(camera, camPivot, false);
    Krom::SetRotationEulerDeg(camera, pitch, 0.f, 0.f);
    Krom::SetPosition(camPivot, 0.f, 0.f, 5.f);
    Krom::SetRotationEulerDeg(camPivot, 0.f, yaw, 0.f);

    // ── Kugel (UV-Sphere) ─────────────────────────────────────────────────────
    //
    // stacks = horizontale Ringe (Nord- nach Suedpol)
    // slices = vertikale Segmente (Laengengrade)
    //
    //        +Y (Nord)
    //         |
    //   phi   |  / theta
    //         | /
    //    -----+-----  XZ-Ebene
    //
    // Position: x = sin(phi)*cos(theta), y = cos(phi), z = sin(phi)*sin(theta)
    // Normale  = normierte Position (Einheitskugel)

    const int   stacks = 24;
    const int   slices = 48;
    const float radius = 1.5f;

    LPENTITY sphere = Krom::CreateMesh("Sphere");
    {
        LPSURFACE surf = Krom::CreateSurface(sphere);

        for (int i = 0; i <= stacks; ++i)
        {
            const float phi = PI * i / stacks;
            for (int j = 0; j <= slices; ++j)
            {
                const float theta = 2.f * PI * j / slices;
                const float x = std::sin(phi) * std::cos(theta);
                const float y = std::cos(phi);
                const float z = std::sin(phi) * std::sin(theta);
                const float u = static_cast<float>(j) / slices;
                const float v = static_cast<float>(i) / stacks;
                Krom::AddVertex(surf,
                    x * radius, y * radius, z * radius,  // Position
                    u, v,                                 // UV
                    x, y, z);                             // Normale (= normierte Position)
            }
        }

        for (int i = 0; i < stacks; ++i)
        {
            for (int j = 0; j < slices; ++j)
            {
                const int v0 = i * (slices + 1) + j;
                const int v1 = v0 + 1;
                const int v2 = v0 + (slices + 1);
                const int v3 = v2 + 1;
                Krom::AddTriangle(surf, v0, v1, v2);
                Krom::AddTriangle(surf, v1, v3, v2);
            }
        }

        Krom::SetPBR(sphere, Krom::PBR{
            .color     = Krom::Texture("weisse_textur.png"),
            .roughness = 0.05f,
            .metallic  = 0.9f,
        });
    }

    // ── Haupt-Loop ────────────────────────────────────────────────────────────

    return Krom::Run([=](float dt) mutable
    {
        const float move = 5.f * dt;
        const float turn = 90.f * dt;

        if (Krom::KeyDown(Krom::Key::W)) Krom::MoveEntity(camPivot,  0.f,  0.f, -move);
        if (Krom::KeyDown(Krom::Key::S)) Krom::MoveEntity(camPivot,  0.f,  0.f,  move);
        if (Krom::KeyDown(Krom::Key::A)) Krom::MoveEntity(camPivot, -move, 0.f,  0.f);
        if (Krom::KeyDown(Krom::Key::D)) Krom::MoveEntity(camPivot,  move, 0.f,  0.f);

        if (Krom::KeyDown(Krom::Key::Left))  { yaw += turn; Krom::SetRotationEulerDeg(camPivot, 0.f, yaw, 0.f); }
        if (Krom::KeyDown(Krom::Key::Right)) { yaw -= turn; Krom::SetRotationEulerDeg(camPivot, 0.f, yaw, 0.f); }
        if (Krom::KeyDown(Krom::Key::Up))    { pitch = std::min(pitch + turn,  89.f); Krom::SetRotationEulerDeg(camera, pitch, 0.f, 0.f); }
        if (Krom::KeyDown(Krom::Key::Down))  { pitch = std::max(pitch - turn, -89.f); Krom::SetRotationEulerDeg(camera, pitch, 0.f, 0.f); }

        if (Krom::KeyHit(Krom::Key::Escape)) Krom::Quit();
    });
}
