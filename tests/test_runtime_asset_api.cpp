// Opt-in integration test of the krom.h asset functions against the real
// runtime: LoadMesh / LoadTexture / LoadMaterial, materials made through krom.h,
// mesh and material assignment (SetMesh, SetMaterialRecursive, SetTexture,
// SetPBR, *FromAsset), procedural meshes (CreateMeshAsset, CreateMeshEntity,
// CreateMesh) and the model import (CreateEntityFromAsset, PrintAssetTree)
// with models of the DemoProject. Start from the repository directory; the
// DX11 window stays hidden. The checks document the current behaviour of the
// wrapper, oddities included (marked "current behaviour").
#include "krom.h"
#include "KromAssetApi.hpp"
#include "addons/animation/AnimationComponents.hpp"
#include "addons/mesh_renderer/MeshRendererComponents.hpp"
#include "core/Debug.hpp"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace {

using engine::math::Vec3;

bool Near(float a, float b, float tolerance = 1.0e-3f)
{
    return std::fabs(a - b) <= tolerance;
}

bool NearVec(const Vec3& a, const Vec3& b, float tolerance = 1.0e-3f)
{
    return Near(a.x, b.x, tolerance) && Near(a.y, b.y, tolerance) && Near(a.z, b.z, tolerance);
}

LPMATERIAL MaterialOf(LPENTITY entity)
{
    const auto* component = Krom::GetComponent<engine::MaterialComponent>(entity);
    return component ? component->material : NULL_MATERIAL;
}

LPMESH MeshOf(LPENTITY entity)
{
    const auto* component = Krom::GetComponent<engine::MeshComponent>(entity);
    return component ? component->mesh : NULL_MESH;
}

const engine::BoundsComponent* BoundsOf(LPENTITY entity)
{
    return Krom::GetComponent<engine::BoundsComponent>(entity);
}

// The entity and all its descendants, depth first, in child order.
void CollectHierarchy(LPENTITY entity, std::vector<LPENTITY>& out)
{
    if (!Krom::IsAlive(entity))
        return;
    out.push_back(entity);
    for (int i = 0, n = Krom::CountChildren(entity); i < n; ++i)
        CollectHierarchy(Krom::GetChild(entity, i), out);
}

std::vector<LPENTITY> Hierarchy(LPENTITY root)
{
    std::vector<LPENTITY> entities;
    CollectHierarchy(root, entities);
    return entities;
}

int Depth(LPENTITY entity)
{
    int depth = 0;
    for (const auto* parent = Krom::GetComponent<engine::ParentComponent>(entity);
         parent && parent->parent.IsValid() && depth < 64;
         parent = Krom::GetComponent<engine::ParentComponent>(parent->parent))
        ++depth;
    return depth;
}

void DumpHierarchy(const char* label, LPENTITY root)
{
    for (const LPENTITY entity : Hierarchy(root))
    {
        const auto* skin = Krom::GetComponent<engine::SkinComponent>(entity);
        const auto* player = Krom::GetComponent<engine::AnimationPlayerComponent>(entity);
        std::printf("INFO: %s depth=%d name='%s' children=%d mesh=%d material=%d bounds=%d skin=%d anim=%d\n",
                    label, Depth(entity), Krom::GetName(entity), Krom::CountChildren(entity),
                    MeshOf(entity).IsValid() ? 1 : 0, MaterialOf(entity).IsValid() ? 1 : 0,
                    BoundsOf(entity) ? 1 : 0, skin ? (skin->skeleton.IsValid() ? 1 : -1) : 0,
                    player ? (player->clip.IsValid() ? 1 : -1) : 0);
    }
}

int CountWith(const std::vector<LPENTITY>& entities, bool (*predicate)(LPENTITY))
{
    int count = 0;
    for (const LPENTITY entity : entities)
        if (predicate(entity))
            ++count;
    return count;
}

bool HasMesh(LPENTITY entity) { return MeshOf(entity).IsValid(); }

// Every mesh entity has a valid material and local bounds with a size.
bool MeshEntityComplete(LPENTITY entity)
{
    if (!HasMesh(entity))
        return true;
    const auto* bounds = BoundsOf(entity);
    return MaterialOf(entity).IsValid() && bounds &&
           (bounds->extentsLocal.x > 0.0f || bounds->extentsLocal.y > 0.0f || bounds->extentsLocal.z > 0.0f);
}

bool HasSkeleton(LPENTITY entity)
{
    const auto* skin = Krom::GetComponent<engine::SkinComponent>(entity);
    return skin && skin->skeleton.IsValid();
}

bool HasPlayingClip(LPENTITY entity)
{
    const auto* player = Krom::GetComponent<engine::AnimationPlayerComponent>(entity);
    return player && player->clip.IsValid() && player->playing && player->looping;
}

// Triangle spanning [-1, 1] in x and y at z = 0, normals zero.
const Krom::MeshVertex kTriangle[3] = {
    {{-1.f, -1.f, 0.f}, {0.f, 0.f, 0.f}, {0.f, 0.f}},
    {{ 1.f, -1.f, 0.f}, {0.f, 0.f, 0.f}, {1.f, 0.f}},
    {{ 0.f,  1.f, 0.f}, {0.f, 0.f, 0.f}, {0.5f, 1.f}},
};

Krom::MeshData TriangleData(const char* name)
{
    Krom::MeshData data{};
    data.name = name;
    data.vertices = kTriangle;
    data.vertexCount = 3u;
    return data;
}

// Same triangle, normals (0, 0, 2): pointing the right way but not unit length.
const Krom::MeshVertex kLongNormalTriangle[3] = {
    {{-1.f, -1.f, 0.f}, {0.f, 0.f, 2.f}, {0.f, 0.f}},
    {{ 1.f, -1.f, 0.f}, {0.f, 0.f, 2.f}, {1.f, 0.f}},
    {{ 0.f,  1.f, 0.f}, {0.f, 0.f, 2.f}, {0.5f, 1.f}},
};

// CPU data of a mesh in the wrapper's asset registry (wrapper internal).
const engine::assets::MeshAsset* MeshAssetOf(LPMESH mesh)
{
    return mesh.IsValid() ? Krom::internal::GetWrapperAssetRegistry().meshes.Get(mesh) : nullptr;
}

