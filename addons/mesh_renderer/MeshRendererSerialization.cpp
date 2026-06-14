#include "addons/mesh_renderer/MeshRendererSerialization.hpp"

#include "addons/mesh_renderer/MeshRendererComponents.hpp"
#include "assets/AssetPipeline.hpp"
#include "assets/AssetRegistry.hpp"
#include "ecs/World.hpp"
#include "core/Logger.hpp"

#include <cstdlib>   // std::stoul
#include <memory>
#include <stdexcept>
#include <string>

namespace engine::addons::mesh_renderer {

void RegisterMeshRendererSerializationHandlers(serialization::SceneSerializer& serializer)
{
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
        if (!c.materialAssetPath.empty())
            w.WriteString("materialAssetPath", c.materialAssetPath);
        if (!c.baseColorTexturePath.empty())
            w.WriteString("baseColorTexturePath", c.baseColorTexturePath);
        if (!c.slotOverrides.empty())
        {
            w.BeginArray("slots");
            for (const MaterialComponent::SlotOverride& slot : c.slotOverrides)
            {
                w.BeginObject();
                w.WriteUint("submeshIndex", slot.submeshIndex);
                if (!slot.materialAssetPath.empty())
                    w.WriteString("materialAssetPath", slot.materialAssetPath);
                w.EndObject();
            }
            w.EndArray();
        }
        w.EndObject();
    });
}

void RegisterMeshRendererDeserializationHandlers(serialization::SceneDeserializer& deserializer)
{
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
}

void UnregisterMeshRendererDeserializationHandlers(serialization::SceneDeserializer& deserializer)
{
    deserializer.UnregisterHandler("MeshComponent");
    deserializer.UnregisterHandler("MaterialComponent");
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
                               ecs::World&             world)
{
    bool anyResolved = false;

    world.ForEachAlive([&](EntityID id)
    {
        auto* mc = world.Get<MeshComponent>(id);
        if (!mc || mc->meshAssetPath.empty())
            return;

        // Mesh schon gueltig und im Registry vorhanden → nichts zu tun.
        if (mc->mesh.IsValid() && registry.meshes.Get(mc->mesh) != nullptr)
            return;

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
    });

    if (anyResolved)
        pipeline.UploadPendingGpuAssets();
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
