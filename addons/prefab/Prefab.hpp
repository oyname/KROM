#pragma once
// =============================================================================
// KROM Engine - addons/prefab/Prefab.hpp
// Lightweight prefab recipe and ECS instantiation helpers.
// =============================================================================
#include "addons/script/ScriptRegistry.hpp"
#include "assets/AssetImporter.hpp"
#include "core/Types.hpp"
#include "core/Math.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine::assets { class AssetRegistry; }
namespace engine::ecs { class World; }

namespace engine::addons::prefab {

struct PrefabEntityRecord
{
    struct ScriptRecord
    {
        std::string className;
        std::string instanceName;
        std::unordered_map<std::string, script::ScriptFieldValue> fieldValues;
    };

    std::string name;
    int32_t parentIndex = -1;

    math::Vec3 localPosition{0.f, 0.f, 0.f};
    math::Quat localRotation = math::Quat::Identity();
    math::Vec3 localScale{1.f, 1.f, 1.f};

    MeshHandle  mesh;
    std::string meshAssetPath; // Registry-Pfad des Meshes (z.B. "model.glb#mesh/0")
    MaterialHandle material;
    std::string materialAssetPath;
    std::string baseColorTexturePath;

    // Per-Submesh-Material-Overrides (entspricht MaterialComponent::slotOverrides)
    struct SlotOverride
    {
        uint32_t    submeshIndex  = 0u;
        MaterialHandle material;
        std::string materialAssetPath;
    };
    std::vector<SlotOverride> slotOverrides;

    SkeletonHandle skeleton;
    std::string skeletonAssetPath;
    AnimationHandle animation;
    std::string animationAssetPath;

    bool active = true;
    bool addBounds = false;
    math::Vec3 boundsCenterLocal{0.f, 0.f, 0.f};
    math::Vec3 boundsExtentsLocal{1.f, 1.f, 1.f};
    bool addObb = false;
    math::Vec3 obbCenterOffset{0.f, 0.f, 0.f};
    math::Vec3 obbHalfExtents{0.5f, 0.5f, 0.5f};
    math::Quat obbOrientation = math::Quat::Identity();
    bool showObbInEditor = false;
    bool playAnimation = false;
    bool loopAnimation = true;
    std::vector<ScriptRecord> scripts;
};

struct PrefabAsset
{
    std::string name = "Prefab";
    std::vector<PrefabEntityRecord> records;
    uint32_t rootIndex = 0u;

    [[nodiscard]] bool Empty() const noexcept { return records.empty(); }
    [[nodiscard]] int32_t FindEntity(std::string_view name) const noexcept;
};

struct PrefabInstance
{
    EntityID root = NULL_ENTITY;
    std::vector<EntityID> entities;

    [[nodiscard]] bool IsValid() const noexcept { return root.IsValid(); }
};

struct PrefabBuildOptions
{
    std::string name = "Prefab";
    bool createSyntheticRoot = true;
    bool playFirstAnimation = true;
    bool loopAnimations = true;
};

struct PrefabInstantiateOptions
{
    math::Vec3 position{0.f, 0.f, 0.f};
    math::Quat rotation = math::Quat::Identity();
    math::Vec3 scale{1.f, 1.f, 1.f};
    bool active = true;
    script::ScriptRegistry* scriptRegistry = nullptr;
};

[[nodiscard]] PrefabAsset BuildPrefabFromImportedBundle(
    assets::ImportedAssetBundle bundle,
    assets::AssetRegistry& registry,
    const PrefabBuildOptions& options = {});

[[nodiscard]] PrefabAsset BuildPrefabFromWorldEntity(
    const ecs::World& world,
    EntityID root,
    const std::string& name = {},
    const assets::AssetRegistry* registry = nullptr,
    const script::ScriptRegistry* scriptRegistry = nullptr);

[[nodiscard]] PrefabInstance InstantiatePrefab(
    ecs::World& world,
    const PrefabAsset& prefab,
    const PrefabInstantiateOptions& options = {});

void ResetPrefabInstance(
    ecs::World& world,
    const PrefabAsset& prefab,
    const PrefabInstance& instance,
    const PrefabInstantiateOptions& options = {});

void DestroyPrefabInstance(
    ecs::World& world,
    const PrefabInstance& instance);

[[nodiscard]] std::string SerializePrefabToJson(const PrefabAsset& prefab);
[[nodiscard]] bool DeserializePrefabFromJson(
    const std::string& json,
    assets::AssetRegistry& registry,
    PrefabAsset& outPrefab,
    std::string* outError = nullptr);

// Zerstoert eine Prefab-Instanz ausgehend von der Root-Entity (BFS ueber Children).
// Sicherer als DestroyPrefabInstance wenn kein PrefabInstance-Handle mehr vorliegt.
void DestroyPrefabInstanceFromRoot(
    ecs::World& world,
    EntityID    root);

[[nodiscard]] bool SavePrefabToFile(
    const PrefabAsset& prefab,
    const std::filesystem::path& path,
    std::string* outError = nullptr);

[[nodiscard]] bool LoadPrefabFromFile(
    const std::filesystem::path& path,
    assets::AssetRegistry& registry,
    PrefabAsset& outPrefab,
    std::string* outError = nullptr);

} // namespace engine::addons::prefab
