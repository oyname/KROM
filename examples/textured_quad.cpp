#include "krom.h"
#include "core/Debug.hpp"

int main()
{
    engine::Debug::ResetMinLevelForBuild();
    Krom::SetAssetRoot("assets");

    if (!Krom::Graphics(Krom::Renderer::OpenGL, 1280, 720, "Textured Quad"))
        return 1;

    LPENTITY camera = Krom::CreateCamera("MainCamera", {0.f, 0.f, 3.f}, 60.f, true);

    LPENTITY light = Krom::CreateDirectionalLight();
    Krom::SetRotationEulerDeg(light, -45.f, -30.f, 0.f);


    LPENTITY quad = Krom::CreateMesh("Quad");
    {
        LPSURFACE surf = Krom::CreateSurface(quad);

        int v0 = Krom::AddVertex(surf, -1.f,  1.f, 0.f,  0.f, 0.f,  0.f, 0.f, 1.f);
        int v1 = Krom::AddVertex(surf,  1.f,  1.f, 0.f,  1.f, 0.f,  0.f, 0.f, 1.f);
        int v2 = Krom::AddVertex(surf, -1.f, -1.f, 0.f,  0.f, 1.f,  0.f, 0.f, 1.f);
        int v3 = Krom::AddVertex(surf,  1.f, -1.f, 0.f,  1.f, 1.f,  0.f, 0.f, 1.f);

        Krom::AddTriangle(surf, v0, v2, v1);
        Krom::AddTriangle(surf, v1, v2, v3);

        Krom::SetPBR(quad, Krom::PBR{
            .color     = Krom::Texture("krom.bmp"),
            .roughness = 0.8f,
        });
    }

    return Krom::Run([=](float dt) mutable
    {
        Krom::TurnEntity(quad, 0.f, 45.f * dt, 0.f);
        if (Krom::KeyHit(Krom::Key::Escape)) Krom::Quit();
    });
}
