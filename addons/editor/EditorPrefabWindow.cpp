// =============================================================================
// KROM Engine - addons/editor/EditorPrefabWindow.cpp
// Prefab Live-Link (ResolvePrefabBindings) + Prefab-Editor-Fenster.
// =============================================================================
#include "EditorPrefabWindow.hpp"
#include "EditorFeature.hpp"
#include "EditorComponents.hpp"
#include "EditorMaterialLibrary.hpp"

#include "addons/prefab/Prefab.hpp"
#include "addons/prefab/PrefabInstanceComponent.hpp"
#include "addons/mesh_renderer/MeshRendererComponents.hpp"
#include "addons/mesh_renderer/MeshRendererSerialization.hpp"
#include "assets/AssetRegistry.hpp"
#include "assets/AssetPipeline.hpp"
#include "ecs/World.hpp"
#include "core/Logger.hpp"
#include "renderer/RenderLayers.hpp"

#ifdef KROM_EDITOR_HAS_IMGUI
#include "imgui.h"
#endif

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

namespace engine::addons::editor {

using EditorFrameContext = engine::renderer::addons::editor::EditorFrameContext;

// ============================================================================
//  Interne Hilfsfunktionen
// ============================================================================

namespace {

// Loest einen relativen Asset-Pfad zu einem absoluten Pfad auf.
std::filesystem::path ResolveAbsPath(const EditorFrameContext& ctx,
                                     const std::string& relPath)
{
    std::filesystem::path p(relPath);
    if (p.is_absolute()) return p;
    if (!ctx.currentProjectRoot.empty())
        return std::filesystem::path(ctx.currentProjectRoot) / "Assets" / p;
    return p;
}

// Markiert alle Entities einer Prefab-Preview-Instanz (rekursiv) mit dem
// Prefab-Preview-Layer damit sie NUR im Preview-RT erscheinen.
void TagPrefabPreviewEntities(ecs::World& world, EntityID root)
{
    if (!root.IsValid() || !world.IsAlive(root))
        return;

    std::vector<EntityID> stack{ root };
    while (!stack.empty())
    {
        EntityID e = stack.back();
        stack.pop_back();
        if (!world.IsAlive(e)) continue;

        world.Add<renderer::addons::editor::EditorPrefabPreviewComponent>(e);

        if (auto* mesh = world.Get<MeshComponent>(e))
            mesh->layerMask = renderer::LAYER_EDITOR_PREFAB_PREVIEW;

        if (const auto* children = world.Get<ChildrenComponent>(e))
            for (EntityID child : children->children)
                if (world.IsAlive(child))
                    stack.push_back(child);
    }
}

// Zerstoert die aktuelle Preview-Instanz.
void DestroyPrefabPreview(EditorFrameContext& ctx)
{
    EntityID& previewRoot = ctx.state.prefabPreviewRootEntity;
    if (previewRoot.IsValid() && ctx.world.IsAlive(previewRoot))
        engine::addons::prefab::DestroyPrefabInstanceFromRoot(ctx.world, previewRoot);
    previewRoot = NULL_ENTITY;
}

// Berechnet AABB aller Records eines Prefabs (lokale Positionen akkumuliert).
// Gibt Zentrum + Radius der umschliessenden Sphere zurueck.
void ComputePrefabBounds(const engine::addons::prefab::PrefabAsset& prefab,
                         math::Vec3& outCenter, float& outRadius)
{
    constexpr float kBig = 1e9f;
    math::Vec3 bMin{ kBig,  kBig,  kBig};
    math::Vec3 bMax{-kBig, -kBig, -kBig};

    for (const auto& record : prefab.records)
    {
        // Approximation: akkumuliere lokale Positionen aller Records
        // (world-space wuerde Transform-Propagation erfordern — zu frueh hier)
        const math::Vec3& p = record.localPosition;
        bMin.x = std::min(bMin.x, p.x); bMax.x = std::max(bMax.x, p.x);
        bMin.y = std::min(bMin.y, p.y); bMax.y = std::max(bMax.y, p.y);
        bMin.z = std::min(bMin.z, p.z); bMax.z = std::max(bMax.z, p.z);

        // Skala beruecksichtigen: Scale * 1 ergibt ungefaehre Ausdehnung
        const math::Vec3 corner = p + record.localScale;
        bMin.x = std::min(bMin.x, corner.x); bMax.x = std::max(bMax.x, corner.x);
        bMin.y = std::min(bMin.y, corner.y); bMax.y = std::max(bMax.y, corner.y);
        bMin.z = std::min(bMin.z, corner.z); bMax.z = std::max(bMax.z, corner.z);
    }

    if (bMin.x > bMax.x) { outCenter = {0,0,0}; outRadius = 1.f; return; }

    outCenter = (bMin + bMax) * 0.5f;
    const math::Vec3 ext = (bMax - bMin) * 0.5f;
    outRadius = std::max({ext.x, ext.y, ext.z, 0.01f});
}

// Spawnt eine Preview-Instanz des angegebenen Prefabs.
void SpawnPrefabPreview(EditorFrameContext& ctx,
                        const engine::addons::prefab::PrefabAsset& prefab)
{
    DestroyPrefabPreview(ctx);

    // Kamera-Ziel und Distanz aus Bounding Box berechnen
    math::Vec3 boundsCenter{0.f, 0.f, 0.f};
    float boundsRadius = 1.f;
    ComputePrefabBounds(prefab, boundsCenter, boundsRadius);
    ctx.state.prefabPreviewCenter       = boundsCenter;
    ctx.state.prefabPreviewBoundsRadius = boundsRadius;
    ctx.state.prefabPreviewDistance     = boundsRadius * 1.6f;

    engine::addons::prefab::PrefabInstantiateOptions opts;
    opts.position = {0.f, 0.f, 0.f};
    opts.scriptRegistry = ctx.scriptRegistry;

    const engine::addons::prefab::PrefabInstance instance =
        engine::addons::prefab::InstantiatePrefab(ctx.world, prefab, opts);

    if (!instance.IsValid())
        return;

    TagPrefabPreviewEntities(ctx.world, instance.root);
    ctx.state.prefabPreviewRootEntity = instance.root;

    if (ctx.assetPipeline)
    {
        engine::addons::mesh_renderer::ResolveMeshAssetBindings(
            *ctx.assetPipeline, ctx.registry, ctx.world);
        ResolveMaterialAssetBindings(ctx);
    }
}

// Laedt und cached das aktuell geoeffnete Prefab.
// Gibt nullptr zurueck wenn kein Prefab offen oder Laden fehlschlug.
static engine::addons::prefab::PrefabAsset* GetOpenPrefab()
{
    static engine::addons::prefab::PrefabAsset s_prefab;
    static std::string                         s_loadedPath;

    // Wird von aussen gesetzt — kein Rueckgabewert hier,
    // sondern ueber separate OpenPrefab()-Funktion.
    return &s_prefab;
}

static std::string& GetOpenPrefabPath()
{
    static std::string s_path;
    return s_path;
}

static engine::addons::prefab::PrefabAsset& GetOpenPrefabAsset()
{
    static engine::addons::prefab::PrefabAsset s_asset;
    return s_asset;
}

} // namespace

// ============================================================================
//  ResolvePrefabBindings — Live-Link Kern
// ============================================================================

void ResolvePrefabBindings(EditorFrameContext& ctx)
{
    if (!ctx.assetPipeline)
        return;

    // Alle Instanz-Roots sammeln (nicht waehrend ForEach modifizieren)
    struct InstanceInfo
    {
        EntityID    root;
        std::string prefabAssetPath;
        math::Vec3  position;
        math::Quat  rotation;
        math::Vec3  scale;
        bool        overrideTransform;
    };
    std::vector<InstanceInfo> instances;

    ctx.world.ForEachAlive([&](EntityID id)
        {
            PrefabInstanceComponent* pic = ctx.world.Get<PrefabInstanceComponent>(id);
            if (!pic || pic->prefabAssetPath.empty()) return;
            InstanceInfo info;
            info.root             = id;
            info.prefabAssetPath  = pic->prefabAssetPath;
            info.overrideTransform = pic->overrideTransform;
            if (const auto* t = ctx.world.Get<TransformComponent>(id))
            {
                info.position = t->localPosition;
                info.rotation = t->localRotation;
                info.scale    = t->localScale;
            }
            else
            {
                info.position = {0.f, 0.f, 0.f};
                info.rotation = math::Quat::Identity();
                info.scale    = {1.f, 1.f, 1.f};
            }
            instances.push_back(info);
        });

    for (const InstanceInfo& info : instances)
    {
        const std::filesystem::path absPath = ResolveAbsPath(ctx, info.prefabAssetPath);

        engine::addons::prefab::PrefabAsset prefab;
        std::string error;
        if (!engine::addons::prefab::LoadPrefabFromFile(absPath, ctx.registry, prefab, &error))
        {
            Debug::LogWarning("ResolvePrefabBindings: Prefab nicht geladen '%s': %s",
                              absPath.string().c_str(),
                              error.c_str());
            continue;
        }

        // Alte Instanz zerstoeren
        engine::addons::prefab::DestroyPrefabInstanceFromRoot(ctx.world, info.root);

        // Neue Instanz spawnen
        engine::addons::prefab::PrefabInstantiateOptions opts;
        opts.scriptRegistry = ctx.scriptRegistry;
        if (info.overrideTransform)
        {
            opts.position = info.position;
            opts.rotation = info.rotation;
            opts.scale    = info.scale;
        }

        const engine::addons::prefab::PrefabInstance instance =
            engine::addons::prefab::InstantiatePrefab(ctx.world, prefab, opts);

        if (!instance.IsValid())
        {
            Debug::LogWarning("ResolvePrefabBindings: Instanziierung fehlgeschlagen '%s'",
                              info.prefabAssetPath.c_str());
            continue;
        }

        // PrefabInstanceComponent auf neue Root-Entity setzen
        ctx.world.Add<PrefabInstanceComponent>(instance.root,
            PrefabInstanceComponent{ info.prefabAssetPath, info.overrideTransform });
    }

    // Einmal alle Meshes + Materialien aufloesen
    engine::addons::mesh_renderer::ResolveMeshAssetBindings(
        *ctx.assetPipeline, ctx.registry, ctx.world);
    ResolveMaterialAssetBindings(ctx);
}

// ============================================================================
//  SaveOpenPrefab
// ============================================================================

bool SaveOpenPrefab(EditorFrameContext& ctx)
{
    const std::string& path = GetOpenPrefabPath();
    if (path.empty())
        return false;

    engine::addons::prefab::PrefabAsset& prefab = GetOpenPrefabAsset();

    std::string error;
    if (!engine::addons::prefab::SavePrefabToFile(prefab, std::filesystem::path(path), &error))
    {
        Debug::LogError("Prefab konnte nicht gespeichert werden: %s", error.c_str());
        ctx.lastFileMessage = "Fehler beim Speichern: " + error;
        return false;
    }

    ctx.state.prefabEditorDirty = false;
    ctx.lastFileMessage = "Prefab gespeichert: " +
                          std::filesystem::path(path).filename().string();

    // Live-Link: alle Szenen-Instanzen synchronisieren
    ResolvePrefabBindings(ctx);

    // Preview neu aufbauen
    SpawnPrefabPreview(ctx, prefab);

    return true;
}

// ============================================================================
//  IsPrefabEditorOpen
// ============================================================================

bool IsPrefabEditorOpen(const EditorFrameContext& ctx)
{
    return ctx.state.prefabEditorOpen;
}

// ============================================================================
//  DrawPrefabEditorWindow
// ============================================================================

void DrawPrefabEditorWindow(EditorFrameContext& ctx,
                            const std::filesystem::path& prefabPath)
{
#ifdef KROM_EDITOR_HAS_IMGUI

    // --- Neues Prefab oeffnen ---
    if (!prefabPath.empty())
    {
        const std::string absStr = prefabPath.string();
        if (GetOpenPrefabPath() != absStr)
        {
            GetOpenPrefabPath() = absStr;

            std::string error;
            engine::addons::prefab::PrefabAsset loaded;
            if (engine::addons::prefab::LoadPrefabFromFile(prefabPath, ctx.registry, loaded, &error))
            {
                GetOpenPrefabAsset()        = std::move(loaded);
                ctx.state.prefabEditorOpen  = true;
                ctx.state.prefabEditorDirty = false;
                ctx.state.prefabEditorAssetPath = absStr;
                SpawnPrefabPreview(ctx, GetOpenPrefabAsset());
            }
            else
            {
                Debug::LogError("Prefab-Editor: Laden fehlgeschlagen: %s", error.c_str());
                ctx.lastFileMessage = "Prefab laden fehlgeschlagen: " + error;
                GetOpenPrefabPath().clear();
            }
        }
        ctx.state.prefabEditorOpen = true;
    }

    if (!ctx.state.prefabEditorOpen)
        return;

    engine::addons::prefab::PrefabAsset& prefab = GetOpenPrefabAsset();
    const std::string& openPath                 = GetOpenPrefabPath();

    // Fenstertitel mit Dirty-Marker
    const std::string title = std::string("Prefab: ") +
                              (openPath.empty() ? "Kein Prefab"
                                                : std::filesystem::path(openPath).filename().string()) +
                              (ctx.state.prefabEditorDirty ? " *" : "") +
                              "###PrefabEditorWindow";

    ImGui::SetNextWindowSize(ImVec2(820.f, 520.f), ImGuiCond_FirstUseEver);
    bool windowOpen = ctx.state.prefabEditorOpen;
    if (!ImGui::Begin(title.c_str(), &windowOpen))
    {
        ImGui::End();
        if (!windowOpen)
        {
            ctx.state.prefabEditorOpen = false;
            DestroyPrefabPreview(ctx);
        }
        return;
    }

    // ── Toolbar ──────────────────────────────────────────────────────────────
    if (ImGui::Button("Speichern"))
        SaveOpenPrefab(ctx);

    ImGui::SameLine();
    if (ImGui::Button("In Szene spawnen") && !openPath.empty())
    {
        engine::addons::prefab::PrefabInstantiateOptions opts;
        opts.scriptRegistry = ctx.scriptRegistry;
        const engine::addons::prefab::PrefabInstance instance =
            engine::addons::prefab::InstantiatePrefab(ctx.world, prefab, opts);
        if (instance.IsValid())
        {
            // Relativen Pfad speichern
            std::string relPath = openPath;
            if (!ctx.currentProjectRoot.empty())
            {
                const std::filesystem::path root =
                    std::filesystem::path(ctx.currentProjectRoot) / "Assets";
                std::error_code ec;
                auto rel = std::filesystem::relative(openPath, root, ec);
                if (!ec) relPath = rel.generic_string();
            }
            ctx.world.Add<PrefabInstanceComponent>(instance.root,
                PrefabInstanceComponent{ relPath, true });
            ctx.state.selectedEntity = instance.root;

            if (ctx.assetPipeline)
            {
                engine::addons::mesh_renderer::ResolveMeshAssetBindings(
                    *ctx.assetPipeline, ctx.registry, ctx.world);
                ResolveMaterialAssetBindings(ctx);
            }
            ctx.lastFileMessage = "Prefab in Szene gespawnt.";
        }
    }

    if (ctx.state.prefabEditorDirty)
    {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.f, 0.8f, 0.2f, 1.f), "(ungespeichert)");
    }

    ImGui::Separator();

    // ── Haupt-Layout: Viewport links, Inspector rechts ────────────────────────
    const float inspectorWidth = 280.f;
    const float viewportWidth  = ImGui::GetContentRegionAvail().x - inspectorWidth - 8.f;
    const float panelHeight    = ImGui::GetContentRegionAvail().y;

    // ── Viewport ─────────────────────────────────────────────────────────────
    ImGui::BeginChild("##prefab_viewport", ImVec2(viewportWidth, panelHeight), false,
                      ImGuiWindowFlags_NoScrollbar);
    {
        if (ctx.prefabPreviewTexture && ctx.prefabPreviewRT.IsValid())
        {
            void* texId = ctx.prefabPreviewTexture();
            const ImVec2 size{ viewportWidth, panelHeight };

            // InvisibleButton als Interaktions-Layer: besitzt Mouse-Focus vollstaendig.
            // Das verhindert dass ImGui den Drag als Fenster-Verschiebung interpretiert.
            // IsItemActive() ist true solange die Maustaste NACH einem Klick auf diesen
            // Button gedrueckt bleibt — unabhaengig davon wo die Maus danach hinwandert.
            ImGui::InvisibleButton("##prefab_viewport_input", size);
            const bool active  = ImGui::IsItemActive();
            const bool hovered = ImGui::IsItemHovered();

            // Bild ueber den Button zeichnen (DrawList, da Button bereits gerendert)
            if (texId)
            {
                const ImVec2 pMin = ImGui::GetItemRectMin();
                const ImVec2 pMax = ImGui::GetItemRectMax();
                const ImVec2 uv0  = ctx.prefabPreviewFlipY ? ImVec2(0, 1) : ImVec2(0, 0);
                const ImVec2 uv1  = ctx.prefabPreviewFlipY ? ImVec2(1, 0) : ImVec2(1, 1);
                ImGui::GetWindowDrawList()->AddImage(texId, pMin, pMax, uv0, uv1);
            }
            else
            {
                ImGui::SetCursorScreenPos(ImGui::GetItemRectMin());
                ImGui::TextDisabled("(Vorschau wird aufgebaut...)");
            }

            const ImGuiIO& io = ImGui::GetIO();

            // Orbit-Drag: IsItemActive() stellt sicher dass der Drag auf DIESEM Button
            // gestartet wurde. Fenster-Verschiebung ist damit ausgeschlossen.
            if (active)
            {
                ctx.state.prefabPreviewYawDeg   -= io.MouseDelta.x * 0.4f;
                ctx.state.prefabPreviewPitchDeg += io.MouseDelta.y * 0.4f;
                ctx.state.prefabPreviewPitchDeg =
                    std::clamp(ctx.state.prefabPreviewPitchDeg, -89.f, 89.f);
            }

            // Scroll wenn Maus ueber Viewport
            if (hovered && io.MouseWheel != 0.f)
            {
                ctx.state.prefabPreviewDistance -=
                    io.MouseWheel * 0.15f * ctx.state.prefabPreviewDistance;
                ctx.state.prefabPreviewDistance =
                    std::clamp(ctx.state.prefabPreviewDistance,
                               ctx.state.prefabPreviewBoundsRadius * 0.3f,
                               ctx.state.prefabPreviewBoundsRadius * 20.f);
            }
        }
        else
        {
            ImGui::TextDisabled("Prefab-Vorschau");
            ImGui::TextDisabled("(kein Render-Target)");
        }
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // ── Inspector ─────────────────────────────────────────────────────────────
    ImGui::BeginChild("##prefab_inspector", ImVec2(inspectorWidth, panelHeight), true);
    {
        ImGui::TextUnformatted("Records");
        ImGui::Separator();

        for (size_t i = 0; i < prefab.records.size(); ++i)
        {
            auto& record = prefab.records[i];
            ImGui::PushID(static_cast<int>(i));

            const bool isRoot = (i == static_cast<size_t>(prefab.rootIndex));
            const std::string nodeLabel = (isRoot ? "[Root] " : "") + record.name;

            const bool open = ImGui::TreeNodeEx(nodeLabel.c_str(),
                ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth);

            if (open)
            {
                // Name
                {
                    char buf[128];
                    std::snprintf(buf, sizeof(buf), "%s", record.name.c_str());
                    ImGui::SetNextItemWidth(-1.f);
                    if (ImGui::InputText("##name", buf, sizeof(buf)))
                    {
                        record.name = buf;
                        ctx.state.prefabEditorDirty = true;
                    }
                }

                // Material
                ImGui::Spacing();
                ImGui::TextDisabled("Material:");
                {
                    const std::string matDisplay =
                        record.materialAssetPath.empty()
                            ? "(kein)"
                            : std::filesystem::path(record.materialAssetPath).filename().string();
                    ImGui::SetNextItemWidth(-1.f);
                    ImGui::TextUnformatted(matDisplay.c_str());
                    if (ImGui::IsItemHovered() && !record.materialAssetPath.empty())
                        ImGui::SetTooltip("%s", record.materialAssetPath.c_str());

                    // Drag & Drop Material (.kmat)
                    if (ImGui::BeginDragDropTarget())
                    {
                        if (const ImGuiPayload* payload =
                            ImGui::AcceptDragDropPayload("KROM_ASSET_MATERIAL"))
                        {
                            const char* droppedPath =
                                static_cast<const char*>(payload->Data);
                            record.materialAssetPath = droppedPath;
                            record.material          = MaterialHandle{};
                            record.baseColorTexturePath.clear();
                            ctx.state.prefabEditorDirty = true;
                            // Preview aktualisieren
                            SpawnPrefabPreview(ctx, prefab);
                        }
                        ImGui::EndDragDropTarget();
                    }
                }

                // Slot-Overrides
                if (!record.slotOverrides.empty())
                {
                    ImGui::Spacing();
                    ImGui::TextDisabled("Slot-Overrides: %u",
                                        static_cast<uint32_t>(record.slotOverrides.size()));
                    for (auto& slot : record.slotOverrides)
                    {
                        const std::string slotDisplay =
                            "Slot " + std::to_string(slot.submeshIndex) + ": " +
                            (slot.materialAssetPath.empty()
                                 ? "(kein)"
                                 : std::filesystem::path(slot.materialAssetPath)
                                       .filename().string());
                        ImGui::TextDisabled("%s", slotDisplay.c_str());
                    }
                }

                // Transform
                ImGui::Spacing();
                ImGui::TextDisabled("Position: %.2f / %.2f / %.2f",
                    record.localPosition.x, record.localPosition.y, record.localPosition.z);
                ImGui::TextDisabled("Scale: %.2f / %.2f / %.2f",
                    record.localScale.x, record.localScale.y, record.localScale.z);

                // Mesh-Pfad
                if (!record.meshAssetPath.empty())
                {
                    ImGui::Spacing();
                    ImGui::TextDisabled("Mesh:");
                    const std::string meshDisplay =
                        std::filesystem::path(record.meshAssetPath).filename().string() +
                        (record.meshAssetPath.find('#') != std::string::npos
                             ? ("  " + record.meshAssetPath.substr(
                                           record.meshAssetPath.rfind('#')))
                             : std::string{});
                    ImGui::TextUnformatted(meshDisplay.c_str());
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", record.meshAssetPath.c_str());
                }

                ImGui::TreePop();
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();

    ImGui::End();

    if (!windowOpen)
    {
        ctx.state.prefabEditorOpen = false;
        DestroyPrefabPreview(ctx);
    }

#endif // KROM_EDITOR_HAS_IMGUI
}

} // namespace engine::addons::editor
