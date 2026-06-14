#include "krom.h"
#include "core/Debug.hpp"

// Hilfsfunktion: fuegt eine Seite (2 Dreiecke) zur Surface hinzu.
// v0=oben-links, v1=oben-rechts, v2=unten-links, v3=unten-rechts
static void AddFace(LPSURFACE surf,
                    float x0, float y0, float z0,
                    float x1, float y1, float z1,
                    float x2, float y2, float z2,
                    float x3, float y3, float z3,
                    float nx, float ny, float nz)
{
    int v0 = Krom::AddVertex(surf, x0,y0,z0,  0.f,0.f,  nx,ny,nz);
    int v1 = Krom::AddVertex(surf, x1,y1,z1,  1.f,0.f,  nx,ny,nz);
    int v2 = Krom::AddVertex(surf, x2,y2,z2,  0.f,1.f,  nx,ny,nz);
    int v3 = Krom::AddVertex(surf, x3,y3,z3,  1.f,1.f,  nx,ny,nz);
    Krom::AddTriangle(surf, v0, v2, v1);
    Krom::AddTriangle(surf, v1, v2, v3);
}

int main()
{
    engine::Debug::ResetMinLevelForBuild();
    Krom::SetAssetRoot("assets");

    if (!Krom::Graphics(Krom::Renderer::OpenGL, 1280, 720, "Textured Cube"))
        return 1;

    LPENTITY camera = Krom::CreateCamera("MainCamera", {0.f, 0.5f, 4.f}, 60.f, true);
    Krom::SetRotationEulerDeg(camera, -10.f, 0.f, 0.f);

    LPENTITY light = Krom::CreateDirectionalLight();
    Krom::SetRotationEulerDeg(light, -45.f, -30.f, 0.f);

    // Wuerfel: 6 Seiten, jede Seite = 2 Dreiecke, eigene Normalen
    LPENTITY cube = Krom::CreateMesh("Cube");
    {
        LPSURFACE surf = Krom::CreateSurface(cube);

        AddFace(surf,  -1, 1, 1,   1, 1, 1,  -1,-1, 1,   1,-1, 1,   0, 0, 1); // Front  +Z
        AddFace(surf,   1, 1,-1,  -1, 1,-1,   1,-1,-1,  -1,-1,-1,   0, 0,-1); // Back   -Z
        AddFace(surf,  -1, 1,-1,  -1, 1, 1,  -1,-1,-1,  -1,-1, 1,  -1, 0, 0); // Links  -X
        AddFace(surf,   1, 1, 1,   1, 1,-1,   1,-1, 1,   1,-1,-1,   1, 0, 0); // Rechts +X
        AddFace(surf,  -1, 1,-1,   1, 1,-1,  -1, 1, 1,   1, 1, 1,   0, 1, 0); // Oben   +Y
        AddFace(surf,  -1,-1, 1,   1,-1, 1,  -1,-1,-1,   1,-1,-1,   0,-1, 0); // Unten  -Y

        Krom::SetPBR(cube, Krom::PBR{
            .color     = Krom::Texture("krom.bmp"),
            .roughness = 0.6f,
        });
    }

    float rotX = 0.f;
    float rotY = 0.f;

    return Krom::Run([=](float dt) mutable
    {
        rotX += 30.f * dt;
        rotY += 45.f * dt;
        Krom::SetRotationEulerDeg(cube, rotX, rotY, 0.f);

        if (Krom::KeyHit(Krom::Key::Escape)) Krom::Quit();
    });
}