// Every vertex of every submesh has exactly the normal expected (within 1e-5).
bool NormalsAll(LPMESH mesh, const Vec3& expected)
{
    const engine::assets::MeshAsset* asset = MeshAssetOf(mesh);
    if (!asset || asset->submeshes.empty())
        return false;
    for (const engine::assets::SubMeshData& submesh : asset->submeshes)
    {
        const size_t vertexCount = submesh.positions.size() / 3u;
        if (vertexCount == 0u || submesh.normals.size() != vertexCount * 3u)
            return false;
        for (size_t v = 0; v < vertexCount; ++v)
        {
            const Vec3 n{submesh.normals[v * 3u], submesh.normals[v * 3u + 1u], submesh.normals[v * 3u + 2u]};
            if (!NearVec(n, expected, 1.0e-5f))
            {
                std::printf("INFO: mesh %u vertex %zu normal=(%g %g %g)\n", mesh.value, v, n.x, n.y, n.z);
                return false;
            }
        }
    }
    return true;
}

// No NaN / infinity in the tangents (an empty tangent channel counts as finite).
bool TangentsFinite(LPMESH mesh)
{
    const engine::assets::MeshAsset* asset = MeshAssetOf(mesh);
    if (!asset)
        return false;
    for (const engine::assets::SubMeshData& submesh : asset->submeshes)
        for (const float value : submesh.tangents)
            if (!std::isfinite(value))
                return false;
    return true;
}

// Error lines logged between BeginLogCapture and EndLogCapture (engine::Debug::onLog).
std::vector<std::string> g_capturedErrors;
bool g_capturing = false;

void BeginLogCapture()
{
    g_capturedErrors.clear();
    g_capturing = true;
}

void EndLogCapture()
{
    g_capturing = false;
}

int CapturedErrorCount()
{
    return static_cast<int>(g_capturedErrors.size());
}

bool CapturedErrorContains(size_t index, const char* text)
{
    return index < g_capturedErrors.size() && g_capturedErrors[index].find(text) != std::string::npos;
}

// Number of captured error lines that contain text.
int CapturedErrorWith(const char* text)
{
    int count = 0;
    for (const std::string& line : g_capturedErrors)
        if (line.find(text) != std::string::npos)
            ++count;
    return count;
}

} // namespace

