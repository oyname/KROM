#include "Prefab.hpp"
#include "assets/MaterialProperties.hpp"
#include "PrefabInstanceComponent.hpp"
#include "PrefabSerialization.hpp"
#include "addons/animation/AnimationComponents.hpp"
#include "addons/mesh_renderer/MeshRendererComponents.hpp"
#include "addons/script/ScriptList.hpp"
#include "addons/script/ScriptSerialization.hpp"
#include "ecs/Components.hpp"
#include "ecs/World.hpp"
#include "serialization/SceneSerializer.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <unordered_set>

namespace engine::addons::prefab {
namespace {
using serialization::JsonValue;
using serialization::JsonType;

std::string Id(const PrefabAsset& prefab, size_t index)
{
    return prefab.records[index].id.empty() ? "legacy-" + std::to_string(index) : prefab.records[index].id;
}

JsonValue Parse(const std::string& text)
{
    std::string error;
    return serialization::JsonParser::Parse(text, error);
}

JsonValue MaterialJson(const std::string& source, const std::string& path, assets::AssetRegistry& registry)
{
    assets::MaterialAsset defaults;
    registry.materials.ForEach([&](MaterialHandle, assets::MaterialAsset& asset) {
        if (!path.empty() && asset.path == path) defaults = asset;
    });
    auto lines = assets::MaterialPropertyLines(assets::SerializeMaterialProperties(defaults));
    for (const auto& [key,line] : assets::MaterialPropertyLines(source)) lines[key]=line;
    JsonValue result; result.type = JsonType::Object;
    for (const auto& [key,line] : lines) {
        JsonValue value; value.type=JsonType::String; value.strVal=line;
        result.objectVal.emplace_back(key,std::move(value));
    }
    return result;
}

void TransformMaterialJson(JsonValue& record, assets::AssetRegistry* registry)
{
    auto transform = [&](JsonValue& object) {
        std::string source,path;
        for (auto& [key,value] : object.objectVal) {
            if (key=="materialAssetPath") path=value.strVal;
            if (key=="materialOverrideSource") source=value.strVal;
        }
        if (registry) {
            if (path.empty() && source.empty()) return;
            auto value=MaterialJson(source,path,*registry);
            for (auto& [key,field] : object.objectVal)
                if (key=="materialOverrideSource") { field=std::move(value); return; }
            object.objectVal.emplace_back("materialOverrideSource",std::move(value));
        } else {
            for (auto& [key,value] : object.objectVal)
                if (key=="materialOverrideSource" && value.IsObject()) {
                    std::string text;
                    for (const auto& [property,line] : value.objectVal) text+=line.strVal+"\n";
                    value=JsonValue{}; value.type=JsonType::String; value.strVal=std::move(text);
                }
        }
    };
    transform(record);
    for (auto& [key,value] : record.objectVal)
        if (key=="slotOverrides") for (auto& slot : value.arrayVal) transform(slot);
}

JsonValue RecordJson(const PrefabEntityRecord& record, assets::AssetRegistry& registry)
{
    PrefabAsset asset;
    asset.records.push_back(record);
    auto result=Parse(SerializePrefabToJson(asset)).Get("records")->At(0);
    TransformMaterialJson(result,&registry);
    return result;
}

bool Equal(const JsonValue& a, const JsonValue& b)
{
    if (a.type != b.type) return false;
    switch (a.type)
    {
    case JsonType::Null: return true;
    case JsonType::Bool: return a.boolVal == b.boolVal;
    case JsonType::Number: return a.numVal == b.numVal;
    case JsonType::String: return a.strVal == b.strVal;
    case JsonType::Array:
        if (a.arrayVal.size() != b.arrayVal.size()) return false;
        for (size_t i = 0; i < a.arrayVal.size(); ++i)
            if (!Equal(a.At(i), b.At(i))) return false;
        return true;
    case JsonType::Object:
        if (a.objectVal.size() != b.objectVal.size()) return false;
        for (const auto& [key, value] : a.objectVal)
            if (!b.Get(key) || !Equal(value, *b.Get(key))) return false;
        return true;
    }
    return false;
}

const JsonValue& Field(const JsonValue& value, const std::string& key)
{
    const auto* field = value.Get(key);
    return field ? *field : JsonValue::Null();
}

std::string ItemKey(const JsonValue& item, const std::string& array)
{
    if (array == "scripts")
    {
        const auto& cls = Field(item, "class").strVal;
        const auto& name = Field(item, "name").strVal;
        return cls + ":" + (name.empty() ? cls : name);
    }
    if (array == "fields") return Field(item, "name").strVal;
    if (array == "slotOverrides") return std::to_string(Field(item, "submeshIndex").AsUint());
    return {};
}

const JsonValue& FindItem(const JsonValue& array, const std::string& key, const std::string& id)
{
    std::unordered_map<std::string, size_t> occurrences;
    for (const auto& item : array.arrayVal)
    {
        const auto raw = ItemKey(item, key);
        const auto identity = raw + "#" + std::to_string(occurrences[raw]++);
        if (identity == id) return item;
    }
    return JsonValue::Null();
}

using Overrides = std::unordered_set<std::string>;

void CollectOverrides(const JsonValue& base, const JsonValue& local, Overrides& paths,
                      const std::string& path = {}, const std::string& key = {})
{
    if (Equal(base, local)) return;
    if (base.IsObject() && local.IsObject())
    {
        std::unordered_set<std::string> visited;
        for (const auto* object : {&base, &local})
            for (const auto& [name, value] : object->objectVal)
                if (visited.insert(name).second && name != "id" && name != "sourceGuid" &&
                    name != "parentIndex" && name != "nestedPrefabJson")
                    CollectOverrides(Field(base, name), Field(local, name), paths, path + "/" + name, name);
        return;
    }
    if ((base.IsArray() || base.IsNull()) && (local.IsArray() || local.IsNull()) &&
        (key == "scripts" || key == "fields" || key == "slotOverrides"))
    {
        std::unordered_set<std::string> visited;
        for (const auto* array : {&base, &local})
        {
            std::unordered_map<std::string, size_t> occurrences;
            for (const auto& item : array->arrayVal)
            {
                const auto raw = ItemKey(item, key);
                const auto id = raw + "#" + std::to_string(occurrences[raw]++);
                if (visited.insert(id).second)
                    CollectOverrides(FindItem(base, key, id), FindItem(local, key, id), paths, path + "/@" + id);
            }
        }
        return;
    }
    paths.insert(path);
}

// Three-way merge: a locally changed property wins; untouched properties inherit.
// Script instances, fields and material slots are matched by identity, not order.
JsonValue Merge(const JsonValue& base, const JsonValue& local, const JsonValue& source,
                const Overrides& overrides, const std::string& key = {}, const std::string& path = {})
{
    if (overrides.count(path)) return local;
    if (Equal(base, source)) return local;
    bool hasDescendantOverride = false;
    for (const auto& overridePath : overrides)
        if (overridePath.compare(0, path.size() + 1u, path + "/") == 0) { hasDescendantOverride = true; break; }
    if (Equal(base, local) && !hasDescendantOverride) return source;
    if (base.IsObject() && local.IsObject() && source.IsObject())
    {
        JsonValue result; result.type = JsonType::Object;
        std::unordered_set<std::string> visited;
        for (const auto* object : {&local, &source})
            for (const auto& [name, ignored] : object->objectVal)
                if (visited.insert(name).second)
                {
                    auto value = Merge(Field(base, name), Field(local, name), Field(source, name), overrides, name, path + "/" + name);
                    if (!value.IsNull()) result.objectVal.emplace_back(name, std::move(value));
                }
        return result;
    }
    if ((base.IsArray() || base.IsNull()) && (local.IsArray() || local.IsNull()) &&
        (source.IsArray() || source.IsNull()) &&
        (key == "scripts" || key == "fields" || key == "slotOverrides"))
    {
        JsonValue result; result.type = JsonType::Array;
        std::unordered_set<std::string> visited;
        for (const auto* array : {&local, &source})
        {
            std::unordered_map<std::string, size_t> occurrences;
            for (const auto& item : array->arrayVal)
            {
                const auto raw = ItemKey(item, key);
                const auto id = raw + "#" + std::to_string(occurrences[raw]++);
                if (!visited.insert(id).second) continue;
                auto value = Merge(FindItem(base, key, id), FindItem(local, key, id), FindItem(source, key, id), overrides, {}, path + "/@" + id);
                if (!value.IsNull()) result.arrayVal.push_back(std::move(value));
            }
        }
        return result.arrayVal.empty() ? JsonValue{} : result;
    }
    return local;
}

void WriteJson(serialization::JsonWriter& writer, const std::string& key, const JsonValue& value)
{
    switch (value.type)
    {
    case JsonType::Object:
        writer.BeginObject(key);
        for (const auto& [name, child] : value.objectVal) WriteJson(writer, name, child);
        writer.EndObject(); break;
    case JsonType::Array:
        writer.BeginArray(key);
        for (const auto& child : value.arrayVal) WriteJson(writer, "", child);
        writer.EndArray(); break;
    case JsonType::String: writer.WriteString(key, value.strVal); break;
    case JsonType::Number:
        if (value.numVal >= 0 && value.numVal <= std::numeric_limits<uint32_t>::max() && std::floor(value.numVal) == value.numVal)
            writer.WriteUint(key, value.AsUint());
        else if (value.numVal >= std::numeric_limits<int32_t>::min() && value.numVal <= std::numeric_limits<int32_t>::max() && std::floor(value.numVal) == value.numVal)
            writer.WriteInt(key, value.AsInt());
        else writer.WriteFloat(key, value.AsFloat());
        break;
    case JsonType::Bool: writer.WriteBool(key, value.boolVal); break;
    case JsonType::Null: break;
    }
}

PrefabEntityRecord ReadMerged(const JsonValue& value, assets::AssetRegistry& registry)
{
    serialization::JsonWriter writer;
    writer.BeginObject(); writer.BeginArray("records");
    auto encoded=value; TransformMaterialJson(encoded,nullptr);
    WriteJson(writer, "", encoded);
    writer.EndArray(); writer.EndObject();
    PrefabAsset asset;
    (void)DeserializePrefabFromJson(writer.GetString(), registry, asset);
    return asset.records.front();
}

template<typename T>
void Set(ecs::World& world, EntityID id, T value)
{
    if (auto* component = world.Get<T>(id)) *component = std::move(value);
    else world.Add<T>(id, std::move(value));
}

void Reparent(ecs::World& world, EntityID entity, EntityID parent)
{
    size_t depth = 0;
    for (auto cursor = parent; cursor.IsValid();)
    {
        if (cursor == entity || ++depth > world.EntityCount()) return;
        const auto* ancestor = world.Get<ParentComponent>(cursor);
        cursor = ancestor ? ancestor->parent : NULL_ENTITY;
    }
    if (const auto* old = world.Get<ParentComponent>(entity))
        if (auto* children = world.Get<ChildrenComponent>(old->parent)) children->Remove(entity);
    if (parent.IsValid() && world.IsAlive(parent))
    {
        Set(world, entity, ParentComponent{parent});
        if (!world.Has<ChildrenComponent>(parent)) world.Add<ChildrenComponent>(parent);
        world.Get<ChildrenComponent>(parent)->Add(entity);
    }
    else if (world.Has<ParentComponent>(entity)) world.Remove<ParentComponent>(entity);
    if (auto* transform = world.Get<TransformComponent>(entity)) { transform->dirty = true; ++transform->localVersion; }
}

PrefabEntityRecord Capture(ecs::World& world, EntityID entity, assets::AssetRegistry& registry,
                           script::ScriptRegistry* scripts)
{
    auto record = BuildPrefabFromWorldEntity(world, entity, {}, &registry, scripts).records.front();
    if (const auto* transform = world.Get<TransformComponent>(entity))
    {
        record.localPosition = transform->localPosition;
        record.localRotation = transform->localRotation;
        record.localScale = transform->localScale;
    }
    return record;
}

void RemapScriptReferences(PrefabEntityRecord& record, const std::unordered_map<std::string, std::string>& remap)
{
    for (auto& script : record.scripts)
        for (auto& [name, value] : script.fieldValues)
            if (value.type == script::ScriptFieldType::Entity)
                if (const auto it = remap.find(value.stringValue); it != remap.end()) value.stringValue = it->second;
}

void ApplyScripts(ecs::World& world, EntityID entity, const PrefabEntityRecord& record,
                   script::ScriptRegistry* registry)
{
    if (!world.Has<script::ScriptList>(entity) && record.scripts.empty()) return;
    if (!world.Has<script::ScriptList>(entity)) world.Add<script::ScriptList>(entity);
    auto* list = world.Get<script::ScriptList>(entity);
    list->SetOwnerEntity(entity);
    auto name = [](const auto& s) { return s.instanceName.empty() ? s.className : s.instanceName; };
    for (size_t i = list->Count(); i-- > 0;)
    {
        const auto& current = list->Instances()[i];
        const auto matches = [&](const auto& wanted) {
            return current.className == wanted.className && name(current) == name(wanted);
        };
        const auto wantedCount = std::count_if(record.scripts.begin(), record.scripts.end(), matches);
        const auto occurrence = std::count_if(list->Instances().begin(), list->Instances().begin() + i, matches);
        if (occurrence >= wantedCount) list->RemoveAt(i, entity);
    }
    std::unordered_map<std::string, size_t> occurrences;
    for (const auto& wanted : record.scripts)
    {
        auto& instances = list->Instances_Mutable();
        const auto identity = wanted.className + ":" + name(wanted);
        size_t occurrence = occurrences[identity]++;
        auto it = std::find_if(instances.begin(), instances.end(), [&](const auto& current) {
            if (current.className != wanted.className || name(current) != name(wanted)) return false;
            if (occurrence > 0u) { --occurrence; return false; }
            return true;
        });
        if (it == instances.end())
        {
            if (!registry || !list->Add(wanted.className, entity, *registry))
                list->AddNameOnly(wanted.className, name(wanted));
            it = std::prev(list->Instances_Mutable().end());
            it->instanceName = name(wanted);
        }
        // The script object, started flag and non-exposed runtime state survive.
        it->fieldValues = wanted.fieldValues;
        if (registry && it->script)
            for (const auto& [field, value] : wanted.fieldValues)
                (void)registry->WriteField(*it->script, it->className, field, value);
    }
    if (list->IsEmpty()) world.Remove<script::ScriptList>(entity);
}

void ApplyChanges(ecs::World& world, EntityID entity, const JsonValue& before,
                   const JsonValue& after, assets::AssetRegistry& registry, script::ScriptRegistry* scripts)
{
    if (Equal(before, after)) return;
    auto changed = [&](std::initializer_list<const char*> keys) {
        for (const auto* key : keys) if (!Equal(Field(before, key), Field(after, key))) return true;
        return false;
    };
    const auto record = ReadMerged(after, registry);
    auto recipe = record;
    recipe.scripts.clear();
    recipe.nestedPrefabJson.clear();
    recipe.parentIndex = -1;
    PrefabAsset asset; asset.records.push_back(recipe);
    ecs::World staging(world.GetComponentMetaRegistry());
    PrefabInstantiateOptions options; options.position = recipe.localPosition;
    options.rotation = recipe.localRotation; options.scale = recipe.localScale;
    options.replaceRootTransform = true;
    const auto staged = InstantiatePrefab(staging, asset, options);
    auto copy = [&]<typename T>(std::initializer_list<const char*> keys) {
        if (!changed(keys)) return;
        if (const auto* c = staging.Get<T>(staged.root)) Set(world, entity, *c);
        else if (world.Has<T>(entity)) world.Remove<T>(entity);
    };
    if (changed({"localPosition", "localRotation", "localScale", "inheritParentScale"}))
    {
        if (!world.Has<TransformComponent>(entity)) world.Add<TransformComponent>(entity);
        auto* t = world.Get<TransformComponent>(entity);
        t->localPosition = record.localPosition; t->localRotation = record.localRotation; t->localScale = record.localScale;
        t->inheritParentScale = record.inheritParentScale;
        t->dirty = true; ++t->localVersion;
    }
    copy.template operator()<NameComponent>({"name"});
    copy.template operator()<ActiveComponent>({"active"});
    copy.template operator()<MeshComponent>({"meshAssetPath", "meshVisible", "castShadows", "receiveShadows", "layerMask"});
    copy.template operator()<MaterialComponent>({"materialAssetPath", "materialOverrideSource", "baseColorTexturePath", "slotOverrides", "materialSubmeshIndex", "meshAssetPath"});
    copy.template operator()<BoundsComponent>({"addBounds", "boundsCenterLocal", "boundsExtentsLocal"});
    copy.template operator()<OBBComponent>({"addObb", "obbCenterOffset", "obbHalfExtents", "obbOrientation", "showObbInEditor", "obbIsTrigger"});
    copy.template operator()<RigidBodyComponent>({"rigidBody"});
    copy.template operator()<SphereColliderComponent>({"sphereCollider"});
    copy.template operator()<CapsuleColliderComponent>({"capsuleCollider"});
    copy.template operator()<CharacterControllerComponent>({"characterController"});
    copy.template operator()<AudioSourceComponent>({"audioSource"});
    copy.template operator()<AudioListenerComponent>({"audioListener"});
    copy.template operator()<ParticleEmitterComponent>({"particleEmitter"});
    copy.template operator()<CameraComponent>({"camera"});
    copy.template operator()<LightComponent>({"light"});
    copy.template operator()<SkinComponent>({"skeletonAssetPath"});
    if (changed({"animationAssetPath", "playAnimation", "loopAnimation"}))
    {
        if (!record.animation.IsValid())
        {
            if (world.Has<AnimationPlayerComponent>(entity)) world.Remove<AnimationPlayerComponent>(entity);
        }
        else
        {
            if (!world.Has<AnimationPlayerComponent>(entity)) world.Add<AnimationPlayerComponent>(entity);
            auto* player = world.Get<AnimationPlayerComponent>(entity);
            player->SetClip(record.animation); player->playing = record.playAnimation; player->looping = record.loopAnimation;
        }
    }
    if (changed({"scripts"})) ApplyScripts(world, entity, record, scripts);
}
} // namespace

void LinkPrefabInstance(ecs::World& world, const PrefabAsset& prefab,
                        const PrefabInstance& instance, const std::string& assetPath, bool overrideTransform)
{
    if (!instance.IsValid()) return;
    RegisterPrefabInstanceComponent(world.GetComponentMetaRegistry());
    PrefabInstanceComponent link;
    link.prefabAssetPath = assetPath; link.overrideTransform = overrideTransform;
    link.baselinePrefabJson = SerializePrefabToJson(prefab);
    for (size_t i = 0; i < instance.entities.size() && i < prefab.records.size(); ++i)
    {
        const auto entity = instance.entities[i];
        if (!world.Has<GuidComponent>(entity))
        {
            static std::atomic<uint64_t> serial{0};
            const auto epoch = std::chrono::high_resolution_clock::now().time_since_epoch().count();
            world.Add<GuidComponent>(entity, GuidComponent{"prefab-link-" + std::to_string(epoch) + "-" + std::to_string(++serial)});
        }
        Set(world, entity, PrefabSourceComponent{Id(prefab, i)});
        link.bindings.push_back({Id(prefab, i), world.Get<GuidComponent>(entity)->guid});
    }
    Set(world, instance.root, std::move(link));
}

bool SynchronizePrefabInstance(ecs::World& world, EntityID root, const PrefabAsset& prefab,
    assets::AssetRegistry& registry, script::ScriptRegistry* scripts, std::string* outError,
    bool preserveLocalOverrides)
{
    const auto* component = world.Get<PrefabInstanceComponent>(root);
    if (!component || prefab.Empty() || prefab.rootIndex >= prefab.records.size()) return false;
    auto link = *component; // Structural changes can move the component.
    PrefabAsset base;
    if (!link.baselinePrefabJson.empty() && !DeserializePrefabFromJson(link.baselinePrefabJson, registry, base, outError)) return false;
    std::unordered_set<std::string> ids;
    for (size_t i = 0; i < prefab.records.size(); ++i)
    {
        const auto& record = prefab.records[i];
        if (!ids.insert(Id(prefab, i)).second || record.parentIndex >= static_cast<int32_t>(prefab.records.size()) ||
            record.parentIndex < -1 || (i != prefab.rootIndex && record.parentIndex < 0))
        { if (outError) *outError = "Invalid prefab identity or hierarchy"; return false; }
        std::unordered_set<int32_t> ancestry;
        int32_t cursor = static_cast<int32_t>(i);
        while (cursor >= 0)
        {
            if (static_cast<size_t>(cursor) >= prefab.records.size() || !ancestry.insert(cursor).second)
            { if (outError) *outError = "Cyclic prefab hierarchy"; return false; }
            cursor = prefab.records[cursor].parentIndex;
        }
    }
    std::unordered_map<std::string, EntityID> guidEntities;
    world.ForEachAlive([&](EntityID id) { if (const auto* guid = world.Get<GuidComponent>(id)) guidEntities[guid->guid] = id; });
    std::unordered_map<std::string, EntityID> entities;
    std::unordered_map<std::string, Overrides> overrides;
    for (const auto& binding : link.bindings)
    {
        if (preserveLocalOverrides)
            overrides[binding.recordId] = Overrides(binding.overriddenProperties.begin(), binding.overriddenProperties.end());
        if (const auto it = guidEntities.find(binding.entityGuid); it != guidEntities.end()) entities[binding.recordId] = it->second;
    }

    // Older scenes have only a path. Bootstrap once using the stored hierarchy.
    if (base.Empty())
    {
        base = prefab;
        std::vector<EntityID> stack{root}; size_t index = 0;
        while (!stack.empty() && index < base.records.size())
        {
            const auto entity = stack.back(); stack.pop_back();
            auto current = Capture(world, entity, registry, scripts);
            current.id = Id(base, index); current.parentIndex = base.records[index].parentIndex;
            base.records[index] = std::move(current);
            entities[Id(base, index)] = entity; ++index;
            if (const auto* children = world.Get<ChildrenComponent>(entity))
                for (auto it = children->children.rbegin(); it != children->children.rend(); ++it) stack.push_back(*it);
        }
    }
    if (Id(base, base.rootIndex) != Id(prefab, prefab.rootIndex))
    { if (outError) *outError = "Prefab root identity changed"; return false; }
    entities[Id(prefab, prefab.rootIndex)] = root;
    std::unordered_map<std::string, size_t> oldIndices;
    for (size_t i = 0; i < base.records.size(); ++i) oldIndices[Id(base, i)] = i;
    std::unordered_map<std::string, std::string> baselineReferences, localReferences, sourceReferences;
    for (size_t i = 0; i < base.records.size(); ++i)
    {
        const auto key = "@prefab:" + Id(base, i);
        if (!base.records[i].sourceGuid.empty()) baselineReferences[base.records[i].sourceGuid] = key;
        if (const auto it = entities.find(Id(base, i)); it != entities.end())
            if (const auto* guid = world.Get<GuidComponent>(it->second)) localReferences[guid->guid] = key;
    }
    for (size_t i = 0; i < prefab.records.size(); ++i)
        if (!prefab.records[i].sourceGuid.empty()) sourceReferences[prefab.records[i].sourceGuid] = "@prefab:" + Id(prefab, i);
    std::vector<EntityID> resolved(prefab.records.size(), NULL_ENTITY);
    std::unordered_set<std::string> locallyDeleted;
    for (size_t i = 0; i < prefab.records.size(); ++i)
    {
        const auto id = Id(prefab, i);
        if (const auto it = entities.find(id); it != entities.end()) resolved[i] = it->second;
        else if (oldIndices.count(id) && preserveLocalOverrides) locallyDeleted.insert(id);
        else if (!oldIndices.count(id))
        {
            // An inner prefab may already have introduced this node before the
            // outer recipe was saved. Match through its own GUID bindings.
            for (int32_t p = prefab.records[i].parentIndex; p >= 0 && !resolved[i].IsValid(); p = prefab.records[p].parentIndex)
            {
                const auto& parent = prefab.records[p];
                if (parent.nestedPrefabJson.empty()) continue;
                const auto owner = entities.find(Id(prefab, p));
                if (owner == entities.end()) continue;
                const auto* nested = world.Get<PrefabInstanceComponent>(owner->second);
                if (!nested) continue;
                const auto sourceLink = ReadPrefabInstanceFields(Parse(parent.nestedPrefabJson));
                for (const auto& sourceBinding : sourceLink.bindings)
                    if (sourceBinding.entityGuid == prefab.records[i].sourceGuid)
                        for (const auto& binding : nested->bindings)
                            if (binding.recordId == sourceBinding.recordId)
                                if (const auto found = guidEntities.find(binding.entityGuid); found != guidEntities.end())
                                { resolved[i] = found->second; entities[id] = found->second; }
            }
        }
    }
    // A locally deleted parent suppresses its inherited subtree, including new children.
    for (size_t i = 0; i < prefab.records.size(); ++i)
        for (int32_t p = prefab.records[i].parentIndex; p >= 0; p = prefab.records[p].parentIndex)
            if (locallyDeleted.count(Id(prefab, p))) { locallyDeleted.insert(Id(prefab, i)); break; }
    for (size_t i = 0; i < prefab.records.size(); ++i)
    {
        const auto id = Id(prefab, i);
        if (locallyDeleted.count(id)) continue;
        const auto& source = prefab.records[i];
        if (!resolved[i].IsValid())
        {
            auto record = source; record.parentIndex = -1; record.nestedPrefabJson.clear();
            PrefabAsset one; one.records.push_back(record);
            resolved[i] = InstantiatePrefab(world, one, {}).root;
            entities[id] = resolved[i];
        }
        else
        {
            auto local = Capture(world, resolved[i], registry, scripts);
            const auto oldIt = oldIndices.find(id);
            auto old = oldIt != oldIndices.end() ? base.records[oldIt->second] : source;
            local.id = old.id; local.parentIndex = old.parentIndex;
            auto inherited = source;
            RemapScriptReferences(old, baselineReferences);
            RemapScriptReferences(local, localReferences);
            RemapScriptReferences(inherited, sourceReferences);
            std::unordered_set<uint32_t> materialSlots;
            for (const auto* record : {&old,&local,&inherited})
                for (const auto& slot : record->slotOverrides) materialSlots.insert(slot.submeshIndex);
            for (auto* record : {&old,&local,&inherited})
                for (auto index : materialSlots) {
                    const auto existing=std::find_if(record->slotOverrides.begin(),record->slotOverrides.end(),
                        [&](const auto& slot) { return slot.submeshIndex==index; });
                    if (existing!=record->slotOverrides.end()) continue;
                    PrefabEntityRecord::SlotOverride slot; slot.submeshIndex=index;
                    slot.materialAssetPath=record->materialAssetPath;
                    slot.materialOverrideSource=record->materialOverrideSource;
                    if (!slot.materialAssetPath.empty()) record->slotOverrides.push_back(std::move(slot));
                }
            auto before = RecordJson(local, registry);
            const auto baseline = RecordJson(old, registry);
            for (auto it=overrides[id].begin();it!=overrides[id].end();)
            {
                bool legacyMaterial=it->ends_with("/materialOverrideSource");
                const std::string prefix="/slotOverrides/@";
                if (it->starts_with(prefix) && it->find('/',prefix.size())==std::string::npos)
                {
                    const auto slotId=it->substr(prefix.size());
                    legacyMaterial=FindItem(Field(baseline,"slotOverrides"),"slotOverrides",slotId).IsObject() &&
                        FindItem(Field(before,"slotOverrides"),"slotOverrides",slotId).IsObject();
                }
                if (legacyMaterial) it=overrides[id].erase(it); else ++it;
            }
            if (preserveLocalOverrides) CollectOverrides(baseline, before, overrides[id]);
            auto merged = preserveLocalOverrides ? Merge(baseline, before, RecordJson(inherited, registry), overrides[id]) : RecordJson(inherited, registry);
            // Position/rotation define scene placement. Scale inherits like other
            // properties unless the instance actually changed it locally.
            if (i == prefab.rootIndex)
                for (auto& [key, value] : merged.objectVal)
                    if (key == "localPosition" || key == "localRotation")
                        value = link.overrideTransform ? Field(before, key) : Field(RecordJson(inherited, registry), key);
                    else if (key == "localScale" && !link.overrideTransform)
                        value = Field(RecordJson(inherited, registry), key);
            ApplyChanges(world, resolved[i], before, merged, registry, scripts);
        }
    }
    for (size_t i = 0; i < prefab.records.size(); ++i)
    {
        if (!resolved[i].IsValid() || i == prefab.rootIndex) continue;
        const auto old = oldIndices.find(Id(prefab, i));
        const auto* current = world.Get<ParentComponent>(resolved[i]);
        bool inherit = old == oldIndices.end();
        if (old != oldIndices.end())
        {
            const auto oldParent = base.records[old->second].parentIndex;
            const auto it = oldParent >= 0 ? entities.find(Id(base, oldParent)) : entities.end();
            inherit = current && it != entities.end() && current->parent == it->second;
        }
        const auto p = prefab.records[i].parentIndex;
        if (inherit && p >= 0 && resolved[p].IsValid() && (!current || current->parent != resolved[p]))
            Reparent(world, resolved[i], resolved[p]);
    }
    // Removed source nodes are deleted individually; locally attached children survive.
    for (const auto& [id, entity] : entities)
    {
        if (ids.count(id) || entity == root || !world.IsAlive(entity)) continue;
        std::vector<EntityID> children;
        if (const auto* c = world.Get<ChildrenComponent>(entity)) children = c->children;
        for (const auto child : children) if (world.IsAlive(child)) Reparent(world, child, root);
        Reparent(world, entity, NULL_ENTITY);
        world.DestroyEntity(entity);
    }
    // Restore newly introduced nested links, remapping their GUIDs to this instance.
    std::unordered_map<std::string, std::string> guidRemap;
    for (size_t i = 0; i < resolved.size(); ++i)
        if (resolved[i].IsValid() && !prefab.records[i].sourceGuid.empty())
            if (const auto* guid = world.Get<GuidComponent>(resolved[i]))
                guidRemap[prefab.records[i].sourceGuid] = guid->guid;
    for (size_t i = 0; i < resolved.size(); ++i)
    {
        if (i == prefab.rootIndex || !resolved[i].IsValid()) continue;
        const auto& source = prefab.records[i];
        if (source.nestedPrefabJson.empty()) continue;
        auto nested = ReadPrefabInstanceFields(Parse(source.nestedPrefabJson));
        const auto* existing = world.Get<PrefabInstanceComponent>(resolved[i]);
        if (existing && existing->prefabAssetPath == nested.prefabAssetPath) continue;
        for (auto& binding : nested.bindings)
            if (const auto it = guidRemap.find(binding.entityGuid); it != guidRemap.end()) binding.entityGuid = it->second;
        Set(world, resolved[i], std::move(nested));
    }
    link.bindings.clear();
    for (size_t i = 0; i < resolved.size(); ++i)
    {
        if (!resolved[i].IsValid()) continue;
        if (!world.Has<GuidComponent>(resolved[i]))
            world.Add<GuidComponent>(resolved[i], GuidComponent{"prefab-migrated-" + std::to_string(root.value) + "-" + std::to_string(resolved[i].value)});
        link.bindings.push_back({Id(prefab, i), world.Get<GuidComponent>(resolved[i])->guid});
        const auto& paths = overrides[Id(prefab, i)];
        link.bindings.back().overriddenProperties.assign(paths.begin(), paths.end());
    }
    // Keep tombstones for local deletions across subsequent synchronizations/reloads.
    for (const auto& id : locallyDeleted) link.bindings.push_back({id, ""});
    link.baselinePrefabJson = SerializePrefabToJson(prefab);
    Set(world, root, std::move(link));
    std::unordered_map<std::string, std::string> references;
    for (size_t i = 0; i < resolved.size(); ++i)
        if (resolved[i].IsValid())
        {
            const auto& guid = world.Get<GuidComponent>(resolved[i])->guid;
            references["@prefab:" + Id(prefab, i)] = guid;
            if (!prefab.records[i].sourceGuid.empty()) references[prefab.records[i].sourceGuid] = guid;
        }
    for (const auto entity : resolved)
        if (auto* list = world.Get<script::ScriptList>(entity))
            for (auto& instance : list->Instances_Mutable())
                for (auto& [name, value] : instance.fieldValues)
                    if (value.type == script::ScriptFieldType::Entity)
                        if (const auto it = references.find(value.stringValue); it != references.end()) value.stringValue = it->second;
    if (scripts) script::ResolveScriptEntityReferences(world, *scripts);
    return true;
}
} // namespace engine::addons::prefab
