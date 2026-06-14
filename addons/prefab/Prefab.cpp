// =============================================================================
// KROM Engine - addons/prefab/Prefab.cpp
// =============================================================================
#include "Prefab.hpp"

#include "addons/animation/AnimationComponents.hpp"
#include "addons/mesh_renderer/MeshRendererComponents.hpp"
#include "addons/script/ScriptList.hpp"
#include "assets/AssetRegistry.hpp"
#include "ecs/Components.hpp"
#include "ecs/World.hpp"
#include "serialization/SceneSerializer.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

namespace engine::addons::prefab {
namespace {

[[nodiscard]] std::string AssetPathOrFallback(
    const std::string& sourcePath,
    const char* kind,
    size_t index,
    const std::string& fallbackName)
{
    if (!sourcePath.empty())
        return sourcePath + "#" + kind + "/" + std::to_string(index);
    if (!fallbackName.empty())
        return std::string(kind) + "/" + fallbackName + "/" + std::to_string(index);
    return std::string(kind) + "/" + std::to_string(index);
}

void AddHierarchy(ecs::World& world,
                  EntityID child,
                  EntityID parent)
{
    if (!child.IsValid() || !parent.IsValid() || child == parent)
        return;

    world.Add<ParentComponent>(child, ParentComponent{ parent });
    if (!world.Has<ChildrenComponent>(parent))
        world.Add<ChildrenComponent>(parent);
    world.Get<ChildrenComponent>(parent)->Add(child);
}

void ApplyRecordComponents(ecs::World& world,
                           EntityID entity,
                           const PrefabEntityRecord& record,
                           bool active,
                           script::ScriptRegistry* scriptRegistry)
{
    world.Add<ActiveComponent>(entity, ActiveComponent{ active && record.active });

    auto& transform = world.Add<TransformComponent>(entity);
    transform.localPosition = record.localPosition;
    transform.localRotation = record.localRotation;
    transform.localScale = record.localScale;
    transform.dirty = true;
    ++transform.localVersion;

    world.Add<WorldTransformComponent>(entity);

    if (!record.name.empty())
        world.Add<NameComponent>(entity, record.name);

    if (record.mesh.IsValid() || !record.meshAssetPath.empty())
    {
        MeshComponent mc{ record.mesh };
        mc.meshAssetPath = record.meshAssetPath;
        world.Add<MeshComponent>(entity, std::move(mc));
        const bool hasMaterial = record.material.IsValid() ||
                                  !record.materialAssetPath.empty() ||
                                  !record.baseColorTexturePath.empty() ||
                                  !record.slotOverrides.empty();
        if (hasMaterial)
        {
            MaterialComponent materialComponent{};
            materialComponent.material              = record.material;
            materialComponent.materialAssetPath     = record.materialAssetPath;
            materialComponent.baseColorTexturePath  = record.baseColorTexturePath;
            for (const PrefabEntityRecord::SlotOverride& so : record.slotOverrides)
            {
                MaterialComponent::SlotOverride compSlot{};
                compSlot.submeshIndex      = so.submeshIndex;
                compSlot.material          = so.material;
                compSlot.materialAssetPath = so.materialAssetPath;
                materialComponent.slotOverrides.push_back(std::move(compSlot));
            }
            world.Add<MaterialComponent>(entity, std::move(materialComponent));
        }
    }

    if (record.addBounds)
    {
        BoundsComponent bounds{};
        bounds.centerLocal = record.boundsCenterLocal;
        bounds.extentsLocal = record.boundsExtentsLocal;
        bounds.localDirty = true;
        world.Add<BoundsComponent>(entity, bounds);
    }

    if (record.addObb)
    {
        OBBComponent obb{};
        obb.centerOffset = record.obbCenterOffset;
        obb.halfExtents = record.obbHalfExtents;
        obb.orientation = record.obbOrientation;
        obb.showInEditor = record.showObbInEditor;
        world.Add<OBBComponent>(entity, obb);
    }

    if (record.skeleton.IsValid())
    {
        SkinComponent skin{};
        skin.skeleton = record.skeleton;
        world.Add<SkinComponent>(entity, std::move(skin));
    }

    if (record.animation.IsValid() && record.playAnimation)
    {
        AnimationPlayerComponent player{};
        player.SetClip(record.animation);
        player.looping = record.loopAnimation;
        player.Play();
        world.Add<AnimationPlayerComponent>(entity, player);
    }

    if (!record.scripts.empty())
    {
        script::ScriptList scripts;
        scripts.SetOwnerEntity(entity);
        for (const PrefabEntityRecord::ScriptRecord& scriptRecord : record.scripts)
        {
            const std::string instanceName = scriptRecord.instanceName.empty()
                ? scriptRecord.className
                : scriptRecord.instanceName;
            if (scriptRegistry && scripts.Add(scriptRecord.className, entity, *scriptRegistry))
            {
                script::ScriptInstance& inst = scripts.Instances_Mutable().back();
                inst.instanceName = instanceName;
                inst.fieldValues = scriptRecord.fieldValues;
                scripts.ApplyStoredFieldValues(scripts.Count() - 1u, *scriptRegistry);
            }
            else
            {
                scripts.AddNameOnly(scriptRecord.className, instanceName);
                scripts.Instances_Mutable().back().fieldValues = scriptRecord.fieldValues;
            }
        }
        if (!scripts.IsEmpty())
            world.Add<script::ScriptList>(entity, std::move(scripts));
    }
}

[[nodiscard]] MaterialHandle DefaultMaterialForMesh(
    const assets::MeshAsset& mesh,
    const std::vector<MaterialHandle>& materials)
{
    if (mesh.submeshes.empty())
        return MaterialHandle{};

    const uint32_t materialIndex = mesh.submeshes[0].materialIndex;
    if (materialIndex < materials.size())
        return materials[materialIndex];
    return MaterialHandle{};
}

[[nodiscard]] bool MeshUsesMultipleMaterialSlots(const assets::MeshAsset& mesh)
{
    std::vector<uint32_t> usedMaterialIndices;
    usedMaterialIndices.reserve(mesh.submeshes.size());

    for (const auto& submesh : mesh.submeshes)
    {
        if (std::find(usedMaterialIndices.begin(), usedMaterialIndices.end(), submesh.materialIndex) ==
            usedMaterialIndices.end())
        {
            usedMaterialIndices.push_back(submesh.materialIndex);
            if (usedMaterialIndices.size() > 1u)
                return true;
        }
    }

    return false;
}

template<typename Store, typename HandleT>
[[nodiscard]] HandleT FindAssetByPath(Store& store, const std::string& path)
{
    if (path.empty())
        return {};

    HandleT found{};
    store.ForEach([&](HandleT handle, const auto& asset) {
        if (!found.IsValid() && asset.path == path)
            found = handle;
    });
    return found;
}

[[nodiscard]] std::string PathForMesh(assets::AssetRegistry& registry, MeshHandle handle)
{
    const auto* asset = registry.meshes.Get(handle);
    return asset ? asset->path : std::string{};
}

[[nodiscard]] std::string PathForMaterial(assets::AssetRegistry& registry, MaterialHandle handle)
{
    const auto* asset = registry.materials.Get(handle);
    return asset ? asset->path : std::string{};
}

[[nodiscard]] std::string PathForSkeleton(assets::AssetRegistry& registry, SkeletonHandle handle)
{
    const auto* asset = registry.skeletons.Get(handle);
    return asset ? asset->path : std::string{};
}

[[nodiscard]] std::string PathForAnimation(assets::AssetRegistry& registry, AnimationHandle handle)
{
    const auto* asset = registry.animations.Get(handle);
    return asset ? asset->path : std::string{};
}

[[nodiscard]] const serialization::JsonValue* OptionalObjectField(
    const serialization::JsonValue& object,
    const char* key)
{
    const serialization::JsonValue* value = object.Get(key);
    return value && !value->IsNull() ? value : nullptr;
}

void WriteScriptFieldValue(serialization::JsonWriter& w,
                           const std::string& name,
                           const script::ScriptFieldValue& value)
{
    switch (value.type)
    {
        case script::ScriptFieldType::Float: w.WriteFloat(name, value.floatValue); break;
        case script::ScriptFieldType::Int:   w.WriteInt(name, value.intValue); break;
        case script::ScriptFieldType::Bool:  w.WriteBool(name, value.boolValue); break;
        case script::ScriptFieldType::Vec3:  w.WriteVec3(name, value.vec3Value); break;
        case script::ScriptFieldType::Entity:
        case script::ScriptFieldType::String:
        case script::ScriptFieldType::Prefab:
            w.WriteString(name, value.stringValue);
            break;
    }
}

[[nodiscard]] const char* ScriptFieldTypeName(script::ScriptFieldType type) noexcept
{
    switch (type)
    {
        case script::ScriptFieldType::Float: return "Float";
        case script::ScriptFieldType::Int: return "Int";
        case script::ScriptFieldType::Bool: return "Bool";
        case script::ScriptFieldType::Vec3: return "Vec3";
        case script::ScriptFieldType::String: return "String";
        case script::ScriptFieldType::Prefab: return "Prefab";
        case script::ScriptFieldType::Entity: return "Entity";
    }
    return "Float";
}

[[nodiscard]] bool ParseScriptFieldType(std::string_view name, script::ScriptFieldType& outType) noexcept
{
    if (name == "Float") { outType = script::ScriptFieldType::Float; return true; }
    if (name == "Int") { outType = script::ScriptFieldType::Int; return true; }
    if (name == "Bool") { outType = script::ScriptFieldType::Bool; return true; }
    if (name == "Vec3") { outType = script::ScriptFieldType::Vec3; return true; }
    if (name == "String") { outType = script::ScriptFieldType::String; return true; }
    if (name == "Prefab") { outType = script::ScriptFieldType::Prefab; return true; }
    if (name == "Entity") { outType = script::ScriptFieldType::Entity; return true; }
    return false;
}

void WriteScriptFieldObject(serialization::JsonWriter& w,
                            const std::string& name,
                            const script::ScriptFieldValue& value)
{
    w.BeginObject();
    w.WriteString("name", name);
    w.WriteString("type", ScriptFieldTypeName(value.type));
    WriteScriptFieldValue(w, "value", value);
    w.EndObject();
}

[[nodiscard]] bool ReadScriptFieldValue(const serialization::JsonValue& node,
                                        script::ScriptFieldType type,
                                        script::ScriptFieldValue& outValue)
{
    outValue.type = type;
    switch (type)
    {
        case script::ScriptFieldType::Float:
            if (!node.IsNumber()) return false;
            outValue.floatValue = node.AsFloat();
            return true;
        case script::ScriptFieldType::Int:
            if (!node.IsNumber()) return false;
            outValue.intValue = node.AsInt();
            return true;
        case script::ScriptFieldType::Bool:
            if (!node.IsBool()) return false;
            outValue.boolValue = node.AsBool();
            return true;
        case script::ScriptFieldType::Vec3:
            if (!node.IsArray() || node.arrayVal.size() < 3u) return false;
            outValue.vec3Value = node.AsVec3();
            return true;
        case script::ScriptFieldType::String:
        case script::ScriptFieldType::Prefab:
        case script::ScriptFieldType::Entity:
            if (!node.IsString()) return false;
            outValue.stringValue = node.AsString();
            return true;
    }
    return false;
}

void WriteRecord(serialization::JsonWriter& w, const PrefabEntityRecord& record)
{
    w.BeginObject();
    w.WriteString("name", record.name);
    w.WriteInt("parentIndex", record.parentIndex);
    w.WriteVec3("localPosition", record.localPosition);
    w.WriteQuat("localRotation", record.localRotation);
    w.WriteVec3("localScale", record.localScale);
    if (!record.meshAssetPath.empty())
        w.WriteString("meshAssetPath", record.meshAssetPath);
    if (!record.materialAssetPath.empty())
        w.WriteString("materialAssetPath", record.materialAssetPath);
    if (!record.baseColorTexturePath.empty())
        w.WriteString("baseColorTexturePath", record.baseColorTexturePath);
    if (!record.slotOverrides.empty())
    {
        w.BeginArray("slotOverrides");
        for (const PrefabEntityRecord::SlotOverride& so : record.slotOverrides)
        {
            w.BeginObject();
            w.WriteUint("submeshIndex", so.submeshIndex);
            if (!so.materialAssetPath.empty())
                w.WriteString("materialAssetPath", so.materialAssetPath);
            w.EndObject();
        }
        w.EndArray();
    }
    if (!record.skeletonAssetPath.empty())
        w.WriteString("skeletonAssetPath", record.skeletonAssetPath);
    if (!record.animationAssetPath.empty())
        w.WriteString("animationAssetPath", record.animationAssetPath);
    w.WriteBool("active", record.active);
    w.WriteBool("addBounds", record.addBounds);
    if (record.addBounds)
    {
        w.WriteVec3("boundsCenterLocal", record.boundsCenterLocal);
        w.WriteVec3("boundsExtentsLocal", record.boundsExtentsLocal);
    }
    w.WriteBool("addObb", record.addObb);
    if (record.addObb)
    {
        w.WriteVec3("obbCenterOffset", record.obbCenterOffset);
        w.WriteVec3("obbHalfExtents", record.obbHalfExtents);
        w.WriteQuat("obbOrientation", record.obbOrientation);
        w.WriteBool("showObbInEditor", record.showObbInEditor);
    }
    w.WriteBool("playAnimation", record.playAnimation);
    w.WriteBool("loopAnimation", record.loopAnimation);
    if (!record.scripts.empty())
    {
        w.BeginArray("scripts");
        for (const PrefabEntityRecord::ScriptRecord& script : record.scripts)
        {
            w.BeginObject();
            w.WriteString("class", script.className);
            if (!script.instanceName.empty() && script.instanceName != script.className)
                w.WriteString("name", script.instanceName);
            if (!script.fieldValues.empty())
            {
                w.BeginArray("fields");
                for (const auto& [fieldName, fieldValue] : script.fieldValues)
                    WriteScriptFieldObject(w, fieldName, fieldValue);
                w.EndArray();
            }
            w.EndObject();
        }
        w.EndArray();
    }
    w.EndObject();
}

[[nodiscard]] PrefabEntityRecord ReadRecord(
    const serialization::JsonValue& value,
    assets::AssetRegistry& registry)
{
    PrefabEntityRecord record;
    if (const auto* v = OptionalObjectField(value, "name")) record.name = v->AsString();
    if (const auto* v = OptionalObjectField(value, "parentIndex")) record.parentIndex = v->AsInt();
    if (const auto* v = OptionalObjectField(value, "localPosition")) record.localPosition = v->AsVec3();
    if (const auto* v = OptionalObjectField(value, "localRotation")) record.localRotation = v->AsQuat();
    if (const auto* v = OptionalObjectField(value, "localScale")) record.localScale = v->AsVec3();

    if (const auto* v = OptionalObjectField(value, "meshAssetPath"))
        record.meshAssetPath = v->AsString();
    if (const auto* v = OptionalObjectField(value, "materialAssetPath"))
        record.materialAssetPath = v->AsString();
    if (const auto* v = OptionalObjectField(value, "baseColorTexturePath"))
        record.baseColorTexturePath = v->AsString();
    if (const auto* slotsVal = value.Get("slotOverrides"))
    {
        if (slotsVal->IsArray())
        {
            for (const serialization::JsonValue& slotVal : slotsVal->arrayVal)
            {
                if (!slotVal.IsObject()) continue;
                PrefabEntityRecord::SlotOverride so;
                if (const auto* v = OptionalObjectField(slotVal, "submeshIndex"))
                    so.submeshIndex = v->AsUint();
                if (const auto* v = OptionalObjectField(slotVal, "materialAssetPath"))
                    so.materialAssetPath = v->AsString();
                so.material = FindAssetByPath<
                    assets::AssetStore<assets::MaterialAsset, MaterialTag>, MaterialHandle>(
                    registry.materials, so.materialAssetPath);
                record.slotOverrides.push_back(std::move(so));
            }
        }
    }
    if (const auto* v = OptionalObjectField(value, "skeletonAssetPath"))
        record.skeletonAssetPath = v->AsString();
    if (const auto* v = OptionalObjectField(value, "animationAssetPath"))
        record.animationAssetPath = v->AsString();

    record.mesh = FindAssetByPath<assets::AssetStore<assets::MeshAsset, MeshTag>, MeshHandle>(
        registry.meshes, record.meshAssetPath);
    record.material = FindAssetByPath<assets::AssetStore<assets::MaterialAsset, MaterialTag>, MaterialHandle>(
        registry.materials, record.materialAssetPath);
    record.skeleton = FindAssetByPath<assets::AssetStore<assets::SkeletonAsset, SkeletonTag>, SkeletonHandle>(
        registry.skeletons, record.skeletonAssetPath);
    record.animation = FindAssetByPath<assets::AssetStore<assets::AnimationClip, AnimationTag>, AnimationHandle>(
        registry.animations, record.animationAssetPath);

    if (const auto* v = OptionalObjectField(value, "active")) record.active = v->AsBool();
    if (const auto* v = OptionalObjectField(value, "addBounds")) record.addBounds = v->AsBool();
    if (const auto* v = OptionalObjectField(value, "boundsCenterLocal")) record.boundsCenterLocal = v->AsVec3();
    if (const auto* v = OptionalObjectField(value, "boundsExtentsLocal")) record.boundsExtentsLocal = v->AsVec3();
    if (const auto* v = OptionalObjectField(value, "addObb")) record.addObb = v->AsBool();
    if (const auto* v = OptionalObjectField(value, "obbCenterOffset")) record.obbCenterOffset = v->AsVec3();
    if (const auto* v = OptionalObjectField(value, "obbHalfExtents")) record.obbHalfExtents = v->AsVec3();
    if (const auto* v = OptionalObjectField(value, "obbOrientation")) record.obbOrientation = v->AsQuat();
    if (const auto* v = OptionalObjectField(value, "showObbInEditor")) record.showObbInEditor = v->AsBool();
    if (const auto* v = OptionalObjectField(value, "playAnimation")) record.playAnimation = v->AsBool();
    if (const auto* v = OptionalObjectField(value, "loopAnimation")) record.loopAnimation = v->AsBool();
    if (const auto* scriptsVal = value.Get("scripts"))
    {
        if (scriptsVal->IsArray())
        {
            for (const serialization::JsonValue& scriptVal : scriptsVal->arrayVal)
            {
                if (!scriptVal.IsObject()) continue;
                PrefabEntityRecord::ScriptRecord scriptRecord;
                if (const auto* v = OptionalObjectField(scriptVal, "class"))
                    scriptRecord.className = v->AsString();
                if (scriptRecord.className.empty())
                    continue;
                if (const auto* v = OptionalObjectField(scriptVal, "name"))
                    scriptRecord.instanceName = v->AsString();
                if (scriptRecord.instanceName.empty())
                    scriptRecord.instanceName = scriptRecord.className;

                if (const auto* fieldsVal = scriptVal.Get("fields"))
                {
                    if (fieldsVal->IsArray())
                    {
                        for (const serialization::JsonValue& fieldVal : fieldsVal->arrayVal)
                        {
                            if (!fieldVal.IsObject()) continue;
                            const auto* nameVal = OptionalObjectField(fieldVal, "name");
                            const auto* typeVal = OptionalObjectField(fieldVal, "type");
                            const auto* valueVal = OptionalObjectField(fieldVal, "value");
                            if (!nameVal || !typeVal || !valueVal) continue;

                            script::ScriptFieldType fieldType{};
                            if (!ParseScriptFieldType(typeVal->AsString(), fieldType))
                                continue;

                            script::ScriptFieldValue fieldValue{};
                            if (ReadScriptFieldValue(*valueVal, fieldType, fieldValue))
                                scriptRecord.fieldValues[nameVal->AsString()] = std::move(fieldValue);
                        }
                    }
                }
                record.scripts.push_back(std::move(scriptRecord));
            }
        }
    }
    return record;
}

void SetError(std::string* outError, std::string message)
{
    if (outError)
        *outError = std::move(message);
}

} // namespace

int32_t PrefabAsset::FindEntity(std::string_view entityName) const noexcept
{
    for (size_t i = 0; i < records.size(); ++i)
    {
        if (records[i].name == entityName)
            return static_cast<int32_t>(i);
    }
    return -1;
}

PrefabAsset BuildPrefabFromImportedBundle(
    assets::ImportedAssetBundle bundle,
    assets::AssetRegistry& registry,
    const PrefabBuildOptions& options)
{
    PrefabAsset prefab;
    prefab.name = options.name.empty() ? std::string("Prefab") : options.name;

    const std::string sourcePath =
        !bundle.meshes.empty() ? bundle.meshes[0].path :
        !bundle.animations.empty() ? bundle.animations[0].path :
        !bundle.skeletons.empty() ? bundle.skeletons[0].path :
        prefab.name;

    std::vector<MaterialHandle> materialHandles;
    materialHandles.reserve(bundle.materials.size());
    for (size_t i = 0; i < bundle.materials.size(); ++i)
    {
        auto material = std::make_unique<assets::MaterialAsset>(std::move(bundle.materials[i]));
        material->path = AssetPathOrFallback(sourcePath, "material", i, material->debugName);
        material->state = assets::AssetState::Loaded;
        material->gpuStatus.dirty = true;
        material->gpuStatus.uploaded = false;
        const std::string path = material->path;
        materialHandles.push_back(registry.GetOrAddMaterial(path, std::move(material)));
    }

    std::vector<SkeletonHandle> skeletonHandles;
    skeletonHandles.reserve(bundle.skeletons.size());
    for (size_t i = 0; i < bundle.skeletons.size(); ++i)
    {
        auto skeleton = std::make_unique<assets::SkeletonAsset>(std::move(bundle.skeletons[i]));
        skeleton->path = AssetPathOrFallback(sourcePath, "skeleton", i, skeleton->debugName);
        skeleton->state = assets::AssetState::Loaded;
        const std::string path = skeleton->path;
        skeletonHandles.push_back(registry.GetOrAddSkeleton(path, std::move(skeleton)));
    }

    std::vector<AnimationHandle> animationHandles;
    animationHandles.reserve(bundle.animations.size());
    for (size_t i = 0; i < bundle.animations.size(); ++i)
    {
        auto animation = std::make_unique<assets::AnimationClip>(std::move(bundle.animations[i]));
        animation->path = AssetPathOrFallback(sourcePath, "animation", i, animation->name);
        animation->state = assets::AssetState::Loaded;
        const std::string path = animation->path;
        animationHandles.push_back(registry.GetOrAddAnimation(path, std::move(animation)));
    }

    std::vector<MeshHandle> meshHandles;
    std::vector<MaterialHandle> meshDefaultMaterials;
    meshHandles.reserve(bundle.meshes.size());
    meshDefaultMaterials.reserve(bundle.meshes.size());
    for (size_t i = 0; i < bundle.meshes.size(); ++i)
    {
        auto mesh = std::make_unique<assets::MeshAsset>(std::move(bundle.meshes[i]));
        mesh->path = AssetPathOrFallback(sourcePath, "mesh", i, mesh->debugName);
        mesh->state = assets::AssetState::Loaded;
        mesh->gpuStatus.dirty = true;
        mesh->gpuStatus.uploaded = false;
        mesh->materialHandles = materialHandles;

        meshDefaultMaterials.push_back(
            MeshUsesMultipleMaterialSlots(*mesh)
                ? MaterialHandle{}
                : DefaultMaterialForMesh(*mesh, materialHandles));
        const std::string path = mesh->path;
        meshHandles.push_back(registry.ReplaceOrAddMesh(path, std::move(mesh)));
    }

    const AnimationHandle firstAnimation =
        (!animationHandles.empty() && options.playFirstAnimation)
            ? animationHandles[0]
            : AnimationHandle{};

    const bool needsSyntheticRoot =
        options.createSyntheticRoot || bundle.nodes.empty();
    if (needsSyntheticRoot)
    {
        PrefabEntityRecord root;
        root.name = prefab.name;
        root.parentIndex = -1;
        root.animation = firstAnimation;
        root.animationAssetPath = PathForAnimation(registry, firstAnimation);
        root.playAnimation = firstAnimation.IsValid();
        root.loopAnimation = options.loopAnimations;
        prefab.rootIndex = 0u;
        prefab.records.push_back(std::move(root));
    }

    const int32_t nodeOffset = needsSyntheticRoot ? 1 : 0;
    if (!needsSyntheticRoot)
        prefab.rootIndex = 0u;

    for (size_t i = 0; i < bundle.nodes.size(); ++i)
    {
        const assets::ImportedSceneNode& node = bundle.nodes[i];

        PrefabEntityRecord record;
        record.name = node.name;
        record.parentIndex = node.parentIndex >= 0
            ? node.parentIndex + nodeOffset
            : (needsSyntheticRoot ? 0 : -1);
        record.localPosition = node.translation;
        record.localRotation = node.rotation;
        record.localScale = node.scale;

        if (node.meshIndex >= 0 && static_cast<size_t>(node.meshIndex) < meshHandles.size())
        {
            const size_t meshIndex = static_cast<size_t>(node.meshIndex);
            record.mesh = meshHandles[meshIndex];
            if (const auto* meshAsset = registry.meshes.Get(meshHandles[meshIndex]))
                record.meshAssetPath = meshAsset->path;
            record.material = meshDefaultMaterials[meshIndex];
            record.materialAssetPath = PathForMaterial(registry, record.material);
            if (const auto* materialAsset = registry.materials.Get(record.material))
                record.baseColorTexturePath = materialAsset->baseColorTexture.path;
            record.addBounds = true;

            const int32_t mappedSkin =
                meshIndex < bundle.meshSkinIndex.size()
                    ? bundle.meshSkinIndex[meshIndex]
                    : node.skinIndex;
            if (mappedSkin >= 0 && static_cast<size_t>(mappedSkin) < skeletonHandles.size())
            {
                record.skeleton = skeletonHandles[static_cast<size_t>(mappedSkin)];
                record.skeletonAssetPath = PathForSkeleton(registry, record.skeleton);
                record.animation = firstAnimation;
                record.animationAssetPath = PathForAnimation(registry, record.animation);
                record.playAnimation = firstAnimation.IsValid();
                record.loopAnimation = options.loopAnimations;
            }
        }

        prefab.records.push_back(std::move(record));
    }

    if (bundle.nodes.empty())
    {
        for (size_t i = 0; i < meshHandles.size(); ++i)
        {
            PrefabEntityRecord record;
            record.name = prefab.name + "_Mesh_" + std::to_string(i);
            record.parentIndex = 0;
            record.mesh = meshHandles[i];
            if (const auto* meshAsset = registry.meshes.Get(meshHandles[i]))
                record.meshAssetPath = meshAsset->path;
            record.material = meshDefaultMaterials[i];
            record.materialAssetPath = PathForMaterial(registry, record.material);
            if (const auto* materialAsset = registry.materials.Get(record.material))
                record.baseColorTexturePath = materialAsset->baseColorTexture.path;
            record.addBounds = true;

            const int32_t mappedSkin =
                i < bundle.meshSkinIndex.size() ? bundle.meshSkinIndex[i] : -1;
            if (mappedSkin >= 0 && static_cast<size_t>(mappedSkin) < skeletonHandles.size())
            {
                record.skeleton = skeletonHandles[static_cast<size_t>(mappedSkin)];
                record.skeletonAssetPath = PathForSkeleton(registry, record.skeleton);
                record.animation = firstAnimation;
                record.animationAssetPath = PathForAnimation(registry, record.animation);
                record.playAnimation = firstAnimation.IsValid();
                record.loopAnimation = options.loopAnimations;
            }

            prefab.records.push_back(std::move(record));
        }
    }

    return prefab;
}

PrefabAsset BuildPrefabFromWorldEntity(
    const ecs::World& world,
    EntityID root,
    const std::string& name,
    const assets::AssetRegistry* registry,
    const script::ScriptRegistry* scriptRegistry)
{
    PrefabAsset prefab;
    if (!world.IsAlive(root))
        return prefab;

    const auto* rootName = world.Get<NameComponent>(root);
    prefab.name = !name.empty()
        ? name
        : (rootName && !rootName->name.empty() ? rootName->name : std::string("Prefab"));
    prefab.rootIndex = 0u;

    std::vector<EntityID> stack;
    std::vector<int32_t> parentIndices;
    stack.push_back(root);
    parentIndices.push_back(-1);

    while (!stack.empty())
    {
        const EntityID entity = stack.back();
        const int32_t parentIndex = parentIndices.back();
        stack.pop_back();
        parentIndices.pop_back();

        const int32_t currentIndex = static_cast<int32_t>(prefab.records.size());
        PrefabEntityRecord record;
        record.parentIndex = parentIndex;

        if (const auto* nc = world.Get<NameComponent>(entity))
            record.name = nc->name;
        if (record.name.empty())
            record.name = currentIndex == 0 ? prefab.name : ("Entity_" + std::to_string(currentIndex));

        if (const auto* active = world.Get<ActiveComponent>(entity))
            record.active = active->active;

        if (const auto* transform = world.Get<TransformComponent>(entity))
        {
            if (currentIndex == 0)
            {
                // Root-Record: Position und Rotation auf Null-Zustand setzen.
                // Das Prefab definiert die Vorlage, nicht die Szenen-Platzierung.
                // Instanzen erhalten ihre Position beim Spawnen ueber PrefabInstantiateOptions.
                record.localPosition = {0.f, 0.f, 0.f};
                record.localRotation = math::Quat::Identity();
                record.localScale    = transform->localScale; // Scale beibehalten (z.B. skaliertes Modell)
            }
            else
            {
                record.localPosition = transform->localPosition;
                record.localRotation = transform->localRotation;
                record.localScale    = transform->localScale;
            }
        }

        if (const auto* mesh = world.Get<MeshComponent>(entity))
        {
            record.mesh = mesh->mesh;
            record.meshAssetPath = mesh->meshAssetPath;
        }

        if (const auto* material = world.Get<MaterialComponent>(entity))
        {
            record.material = material->material;
            record.materialAssetPath = material->materialAssetPath;
            record.baseColorTexturePath = material->baseColorTexturePath;
            if (registry && record.material.IsValid())
            {
                if (const auto* materialAsset = registry->materials.Get(record.material))
                {
                    if (record.materialAssetPath.empty())
                        record.materialAssetPath = materialAsset->path;
                    if (record.baseColorTexturePath.empty())
                        record.baseColorTexturePath = materialAsset->baseColorTexture.path;
                }
            }

            // Per-Slot-Overrides übernehmen
            for (const auto& slot : material->slotOverrides)
            {
                PrefabEntityRecord::SlotOverride so;
                so.submeshIndex      = slot.submeshIndex;
                so.material          = slot.material;
                so.materialAssetPath = slot.materialAssetPath;
                if (so.materialAssetPath.empty() && registry && slot.material.IsValid())
                {
                    if (const auto* matAsset = registry->materials.Get(slot.material))
                        so.materialAssetPath = matAsset->path;
                }
                record.slotOverrides.push_back(std::move(so));
            }
        }
        if (registry && record.mesh.IsValid() &&
            (!record.material.IsValid() || record.materialAssetPath.empty() || record.baseColorTexturePath.empty()))
        {
            if (const auto* meshAsset = registry->meshes.Get(record.mesh))
            {
                MaterialHandle meshMaterial = MaterialHandle::Invalid();
                if (!meshAsset->submeshes.empty())
                {
                    const uint32_t materialIndex = meshAsset->submeshes.front().materialIndex;
                    if (materialIndex < meshAsset->materialHandles.size())
                        meshMaterial = meshAsset->materialHandles[materialIndex];
                }
                if (!meshMaterial.IsValid() && !meshAsset->materialHandles.empty())
                    meshMaterial = meshAsset->materialHandles.front();

                if (const auto* materialAsset = registry->materials.Get(meshMaterial))
                {
                    if (!record.material.IsValid())
                        record.material = meshMaterial;
                    if (record.materialAssetPath.empty())
                        record.materialAssetPath = materialAsset->path;
                    // baseColorTexturePath nur setzen, wenn kein expliziter .kmat-Pfad vorhanden.
                    // Ein zugewiesenes .kmat enthält Textur-Bindings intern — der Fallback-Pfad
                    // würde beim Laden das .kmat durch ein neues LegacyLit-Material ersetzen.
                    const bool hasMaterialAssetFile =
                        !record.materialAssetPath.empty() &&
                        record.materialAssetPath.find('#') == std::string::npos;
                    if (record.baseColorTexturePath.empty() && !hasMaterialAssetFile)
                        record.baseColorTexturePath = materialAsset->baseColorTexture.path;
                }
            }
        }

        if (const auto* bounds = world.Get<BoundsComponent>(entity))
        {
            record.addBounds = true;
            record.boundsCenterLocal = bounds->centerLocal;
            record.boundsExtentsLocal = bounds->extentsLocal;
        }

        if (const auto* obb = world.Get<OBBComponent>(entity))
        {
            record.addObb = true;
            record.obbCenterOffset = obb->centerOffset;
            record.obbHalfExtents = obb->halfExtents;
            record.obbOrientation = obb->orientation;
            record.showObbInEditor = obb->showInEditor;
        }

        if (const auto* skin = world.Get<SkinComponent>(entity))
            record.skeleton = skin->skeleton;

        if (const auto* player = world.Get<AnimationPlayerComponent>(entity))
        {
            record.animation = player->clip;
            record.playAnimation = player->playing;
            record.loopAnimation = player->looping;
        }

        if (const auto* scriptList = world.Get<script::ScriptList>(entity))
        {
            for (const script::ScriptInstance& inst : scriptList->Instances())
            {
                if (inst.className.empty())
                    continue;

                PrefabEntityRecord::ScriptRecord scriptRecord;
                scriptRecord.className = inst.className;
                scriptRecord.instanceName = inst.instanceName.empty()
                    ? inst.className
                    : inst.instanceName;
                scriptRecord.fieldValues = inst.fieldValues;

                if (scriptRegistry && inst.script)
                {
                    if (const auto* fields = scriptRegistry->GetFields(inst.className))
                    {
                        for (const script::ScriptFieldMeta& field : *fields)
                        {
                            script::ScriptFieldValue value{};
                            if (scriptRegistry->ReadField(*inst.script, inst.className, field.name, value))
                            {
                                if (value.type == script::ScriptFieldType::Entity &&
                                    value.entityValue.IsValid())
                                {
                                    if (const auto* guid = world.Get<GuidComponent>(value.entityValue))
                                        value.stringValue = guid->guid;
                                }
                                scriptRecord.fieldValues[field.name] = std::move(value);
                            }
                        }
                    }
                }

                record.scripts.push_back(std::move(scriptRecord));
            }
        }

        prefab.records.push_back(std::move(record));

        if (const auto* children = world.Get<ChildrenComponent>(entity))
        {
            for (auto it = children->children.rbegin(); it != children->children.rend(); ++it)
            {
                if (world.IsAlive(*it))
                {
                    stack.push_back(*it);
                    parentIndices.push_back(currentIndex);
                }
            }
        }
    }

    return prefab;
}

PrefabInstance InstantiatePrefab(
    ecs::World& world,
    const PrefabAsset& prefab,
    const PrefabInstantiateOptions& options)
{
    PrefabInstance instance;
    if (prefab.records.empty())
        return instance;

    instance.entities.assign(prefab.records.size(), NULL_ENTITY);

    for (size_t i = 0; i < prefab.records.size(); ++i)
    {
        const PrefabEntityRecord& record = prefab.records[i];
        const EntityID entity = world.CreateEntity();
        instance.entities[i] = entity;

        ApplyRecordComponents(world, entity, record, options.active, options.scriptRegistry);
    }

    for (size_t i = 0; i < prefab.records.size(); ++i)
    {
        const int32_t parentIndex = prefab.records[i].parentIndex;
        if (parentIndex >= 0 && static_cast<size_t>(parentIndex) < instance.entities.size())
            AddHierarchy(world, instance.entities[i], instance.entities[static_cast<size_t>(parentIndex)]);
    }

    if (prefab.rootIndex < instance.entities.size())
    {
        instance.root = instance.entities[prefab.rootIndex];
        if (auto* transform = world.Get<TransformComponent>(instance.root))
        {
            transform->localPosition = options.position;
            transform->localRotation = options.rotation;
            transform->localScale = options.scale;
            transform->dirty = true;
            ++transform->localVersion;
        }
    }

    return instance;
}

void ResetPrefabInstance(
    ecs::World& world,
    const PrefabAsset& prefab,
    const PrefabInstance& instance,
    const PrefabInstantiateOptions& options)
{
    const size_t count = std::min(prefab.records.size(), instance.entities.size());
    for (size_t i = 0; i < count; ++i)
    {
        const EntityID entity = instance.entities[i];
        if (!world.IsAlive(entity))
            continue;

        const PrefabEntityRecord& record = prefab.records[i];

        if (auto* active = world.Get<ActiveComponent>(entity))
            active->active = options.active && record.active;

        if (auto* transform = world.Get<TransformComponent>(entity))
        {
            transform->localPosition = record.localPosition;
            transform->localRotation = record.localRotation;
            transform->localScale = record.localScale;
            transform->dirty = true;
            ++transform->localVersion;
        }

        if (auto* player = world.Get<AnimationPlayerComponent>(entity))
        {
            player->currentTime = 0.f;
            player->bindingsDirty = true;
            player->looping = record.loopAnimation;
            player->playing = record.playAnimation;
        }
    }

    if (prefab.rootIndex < instance.entities.size())
    {
        if (auto* transform = world.Get<TransformComponent>(instance.entities[prefab.rootIndex]))
        {
            transform->localPosition = options.position;
            transform->localRotation = options.rotation;
            transform->localScale = options.scale;
            transform->dirty = true;
            ++transform->localVersion;
        }
    }
}

void DestroyPrefabInstance(
    ecs::World& world,
    const PrefabInstance& instance)
{
    for (auto it = instance.entities.rbegin(); it != instance.entities.rend(); ++it)
    {
        if (world.IsAlive(*it))
            world.DestroyEntity(*it);
    }
}

void DestroyPrefabInstanceFromRoot(ecs::World& world, EntityID root)
{
    if (!root.IsValid() || !world.IsAlive(root))
        return;

    // BFS sammeln, dann rueckwaerts zerstoeren (Kinder vor Eltern).
    std::vector<EntityID> toDestroy;
    std::vector<EntityID> stack{ root };
    while (!stack.empty())
    {
        EntityID e = stack.back();
        stack.pop_back();
        if (!world.IsAlive(e))
            continue;
        toDestroy.push_back(e);
        if (const auto* children = world.Get<ChildrenComponent>(e))
            for (EntityID child : children->children)
                if (world.IsAlive(child))
                    stack.push_back(child);
    }

    for (auto it = toDestroy.rbegin(); it != toDestroy.rend(); ++it)
        if (world.IsAlive(*it))
            world.DestroyEntity(*it);
}

std::string SerializePrefabToJson(const PrefabAsset& prefab)
{
    serialization::JsonWriter w;
    w.BeginObject();
    w.WriteString("type", "KromPrefab");
    w.WriteUint("version", 1u);
    w.WriteString("name", prefab.name);
    w.WriteUint("rootIndex", prefab.rootIndex);
    w.BeginArray("records");
    for (const PrefabEntityRecord& record : prefab.records)
        WriteRecord(w, record);
    w.EndArray();
    w.EndObject();
    return w.GetString();
}

bool DeserializePrefabFromJson(
    const std::string& json,
    assets::AssetRegistry& registry,
    PrefabAsset& outPrefab,
    std::string* outError)
{
    std::string parseError;
    const serialization::JsonValue root = serialization::JsonParser::Parse(json, parseError);
    if (!parseError.empty())
    {
        SetError(outError, "JSON parse error: " + parseError);
        return false;
    }
    if (!root.IsObject())
    {
        SetError(outError, "Prefab root is not an object");
        return false;
    }

    const auto* records = root.Get("records");
    if (!records || !records->IsArray())
    {
        SetError(outError, "Prefab has no records array");
        return false;
    }

    PrefabAsset prefab;
    if (const auto* v = OptionalObjectField(root, "name"))
        prefab.name = v->AsString();
    if (prefab.name.empty())
        prefab.name = "Prefab";
    if (const auto* v = OptionalObjectField(root, "rootIndex"))
        prefab.rootIndex = v->AsUint();

    prefab.records.reserve(records->arrayVal.size());
    for (const serialization::JsonValue& recordValue : records->arrayVal)
    {
        if (!recordValue.IsObject())
            continue;
        prefab.records.push_back(ReadRecord(recordValue, registry));
    }

    if (prefab.records.empty())
    {
        SetError(outError, "Prefab contains no entity records");
        return false;
    }
    if (prefab.rootIndex >= prefab.records.size())
    {
        SetError(outError, "Prefab rootIndex is outside records");
        return false;
    }

    outPrefab = std::move(prefab);
    SetError(outError, {});
    return true;
}

bool SavePrefabToFile(
    const PrefabAsset& prefab,
    const std::filesystem::path& path,
    std::string* outError)
{
    std::error_code ec;
    if (!path.parent_path().empty())
        std::filesystem::create_directories(path.parent_path(), ec);
    if (ec)
    {
        SetError(outError, "Could not create prefab directory: " + ec.message());
        return false;
    }

    std::ofstream out(path, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!out)
    {
        SetError(outError, "Could not open prefab file for writing: " + path.string());
        return false;
    }

    out << SerializePrefabToJson(prefab);
    if (!out)
    {
        SetError(outError, "Could not write prefab file: " + path.string());
        return false;
    }

    SetError(outError, {});
    return true;
}

bool LoadPrefabFromFile(
    const std::filesystem::path& path,
    assets::AssetRegistry& registry,
    PrefabAsset& outPrefab,
    std::string* outError)
{
    std::ifstream in(path, std::ios::in | std::ios::binary);
    if (!in)
    {
        SetError(outError, "Could not open prefab file for reading: " + path.string());
        return false;
    }

    const std::string json((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    return DeserializePrefabFromJson(json, registry, outPrefab, outError);
}

} // namespace engine::addons::prefab
