#include "addons/mesh_renderer/MeshRendererSerialization.hpp"

#include "addons/mesh_renderer/MeshRendererComponents.hpp"
#include "addons/animation/AnimationComponents.hpp"
#include "assets/AssetPipeline.hpp"
#include "assets/AssetRegistry.hpp"
#include "ecs/World.hpp"
#include "core/Logger.hpp"
#include "core/PathUtf8.hpp"

#include <algorithm>
#include <cstdlib>   // std::stoul
#include <vector>
#include <memory>
#include <stdexcept>
#include <string>

namespace engine::addons::mesh_renderer {

namespace {

// Scenes store material references relative to the project's Assets folder.
// A non-portable path (absolute, or outside the project such as the engine's
// own editor/dpad_*.mat gizmo materials) would only resolve on the machine that
// wrote it, so it is dropped together with its override source.
bool ShouldWriteMaterialPath(const std::string& path)
{
    if (path.empty())
        return false;
    if (IsPortableAssetPath(path))
        return true;
    Debug::LogWarning("MeshRendererSerialization: material path '%s' is not relative to the "
                      "project's Assets folder and is not saved", path.c_str());
    return false;
}

} // namespace

void RegisterSkinnedMeshSerializationHandlers(serialization::SceneSerializer& serializer);

void RegisterMeshRendererSerializationHandlers(serialization::SceneSerializer& serializer)
{
    RegisterSkinnedMeshSerializationHandlers(serializer);
    serializer.RegisterSerializer<MeshComponent>([](serialization::JsonWriter& w, const MeshComponent& c) {
        w.BeginObject();
        w.WriteString("type", "MeshComponent");
        // meshAssetPath ist die primäre Referenz (sessionübergreifend stabil).
        // meshHandle wird als Fallback für alte Scenes gespeichert, ist aber beim
        // Neu-Laden bedeutungslos — ResolveMeshAssetBindings lädt über meshAssetPath.
        if (!c.meshAssetPath.empty())
            w.WriteString("meshAssetPath", c.meshAssetPath);
        w.WriteBool("visible",     c.visible);
        w.WriteBool("castShadows", c.castShadows);
        w.WriteUint("layerMask",   c.layerMask);
        w.EndObject();
    });

    serializer.RegisterSerializer<MaterialComponent>([](serialization::JsonWriter& w, const MaterialComponent& c) {
        w.BeginObject();
        w.WriteString("type", "MaterialComponent");
        w.WriteUint("submeshIndex", c.submeshIndex);
        const bool writeMaterialPath = ShouldWriteMaterialPath(c.materialAssetPath);
        if (writeMaterialPath)
            w.WriteString("materialAssetPath", c.materialAssetPath);
        if (!c.materialOverrideSource.empty() && (writeMaterialPath || c.materialAssetPath.empty()))
            w.WriteString("materialOverrideSource", c.materialOverrideSource);
        if (!c.baseColorTexturePath.empty())
            w.WriteString("baseColorTexturePath", c.baseColorTexturePath);
        if (!c.slotOverrides.empty())
        {
            w.BeginArray("slots");
            for (const MaterialComponent::SlotOverride& slot : c.slotOverrides)
            {
                w.BeginObject();
                w.WriteUint("submeshIndex", slot.submeshIndex);
                const bool writeSlotPath = ShouldWriteMaterialPath(slot.materialAssetPath);
                if (writeSlotPath)
                    w.WriteString("materialAssetPath", slot.materialAssetPath);
                if (!slot.materialOverrideSource.empty() && (writeSlotPath || slot.materialAssetPath.empty()))
                    w.WriteString("materialOverrideSource", slot.materialOverrideSource);
                w.EndObject();
            }
            w.EndArray();
        }
        w.EndObject();
    });
}

// Internal helpers (declared above RegisterMeshRendererSerializationHandlers).
void RegisterSkinnedMeshSerializationHandlers(serialization::SceneSerializer& serializer)
{
    // Skinned meshes: only the asset references and playback settings; handles,
    // palettes and GPU buffers are rebuilt on load (ResolveAnimationAssetBindings).
    serializer.RegisterSerializer<SkinComponent>([](serialization::JsonWriter& w, const SkinComponent& c) {
        w.BeginObject();
        w.WriteString("type", "SkinComponent");
        if (!c.skeletonAssetPath.empty())
            w.WriteString("skeletonAssetPath", c.skeletonAssetPath);
        w.EndObject();
    });
    serializer.RegisterSerializer<AnimationPlayerComponent>([](serialization::JsonWriter& w, const AnimationPlayerComponent& c) {
        w.BeginObject();
        w.WriteString("type", "AnimationPlayerComponent");
        if (!c.clipAssetPath.empty())
            w.WriteString("clipAssetPath", c.clipAssetPath);
        w.WriteBool("playing", c.playing);
        w.WriteBool("looping", c.looping);
        w.WriteFloat("speed", c.speed);
        w.EndObject();
    });
}

void RegisterSkinnedMeshDeserializationHandlers(serialization::SceneDeserializer& deserializer)
{
    deserializer.RegisterHandler<SkinComponent>([](const serialization::JsonValue& v, ecs::World& w, EntityID id) {
        SkinComponent skin{};
        if (const auto* p = v.Get("skeletonAssetPath")) skin.skeletonAssetPath = p->AsString();
        w.Add<SkinComponent>(id, std::move(skin));
    });
    deserializer.RegisterHandler<AnimationPlayerComponent>([](const serialization::JsonValue& v, ecs::World& w, EntityID id) {
        AnimationPlayerComponent player{};
        if (const auto* p = v.Get("clipAssetPath")) player.clipAssetPath = p->AsString();
        if (const auto* p = v.Get("playing")) player.playing = p->AsBool();
        if (const auto* p = v.Get("looping")) player.looping = p->AsBool();
        if (const auto* p = v.Get("speed"))   player.speed   = p->AsFloat();
        w.Add<AnimationPlayerComponent>(id, std::move(player));
    });
}

void RegisterMeshRendererDeserializationHandlers(serialization::SceneDeserializer& deserializer)
{
    RegisterSkinnedMeshDeserializationHandlers(deserializer);
    deserializer.RegisterHandler<MeshComponent>([](const serialization::JsonValue& v, ecs::World& w, EntityID id) {
        MeshComponent mc{};
        // meshAssetPath: primär für das Neuladen (ResolveMeshAssetBindings).
        if (const auto* p = v.Get("meshAssetPath")) mc.meshAssetPath = p->AsString();
        // castShadows / layerMask direkt übernehmen
        if (const auto* vi = v.Get("visible"))     mc.visible     = vi->AsBool();
        if (const auto* s  = v.Get("castShadows")) mc.castShadows = s->AsBool();
        if (const auto* l  = v.Get("layerMask"))   mc.layerMask   = l->AsUint();
        // mesh-Handle bleibt ungültig → ResolveMeshAssetBindings füllt ihn
        w.Add<MeshComponent>(id, mc);
    });

    deserializer.RegisterHandler<MaterialComponent>([](const serialization::JsonValue& v, ecs::World& w, EntityID id) {
        MaterialComponent mc{};
        // material-Handle bleibt ungültig → ResolveMaterialAssetBindings füllt ihn
        if (const auto* s = v.Get("submeshIndex")) mc.submeshIndex = s->AsUint();
        if (const auto* p = v.Get("materialAssetPath")) mc.materialAssetPath = p->AsString();
        if (const auto* p = v.Get("materialOverrideSource")) mc.materialOverrideSource = p->AsString();
        if (const auto* p = v.Get("baseColorTexturePath")) mc.baseColorTexturePath = p->AsString();
        if (const auto* slots = v.Get("slots"))
        {
            if (slots->IsArray())
            {
                for (const serialization::JsonValue& item : slots->arrayVal)
                {
                    if (!item.IsObject())
                        continue;
                    MaterialComponent::SlotOverride slot{};
                    if (const auto* s = item.Get("submeshIndex")) slot.submeshIndex = s->AsUint();
                    if (const auto* p = item.Get("materialAssetPath")) slot.materialAssetPath = p->AsString();
                    if (const auto* p = item.Get("materialOverrideSource")) slot.materialOverrideSource = p->AsString();
                    if (!slot.materialAssetPath.empty())
                        mc.slotOverrides.push_back(std::move(slot));
                }
            }
        }
        w.Add<MaterialComponent>(id, mc);
    });
}

void UnregisterMeshRendererSerializationHandlers(serialization::SceneSerializer& serializer)
{
    serializer.UnregisterSerializer<MeshComponent>();
    serializer.UnregisterSerializer<MaterialComponent>();
    serializer.UnregisterSerializer<SkinComponent>();
    serializer.UnregisterSerializer<AnimationPlayerComponent>();
}

void UnregisterMeshRendererDeserializationHandlers(serialization::SceneDeserializer& deserializer)
{
    deserializer.UnregisterHandler("MeshComponent");
    deserializer.UnregisterHandler("MaterialComponent");
    deserializer.UnregisterHandler("SkinComponent");
    deserializer.UnregisterHandler("AnimationPlayerComponent");
}

void RegisterMeshRendererAddon(ecs::ComponentMetaRegistry* components,
                                serialization::SceneSerializer* serializer,
                                serialization::SceneDeserializer* deserializer)
{
    if (components)  RegisterMeshRendererComponents(*components);
    if (serializer)  RegisterMeshRendererSerializationHandlers(*serializer);
    if (deserializer) RegisterMeshRendererDeserializationHandlers(*deserializer);
}

// ---------------------------------------------------------------------------
// ResolveMeshAssetBindings
// Laeuft nach dem Deserialisieren einer Scene und restauriert alle MeshComponent-
// Handles ueber ihren meshAssetPath.
//
// Fragment-Pfade (z.B. "model.glb#mesh/2") werden aufgeloest indem das Quell-
// Bundle re-importiert und der Mesh am angegebenen Index extrahiert wird.
// Direkte Pfade (z.B. "mesh.kmesh") werden ueber AssetPipeline::LoadMesh geladen.
// ---------------------------------------------------------------------------
void ResolveMeshAssetBindings(assets::AssetPipeline&  pipeline,
                               assets::AssetRegistry&  registry,
                               ecs::World&             world,
                               std::span<const EntityID> entities)
{
    bool anyResolved = false;

    const auto resolve = [&](EntityID id)
    {
        auto* mc = world.Get<MeshComponent>(id);
        if (!mc || mc->meshAssetPath.empty())
            return;

        // Mesh schon gueltig und im Registry vorhanden → nichts zu tun.
        if (mc->mesh.IsValid() && registry.meshes.Get(mc->mesh) != nullptr)
            return;

        // Aeltere Szenen speichern absolute Pfade unter dem Asset-Root — auf relativ
        // umstellen, damit das naechste Speichern portable Referenzen schreibt.
        {
            const size_t fragmentPos = mc->meshAssetPath.rfind('#');
            const std::string base = mc->meshAssetPath.substr(0, fragmentPos);
            const std::string relative = pipeline.ToAssetRelative(PathFromUtf8(base));
            if (relative != base)
                mc->meshAssetPath = relative +
                    (fragmentPos == std::string::npos ? std::string() : mc->meshAssetPath.substr(fragmentPos));
        }

        const std::string& assetPath = mc->meshAssetPath;

        // Fragment-Pfad erkennen: "basefile.glb#mesh/<index>"
        const size_t hashPos = assetPath.rfind('#');
        if (hashPos != std::string::npos)
        {
            const std::string basePath = assetPath.substr(0, hashPos);
            const std::string fragment = assetPath.substr(hashPos + 1u);

            // Fragment parsen: "mesh/<index>"
            size_t meshIndex = 0u;
            const std::string meshPrefix = "mesh/";
            if (fragment.size() <= meshPrefix.size() ||
                fragment.compare(0u, meshPrefix.size(), meshPrefix) != 0)
            {
                Debug::LogError("ResolveMeshAssetBindings: Ungueltiges Mesh-Fragment: '%s'",
                                assetPath.c_str());
                mc->mesh = MeshHandle::Invalid();
                return;
            }

            try
            {
                meshIndex = static_cast<size_t>(std::stoul(fragment.substr(meshPrefix.size())));
            }
            catch (const std::exception&)
            {
                Debug::LogError("ResolveMeshAssetBindings: Ungueltiger Mesh-Index: '%s'",
                                assetPath.c_str());
                mc->mesh = MeshHandle::Invalid();
                return;
            }

            // Bundle re-importieren
            assets::ImportedAssetBundle bundle = pipeline.ImportBundle(basePath);
            if (!bundle.Ok() || meshIndex >= bundle.meshes.size())
            {
                Debug::LogError("ResolveMeshAssetBindings: Konnte Bundle nicht laden "
                                "oder Index ausserhalb: '%s'", assetPath.c_str());
                mc->mesh = MeshHandle::Invalid();
                return;
            }

            auto meshAsset = std::make_unique<assets::MeshAsset>(
                std::move(bundle.meshes[meshIndex]));
            meshAsset->path             = assetPath;
            meshAsset->state            = assets::AssetState::Loaded;
            meshAsset->gpuStatus.dirty  = true;
            meshAsset->gpuStatus.uploaded = false;
            // materialHandles aus dem ImportedBundle sind CPU-AssetRegistry-Handles
            // des Importers, KEINE GPU-Runtime-Handles. Wenn EmitSubmeshProxies sie
            // als Fallback verwendet (weil MaterialComponent::material noch Invalid
            // ist), landet ein falscher Handle im GPU-Draw-Call → VK_ERROR_DEVICE_LOST.
            // Die korrekte GPU-Material-Zuweisung erfolgt danach durch
            // ResolveMaterialAssetBindings → AcquireSharedRuntimeMaterial.
            meshAsset->materialHandles.clear();

            const MeshHandle handle =
                registry.GetOrAddMesh(assetPath, std::move(meshAsset));
            mc->mesh = handle;
            anyResolved = true;
        }
        else
        {
            // Normaler Pfad → LoadMesh (kmesh, .mesh, …)
            const MeshHandle handle = pipeline.LoadMesh(assetPath);
            if (handle.IsValid())
            {
                mc->mesh = handle;
                anyResolved = true;
            }
            else
            {
                Debug::LogError("ResolveMeshAssetBindings: Mesh konnte nicht geladen "
                                "werden: '%s'", assetPath.c_str());
            }
        }
    };
    if (entities.empty())
        world.ForEachAlive(resolve);
    else
        for (const EntityID id : entities)
            resolve(id);

    if (anyResolved)
        pipeline.UploadPendingGpuAssets();

    ResolveAnimationAssetBindings(pipeline, registry, world, entities);
}

std::string ToAssetRelativeFragmentPath(const assets::AssetPipeline& pipeline, const std::string& assetPath)
{
    if (assetPath.empty() || IsPortableAssetPath(assetPath))
        return assetPath;
    const size_t hashPos = assetPath.rfind('#');
    const std::string base = assetPath.substr(0, hashPos);
    const std::string relative = pipeline.ToAssetRelative(PathFromUtf8(base));
    if (!IsPortableAssetPath(relative))
        return assetPath;
    return relative + (hashPos == std::string::npos ? std::string() : assetPath.substr(hashPos));
}

bool ImportModelAnimationAssets(assets::AssetPipeline& pipeline,
                                assets::AssetRegistry& registry,
                                const std::string&     assetPath)
{
    const size_t hashPos = assetPath.rfind('#');
    if (hashPos == std::string::npos)
        return false;
    const std::string base = assetPath.substr(0, hashPos);
    assets::ImportedAssetBundle bundle = pipeline.ImportBundle(base);
    if (!bundle.Ok())
    {
        Debug::LogWarning("ImportModelAnimationAssets: could not import '%s' for '%s': %s",
                          base.c_str(), assetPath.c_str(), bundle.error.c_str());
        return false;
    }
    for (size_t i = 0; i < bundle.skeletons.size(); ++i)
    {
        auto skeleton = std::make_unique<assets::SkeletonAsset>(std::move(bundle.skeletons[i]));
        skeleton->path  = base + "#skeleton/" + std::to_string(i);
        skeleton->state = assets::AssetState::Loaded;
        const std::string key = skeleton->path;
        (void)registry.GetOrAddSkeleton(key, std::move(skeleton));
    }
    for (size_t i = 0; i < bundle.animations.size(); ++i)
    {
        auto animation = std::make_unique<assets::AnimationClip>(std::move(bundle.animations[i]));
        animation->path  = base + "#animation/" + std::to_string(i);
        animation->state = assets::AssetState::Loaded;
        const std::string key = animation->path;
        (void)registry.GetOrAddAnimation(key, std::move(animation));
    }
    return true;
}

void ResolveAnimationAssetBindings(assets::AssetPipeline&  pipeline,
                                   assets::AssetRegistry&  registry,
                                   ecs::World&             world,
                                   std::span<const EntityID> entities)
{
    // Models that failed once in this pass are not re-imported for every entity.
    std::vector<std::string> failedModels;
    const auto importOnce = [&](const std::string& path) {
        const std::string base = path.substr(0, path.rfind('#'));
        if (std::find(failedModels.begin(), failedModels.end(), base) != failedModels.end())
            return;
        if (!ImportModelAnimationAssets(pipeline, registry, path))
            failedModels.push_back(base);
    };

    const auto resolve = [&](EntityID id)
    {
        if (auto* skin = world.Get<SkinComponent>(id);
            skin && !skin->skeletonAssetPath.empty() && !registry.skeletons.Get(skin->skeleton))
        {
            skin->skeletonAssetPath = ToAssetRelativeFragmentPath(pipeline, skin->skeletonAssetPath);
            SkeletonHandle handle = registry.FindSkeletonByPath(skin->skeletonAssetPath);
            if (!handle.IsValid())
            {
                importOnce(skin->skeletonAssetPath);
                handle = registry.FindSkeletonByPath(skin->skeletonAssetPath);
            }
            skin->skeleton = handle;
            skin->bonePalette.clear();
        }
        if (auto* player = world.Get<AnimationPlayerComponent>(id);
            player && !player->clipAssetPath.empty() && !registry.animations.Get(player->clip))
        {
            player->clipAssetPath = ToAssetRelativeFragmentPath(pipeline, player->clipAssetPath);
            AnimationHandle handle = registry.FindAnimationByPath(player->clipAssetPath);
            if (!handle.IsValid())
            {
                importOnce(player->clipAssetPath);
                handle = registry.FindAnimationByPath(player->clipAssetPath);
            }
            player->SetClip(handle);
        }
    };
    if (entities.empty())
        world.ForEachAlive(resolve);
    else
        for (const EntityID id : entities)
            resolve(id);
}

void UnregisterMeshRendererAddon(ecs::ComponentMetaRegistry* components,
                                  serialization::SceneSerializer* serializer,
                                  serialization::SceneDeserializer* deserializer)
{
    if (deserializer) UnregisterMeshRendererDeserializationHandlers(*deserializer);
    if (serializer)  UnregisterMeshRendererSerializationHandlers(*serializer);
    if (components)  { components->Unregister<MeshComponent>(); components->Unregister<MaterialComponent>(); }
}

} // namespace engine::addons::mesh_renderer
