// Opt-in integration test of the krom.h material functions (default material,
// materials made in code, .mat files, the per-mesh cache) against the real
// runtime. Start from the repository directory; the DX11 window stays hidden.
#include "krom.h"
#include "addons/mesh_renderer/MeshRendererComponents.hpp"
#include <cstdio>

namespace {
LPMATERIAL MaterialOf(LPENTITY entity)
{
    const auto* component = Krom::GetComponent<engine::MaterialComponent>(entity);
    return component ? component->material : NULL_MATERIAL;
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
    config.width = 64;
    config.height = 64;
    if (!Krom::Graphics(config))
        return 2;

    int failures = 0;
    const auto check = [&](bool ok, const char* message) {
        std::printf("%s: %s\n", ok ? "PASS" : "FAIL", message);
        if (!ok) ++failures;
    };

    Krom::CreateCamera("Camera", {0.0f, 0.0f, 5.0f});
    Krom::CreateDirectionalLight();

    const Krom::MeshVertex vertices[3] = {
        {{-1.f, -1.f, 0.f}, {0.f, 0.f, 0.f}, {0.f, 0.f}},
        {{1.f, -1.f, 0.f}, {0.f, 0.f, 0.f}, {1.f, 0.f}},
        {{0.f, 1.f, 0.f}, {0.f, 0.f, 0.f}, {0.5f, 1.f}},
    };
    Krom::MeshData meshData{};
    meshData.vertices = vertices;
    meshData.vertexCount = 3u;
    const LPMESH mesh = Krom::CreateMeshAsset(meshData);
    check(mesh.IsValid(), "procedural mesh asset");

    // Default material: assigned by SetMesh, one per mesh.
    const LPENTITY first = Krom::CreateEntity("First");
    const LPENTITY second = Krom::CreateEntity("Second");
    check(Krom::SetMesh(first, mesh) && Krom::SetMesh(second, mesh), "two entities share the mesh");
    const LPMATERIAL defaultMaterial = MaterialOf(first);
    check(defaultMaterial.IsValid(), "SetMesh assigns the default material");
    check(MaterialOf(second) == defaultMaterial, "the default material is cached per mesh");

    // Lit material made in code: realized once per material and mesh.
    const LPMATERIAL lit = Krom::CreateLitMaterial(nullptr, {1.f, 0.f, 0.f, 1.f});
    check(lit.IsValid(), "CreateLitMaterial");
    check(Krom::SetMaterial(first, lit) && Krom::SetMaterial(second, lit), "SetMaterial with a code material");
    const LPMATERIAL litRuntime = MaterialOf(first);
    check(litRuntime.IsValid() && litRuntime != defaultMaterial, "the code material replaces the default");
    check(MaterialOf(second) == litRuntime, "the runtime material is cached per material and mesh");
    check(!Krom::SetMaterial(first, NULL_MATERIAL), "SetMaterial rejects a null material");

    // PBR made in code.
    Krom::PBR pbr{};
    pbr.color = Krom::Color(0.2f, 0.4f, 0.8f);
    pbr.roughness = 0.3f;
    check(Krom::SetPBR(first, pbr, false), "SetPBR");
    const LPMATERIAL pbrRuntime = MaterialOf(first);
    check(pbrRuntime.IsValid() && pbrRuntime != litRuntime, "SetPBR assigns a new runtime material");

    // .mat files: pbr-lit and custom shader.
    const LPMATERIAL blockAsset = Krom::LoadMaterial("Materials/Block.mat");
    check(blockAsset.IsValid(), "LoadMaterial pbr-lit");
    check(Krom::SetMaterial(first, blockAsset) && MaterialOf(first).IsValid() && MaterialOf(first) != pbrRuntime,
          "SetMaterial with a pbr-lit .mat");
    const LPMATERIAL customAsset = Krom::LoadMaterial("Materials/animated_color.mat");
    check(customAsset.IsValid(), "LoadMaterial custom");
    check(Krom::SetMaterial(second, customAsset) && MaterialOf(second).IsValid() && MaterialOf(second) != litRuntime,
          "SetMaterial with a custom-shader .mat");

    // Textures.
    const LPTEXTURE texture = Krom::LoadTexture("Textures/albedo.png");
    check(texture.IsValid(), "LoadTexture");
    check(Krom::CreateTextureMaterial(texture).IsValid(), "CreateTextureMaterial from a loaded texture");
    check(!Krom::CreateTextureMaterial(NULL_TEXTURE).IsValid(), "CreateTextureMaterial rejects an unknown texture");
    const LPENTITY third = Krom::CreateMeshEntity(meshData);
    check(third.IsValid() && MaterialOf(third).IsValid(), "CreateMeshEntity brings a material");
    const LPMATERIAL beforeTexture = MaterialOf(third);
    check(Krom::SetTexture(third, "Textures/albedo.png") && MaterialOf(third).IsValid() &&
          MaterialOf(third) != beforeTexture, "SetTexture assigns a textured material");

    // The materials have to survive real frames.
    int frame = 0;
    const int runResult = Krom::Run([&](float) {
        if (++frame == 3)
            Krom::Quit();
    });
    check(runResult == 0 && frame == 3, "three frames render with these materials");

    Krom::Shutdown();
    std::printf("%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
