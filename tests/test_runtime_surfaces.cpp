// Opt-in integration test of the krom.h surface API against the real runtime.
// Start from the repository directory; the DX11 window stays hidden.
#include "krom.h"
#include <cmath>
#include <cstdio>

namespace {
bool Near(float a, float b)
{
    return std::fabs(a - b) <= 1.0e-4f;
}
}

int main()
{
    const auto sourceRoot = std::filesystem::path(__FILE__).parent_path().parent_path();
    Krom::SetAssetRoot(sourceRoot / "projects/DemoProject/Assets");
    Krom::GraphicsConfig config{};
    config.backend = Krom::Renderer::DX11;
    config.visible = false;
    config.audioDevice = false;
    config.enableForwardPlus = false;
    config.enableGtao = false;
    config.width = 64;
    config.height = 64;
    if (!Krom::Graphics(config))
        return 2;

    int failures = 0;
    const auto check = [&](bool ok, const char* message) {
        std::printf("%s: %s\n", ok ? "PASS" : "FAIL", message);
        if (!ok) ++failures;
    };

    // Invalid inputs.
    check(!Krom::CreateSurface(NULL_LPENTITY).IsValid(), "no surface for null entity");
    const LPENTITY plain = Krom::CreateEntity("NoMesh");
    check(!Krom::CreateSurface(plain).IsValid(), "no surface for entity without mesh");
    check(Krom::AddVertex(NULL_SURFACE, 0.f, 0.f, 0.f) == -1, "AddVertex on null surface fails");
    check(!Krom::AddTriangle(NULL_SURFACE, 0, 1, 2), "AddTriangle on null surface fails");
    check(Krom::CountVertices(NULL_SURFACE) == 0 && Krom::CountTriangles(NULL_SURFACE) == 0,
          "null surface has no vertices or triangles");

    // Two surfaces on one mesh get separate ids and separate submeshes.
    const LPENTITY entity = Krom::CreateMesh("SurfaceMesh");
    const LPSURFACE surface = Krom::CreateSurface(entity);
    const LPSURFACE second = Krom::CreateSurface(entity);
    check(surface.IsValid() && second.IsValid() && surface != second, "two surfaces with distinct ids");

    check(Krom::AddVertex(surface, -1.f, -1.f, 0.f) == 0, "first vertex index 0");
    check(Krom::AddVertex(surface, 1.f, -1.f, 0.f, 1.f, 0.f) == 1, "second vertex index 1");
    check(Krom::AddVertex(surface, 0.f, 1.f, 0.f, 0.5f, 1.f, 0.f, 0.f, 1.f) == 2, "vertex with normal index 2");
    check(Krom::CountVertices(surface) == 3, "three vertices");
    check(Krom::CountVertices(second) == 0, "second surface still empty");
    check(Krom::HasComponent<engine::BoundsComponent>(entity), "touching a surface adds BoundsComponent");

    check(Krom::AddTriangle(surface, 0, 1, 2), "valid triangle accepted");
    check(!Krom::AddTriangle(surface, 0, 1, 3), "out-of-range index rejected");
    check(!Krom::AddTriangle(surface, -1, 1, 2), "negative index rejected");
    check(Krom::CountTriangles(surface) == 1, "one triangle");

    check(Krom::VertexNormal(surface, 0, 0.f, 0.f, 1.f), "VertexNormal on valid index");
    check(!Krom::VertexNormal(surface, 3, 0.f, 0.f, 1.f), "VertexNormal out of range rejected");
    check(Krom::VertexTexCoords(surface, 0, 0.f, 0.f), "VertexTexCoords on valid index");
    check(!Krom::VertexTexCoords(surface, -1, 0.f, 0.f), "VertexTexCoords negative index rejected");

    int frame = 0;
    const int runResult = Krom::Run([&](float) {
        ++frame;
        if (frame == 2)
        {
            engine::math::Vec3 center{};
            engine::math::Vec3 extents{};
            float sphere = 0.f;
            const bool hasBounds = Krom::GetWorldBounds(entity, center, extents, sphere);
            check(hasBounds && Near(center.x, 0.f) && Near(center.y, 0.f) &&
                  Near(extents.x, 1.f) && Near(extents.y, 1.f) && Near(extents.z, 0.f),
                  "world bounds follow surface vertices after a frame");

            // Changes after the first upload still go through.
            check(Krom::AddVertex(surface, 2.f, 2.f, 0.f) == 3, "vertex added after rendering");
            check(Krom::AddTriangle(surface, 1, 3, 2), "triangle added after rendering");
        }
        else if (frame == 3)
        {
            check(Krom::CountTriangles(surface) == 2, "two triangles after rendering");
            Krom::DestroyEntity(entity);
            check(Krom::CountVertices(surface) == 0 && Krom::AddVertex(surface, 0.f, 0.f, 0.f) == -1,
                  "destroying the entity removes its surfaces");
            check(Krom::CountVertices(second) == 0 && !Krom::AddTriangle(second, 0, 1, 2),
                  "second surface removed as well");
            Krom::Quit();
        }
    });
    Krom::Shutdown();
    return failures == 0 && runResult == 0 ? 0 : 1;
}
