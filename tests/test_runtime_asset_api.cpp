// Opt-in integration test of the krom.h asset functions against the real
// runtime: LoadMesh / LoadTexture / LoadMaterial, materials made through krom.h,
// mesh and material assignment (SetMesh, SetMaterialRecursive, SetTexture,
// SetPBR, *FromAsset), procedural meshes (CreateMeshAsset, CreateMeshEntity,
// CreateMesh) and the model import (CreateEntityFromAsset, PrintAssetTree)
// with models of the DemoProject. Start from the repository directory; the
// DX11 window stays hidden. The checks document the current behaviour of the
// wrapper, oddities included (marked "current behaviour").
#include "krom.h"
#include "addons/animation/AnimationComponents.hpp"
#include "addons/mesh_renderer/MeshRendererComponents.hpp"
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

} // namespace

int main()
{
    int failures = 0;
    const auto check = [&](bool ok, const char* message) {
        std::printf("%s: %s\n", ok ? "PASS" : "FAIL", message);
        if (!ok) ++failures;
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
    const LPMESH missingMesh = Krom::LoadMesh("Models/does_not_exist.glb");
    check(missingMesh.IsValid(),
          "LoadMesh of a missing file still returns a valid handle (current behaviour: failed asset in the registry)");
    check(!Krom::LoadMesh(nullptr).IsValid(), "LoadMesh(nullptr) returns null");

    const LPTEXTURE texture = Krom::LoadTexture("Textures/albedo.png");
    check(texture.IsValid(), "LoadTexture of a .png file");
    check(Krom::LoadTexture("Textures/albedo.png") == texture, "LoadTexture of the same path returns the same handle");
    check(!Krom::LoadTexture("Textures/does_not_exist.png").IsValid(), "LoadTexture of a missing file returns null");
    check(!Krom::LoadTexture(nullptr).IsValid(), "LoadTexture(nullptr) returns null");

    const LPMATERIAL blockAsset = Krom::LoadMaterial("Materials/Block.mat");
    check(blockAsset.IsValid(), "LoadMaterial of a .mat file");
    check(Krom::LoadMaterial("Materials/Block.mat") == blockAsset, "LoadMaterial of the same path returns the same handle");
    const LPMATERIAL missingMaterial = Krom::LoadMaterial("Materials/does_not_exist.mat");
    check(missingMaterial.IsValid(),
          "LoadMaterial of a missing file still returns a valid handle (current behaviour)");
    check(!Krom::LoadMaterial(nullptr).IsValid(), "LoadMaterial(nullptr) returns null");

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
    const bool missingMeshSet = Krom::SetMeshFromAsset(missingFromAsset, "Models/does_not_exist.glb");
    check(missingMeshSet && MeshOf(missingFromAsset) == missingMesh,
          "SetMeshFromAsset of a missing file returns true with the failed mesh (current behaviour)");
    check(!MaterialOf(missingFromAsset).IsValid(),
          "SetMeshFromAsset of a missing file leaves the entity without a material (current behaviour)");

    check(Krom::SetMaterialFromAsset(fromAsset, "Materials/Block.mat"), "SetMaterialFromAsset");
    const auto* fromAssetMaterial = Krom::GetComponent<engine::MaterialComponent>(fromAsset);
    check(fromAssetMaterial && fromAssetMaterial->material.IsValid() &&
          fromAssetMaterial->materialAssetPath == "Materials/Block.mat",
          "SetMaterialFromAsset assigns a runtime material and remembers the asset path");
    check(fromAssetMaterial && fromAssetMaterial->material != blockAsset,
          "SetMaterialFromAsset realizes the asset handle into a runtime material");
    check(!Krom::SetMaterialFromAsset(fromAsset, nullptr), "SetMaterialFromAsset(nullptr) returns false");
    const LPENTITY missingMaterialHolder = Krom::CreateMeshEntity(TriangleData("MissingMaterialHolder"));
    const bool missingMaterialSet = Krom::SetMaterialFromAsset(missingMaterialHolder, "Materials/does_not_exist.mat");
    const auto* missingMaterialComponent = Krom::GetComponent<engine::MaterialComponent>(missingMaterialHolder);
    check(missingMaterialSet && MaterialOf(missingMaterialHolder).IsValid() && missingMaterialComponent &&
          missingMaterialComponent->materialAssetPath == "Materials/does_not_exist.mat",
          "SetMaterialFromAsset of a missing file returns true, assigns a material and stores the path (current behaviour)");
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

    // Missing file and null path.
    const LPENTITY missingModel = Krom::CreateEntityFromAsset("Models/does_not_exist.glb", "MissingModel");
    // The fallback logs "no bundle importer", although the glTF importer exists and only the file is missing.
    check(missingModel.IsValid() && std::strcmp(Krom::GetName(missingModel), "MissingModel") == 0 &&
          MeshOf(missingModel) == missingMesh && !MaterialOf(missingModel).IsValid(),
          "CreateEntityFromAsset of a missing file falls back to a single entity with the failed mesh, no material (current behaviour)");
    const LPENTITY missingUnnamed = Krom::CreateEntityFromAsset("Models/does_not_exist.glb");
    check(missingUnnamed.IsValid() && std::strcmp(Krom::GetName(missingUnnamed), "Models/does_not_exist.glb") == 0,
          "the fallback entity without a name is named after the path (current behaviour)");
    check(!Krom::CreateEntityFromAsset(nullptr).IsValid() && !Krom::CreateEntityFromAsset("").IsValid(),
          "CreateEntityFromAsset(nullptr / \"\") returns null");

    // Entities with a failed mesh are not drawn; remove them before rendering.
    check(Krom::DestroyEntity(missingModel) && Krom::DestroyEntity(missingUnnamed) &&
          Krom::DestroyEntity(missingFromAsset),
          "test cleanup: entities with the failed mesh destroyed");

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

    Krom::Shutdown();
    std::filesystem::remove_all(scratch, ec);
    std::printf("%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