int main()
{
    int failures = 0;
    const auto check = [&](bool ok, const char* message) {
        std::printf("%s: %s\n", ok ? "PASS" : "FAIL", message);
        if (!ok) ++failures;
    };
    engine::Debug::onLog = [](engine::LogLevel level, const char* message) {
        if (g_capturing && level >= engine::LogLevel::Error && message)
            g_capturedErrors.emplace_back(message);
    };

#if KROM_WRAPPER_HAS_GLTF
    std::printf("INFO: glTF importer registered (KROM_WRAPPER_HAS_GLTF=1)\n");
#else
    std::printf("INFO: no glTF importer (KROM_WRAPPER_HAS_GLTF=0)\n");
#endif

    // --- Before Graphics(): no asset pipeline, no world, not initialized -------
    // (Krom::GetComponent needs the world, so only return values are checked.)
    const Krom::MeshData earlyData = TriangleData("Early");
    check(!Krom::LoadMesh("Models/wuerfel.glb").IsValid(), "LoadMesh before Graphics returns null");
    check(!Krom::LoadTexture("Textures/albedo.png").IsValid(), "LoadTexture before Graphics returns null");
    check(!Krom::LoadMaterial("Materials/Block.mat").IsValid(), "LoadMaterial before Graphics returns null");
    check(!Krom::CreateTextureMaterial("Textures/albedo.png").IsValid(),
          "CreateTextureMaterial(path) before Graphics returns null");
    check(!Krom::CreateTextureMaterial(NULL_TEXTURE).IsValid(),
          "CreateTextureMaterial(handle) before Graphics returns null (logs: no known path)");
    check(!Krom::CreateLitMaterial().IsValid(), "CreateLitMaterial before Graphics returns null");
    check(!Krom::CreateMeshAsset(earlyData).IsValid(), "CreateMeshAsset before Graphics returns null");
    check(!Krom::CreateMeshEntity(earlyData).IsValid(), "CreateMeshEntity before Graphics returns null");
    check(!Krom::CreateMesh("EarlyMesh").IsValid(), "CreateMesh before Graphics returns null");
    check(!Krom::SetMesh(NULL_LPENTITY, NULL_MESH) && !Krom::SetMaterial(NULL_LPENTITY, NULL_MATERIAL) &&
          !Krom::SetMaterialRecursive(NULL_LPENTITY, NULL_MATERIAL),
          "SetMesh / SetMaterial / SetMaterialRecursive before Graphics return false");
    check(!Krom::SetTexture(NULL_LPENTITY, "Textures/albedo.png") && !Krom::SetTexture(NULL_LPENTITY, NULL_TEXTURE),
          "SetTexture (path / handle) before Graphics return false");
    check(!Krom::SetPBR(NULL_LPENTITY, Krom::PBR{}), "SetPBR before Graphics returns false");
    check(!Krom::SetMeshFromAsset(NULL_LPENTITY, "Models/wuerfel.glb") &&
          !Krom::SetMaterialFromAsset(NULL_LPENTITY, "Materials/Block.mat"),
          "SetMeshFromAsset / SetMaterialFromAsset before Graphics return false");
    check(!Krom::CreateEntityFromAsset("Models/car.glb").IsValid(), "CreateEntityFromAsset before Graphics returns null");
    check(!Krom::PrintAssetTree("Models/car.glb"), "PrintAssetTree before Graphics returns false (logs an error)");

    const auto sourceRoot = std::filesystem::path(__FILE__).parent_path().parent_path();
    const auto assetRoot = sourceRoot / "projects/DemoProject/Assets";

    // LoadMesh of a model writes a .kmesh cache next to the file; use a copy
    // outside the repository for the LoadMesh / SetMeshFromAsset checks.
    const auto scratch = sourceRoot / "out/krom-asset-api-test";
    std::error_code ec;
    std::filesystem::remove_all(scratch, ec);
    std::filesystem::create_directories(scratch, ec);
    const auto cubeCopy = scratch / "wuerfel.glb";
    std::filesystem::copy_file(assetRoot / "Models/wuerfel.glb", cubeCopy,
                               std::filesystem::copy_options::overwrite_existing, ec);
    const std::string cubeCopyPath = cubeCopy.generic_string();
    check(!ec && std::filesystem::exists(cubeCopy), "test setup: copy of wuerfel.glb outside the repository");

    Krom::SetAssetRoot(assetRoot);
    Krom::GraphicsConfig config{};
    config.backend = Krom::Renderer::DX11;
    config.visible = false;
    config.audioDevice = false;
    config.width = 64;
    config.height = 64;
    if (!Krom::Graphics(config))
        return 2;

    // --- LoadMesh / LoadTexture / LoadMaterial ---------------------------------
    const LPMESH loadedMesh = Krom::LoadMesh(cubeCopyPath.c_str());
    check(loadedMesh.IsValid(), "LoadMesh of a .glb file");
    check(Krom::LoadMesh(cubeCopyPath.c_str()) == loadedMesh, "LoadMesh of the same path returns the same handle");
    check(std::filesystem::exists(scratch / "wuerfel.kmesh"), "LoadMesh writes a .kmesh cache next to the model");
    BeginLogCapture();
    const LPMESH missingMesh = Krom::LoadMesh("Models/does_not_exist.glb");
    EndLogCapture();
    check(!missingMesh.IsValid(), "LoadMesh of a missing file returns null");
    check(CapturedErrorCount() == 1 && CapturedErrorContains(0, "LoadMesh") &&
          CapturedErrorContains(0, "Models/does_not_exist.glb") && CapturedErrorContains(0, "not found"),
          "LoadMesh of a missing file logs exactly one error naming the function, the path and 'not found'");
    check(!Krom::LoadMesh(nullptr).IsValid(), "LoadMesh(nullptr) returns null");
    check(!Krom::LoadMesh("").IsValid(), "LoadMesh(\"\") returns null");
    const auto brokenGlb = scratch / "broken.glb";
    {
        std::FILE* file = std::fopen(brokenGlb.string().c_str(), "wb");
        if (file)
        {
            std::fputs("this is not a glb file", file);
            std::fclose(file);
        }
    }
    const std::string brokenGlbPath = brokenGlb.generic_string();
    BeginLogCapture();
    const LPMESH brokenMesh = Krom::LoadMesh(brokenGlbPath.c_str());
    EndLogCapture();
    check(!brokenMesh.IsValid(), "LoadMesh of a file that exists but cannot be imported returns null");
    check(CapturedErrorWith("LoadMesh") == 1 && CapturedErrorWith("could not be loaded") == 1,
          "LoadMesh of a broken file logs one wrapper error 'could not be loaded'");

    const LPTEXTURE texture = Krom::LoadTexture("Textures/albedo.png");
    check(texture.IsValid(), "LoadTexture of a .png file");
    check(Krom::LoadTexture("Textures/albedo.png") == texture, "LoadTexture of the same path returns the same handle");
    check(!Krom::LoadTexture("Textures/does_not_exist.png").IsValid(), "LoadTexture of a missing file returns null");
    check(!Krom::LoadTexture(nullptr).IsValid(), "LoadTexture(nullptr) returns null");

    const LPMATERIAL blockAsset = Krom::LoadMaterial("Materials/Block.mat");
    check(blockAsset.IsValid(), "LoadMaterial of a .mat file");
    check(Krom::LoadMaterial("Materials/Block.mat") == blockAsset, "LoadMaterial of the same path returns the same handle");
    BeginLogCapture();
    const LPMATERIAL missingMaterial = Krom::LoadMaterial("Materials/does_not_exist.mat");
    EndLogCapture();
    check(!missingMaterial.IsValid(), "LoadMaterial of a missing file returns null");
    check(CapturedErrorCount() == 1 && CapturedErrorContains(0, "LoadMaterial") &&
          CapturedErrorContains(0, "Materials/does_not_exist.mat") && CapturedErrorContains(0, "not found"),
          "LoadMaterial of a missing file logs exactly one error naming the function, the path and 'not found'");
    check(!Krom::LoadMaterial(nullptr).IsValid(), "LoadMaterial(nullptr) returns null");
    check(!Krom::LoadMaterial("").IsValid(), "LoadMaterial(\"\") returns null");

    // --- Materials made through krom.h -----------------------------------------
    const LPMATERIAL texturedByPath = Krom::CreateTextureMaterial("Textures/albedo.png");
    check(texturedByPath.IsValid(), "CreateTextureMaterial(path)");
    const LPMATERIAL texturedByHandle = Krom::CreateTextureMaterial(texture);
    check(texturedByHandle.IsValid() && texturedByHandle != texturedByPath,
          "CreateTextureMaterial(handle) makes a new material each call");
    const LPMATERIAL unlitModel = Krom::CreateTextureMaterial("Textures/albedo.png", Krom::MaterialModel::Unlit);
    check(unlitModel.IsValid(), "CreateTextureMaterial with a non-Lit model warns and still returns a material");
    check(Krom::CreateTextureMaterial(nullptr).IsValid(),
          "CreateTextureMaterial(nullptr path) returns an untextured material (current behaviour)");
    check(Krom::CreateTextureMaterial("Textures/does_not_exist.png").IsValid(),
          "CreateTextureMaterial with a missing texture file still returns a material (current behaviour)");
    check(!Krom::CreateTextureMaterial(NULL_TEXTURE).IsValid(), "CreateTextureMaterial rejects an unknown handle");
    const LPMATERIAL litA = Krom::CreateLitMaterial(nullptr, {0.2f, 0.8f, 0.2f, 1.0f});
    const LPMATERIAL litB = Krom::CreateLitMaterial("Textures/albedo.png");
    check(litA.IsValid() && litB.IsValid() && litA != litB, "CreateLitMaterial with and without a texture");

    // --- CreateMeshAsset ---------------------------------------------------------
    Krom::MeshData noIndices = TriangleData("NoIndices");
    const LPMESH meshNoIndices = Krom::CreateMeshAsset(noIndices);
    check(meshNoIndices.IsValid(), "CreateMeshAsset without indices");
    const uint32_t indices[3] = {0u, 1u, 2u};
    Krom::MeshData withIndices = TriangleData("WithIndices");
    withIndices.indices = indices;
    withIndices.indexCount = 3u;
    const LPMESH meshWithIndices = Krom::CreateMeshAsset(withIndices);
    check(meshWithIndices.IsValid() && meshWithIndices != meshNoIndices,
          "CreateMeshAsset with indices makes a new mesh");
    Krom::MeshData indexPointerNoCount = TriangleData("IndexPointerNoCount");
    indexPointerNoCount.indices = indices;
    check(Krom::CreateMeshAsset(indexPointerNoCount).IsValid(),
          "CreateMeshAsset with an index pointer but indexCount 0 generates indices");
    Krom::MeshData plainFlags = TriangleData("NoGeneration");
    plainFlags.generateNormals = false;
    plainFlags.generateTangents = false;
    check(Krom::CreateMeshAsset(plainFlags).IsValid(), "CreateMeshAsset with generateNormals / generateTangents off");
    // generateNormals = false: normals stay exactly as supplied, also after the
    // mesh got an entity and materials (and, further below, after frames ran).
    Krom::MeshData zeroNormalsOff = TriangleData("ZeroNormalsOff");
    zeroNormalsOff.generateNormals = false;
    const LPMESH zeroOffMesh = Krom::CreateMeshAsset(zeroNormalsOff);
    check(NormalsAll(zeroOffMesh, {0.f, 0.f, 0.f}), "generateNormals=false keeps zero normals in CreateMeshAsset");
    check(TangentsFinite(zeroOffMesh), "generateNormals=false with zero normals gives finite tangents");
    Krom::MeshData zeroNormalsOn = TriangleData("ZeroNormalsOn");
    const LPMESH zeroOnMesh = Krom::CreateMeshAsset(zeroNormalsOn);
    check(NormalsAll(zeroOnMesh, {0.f, 0.f, 1.f}), "generateNormals=true fills zero normals with unit face normals");
    Krom::MeshData longNormalsOff = TriangleData("LongNormalsOff");
    longNormalsOff.vertices = kLongNormalTriangle;
    longNormalsOff.generateNormals = false;
    const LPMESH longOffMesh = Krom::CreateMeshAsset(longNormalsOff);
    check(NormalsAll(longOffMesh, {0.f, 0.f, 2.f}), "generateNormals=false keeps non-unit normals unnormalised");
    Krom::MeshData longNormalsOn = TriangleData("LongNormalsOn");
    longNormalsOn.vertices = kLongNormalTriangle;
    const LPMESH longOnMesh = Krom::CreateMeshAsset(longNormalsOn);
    check(NormalsAll(longOnMesh, {0.f, 0.f, 1.f}), "generateNormals=true normalises non-unit normals");

    const LPENTITY zeroOffEntity = Krom::CreateEntity("ZeroNormalsOffEntity");
    check(Krom::SetMesh(zeroOffEntity, zeroOffMesh) && MaterialOf(zeroOffEntity).IsValid() &&
          NormalsAll(zeroOffMesh, {0.f, 0.f, 0.f}),
          "generateNormals=false: zero normals stay zero after SetMesh (default material)");
    check(Krom::SetPBR(zeroOffEntity, Krom::PBR{}) && NormalsAll(zeroOffMesh, {0.f, 0.f, 0.f}) &&
          TangentsFinite(zeroOffMesh),
          "generateNormals=false: zero normals stay zero after SetPBR");
    check(Krom::SetMaterialFromAsset(zeroOffEntity, "Materials/Block.mat") && NormalsAll(zeroOffMesh, {0.f, 0.f, 0.f}),
          "generateNormals=false: zero normals stay zero after SetMaterialFromAsset");
    check(Krom::SetMaterial(zeroOffEntity, litB) && NormalsAll(zeroOffMesh, {0.f, 0.f, 0.f}),
          "generateNormals=false: zero normals stay zero after SetMaterial");
    const LPENTITY longOffEntity = Krom::CreateEntity("LongNormalsOffEntity");
    check(Krom::SetMesh(longOffEntity, longOffMesh) && Krom::SetPBR(longOffEntity, Krom::PBR{}) &&
          Krom::SetMaterialFromAsset(longOffEntity, "Materials/Block.mat") &&
          NormalsAll(longOffMesh, {0.f, 0.f, 2.f}),
          "generateNormals=false: non-unit normals stay unnormalised after SetMesh / SetPBR / SetMaterialFromAsset");
    const LPENTITY zeroOffProcedural = Krom::CreateMeshEntity(zeroNormalsOff);
    check(zeroOffProcedural.IsValid() && NormalsAll(MeshOf(zeroOffProcedural), {0.f, 0.f, 0.f}),
          "generateNormals=false: CreateMeshEntity keeps zero normals");

    Krom::MeshData unnamed = TriangleData(nullptr);
    check(Krom::CreateMeshAsset(unnamed).IsValid(), "CreateMeshAsset with a null name");
    Krom::MeshData empty{};
    check(!Krom::CreateMeshAsset(empty).IsValid(), "CreateMeshAsset rejects empty input");
    Krom::MeshData zeroCount = TriangleData("ZeroCount");
    zeroCount.vertexCount = 0u;
    check(!Krom::CreateMeshAsset(zeroCount).IsValid(), "CreateMeshAsset rejects vertexCount 0");

    // --- SetMesh ------------------------------------------------------------------
    const LPENTITY meshHolder = Krom::CreateEntity("MeshHolder");
    check(!Krom::HasComponent<engine::MeshComponent>(meshHolder), "a new entity has no mesh");
    check(Krom::SetMesh(meshHolder, meshNoIndices), "SetMesh");
    check(MeshOf(meshHolder) == meshNoIndices, "SetMesh adds the MeshComponent");
    const auto* holderBounds = BoundsOf(meshHolder);
    check(holderBounds && NearVec(holderBounds->centerLocal, {0.f, 0.f, 0.f}) &&
          NearVec(holderBounds->extentsLocal, {1.f, 1.f, 0.f}),
          "SetMesh adds local bounds of the mesh");
    const LPMATERIAL defaultMaterial = MaterialOf(meshHolder);
    check(defaultMaterial.IsValid(), "SetMesh assigns the default material");
    check(Krom::SetMesh(meshHolder, meshWithIndices) && MeshOf(meshHolder) == meshWithIndices &&
          MaterialOf(meshHolder) == defaultMaterial,
          "SetMesh again replaces the mesh and keeps the material");
    check(!Krom::SetMesh(meshHolder, NULL_MESH) && MeshOf(meshHolder) == meshWithIndices, "SetMesh rejects a null mesh");
    check(!Krom::SetMesh(NULL_LPENTITY, meshNoIndices), "SetMesh rejects a null entity");
    const LPENTITY deadEntity = Krom::CreateEntity("Dead");
    Krom::DestroyEntity(deadEntity);
    check(!Krom::SetMesh(deadEntity, meshNoIndices), "SetMesh rejects a destroyed entity");
    check(!Krom::SetMaterial(deadEntity, litA) && !Krom::SetMaterialRecursive(deadEntity, litA) &&
          !Krom::SetPBR(deadEntity, Krom::PBR{}) && !Krom::SetTexture(deadEntity, "Textures/albedo.png"),
          "material setters reject a destroyed entity");

    // --- SetMeshFromAsset / SetMaterialFromAsset ---------------------------------
    const LPENTITY fromAsset = Krom::CreateEntity("FromAsset");
    check(Krom::SetMeshFromAsset(fromAsset, cubeCopyPath.c_str()) && MeshOf(fromAsset) == loadedMesh,
          "SetMeshFromAsset loads and assigns the mesh");
    const auto* fromAssetBounds = BoundsOf(fromAsset);
    check(fromAssetBounds && fromAssetBounds->extentsLocal.x > 0.0f && fromAssetBounds->extentsLocal.y > 0.0f &&
          fromAssetBounds->extentsLocal.z > 0.0f,
          "SetMeshFromAsset sets bounds of the loaded model");
    check(MaterialOf(fromAsset).IsValid(), "SetMeshFromAsset assigns a material");
    check(!Krom::SetMeshFromAsset(fromAsset, nullptr), "SetMeshFromAsset(nullptr) returns false");
    const LPENTITY missingFromAsset = Krom::CreateEntity("MissingFromAsset");
    BeginLogCapture();
    const bool missingMeshSet = Krom::SetMeshFromAsset(missingFromAsset, "Models/does_not_exist.glb");
    EndLogCapture();
    check(!missingMeshSet, "SetMeshFromAsset of a missing file returns false");
    check(!Krom::HasComponent<engine::MeshComponent>(missingFromAsset) &&
          !Krom::HasComponent<engine::MaterialComponent>(missingFromAsset) &&
          !Krom::HasComponent<engine::BoundsComponent>(missingFromAsset),
          "SetMeshFromAsset of a missing file leaves the entity untouched (no mesh, material or bounds)");
    check(CapturedErrorCount() == 1 && CapturedErrorContains(0, "not found"),
          "SetMeshFromAsset of a missing file logs exactly one 'not found' error");
    check(Krom::SetMeshFromAsset(missingFromAsset, cubeCopyPath.c_str()) == true &&
          !Krom::SetMeshFromAsset(missingFromAsset, "Models/does_not_exist.glb") &&
          MeshOf(missingFromAsset) == loadedMesh,
          "SetMeshFromAsset of a missing file keeps the mesh the entity already had");

    check(Krom::SetMaterialFromAsset(fromAsset, "Materials/Block.mat"), "SetMaterialFromAsset");
    const auto* fromAssetMaterial = Krom::GetComponent<engine::MaterialComponent>(fromAsset);
    check(fromAssetMaterial && fromAssetMaterial->material.IsValid() &&
          fromAssetMaterial->materialAssetPath == "Materials/Block.mat",
          "SetMaterialFromAsset assigns a runtime material and remembers the asset path");
    check(fromAssetMaterial && fromAssetMaterial->material != blockAsset,
          "SetMaterialFromAsset realizes the asset handle into a runtime material");
    check(!Krom::SetMaterialFromAsset(fromAsset, nullptr), "SetMaterialFromAsset(nullptr) returns false");
    const LPENTITY missingMaterialHolder = Krom::CreateMeshEntity(TriangleData("MissingMaterialHolder"));
    const LPMATERIAL holderMaterialBefore = MaterialOf(missingMaterialHolder);
    BeginLogCapture();
    const bool missingMaterialSet = Krom::SetMaterialFromAsset(missingMaterialHolder, "Materials/does_not_exist.mat");
    EndLogCapture();
    const auto* missingMaterialComponent = Krom::GetComponent<engine::MaterialComponent>(missingMaterialHolder);
    check(!missingMaterialSet && holderMaterialBefore.IsValid() &&
          MaterialOf(missingMaterialHolder) == holderMaterialBefore && missingMaterialComponent &&
          missingMaterialComponent->materialAssetPath.empty(),
          "SetMaterialFromAsset of a missing file returns false, keeps the material and stores no path");
    check(CapturedErrorCount() == 1 && CapturedErrorContains(0, "not found"),
          "SetMaterialFromAsset of a missing file logs exactly one 'not found' error");
    const LPMATERIAL blockRuntimeBefore = MaterialOf(fromAsset);
    const bool missingOverBlock = Krom::SetMaterialFromAsset(fromAsset, "Materials/does_not_exist.mat");
    const auto* fromAssetMaterialAfter = Krom::GetComponent<engine::MaterialComponent>(fromAsset);
    check(!missingOverBlock && blockRuntimeBefore.IsValid() && fromAssetMaterialAfter &&
          fromAssetMaterialAfter->material == blockRuntimeBefore &&
          fromAssetMaterialAfter->materialAssetPath == "Materials/Block.mat",
          "SetMaterialFromAsset of a missing file keeps the previous material and materialAssetPath");
    check(!Krom::SetMaterialFromAsset(NULL_LPENTITY, "Materials/Block.mat"),
          "SetMaterialFromAsset rejects a null entity");

    // --- SetMaterialRecursive / SetTexture / SetPBR over a hierarchy -------------
    // root (no mesh) -> meshChild (mesh) -> meshGrandchild (mesh)
    //               -> emptyChild (no mesh)
    const LPENTITY root = Krom::CreateEntity("HierarchyRoot");
    const LPENTITY meshChild = Krom::CreateMeshEntity(TriangleData("MeshChild"));
    const LPENTITY meshGrandchild = Krom::CreateMeshEntity(TriangleData("MeshGrandchild"));
    const LPENTITY emptyChild = Krom::CreateEntity("EmptyChild");
    check(Krom::SetParent(meshChild, root) && Krom::SetParent(meshGrandchild, meshChild) &&
          Krom::SetParent(emptyChild, root),
          "test setup: hierarchy");
    const LPMATERIAL childBefore = MaterialOf(meshChild);
    const LPMATERIAL grandchildBefore = MaterialOf(meshGrandchild);
    check(childBefore.IsValid() && grandchildBefore.IsValid(), "CreateMeshEntity brings a material");

    check(Krom::SetMaterialRecursive(root, litA), "SetMaterialRecursive");
    check(MaterialOf(meshChild).IsValid() && MaterialOf(meshChild) != childBefore &&
          MaterialOf(meshGrandchild).IsValid() && MaterialOf(meshGrandchild) != grandchildBefore,
          "SetMaterialRecursive reaches mesh children and grandchildren");
    check(MaterialOf(meshChild) != litA, "SetMaterialRecursive realizes runtime materials");
    check(!Krom::HasComponent<engine::MaterialComponent>(root) &&
          !Krom::HasComponent<engine::MaterialComponent>(emptyChild),
          "SetMaterialRecursive skips entities without a mesh");
    check(!Krom::SetMaterialRecursive(root, NULL_MATERIAL), "SetMaterialRecursive rejects a null material");

    const LPMATERIAL childRecursive = MaterialOf(meshChild);
    const LPMATERIAL grandchildRecursive = MaterialOf(meshGrandchild);
    check(Krom::SetTexture(meshChild, "Textures/albedo.png", Krom::MaterialModel::Lit, false),
          "SetTexture non-recursive");
    check(MaterialOf(meshChild) != childRecursive && MaterialOf(meshGrandchild) == grandchildRecursive,
          "SetTexture non-recursive changes only the entity");
    const LPMATERIAL childTextured = MaterialOf(meshChild);
    check(Krom::SetTexture(root, texture) && MaterialOf(meshChild) != childTextured &&
          MaterialOf(meshGrandchild) != grandchildRecursive,
          "SetTexture(handle) recursive changes the mesh descendants");
    check(!Krom::HasComponent<engine::MaterialComponent>(root) &&
          !Krom::HasComponent<engine::MaterialComponent>(emptyChild),
          "SetTexture recursive skips entities without a mesh");
    check(!Krom::SetTexture(root, "") && !Krom::SetTexture(root, static_cast<const char*>(nullptr)),
          "SetTexture rejects an empty / null path");
    check(!Krom::SetTexture(root, NULL_TEXTURE), "SetTexture rejects an unknown texture handle");

    const LPMATERIAL childBeforePbr = MaterialOf(meshChild);
    const LPMATERIAL grandchildBeforePbr = MaterialOf(meshGrandchild);
    Krom::PBR pbr{};
    pbr.color = Krom::Color(0.8f, 0.3f, 0.1f);
    pbr.metallic = 0.5f;
    check(Krom::SetPBR(root, pbr), "SetPBR recursive");
    check(MaterialOf(meshChild) != childBeforePbr && MaterialOf(meshGrandchild) != grandchildBeforePbr &&
          MaterialOf(meshChild).IsValid() && MaterialOf(meshGrandchild).IsValid(),
          "SetPBR recursive changes the mesh descendants");
    check(!Krom::HasComponent<engine::MaterialComponent>(root), "SetPBR recursive skips the root without a mesh");

    // Non-recursive assignment to an entity without a mesh.
    check(Krom::SetMaterial(emptyChild, litA), "SetMaterial on an entity without a mesh returns true");
    check(MaterialOf(emptyChild) == litA,
          "SetMaterial without a mesh stores the unrealized asset handle (current behaviour)");

    // --- CreateMeshEntity / CreateMesh ---------------------------------------------
    const LPENTITY meshEntity = Krom::CreateMeshEntity(TriangleData("NamedMeshEntity"));
    check(meshEntity.IsValid() && std::strcmp(Krom::GetName(meshEntity), "NamedMeshEntity") == 0,
          "CreateMeshEntity names the entity after MeshData::name");
    check(MeshOf(meshEntity).IsValid() && MaterialOf(meshEntity).IsValid() && BoundsOf(meshEntity),
          "CreateMeshEntity has mesh, material and bounds");
    const LPENTITY unnamedMeshEntity = Krom::CreateMeshEntity(TriangleData(nullptr));
    check(unnamedMeshEntity.IsValid() && std::strcmp(Krom::GetName(unnamedMeshEntity), "ProceduralEntity") == 0,
          "CreateMeshEntity without a name is called ProceduralEntity");
    check(!Krom::CreateMeshEntity(empty).IsValid(), "CreateMeshEntity rejects empty input");

    const LPENTITY emptyMesh = Krom::CreateMesh("EmptyMesh");
    check(emptyMesh.IsValid() && std::strcmp(Krom::GetName(emptyMesh), "EmptyMesh") == 0 && MeshOf(emptyMesh).IsValid(),
          "CreateMesh makes an entity with an empty mesh");
    check(BoundsOf(emptyMesh) && !MaterialOf(emptyMesh).IsValid(),
          "CreateMesh has bounds but no material while the mesh is empty (current behaviour)");
    const LPENTITY defaultNamedMesh = Krom::CreateMesh(nullptr);
    check(defaultNamedMesh.IsValid() && std::strcmp(Krom::GetName(defaultNamedMesh), "Mesh") == 0,
          "CreateMesh(nullptr) is called Mesh");
    check(MeshOf(defaultNamedMesh) != MeshOf(emptyMesh), "CreateMesh makes a new mesh each call");

    // --- Model import ---------------------------------------------------------------
    check(Krom::PrintAssetTree("Models/car.glb"), "PrintAssetTree of a static model");
    check(Krom::PrintAssetTree("Models/CesiumMan.glb"), "PrintAssetTree of a skinned model");
    check(!Krom::PrintAssetTree("Models/does_not_exist.glb"), "PrintAssetTree of a missing file returns false");
    check(!Krom::PrintAssetTree(nullptr) && !Krom::PrintAssetTree(""), "PrintAssetTree(nullptr / \"\") returns false");

    const LPENTITY car = Krom::CreateEntityFromAsset("Models/car.glb");
    check(car.IsValid(), "CreateEntityFromAsset of a static model");
    DumpHierarchy("car", car);
    check(std::strcmp(Krom::GetName(car), "car") == 0, "the root is named after the file");
    check(!Krom::HasComponent<engine::ParentComponent>(car), "the root has no parent");
    check(!HasMesh(car), "the static model gets a synthetic root without a mesh");
    const std::vector<LPENTITY> carEntities = Hierarchy(car);
    std::printf("INFO: car entities=%zu meshes=%d rootChildren=%d\n", carEntities.size(),
                CountWith(carEntities, HasMesh), Krom::CountChildren(car));
    // car.glb: one node 'Circle.005' with one mesh.
    check(carEntities.size() == 2u && Krom::CountChildren(car) == 1 && CountWith(carEntities, HasMesh) == 1,
          "the static model: synthetic root with one mesh child");
    check(Krom::GetChild(car, 0) == Krom::FindChild(car, "Circle.005") && HasMesh(Krom::GetChild(car, 0)),
          "the mesh child keeps the node name");
    check(CountWith(carEntities, MeshEntityComplete) == static_cast<int>(carEntities.size()),
          "every mesh entity of the static model has a material and bounds");
    check(CountWith(carEntities, HasSkeleton) == 0 && CountWith(carEntities, HasPlayingClip) == 0,
          "the static model has no skin and no animation");
    Vec3 carCenter{};
    Vec3 carExtents{};
    float carSphere = 0.0f;
    check(Krom::GetWorldBounds(car, carCenter, carExtents, carSphere) && carSphere > 0.0f,
          "GetWorldBounds of the imported model");
    std::printf("INFO: car world bounds center=(%.3f %.3f %.3f) extents=(%.3f %.3f %.3f)\n",
                carCenter.x, carCenter.y, carCenter.z, carExtents.x, carExtents.y, carExtents.z);
    check(NearVec(carCenter, {0.f, 0.f, 0.f}, 1.0e-2f) && carExtents.x > 0.0f && carExtents.y > 0.0f &&
          carExtents.z > 0.0f,
          "the imported model is centred on its root");

    const LPENTITY carAgain = Krom::CreateEntityFromAsset("Models/car.glb", "CarAgain");
    check(carAgain.IsValid() && carAgain != car && Krom::IsAlive(car), "importing the same file twice gives a second root");
    check(std::strcmp(Krom::GetName(carAgain), "CarAgain") == 0, "the name argument names the root");
    const std::vector<LPENTITY> carAgainEntities = Hierarchy(carAgain);
    check(carAgainEntities.size() == carEntities.size(), "the second import has the same hierarchy size");
    bool sameMeshes = carAgainEntities.size() == carEntities.size();
    bool newMaterials = sameMeshes;
    bool sameNames = sameMeshes;
    for (size_t i = 0; sameMeshes && i < carEntities.size(); ++i)
    {
        sameMeshes = sameMeshes && MeshOf(carEntities[i]) == MeshOf(carAgainEntities[i]);
        if (i > 0)
            sameNames = sameNames && std::strcmp(Krom::GetName(carEntities[i]), Krom::GetName(carAgainEntities[i])) == 0;
        if (HasMesh(carEntities[i]))
            newMaterials = newMaterials && MaterialOf(carEntities[i]) != MaterialOf(carAgainEntities[i]);
    }
    check(sameNames, "the second import has the same child names");
    check(sameMeshes, "the second import reuses the mesh handles (registry entries replaced by path)");
    check(newMaterials, "the second import creates new runtime materials (not cached)");

    // Sub-asset keys of an imported model (as stored in prefabs and scenes) load
    // from the registry and keep the asset Loaded (car.glb has no materials;
    // the material key is checked with CesiumMan below).
    const LPMESH carSubMesh = Krom::LoadMesh("Models/car.glb#mesh/0");
    const engine::assets::MeshAsset* carSubMeshAsset = MeshAssetOf(carSubMesh);
    check(carSubMeshAsset && carSubMeshAsset->state == engine::assets::AssetState::Loaded &&
          carSubMesh == MeshOf(Krom::GetChild(car, 0)),
          "LoadMesh of an imported sub-asset key returns the imported mesh, still Loaded");
    BeginLogCapture();
    const LPMATERIAL unknownSubMaterial = Krom::LoadMaterial("Models/car.glb#material/99");
    const LPMATERIAL missingModelSubMaterial = Krom::LoadMaterial("Models/does_not_exist.glb#material/0");
    EndLogCapture();
    check(!unknownSubMaterial.IsValid() && !missingModelSubMaterial.IsValid() && CapturedErrorCount() == 2 &&
          CapturedErrorContains(0, "could not be loaded") && CapturedErrorContains(1, "not found"),
          "LoadMaterial of an unknown sub-asset / a sub-asset of a missing model returns null with one error each");

    const LPENTITY cesium = Krom::CreateEntityFromAsset("Models/CesiumMan.glb");
    check(cesium.IsValid(), "CreateEntityFromAsset of a skinned model");
    DumpHierarchy("CesiumMan", cesium);
    check(std::strcmp(Krom::GetName(cesium), "CesiumMan") == 0, "the skinned root is named after the file");
    const std::vector<LPENTITY> cesiumEntities = Hierarchy(cesium);
    std::printf("INFO: CesiumMan entities=%zu meshes=%d skins=%d clips=%d rootChildren=%d\n", cesiumEntities.size(),
                CountWith(cesiumEntities, HasMesh), CountWith(cesiumEntities, HasSkeleton),
                CountWith(cesiumEntities, HasPlayingClip), Krom::CountChildren(cesium));
    // CesiumMan.glb: 22 nodes (Z_UP -> Armature -> mesh 'Cesium_Man' + joints), one skin, one clip.
    check(cesiumEntities.size() == 23u && Krom::CountChildren(cesium) == 1 &&
          std::strcmp(Krom::GetName(Krom::GetChild(cesium, 0)), "Z_UP") == 0,
          "the skinned model: synthetic root + 22 node entities, first child Z_UP");
    const LPENTITY cesiumMesh = Krom::FindChild(cesium, "Cesium_Man");
    check(HasMesh(cesiumMesh) && CountWith(cesiumEntities, HasMesh) == 1, "the skinned model has one mesh entity, Cesium_Man");
    check(Krom::FindChild(cesium, "Skeleton_neck_joint_2").IsValid() &&
          !HasMesh(Krom::FindChild(cesium, "Skeleton_neck_joint_2")),
          "joint nodes become entities without a mesh");
    check(CountWith(cesiumEntities, MeshEntityComplete) == static_cast<int>(cesiumEntities.size()),
          "every mesh entity of the skinned model has a material and bounds");
    check(HasSkeleton(cesiumMesh) && CountWith(cesiumEntities, HasSkeleton) == 1,
          "the skinned mesh has a SkinComponent with a skeleton");
    check(CountWith(cesiumEntities, HasPlayingClip) == 2, "two entities play the clip (root and skinned mesh)");
    check(HasPlayingClip(cesium), "the synthetic root plays the first clip, looping");
    bool skinsPlay = true;
    for (const LPENTITY entity : cesiumEntities)
        if (HasSkeleton(entity))
            skinsPlay = skinsPlay && HasPlayingClip(entity);
    check(skinsPlay, "the skinned mesh entities play the first clip too");
    const LPMATERIAL cesiumSubMaterial = Krom::LoadMaterial("Models/CesiumMan.glb#material/0");
    const auto* cesiumSubMaterialAsset = cesiumSubMaterial.IsValid()
        ? Krom::internal::GetWrapperAssetRegistry().materials.Get(cesiumSubMaterial) : nullptr;
    check(cesiumSubMaterialAsset && cesiumSubMaterialAsset->state == engine::assets::AssetState::Loaded &&
          cesiumSubMaterialAsset->debugName == "Cesium_Man-effect",
          "LoadMaterial of an imported sub-asset key returns the imported material, still Loaded");

    // Missing file, broken file, extension without importer, null path.
    const size_t entitiesBeforeMissing = Krom::GetWorld().EntityCount();
    BeginLogCapture();
    const LPENTITY missingModel = Krom::CreateEntityFromAsset("Models/does_not_exist.glb", "MissingModel");
    EndLogCapture();
    check(!missingModel.IsValid(), "CreateEntityFromAsset of a missing file returns null");
    check(Krom::GetWorld().EntityCount() == entitiesBeforeMissing && !Krom::FindEntity("MissingModel").IsValid(),
          "CreateEntityFromAsset of a missing file leaves no entity behind");
    check(CapturedErrorCount() == 1 && CapturedErrorContains(0, "CreateEntityFromAsset") &&
          CapturedErrorContains(0, "Models/does_not_exist.glb") && CapturedErrorContains(0, "not found"),
          "CreateEntityFromAsset of a missing file logs exactly one 'not found' error naming the function and path");
    const LPENTITY missingUnnamed = Krom::CreateEntityFromAsset("Models/does_not_exist.glb");
    check(!missingUnnamed.IsValid() && Krom::GetWorld().EntityCount() == entitiesBeforeMissing &&
          !Krom::FindEntity("Models/does_not_exist.glb").IsValid(),
          "CreateEntityFromAsset of a missing file without a name returns null and leaves nothing behind");

    BeginLogCapture();
    const LPENTITY brokenModel = Krom::CreateEntityFromAsset(brokenGlbPath.c_str(), "BrokenModel");
    EndLogCapture();
    check(!brokenModel.IsValid() && Krom::GetWorld().EntityCount() == entitiesBeforeMissing &&
          !Krom::FindEntity("BrokenModel").IsValid(),
          "CreateEntityFromAsset of a file the importer rejects returns null and leaves nothing behind");
    check(CapturedErrorWith("cgltf") >= 1 && CapturedErrorWith("lade als einfaches Mesh") == 0 &&
          CapturedErrorWith("simple mesh") == 0,
          "CreateEntityFromAsset of a rejected file reports the import error and does not fall back to a simple mesh");

    // Extension without a registered importer: the simple text mesh format of
    // AssetPipeline::ReloadMesh ("v", "vt", "vn", "i" lines) still loads as one entity.
    const auto textMesh = scratch / "triangle.kromtxt";
    {
        std::FILE* file = std::fopen(textMesh.string().c_str(), "wb");
        if (file)
        {
            std::fputs("v -1 -1 0\nv 1 -1 0\nv 0 1 0\n"
                       "vt 0 0\nvt 1 0\nvt 0.5 1\n"
                       "vn 0 0 1\nvn 0 0 1\nvn 0 0 1\n"
                       "i 0 1 2\n", file);
            std::fclose(file);
        }
    }
    const std::string textMeshPath = textMesh.generic_string();
    const LPENTITY textModel = Krom::CreateEntityFromAsset(textMeshPath.c_str(), "TextModel");
    check(textModel.IsValid() && std::strcmp(Krom::GetName(textModel), "TextModel") == 0 && HasMesh(textModel) &&
          Krom::GetWorld().EntityCount() == entitiesBeforeMissing + 1u,
          "CreateEntityFromAsset of an extension without importer falls back to a single mesh entity");
    const auto brokenText = scratch / "empty.kromtxt";
    {
        std::FILE* file = std::fopen(brokenText.string().c_str(), "wb");
        if (file)
        {
            std::fputs("# no vertices\n", file);
            std::fclose(file);
        }
    }
    const std::string brokenTextPath = brokenText.generic_string();
    const size_t entitiesBeforeBrokenText = Krom::GetWorld().EntityCount();
    const LPENTITY brokenTextModel = Krom::CreateEntityFromAsset(brokenTextPath.c_str(), "EmptyTextModel");
    check(!brokenTextModel.IsValid() && Krom::GetWorld().EntityCount() == entitiesBeforeBrokenText &&
          !Krom::FindEntity("EmptyTextModel").IsValid(),
          "a failed simple-mesh fallback destroys the entity it created");
    check(!Krom::CreateEntityFromAsset(nullptr).IsValid() && !Krom::CreateEntityFromAsset("").IsValid(),
          "CreateEntityFromAsset(nullptr / \"\") returns null");

    check(Krom::DestroyEntity(textModel) && Krom::DestroyEntity(missingFromAsset),
          "test cleanup: fallback entities destroyed");

    // --- Render the imported models, then without them ------------------------------
    Krom::SetPosition(car, -2.5f, 0.0f, 0.0f);
    Krom::SetPosition(carAgain, 2.5f, 0.0f, 0.0f);
    Krom::SetPosition(cesium, 0.0f, 0.0f, 0.0f);
    Krom::SetPosition(root, 0.0f, 3.0f, 0.0f);
    const LPENTITY camera = Krom::CreateCamera("Camera", {0.0f, 1.0f, 10.0f});
    Krom::LookAt(camera, Vec3{0.0f, 0.0f, 0.0f});
    Krom::CreateDirectionalLight("Sun", {0.0f, 5.0f, 5.0f}, {-45.0f, 30.0f, 0.0f});

    std::vector<LPENTITY> imported = carEntities;
    imported.insert(imported.end(), carAgainEntities.begin(), carAgainEntities.end());
    imported.insert(imported.end(), cesiumEntities.begin(), cesiumEntities.end());

    float clipTimeBefore = -1.0f;
    float clipTimeAfter = -1.0f;
    bool destroyed = false;
    bool allGone = false;
    int frame = 0;
    const int runResult = Krom::Run([&](float) {
        ++frame;
        if (frame == 1)
        {
            if (const auto* player = Krom::GetComponent<engine::AnimationPlayerComponent>(cesium))
                clipTimeBefore = player->currentTime;
        }
        if (frame == 4)
        {
            if (const auto* player = Krom::GetComponent<engine::AnimationPlayerComponent>(cesium))
                clipTimeAfter = player->currentTime;
            destroyed = Krom::DestroyEntity(car) && Krom::DestroyEntity(carAgain) && Krom::DestroyEntity(cesium);
            allGone = true;
            for (const LPENTITY entity : imported)
                allGone = allGone && !Krom::IsAlive(entity);
        }
        if (frame == 7)
            Krom::Quit();
    });
    check(runResult == 0 && frame == 7, "seven frames render (three with the imported models, three without)");
    std::printf("INFO: clip time frame1=%.4f frame4=%.4f\n", clipTimeBefore, clipTimeAfter);
    check(clipTimeAfter > clipTimeBefore, "the clip of the skinned model advances while rendering");
    check(destroyed && allGone, "DestroyEntity removes the imported hierarchies");
    check(Krom::IsAlive(root) && Krom::IsAlive(meshChild), "the other test entities survive");
    check(Krom::IsAlive(zeroOffEntity) && NormalsAll(zeroOffMesh, {0.f, 0.f, 0.f}) && TangentsFinite(zeroOffMesh),
          "generateNormals=false: zero normals stay zero after frames rendered");
    check(Krom::IsAlive(longOffEntity) && NormalsAll(longOffMesh, {0.f, 0.f, 2.f}),
          "generateNormals=false: non-unit normals stay unnormalised after frames rendered");

    Krom::Shutdown();
    std::filesystem::remove_all(scratch, ec);
    std::printf("%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
