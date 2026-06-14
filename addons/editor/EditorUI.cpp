#include "addons/editor/EditorUI.hpp"
#include "core/Debug.hpp"
#include "addons/editor/EditorCommands.hpp"
#include "addons/editor/EditorFeature.hpp"
#include "addons/editor/EditorAssetBrowser.hpp"
#include "addons/editor/EditorFileNaming.hpp"
#include "addons/editor/EditorScriptAssets.hpp"
#include "addons/editor/EditorComponents.hpp"
#include "addons/editor/EditorMaterialLibrary.hpp"
#include "addons/debug_draw/DebugDraw.hpp"
#include "addons/mesh_renderer/MeshSceneQueries.hpp"
#include "addons/mesh_renderer/MeshRendererSerialization.hpp"
#include "addons/prefab/Prefab.hpp"
#include "addons/prefab/PrefabInstanceComponent.hpp"
#include "EditorPrefabWindow.hpp"
#include "collision/SceneQueries.hpp"

#ifdef KROM_EDITOR_HAS_IMGUI

#include "addons/camera/CameraComponents.hpp"
#include "addons/lighting/LightingComponents.hpp"
#include "addons/lit/LitMaterial.hpp"
#include "addons/mesh_renderer/MeshRendererComponents.hpp"
#include "addons/script/ScriptList.hpp"
#include "renderer/RenderLayers.hpp"
#include "assets/AssetPipeline.hpp"
#include "assets/MeshTangents.hpp"
#include "ecs/World.hpp"
#include "ecs/Components.hpp"
#include "core/Math.hpp"
#include "core/Types.hpp"

#include "imgui.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <string>
#include <sstream>
#include <utility>
#include <unordered_set>
#include <vector>

namespace engine::renderer::addons::editor {
namespace {

static bool IsScenePickEntity(const ecs::World& world, EntityID entity);

std::string RuntimeScriptFieldKey(const std::string& entityGuid,
                                  uint32_t scriptIndex,
                                  const std::string& fieldName)
{
    return entityGuid + "|" + std::to_string(scriptIndex) + "|" + fieldName;
}

std::string RuntimeScriptFieldValueText(const engine::script::ScriptFieldValue& value)
{
    std::ostringstream out;
    switch (value.type)
    {
    case engine::script::ScriptFieldType::Float:
        out << value.floatValue;
        break;
    case engine::script::ScriptFieldType::Int:
        out << value.intValue;
        break;
    case engine::script::ScriptFieldType::Bool:
        out << (value.boolValue ? "true" : "false");
        break;
    case engine::script::ScriptFieldType::Vec3:
        out << value.vec3Value.x << ", " << value.vec3Value.y << ", " << value.vec3Value.z;
        break;
    case engine::script::ScriptFieldType::String:
    case engine::script::ScriptFieldType::Prefab:
    case engine::script::ScriptFieldType::Entity:
        out << value.stringValue;
        break;
    }
    return out.str();
}

EntityID FindEntityByGuid(ecs::World& world, const std::string& guid)
{
    if (guid.empty())
        return NULL_ENTITY;

    EntityID result = NULL_ENTITY;
    world.ForEachAlive([&](EntityID entity)
    {
        if (result.IsValid())
            return;
        const auto* component = world.Get<GuidComponent>(entity);
        if (component && component->guid == guid)
            result = entity;
    });
    return result;
}

std::string EntityLabel(const ecs::World& world, EntityID entity)
{
    if (!entity.IsValid())
        return "(None)";
    std::string label;
    if (const auto* name = world.Get<NameComponent>(entity))
        label = name->name;
    if (label.empty())
        label = "Entity";
    return label + " #" + std::to_string(entity.value);
}

std::string EnsureEntityGuid(ecs::World& world, EntityID entity)
{
    if (!entity.IsValid() || !world.IsAlive(entity))
        return {};

    if (auto* guid = world.Get<GuidComponent>(entity))
    {
        if (!guid->guid.empty())
            return guid->guid;
    }

    static uint32_t counter = 1u;
    std::string value;
    do
    {
        value = "entity-" + std::to_string(entity.value) + "-" + std::to_string(counter++);
    } while (FindEntityByGuid(world, value).IsValid());

    if (auto* guid = world.Get<GuidComponent>(entity))
        guid->guid = value;
    else
        world.Add<GuidComponent>(entity, GuidComponent{value});
    return value;
}

bool DrawScriptFieldEditor(const char* fieldName,
                           EditorFrameContext& ctx,
                           engine::script::ScriptFieldType fieldType,
                           engine::script::ScriptFieldValue& value,
                           bool hasLiveValue)
{
    using engine::script::ScriptFieldType;

    bool changed = false;
    ImGui::PushID(fieldName);
    const float labelWidth = std::min(118.f, ImGui::GetContentRegionAvail().x * 0.42f);
    ImGui::TextUnformatted(fieldName);
    if (hasLiveValue && ImGui::IsItemHovered())
        ImGui::SetTooltip("Live-Wert aus der laufenden Runtime");
    ImGui::SameLine(labelWidth);
    ImGui::SetNextItemWidth(-1.f);
    if (hasLiveValue)
        ImGui::BeginDisabled();
    switch (fieldType)
    {
    case ScriptFieldType::Float:
        changed = ImGui::DragFloat("##value", &value.floatValue, 0.1f);
        break;
    case ScriptFieldType::Int:
        changed = ImGui::DragInt("##value", &value.intValue, 1.0f);
        break;
    case ScriptFieldType::Bool:
        changed = ImGui::Checkbox("##value", &value.boolValue);
        break;
    case ScriptFieldType::Vec3:
        changed = ImGui::DragFloat3("##value", &value.vec3Value.x, 0.1f);
        break;
    case ScriptFieldType::String:
    case ScriptFieldType::Prefab:
    {
        std::array<char, 512> text{};
        std::snprintf(text.data(), text.size(), "%s", value.stringValue.c_str());
        changed = ImGui::InputText("##value", text.data(), text.size());
        if (changed)
            value.stringValue = text.data();
        if (fieldType == ScriptFieldType::Prefab && ImGui::IsItemHovered())
            ImGui::SetTooltip("Relativer Pfad zu Assets, z.B. Prefabs/Bullet.prefab");
        break;
    }
    case ScriptFieldType::Entity:
    {
        if (!value.entityValue.IsValid() && !value.stringValue.empty())
            value.entityValue = FindEntityByGuid(ctx.world, value.stringValue);

        const std::string preview = EntityLabel(ctx.world, value.entityValue);
        if (ImGui::BeginCombo("##value", preview.c_str()))
        {
            const bool noneSelected = !value.entityValue.IsValid();
            if (ImGui::Selectable("(None)", noneSelected))
            {
                value.entityValue = NULL_ENTITY;
                value.stringValue.clear();
                changed = true;
            }
            if (noneSelected)
                ImGui::SetItemDefaultFocus();

            ctx.world.ForEachAlive([&](EntityID candidate)
            {
                if (!IsScenePickEntity(ctx.world, candidate))
                    return;
                const bool selected = candidate == value.entityValue;
                const std::string label = EntityLabel(ctx.world, candidate);
                if (ImGui::Selectable(label.c_str(), selected))
                {
                    value.entityValue = candidate;
                    value.stringValue = EnsureEntityGuid(ctx.world, candidate);
                    changed = true;
                }
                if (selected)
                    ImGui::SetItemDefaultFocus();
            });
            ImGui::EndCombo();
        }
        if (ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("KROM_EDITOR_ENTITY"))
            {
                if (payload->DataSize == sizeof(EntityID))
                {
                    const EntityID dropped = *static_cast<const EntityID*>(payload->Data);
                    if (IsScenePickEntity(ctx.world, dropped))
                    {
                        value.entityValue = dropped;
                        value.stringValue = EnsureEntityGuid(ctx.world, dropped);
                        changed = true;
                    }
                }
            }
            ImGui::EndDragDropTarget();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("X"))
        {
            value.entityValue = NULL_ENTITY;
            value.stringValue.clear();
            changed = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Entity-Referenz loeschen");
        break;
    }
    }
    if (hasLiveValue)
    {
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("Live");
    }
    ImGui::PopID();
    return changed && !hasLiveValue;
}

static float EditorPickMaxDistance(const EditorCameraState& cam) noexcept
{
    return std::max(cam.farPlane, 100000.f);
}

static bool ShouldCancelSelectionChangeForActiveInput()
{
    ImGuiIO& io = ImGui::GetIO();
    return io.WantTextInput;
}

static bool IsScenePickEntity(const ecs::World& world, EntityID entity)
{
    if (!entity.IsValid() || !world.IsAlive(entity))
        return false;
    if (world.Has<EditorMaterialPreviewComponent>(entity)) return false;
    if (world.Has<EditorDpadGizmoComponent>(entity)) return false;
    if (world.Has<EditorRotateGizmoComponent>(entity)) return false;
    if (world.Has<EditorScaleGizmoComponent>(entity)) return false;
    if (world.Has<EditorAssetThumbnailComponent>(entity)) return false;
    if (world.Has<EditorPrefabPreviewComponent>(entity)) return false;
    if (world.Has<EditorRuntimeGizmoTag>(entity)) return false;
    return true;
}

static std::string SanitizeAssetStem(std::string name)
{
    for (char& c : name)
    {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (!std::isalnum(uc) && c != '_' && c != '-')
            c = '_';
    }
    return name.empty() ? std::string("Prefab") : name;
}

static bool SaveEntityAsPrefab(EditorFrameContext& ctx, EntityID entity)
{
    if (!entity.IsValid() || !ctx.world.IsAlive(entity))
        return false;

    std::string prefabName = "Prefab";
    if (const auto* name = ctx.world.Get<NameComponent>(entity))
        if (!name->name.empty())
            prefabName = name->name;

    const engine::addons::prefab::PrefabAsset prefab =
        engine::addons::prefab::BuildPrefabFromWorldEntity(
            ctx.world, entity, prefabName, &ctx.registry, ctx.scriptRegistry);
    if (prefab.Empty())
    {
        ctx.lastFileMessage = "Prefab konnte nicht erstellt werden.";
        return false;
    }

    const std::filesystem::path projectRoot(ctx.currentProjectRoot);
    const std::filesystem::path prefabDir = projectRoot.empty()
        ? (std::filesystem::path("assets") / "Prefabs")
        : (projectRoot / "Assets" / "Prefabs");
    const std::filesystem::path prefabPath =
        MakeUniqueFilesystemPath(prefabDir, SanitizeAssetStem(prefabName), ".prefab");

    std::string error;
    if (!engine::addons::prefab::SavePrefabToFile(prefab, prefabPath, &error))
    {
        ctx.lastFileMessage = error.empty() ? "Prefab konnte nicht gespeichert werden." : error;
        return false;
    }

    ctx.lastFileMessage = "Prefab gespeichert: " + prefabPath.string();

    // Live-Link: alle Szenen-Instanzen synchronisieren
    engine::addons::editor::ResolvePrefabBindings(ctx);

    return true;
}

static std::filesystem::path ResolveSelectedPrefabPath(const EditorFrameContext& ctx)
{
    std::filesystem::path path(ctx.state.selectedPrefabAssetPath);
    if (path.empty() || path.is_absolute())
        return path;
    if (!ctx.currentProjectRoot.empty())
        return std::filesystem::path(ctx.currentProjectRoot) / "Assets" / path;
    return path;
}

static bool SpawnPrefabAsset(EditorFrameContext& ctx,
                             const std::filesystem::path& path,
                             const math::Vec3* worldPosition = nullptr)
{
    engine::addons::prefab::PrefabAsset prefab;
    std::string error;
    if (!engine::addons::prefab::LoadPrefabFromFile(path, ctx.registry, prefab, &error))
    {
        ctx.lastFileMessage = error.empty() ? "Prefab konnte nicht geladen werden." : error;
        return false;
    }

    engine::addons::prefab::PrefabInstantiateOptions options{};
    options.scriptRegistry = ctx.scriptRegistry;
    const engine::addons::prefab::PrefabInstance instance =
        engine::addons::prefab::InstantiatePrefab(ctx.world, prefab, options);
    if (!instance.IsValid())
    {
        ctx.lastFileMessage = "Prefab konnte nicht instanziiert werden.";
        return false;
    }

    if (ctx.assetPipeline)
    {
        engine::addons::mesh_renderer::ResolveMeshAssetBindings(
            *ctx.assetPipeline, ctx.registry, ctx.world);
        ResolveMaterialAssetBindings(ctx);
    }

    if (worldPosition)
    {
        if (auto* transform = ctx.world.Get<TransformComponent>(instance.root))
        {
            transform->localPosition = *worldPosition;
            transform->dirty = true;
            ++transform->localVersion;
        }
    }

    // PrefabInstanceComponent: Live-Link zur Quell-Datei
    {
        std::string relPath = path.generic_string();
        if (!ctx.currentProjectRoot.empty())
        {
            const std::filesystem::path assetsRoot =
                std::filesystem::path(ctx.currentProjectRoot) / "Assets";
            std::error_code ec;
            auto rel = std::filesystem::relative(path, assetsRoot, ec);
            if (!ec) relPath = rel.generic_string();
        }
        ctx.world.Add<PrefabInstanceComponent>(instance.root,
            PrefabInstanceComponent{ relPath, /*overrideTransform=*/true });
    }

    ctx.state.selectedEntity = instance.root;
    ctx.lastFileMessage = "Prefab geladen: " + path.filename().string();
    return true;
}

static const char* EditorProjectBackendLabel(renderer::DeviceFactory::BackendType backend) noexcept
{
    switch (backend)
    {
    case renderer::DeviceFactory::BackendType::DirectX11: return "DirectX11";
    case renderer::DeviceFactory::BackendType::OpenGL: return "OpenGL";
    case renderer::DeviceFactory::BackendType::Vulkan: return "Vulkan";
    default: return "Unknown";
    }
}

static renderer::DeviceFactory::BackendType EditorProjectBackendFromSelection(int selection) noexcept
{
    switch (selection)
    {
    case 1: return renderer::DeviceFactory::BackendType::DirectX11;
    case 2: return renderer::DeviceFactory::BackendType::OpenGL;
    default: return renderer::DeviceFactory::BackendType::Vulkan;
    }
}

struct StartProjectEntry
{
    std::string name;
    std::string path;
    std::string projectFile;
    std::filesystem::file_time_type writeTime{};
};

static std::string ReadProjectDisplayName(const std::filesystem::path& projectFile,
                                          const std::string& fallback)
{
    std::ifstream in(projectFile, std::ios::binary);
    if (!in)
        return fallback;

    const std::string json((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    const std::string key = "\"name\"";
    const size_t keyPos = json.find(key);
    if (keyPos == std::string::npos)
        return fallback;
    const size_t colon = json.find(':', keyPos + key.size());
    if (colon == std::string::npos)
        return fallback;
    const size_t quote0 = json.find('"', colon + 1u);
    if (quote0 == std::string::npos)
        return fallback;
    const size_t quote1 = json.find('"', quote0 + 1u);
    if (quote1 == std::string::npos || quote1 <= quote0 + 1u)
        return fallback;
    return json.substr(quote0 + 1u, quote1 - quote0 - 1u);
}

static std::vector<StartProjectEntry> ScanStartProjects(EditorFrameContext& ctx)
{
    std::vector<StartProjectEntry> projects;
    std::filesystem::path root = ctx.state.projectParentDirBuffer.data();
    if (root.empty() && !ctx.currentProjectRoot.empty())
        root = std::filesystem::path(ctx.currentProjectRoot).parent_path();
    if (root.empty() || !std::filesystem::exists(root))
        return projects;

    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(root, ec))
    {
        if (ec || !entry.is_directory())
            continue;
        const std::filesystem::path projectFile = entry.path() / "krom-project.json";
        if (!std::filesystem::exists(projectFile))
            continue;

        StartProjectEntry p{};
        p.name = ReadProjectDisplayName(projectFile, entry.path().filename().string());
        p.path = entry.path().string();
        p.projectFile = projectFile.string();
        p.writeTime = std::filesystem::last_write_time(projectFile, ec);
        projects.push_back(std::move(p));
    }

    std::sort(projects.begin(), projects.end(), [](const StartProjectEntry& a,
                                                   const StartProjectEntry& b) {
        if (a.writeTime != b.writeTime)
            return a.writeTime > b.writeTime;
        return a.name < b.name;
    });
    return projects;
}

static void* GetStartEditorTextureId(EditorFrameContext& ctx,
                                     const char* filename,
                                     std::string& cachedPath,
                                     TextureHandle& cachedTexture)
{
    if (!ctx.assetPipeline || !ctx.editorTextureId || ctx.engineEditorDir.empty())
        return nullptr;

    const std::string path = (std::filesystem::path(ctx.engineEditorDir) / filename).string();
    if (!cachedTexture.IsValid() || cachedPath != path)
    {
        cachedPath = path;
        const TextureHandle assetTexture = ctx.assetPipeline->LoadTexture(path);
        if (!assetTexture.IsValid())
            return nullptr;
        ctx.assetPipeline->UploadPendingGpuAssets();
        cachedTexture = ctx.assetPipeline->GetGpuTexture(assetTexture);
    }

    return cachedTexture.IsValid() ? ctx.editorTextureId(cachedTexture) : nullptr;
}

static void* GetStartPreviewTextureId(EditorFrameContext& ctx)
{
    static std::string s_previewPath;
    static TextureHandle s_previewTexture;
    return GetStartEditorTextureId(ctx, "preview.png", s_previewPath, s_previewTexture);
}

static void DrawStartEnginePreview(EditorFrameContext& ctx, const ImVec2& size)
{
    ImGui::BeginChild("##StartEnginePreview", size, false);
    const ImVec2 min = ImGui::GetWindowPos();
    const ImVec2 max = ImVec2(min.x + ImGui::GetWindowSize().x, min.y + ImGui::GetWindowSize().y);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(min, max, IM_COL32(38, 45, 50, 255));

    constexpr float kPreviewAspect = 572.f / 852.f;
    const float pad = 0.f;
    const float maxW = std::max(1.f, size.x - pad * 2.f);
    const float maxH = std::max(1.f, size.y - pad * 2.f);
    float imageW = maxW;
    float imageH = imageW / kPreviewAspect;
    if (imageH > maxH)
    {
        imageH = maxH;
        imageW = imageH * kPreviewAspect;
    }
    const ImVec2 imageMin(min.x + (size.x - imageW) * 0.5f,
                          min.y + (size.y - imageH) * 0.5f);
    const ImVec2 imageMax(imageMin.x + imageW, imageMin.y + imageH);
    if (void* texId = GetStartPreviewTextureId(ctx))
        dl->AddImage(reinterpret_cast<ImTextureID>(texId), imageMin, imageMax);
    dl->AddRect(min, max, IM_COL32(56, 63, 68, 255));
    ImGui::EndChild();
}

static void* GetStartLogoTextureId(EditorFrameContext& ctx)
{
    static std::string s_logoPath;
    static TextureHandle s_logoTexture;
    return GetStartEditorTextureId(ctx, "logo.png", s_logoPath, s_logoTexture);
}

static void DrawStartLogo(EditorFrameContext& ctx, const ImVec2& size)
{
    ImGui::BeginChild("##StartLogoBox", size, false);
    constexpr float kLogoAspect = 629.f / 145.f;
    const float pad = 14.f;
    const float maxW = std::max(1.f, size.x - pad * 2.f);
    const float maxH = std::max(1.f, size.y - pad * 2.f);
    float logoW = maxW;
    float logoH = logoW / kLogoAspect;
    if (logoH > maxH)
    {
        logoH = maxH;
        logoW = logoH * kLogoAspect;
    }

    ImGui::SetCursorPos(ImVec2((size.x - logoW) * 0.5f, (size.y - logoH) * 0.5f));
    if (void* texId = GetStartLogoTextureId(ctx))
    {
        ImGui::Image(reinterpret_cast<ImTextureID>(texId), ImVec2(logoW, logoH));
    }
    else
    {
        ImGui::SetWindowFontScale(2.0f);
        ImGui::TextUnformatted("KROM ENGINE");
        ImGui::SetWindowFontScale(1.f);
    }
    ImGui::EndChild();
}

static math::Vec3 WorldPosition(EditorFrameContext& ctx, EntityID entity);
static math::Quat WorldRotation(EditorFrameContext& ctx, EntityID entity);
static math::Vec3 WorldScale(EditorFrameContext& ctx, EntityID entity);
static math::Mat4 CurrentWorldMatrix(EditorFrameContext& ctx, EntityID entity) noexcept;
static void SetWorldTransform(EditorFrameContext& ctx,
                              EntityID entity,
                              TransformComponent& transform,
                              const math::Mat4& desiredWorldMatrix);
static void SetWorldPosition(EditorFrameContext& ctx, EntityID entity, TransformComponent& transform, const math::Vec3& worldPosition);
static void SetWorldRotation(EditorFrameContext& ctx, EntityID entity, TransformComponent& transform, const math::Quat& worldRotation);

// Unity-Inspector-Konvention:
// Euler-Werte X/Y/Z werden in der Reihenfolge Z, dann X, dann Y angewendet.
// Quaternion nach Euler-Winkeln (Grad), Reihenfolge passend zu Quat::FromEulerDeg.
static math::Vec3 QuatToEulerDeg(const math::Quat& q) noexcept
{
    float sinPitch = 2.f * (q.w * q.x - q.y * q.z);
    float clamped  = std::clamp(sinPitch, -1.f, 1.f);
    float pitch    = std::asin(clamped);
    float yaw, roll;
    if (std::abs(sinPitch) < 0.9999f)
    {
        yaw  = std::atan2(2.f * (q.x * q.z + q.w * q.y),
                          1.f - 2.f * (q.x * q.x + q.y * q.y));
        roll = std::atan2(2.f * (q.x * q.y + q.w * q.z),
                          1.f - 2.f * (q.x * q.x + q.z * q.z));
    }
    else
    {
        yaw  = std::atan2(-2.f * (q.x * q.z - q.w * q.y),
                           1.f -  2.f * (q.y * q.y + q.z * q.z));
        roll = 0.f;
    }
    return { pitch * math::RAD_TO_DEG, yaw * math::RAD_TO_DEG, roll * math::RAD_TO_DEG };
}

static float NormalizeAngleDeg(float angle) noexcept
{
    while (angle > 360.f)  angle -= 360.f;
    while (angle < 0.f)    angle += 360.f;
    return angle;
}

static math::Vec3 NormalizeEulerDeg(const math::Vec3& euler) noexcept
{
    return {
        NormalizeAngleDeg(euler.x),
        NormalizeAngleDeg(euler.y),
        NormalizeAngleDeg(euler.z)
    };
}

static math::Vec3 PrepareRotationEditEuler(EditorFrameContext& ctx,
                                           EntityID entity,
                                           EditorTransformSpace space,
                                           const math::Quat& rotation) noexcept
{
    EditorState& state = ctx.state;
    const float quatDot = std::abs(
        state.rotationEditQuat.x * rotation.x +
        state.rotationEditQuat.y * rotation.y +
        state.rotationEditQuat.z * rotation.z +
        state.rotationEditQuat.w * rotation.w);
    const bool needsRefresh =
        !state.rotationEditInitialized ||
        state.rotationEditEntity != entity ||
        state.rotationEditSpace != space ||
        (!state.rotationEditActive && quatDot < 0.9999f);

    if (needsRefresh)
    {
        state.rotationEditEntity      = entity;
        state.rotationEditSpace       = space;
        state.rotationEditEuler       = NormalizeEulerDeg(QuatToEulerDeg(rotation));
        state.rotationEditQuat        = rotation;
        state.rotationEditInitialized = true;
    }

    return state.rotationEditEuler;
}

static void CommitRotationEditState(EditorFrameContext& ctx,
                                    EntityID entity,
                                    EditorTransformSpace space,
                                    const math::Vec3& euler,
                                    const math::Quat& rotation) noexcept
{
    EditorState& state = ctx.state;
    state.rotationEditEntity      = entity;
    state.rotationEditSpace       = space;
    state.rotationEditEuler       = NormalizeEulerDeg(euler);
    state.rotationEditQuat        = rotation;
    state.rotationEditActive      = ImGui::IsItemActive();
    state.rotationEditInitialized = true;
}

void DrawEntityList(EditorFrameContext& ctx)
{
    ImGui::SetNextWindowPos(ImVec2(10.f, 10.f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(260.f, 380.f), ImGuiCond_FirstUseEver);
    ImGui::Begin("Scene##editor");

    static bool showMeshes = true;
    static bool showCameras = true;
    static bool showLights = true;
    static bool showOther = true;

    ImGui::Checkbox("Mesh", &showMeshes);
    ImGui::SameLine();
    ImGui::Checkbox("Camera", &showCameras);
    ImGui::SameLine();
    ImGui::Checkbox("Light", &showLights);
    ImGui::Checkbox("Other", &showOther);
    ImGui::Separator();

    std::vector<EntityID> entities;
    ctx.world.ForEachAlive([&](EntityID id) {
        entities.push_back(id);
    });
    std::sort(entities.begin(), entities.end(), [&](EntityID a, EntityID b) {
        const uint32_t orderA = GetEntityHierarchyOrder(ctx, a);
        const uint32_t orderB = GetEntityHierarchyOrder(ctx, b);
        if (orderA != orderB)
            return orderA < orderB;
        return a.value < b.value;
    });

    auto passesFilter = [&](EntityID id) {
        // Laufzeit-Gizmo-Entities sind rein visuelle Hilfsobjekte — nie in der Liste anzeigen
        if (ctx.world.Get<EditorRuntimeGizmoTag>(id) != nullptr)
            return false;
        if (ctx.world.Get<EditorMaterialPreviewComponent>(id) != nullptr)
            return false;
        if (ctx.world.Get<EditorAssetThumbnailComponent>(id) != nullptr)
            return false;
        // Zweite Absicherung: Meshes auf dem Editor-Gizmo-Layer nie anzeigen
        if (const auto* mc = ctx.world.Get<MeshComponent>(id))
            if (mc->layerMask == renderer::LAYER_EDITOR_GIZMO ||
                mc->layerMask == renderer::LAYER_EDITOR_GIZMO_DEPTH ||
                mc->layerMask == renderer::LAYER_EDITOR_MATERIAL_PREVIEW ||
                mc->layerMask == renderer::LAYER_EDITOR_ASSET_THUMBNAIL)
                return false;
        const bool isCamera = ctx.world.Get<CameraComponent>(id) != nullptr;
        const bool isLight = ctx.world.Get<LightComponent>(id) != nullptr;
        const bool isMesh = ctx.world.Get<MeshComponent>(id) != nullptr;
        const bool isOther = !isCamera && !isLight && !isMesh;
        return !((isMesh && !showMeshes) || (isCamera && !showCameras) ||
                 (isLight && !showLights) || (isOther && !showOther));
    };

    auto makeLabel = [&](EntityID id, char* label, size_t labelSize) {
        const bool isCamera = ctx.world.Get<CameraComponent>(id) != nullptr;
        const bool isLight = ctx.world.Get<LightComponent>(id) != nullptr;
        const bool isMesh = ctx.world.Get<MeshComponent>(id) != nullptr;
        const char* typePrefix = "[Entity]";
        if (isCamera)
            typePrefix = "[Camera]";
        else if (isLight)
            typePrefix = "[Light]";
        else if (isMesh)
            typePrefix = "[Mesh]";

        const char* name = nullptr;
        if (const auto* nc = ctx.world.Get<NameComponent>(id))
            name = nc->name.c_str();

        if (name && name[0] != '\0')
            std::snprintf(label, labelSize, "%s %s", typePrefix, name);
        else
            std::snprintf(label, labelSize, "%s Entity %u", typePrefix, id.Index());
    };

    auto isDirectChildOf = [&](EntityID child, EntityID parent) {
        const auto* pc = ctx.world.Get<ParentComponent>(child);
        return pc && pc->parent == parent;
    };

    auto hasVisibleDescendant = [&](auto&& self, EntityID parent, uint32_t depth) -> bool {
        if (depth > 32u)
            return false;

        for (EntityID candidate : entities)
        {
            if (!isDirectChildOf(candidate, parent))
                continue;
            if (passesFilter(candidate) || self(self, candidate, depth + 1u))
                return true;
        }
        return false;
    };

    auto drawEntity = [&](auto&& self, EntityID id, uint32_t depth) -> void {
        if (depth > 32u)
            return;

        const bool visibleSelf = passesFilter(id);
        const bool visibleDescendant = hasVisibleDescendant(hasVisibleDescendant, id, depth);
        if (!visibleSelf && !visibleDescendant)
            return;

        char label[112];
        makeLabel(id, label, sizeof(label));

        bool selected = (ctx.state.selectedEntity == id)
                     || IsEntityInMultiSelection(ctx.state, id);
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow |
                                   ImGuiTreeNodeFlags_SpanAvailWidth |
                                   ImGuiTreeNodeFlags_DefaultOpen;
        if (selected)
            flags |= ImGuiTreeNodeFlags_Selected;
        if (!visibleDescendant)
            flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;

        if (!visibleSelf)
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));

        const bool open = ImGui::TreeNodeEx(
            reinterpret_cast<void*>(static_cast<uintptr_t>(id.value)),
            flags,
            "%s",
            label);

        // Rechtsklick-Kontextmenü pro Entity
        if (visibleSelf && ImGui::BeginPopupContextItem())
        {
            ClearMultiSelection(ctx);
            ctx.state.selectedEntity  = id;
            ctx.state.selectionSource = SelectionSource::Outliner;

            if (ImGui::MenuItem("Duplizieren"))
            {
                const EntityID dup = DuplicateEntity(ctx, id);
                if (dup.IsValid())
                    ctx.state.selectedEntity = dup;
            }
            if (ImGui::MenuItem("Nach oben"))
            {
                ExecuteSceneMutation(ctx, "Hierarchie-Reihenfolge geaendert", [&]() {
                    MoveEntityInSiblingOrder(ctx, id, -1);
                });
            }
            if (ImGui::MenuItem("Nach unten"))
            {
                ExecuteSceneMutation(ctx, "Hierarchie-Reihenfolge geaendert", [&]() {
                    MoveEntityInSiblingOrder(ctx, id, 1);
                });
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Loeschen"))
                ctx.state.deleteConfirmOpen = true;
            ImGui::Separator();
            if (ImGui::MenuItem("Als Prefab speichern"))
                SaveEntityAsPrefab(ctx, id);
            ImGui::Separator();
            if (ImGui::BeginMenu("Erstellen"))
            {
                if (ImGui::MenuItem("Leere Entity"))
                    CreateEmptyEntity(ctx);
                ImGui::Separator();
                if (ImGui::MenuItem("Camera"))
                    CreateCameraEntity(ctx);
                ImGui::Separator();
                if (ImGui::MenuItem("Directional Light"))
                    CreateLightEntity(ctx, LightType::Directional);
                if (ImGui::MenuItem("Point Light"))
                    CreateLightEntity(ctx, LightType::Point);
                if (ImGui::MenuItem("Spot Light"))
                    CreateLightEntity(ctx, LightType::Spot);
                ImGui::EndMenu();
            }

            ImGui::EndPopup();
        }

        if (ImGui::IsItemHovered() &&
            ImGui::IsMouseReleased(ImGuiMouseButton_Left) &&
            ImGui::GetIO().MouseDragMaxDistanceSqr[ImGuiMouseButton_Left] < 36.0f &&
            visibleSelf &&
            !ShouldCancelSelectionChangeForActiveInput())
        {
            if (ImGui::GetIO().KeyCtrl)
                AddToMultiSelection(ctx, id);
            else
            {
                ClearMultiSelection(ctx);
                ctx.state.selectedEntity  = id;
                ctx.state.selectionSource = SelectionSource::Outliner;
            }
        }

        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) &&
            visibleSelf && !ShouldCancelSelectionChangeForActiveInput())
        {
            ClearMultiSelection(ctx);
            ctx.state.selectedEntity  = id;
            ctx.state.selectionSource = SelectionSource::Outliner;
            const math::Vec3 target   = WorldPosition(ctx, id);
            EditorCameraState& cam    = ctx.state.editorCamera;
            const math::Vec3 toTarget = target - cam.position;
            const float      dist     = toTarget.Length();
            if (dist > 0.001f)
            {
                const math::Vec3 dir = toTarget.Normalized();
                cam.yawDeg   = std::atan2(-dir.x, -dir.z) * math::RAD_TO_DEG;
                cam.pitchDeg = std::clamp(
                    std::asin(std::clamp(dir.y, -1.f, 1.f)) * math::RAD_TO_DEG,
                    -89.f, 89.f);
            }
        }

        if (visibleSelf && ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID))
        {
            ImGui::SetDragDropPayload("KROM_EDITOR_ENTITY", &id, sizeof(EntityID));
            ImGui::TextUnformatted(label);
            ImGui::EndDragDropSource();
        }

        if (visibleSelf && ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("KROM_EDITOR_ENTITY"))
            {
                if (payload->DataSize == sizeof(EntityID))
                {
                    EntityID dragged = *static_cast<const EntityID*>(payload->Data);
                    const auto* dp = ctx.world.Get<ParentComponent>(dragged);
                    const auto* tp = ctx.world.Get<ParentComponent>(id);
                    Debug::Log("[Drop] dragged=%u(parent=%u) -> target=%u(parent=%u)",
                               dragged.value, dp ? dp->parent.value : 0u,
                               id.value,      tp ? tp->parent.value : 0u);
                    const bool ok = ReparentEntity(ctx, dragged, id);
                    if (!ok)
                    {
                        const auto* dpAfter = ctx.world.Get<ParentComponent>(dragged);
                        Debug::Log("[Drop] FAILED dragged=%u (currentParent=%u, target=%u) – bereits Kind oder Zyklus",
                                   dragged.value,
                                   dpAfter ? dpAfter->parent.value : 0u,
                                   id.value);
                    }
                }
            }
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("KROM_ASSET_MATERIAL"))
            {
                const char* materialPath = static_cast<const char*>(payload->Data);
                if (materialPath && materialPath[0] != '\0')
                {
                    ExecuteSceneMutation(ctx, "Material zugewiesen", [&]() {
                        SelectMaterialAsset(ctx, std::filesystem::path(materialPath));
                        ApplyMaterialAssetToEntity(ctx, id, std::filesystem::path(materialPath));
                    });
                }
            }
            ImGui::EndDragDropTarget();
        }

        if (!visibleSelf)
            ImGui::PopStyleColor();

        if (visibleDescendant && open)
        {
            for (EntityID candidate : entities)
            {
                if (isDirectChildOf(candidate, id))
                    self(self, candidate, depth + 1u);
            }
            ImGui::TreePop();
        }
    };

    for (EntityID id : entities)
    {
        const auto* pc = ctx.world.Get<ParentComponent>(id);
        if (pc && pc->parent.IsValid() && ctx.world.IsAlive(pc->parent))
            continue;
        drawEntity(drawEntity, id, 0u);
    }

    const ImVec2 rootDropSize{
        std::max(1.f, ImGui::GetContentRegionAvail().x),
        std::max(32.f, ImGui::GetContentRegionAvail().y)
    };
    ImGui::InvisibleButton("##SceneRootDropZone", rootDropSize);
    if (ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("KROM_EDITOR_ENTITY"))
        {
            if (payload->DataSize == sizeof(EntityID))
            {
                EntityID dragged = *static_cast<const EntityID*>(payload->Data);
                ReparentEntity(ctx, dragged, NULL_ENTITY);
            }
        }
        ImGui::EndDragDropTarget();
    }

    if (ImGui::BeginPopupContextWindow("SceneCreateContext##editor",
                                       ImGuiPopupFlags_MouseButtonRight))
    {
        if (ctx.state.selectedEntity.IsValid() &&
            ctx.world.IsAlive(ctx.state.selectedEntity))
        {
            if (ImGui::MenuItem("Duplizieren"))
            {
                const EntityID dup = DuplicateEntity(ctx, ctx.state.selectedEntity);
                if (dup.IsValid())
                    ctx.state.selectedEntity = dup;
            }
            if (ImGui::MenuItem("Nach oben"))
            {
                ExecuteSceneMutation(ctx, "Hierarchie-Reihenfolge geaendert", [&]() {
                    MoveEntityInSiblingOrder(ctx, ctx.state.selectedEntity, -1);
                });
            }
            if (ImGui::MenuItem("Nach unten"))
            {
                ExecuteSceneMutation(ctx, "Hierarchie-Reihenfolge geaendert", [&]() {
                    MoveEntityInSiblingOrder(ctx, ctx.state.selectedEntity, 1);
                });
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Als Prefab speichern"))
                SaveEntityAsPrefab(ctx, ctx.state.selectedEntity);
            ImGui::Separator();
        }

        if (ImGui::BeginMenu("Erstellen"))
        {
            if (ImGui::MenuItem("Leere Entity"))
                CreateEmptyEntity(ctx);

            ImGui::Separator();
            if (ImGui::MenuItem("Camera"))
                CreateCameraEntity(ctx);

            ImGui::Separator();
            if (ImGui::MenuItem("Directional Light"))
                CreateLightEntity(ctx, LightType::Directional);
            if (ImGui::MenuItem("Point Light"))
                CreateLightEntity(ctx, LightType::Point);
            if (ImGui::MenuItem("Spot Light"))
                CreateLightEntity(ctx, LightType::Spot);
            ImGui::EndMenu();
        }
        ImGui::EndPopup();
    }

    ImGui::End();
}

// Liefert den MaterialHandle der Entity: erst MaterialComponent, dann MeshAsset-Fallback.
static MaterialHandle GetEntityMaterial(EditorFrameContext& ctx, EntityID entity)
{
    if (auto* mc = ctx.world.Get<MaterialComponent>(entity))
        if (mc->material.IsValid())
            return mc->material;

    if (auto* meshComp = ctx.world.Get<MeshComponent>(entity))
        if (auto* mesh = ctx.registry.meshes.Get(meshComp->mesh))
            if (!mesh->materialHandles.empty())
                return mesh->materialHandles[0];

    return MaterialHandle::Invalid();
}

static const assets::MaterialAsset* GetMaterialAsset(EditorFrameContext& ctx, MaterialHandle handle)
{
    return handle.IsValid() ? ctx.registry.materials.Get(handle) : nullptr;
}

static std::string MaterialDisplayName(const assets::MaterialAsset* material,
                                       MaterialHandle materialHandle)
{
    if (material)
    {
        if (!material->debugName.empty())
            return material->debugName;
        if (!material->path.empty())
            return std::filesystem::path(material->path).filename().string();
    }
    return materialHandle.IsValid() ? "Runtime Material" : "None";
}

static bool CanOpenMaterialAssetPath(const std::string& path)
{
    return !path.empty() && std::filesystem::path(path).extension() == ".mat";
}

static bool IsInspectorMaterialSelection(EditorFrameContext& ctx,
                                         EntityID entity,
                                         uint32_t submeshIndex,
                                         const std::string& materialPath)
{
    return ctx.state.inspectorMaterialEntity == entity &&
           ctx.state.inspectorMaterialSlot == static_cast<int>(submeshIndex) &&
           std::filesystem::path(ctx.state.inspectorMaterialAssetPath).generic_string() ==
               std::filesystem::path(materialPath).generic_string();
}

static void SelectInspectorMaterial(EditorFrameContext& ctx,
                                    EntityID entity,
                                    uint32_t submeshIndex,
                                    const std::string& materialPath)
{
    if (!CanOpenMaterialAssetPath(materialPath))
        return;
    ctx.state.inspectorMaterialEntity = entity;
    ctx.state.inspectorMaterialSlot = static_cast<int>(submeshIndex);
    ctx.state.inspectorMaterialAssetPath = materialPath;
    SelectMaterialAsset(ctx, materialPath);
}

static const char* PrimaryEntityType(EditorFrameContext& ctx, EntityID entity)
{
    if (ctx.world.Get<CameraComponent>(entity))
        return "Camera";
    if (ctx.world.Get<LightComponent>(entity))
        return "Light";
    if (ctx.world.Get<MeshComponent>(entity))
        return "Mesh";
    return "Entity";
}

static std::string ComponentSummary(EditorFrameContext& ctx, EntityID entity)
{
    std::string summary;
    auto append = [&](const char* name) {
        if (!summary.empty())
            summary += ", ";
        summary += name;
    };

    if (ctx.world.Get<TransformComponent>(entity))
        append("Transform");
    if (ctx.world.Get<CameraComponent>(entity))
        append("Camera");
    if (ctx.world.Get<LightComponent>(entity))
        append("Light");
    if (ctx.world.Get<MeshComponent>(entity))
        append("Mesh");
    if (ctx.world.Get<MaterialComponent>(entity))
        append("Material");
    if (ctx.world.Get<ParentComponent>(entity))
        append("Parent");
    if (ctx.world.Get<ChildrenComponent>(entity))
        append("Children");

    return summary.empty() ? std::string("None") : summary;
}

static math::Vec3 MatrixTranslation(const math::Mat4& m) noexcept
{
    return {m.m[3][0], m.m[3][1], m.m[3][2]};
}

static math::Quat QuatFromRotationMatrix(const math::Mat4& m) noexcept
{
    const float trace = m.m[0][0] + m.m[1][1] + m.m[2][2];
    if (trace > 0.f)
    {
        const float s = std::sqrt(trace + 1.f) * 2.f;
        return { (m.m[1][2] - m.m[2][1]) / s,
                 (m.m[2][0] - m.m[0][2]) / s,
                 (m.m[0][1] - m.m[1][0]) / s,
                 0.25f * s };
    }
    if (m.m[0][0] > m.m[1][1] && m.m[0][0] > m.m[2][2])
    {
        const float s = std::sqrt(1.f + m.m[0][0] - m.m[1][1] - m.m[2][2]) * 2.f;
        return { 0.25f * s,
                 (m.m[1][0] + m.m[0][1]) / s,
                 (m.m[2][0] + m.m[0][2]) / s,
                 (m.m[1][2] - m.m[2][1]) / s };
    }
    if (m.m[1][1] > m.m[2][2])
    {
        const float s = std::sqrt(1.f + m.m[1][1] - m.m[0][0] - m.m[2][2]) * 2.f;
        return { (m.m[1][0] + m.m[0][1]) / s,
                 0.25f * s,
                 (m.m[2][1] + m.m[1][2]) / s,
                 (m.m[2][0] - m.m[0][2]) / s };
    }

    const float s = std::sqrt(1.f + m.m[2][2] - m.m[0][0] - m.m[1][1]) * 2.f;
    return { (m.m[2][0] + m.m[0][2]) / s,
             (m.m[2][1] + m.m[1][2]) / s,
             0.25f * s,
             (m.m[0][1] - m.m[1][0]) / s };
}

static math::Vec3 MatrixScale(const math::Mat4& m) noexcept
{
    const math::Vec3 x{m.m[0][0], m.m[0][1], m.m[0][2]};
    const math::Vec3 y{m.m[1][0], m.m[1][1], m.m[1][2]};
    const math::Vec3 z{m.m[2][0], m.m[2][1], m.m[2][2]};
    return {x.Length(), y.Length(), z.Length()};
}

static void DecomposeTRS(const math::Mat4& m,
                         math::Vec3& outTranslation,
                         math::Quat& outRotation,
                         math::Vec3& outScale) noexcept
{
    outTranslation = MatrixTranslation(m);
    outScale = MatrixScale(m);

    math::Mat4 rotationMatrix = math::Mat4::Identity();
    if (outScale.x > math::EPSILON)
    {
        rotationMatrix.m[0][0] = m.m[0][0] / outScale.x;
        rotationMatrix.m[0][1] = m.m[0][1] / outScale.x;
        rotationMatrix.m[0][2] = m.m[0][2] / outScale.x;
    }
    if (outScale.y > math::EPSILON)
    {
        rotationMatrix.m[1][0] = m.m[1][0] / outScale.y;
        rotationMatrix.m[1][1] = m.m[1][1] / outScale.y;
        rotationMatrix.m[1][2] = m.m[1][2] / outScale.y;
    }
    if (outScale.z > math::EPSILON)
    {
        rotationMatrix.m[2][0] = m.m[2][0] / outScale.z;
        rotationMatrix.m[2][1] = m.m[2][1] / outScale.z;
        rotationMatrix.m[2][2] = m.m[2][2] / outScale.z;
    }

    outRotation = QuatFromRotationMatrix(rotationMatrix).Normalized();
}

static bool HasNonUniformScale(const math::Vec3& scale) noexcept
{
    return std::abs(scale.x - scale.y) > 1e-4f ||
           std::abs(scale.x - scale.z) > 1e-4f ||
           std::abs(scale.y - scale.z) > 1e-4f;
}

static bool HasNonUniformScaledParent(EditorFrameContext& ctx, EntityID entity) noexcept
{
    const auto* transform = ctx.world.Get<TransformComponent>(entity);
    if (!transform || !transform->inheritParentScale)
        return false;

    const auto* parent = ctx.world.Get<ParentComponent>(entity);
    if (!parent || !parent->parent.IsValid() || !ctx.world.IsAlive(parent->parent))
        return false;

    for (EntityID cursor = parent->parent; cursor.IsValid() && ctx.world.IsAlive(cursor); )
    {
        const auto* wt = ctx.world.Get<WorldTransformComponent>(cursor);
        if (wt && HasNonUniformScale(wt->scale))
            return true;

        const auto* next = ctx.world.Get<ParentComponent>(cursor);
        cursor = next ? next->parent : NULL_ENTITY;
    }

    return false;
}

static math::Mat4 CurrentWorldMatrix(EditorFrameContext& ctx, EntityID entity) noexcept
{
    if (const auto* wt = ctx.world.Get<WorldTransformComponent>(entity))
        return wt->matrix;

    const auto* t = ctx.world.Get<TransformComponent>(entity);
    if (!t)
        return math::Mat4::Identity();

    return math::Mat4::TRS(WorldPosition(ctx, entity), WorldRotation(ctx, entity), WorldScale(ctx, entity));
}

static void SetWorldTransform(EditorFrameContext& ctx,
                              EntityID entity,
                              TransformComponent& transform,
                              const math::Mat4& desiredWorldMatrix)
{
    math::Vec3 worldPosition{};
    math::Quat worldRotation = math::Quat::Identity();
    math::Vec3 worldScale{1.f, 1.f, 1.f};
    DecomposeTRS(desiredWorldMatrix, worldPosition, worldRotation, worldScale);

    if (const auto* parent = ctx.world.Get<ParentComponent>(entity))
    {
        if (parent->parent.IsValid())
        {
            if (const auto* parentWorld = ctx.world.Get<WorldTransformComponent>(parent->parent))
            {
                const math::Quat invParentRotation = parentWorld->rotation.Conjugate().Normalized();
                transform.localPosition = invParentRotation.Rotate(worldPosition - parentWorld->position);
                if (transform.inheritParentScale)
                {
                    transform.localPosition = {
                        std::abs(parentWorld->scale.x) > math::EPSILON ? transform.localPosition.x / parentWorld->scale.x : transform.localPosition.x,
                        std::abs(parentWorld->scale.y) > math::EPSILON ? transform.localPosition.y / parentWorld->scale.y : transform.localPosition.y,
                        std::abs(parentWorld->scale.z) > math::EPSILON ? transform.localPosition.z / parentWorld->scale.z : transform.localPosition.z
                    };
                }
                transform.localRotation = (invParentRotation * worldRotation).Normalized();
                if (transform.inheritParentScale)
                {
                    transform.localScale = {
                        std::abs(parentWorld->scale.x) > math::EPSILON ? worldScale.x / parentWorld->scale.x : worldScale.x,
                        std::abs(parentWorld->scale.y) > math::EPSILON ? worldScale.y / parentWorld->scale.y : worldScale.y,
                        std::abs(parentWorld->scale.z) > math::EPSILON ? worldScale.z / parentWorld->scale.z : worldScale.z
                    };
                }
                else
                {
                    transform.localScale = worldScale;
                }
                transform.dirty = true;
                return;
            }
        }
    }

    transform.localPosition = worldPosition;
    transform.localRotation = worldRotation;
    transform.localScale    = worldScale;
    transform.dirty = true;
}


static math::Vec3 WorldPosition(EditorFrameContext& ctx, EntityID entity)
{
    // Weltposition exakt wie TransformSystem: parent.WTC * localPosition.
    // Nicht aus child.WTC lesen – das kann veraltet sein wenn child.dirty=true
    // aber TransformSystem noch nicht gelaufen ist (gleicher Frame).
    const auto* t = ctx.world.Get<TransformComponent>(entity);
    if (!t)
    {
        if (const auto* wt = ctx.world.Get<WorldTransformComponent>(entity))
            return wt->position;
        return {};
    }
    if (const auto* p = ctx.world.Get<ParentComponent>(entity))
    {
        if (p->parent.IsValid() && ctx.world.IsAlive(p->parent))
        {
            if (const auto* parentWtc = ctx.world.Get<WorldTransformComponent>(p->parent))
            {
                const math::Vec3 localOffset = t->inheritParentScale
                    ? math::Vec3{
                        parentWtc->scale.x * t->localPosition.x,
                        parentWtc->scale.y * t->localPosition.y,
                        parentWtc->scale.z * t->localPosition.z
                    }
                    : t->localPosition;
                return parentWtc->position + parentWtc->rotation.Rotate(localOffset);
            }
        }
    }
    // Root-Entity: localPosition == Weltposition
    return t->localPosition;
}

static math::Quat WorldRotation(EditorFrameContext& ctx, EntityID entity)
{
    // Rotation direkt aus der Quaternion-Kette berechnen (nicht aus der Weltmatrix),
    // weil MatrixRotation() bei gescherten Matrizen (nicht-uniforme Parent-Skalierung
    // + Rotation) falsche Ergebnisse liefert – Spaltennormalisierung gibt nur eine
    // Näherung der Rotation. Die Quat-Komposition ist immer exakt.
    const auto* t = ctx.world.Get<TransformComponent>(entity);
    if (!t) return math::Quat::Identity();

    if (const auto* p = ctx.world.Get<ParentComponent>(entity))
        if (p->parent.IsValid() && ctx.world.IsAlive(p->parent))
            return (WorldRotation(ctx, p->parent) * t->localRotation).Normalized();

    return t->localRotation;
}

static math::Vec3 WorldScale(EditorFrameContext& ctx, EntityID entity)
{
    // Komponentenweise Weltskala: parentWorldScale * localScale.
    // MatrixScale(child.WTC) wäre bei gescherten Matrizen (nicht-uniformer
    // Parent-Scale + Rotation) ungenau – Spaltenlängen sind dann verzerrt.
    const auto* t = ctx.world.Get<TransformComponent>(entity);
    if (!t)
    {
        if (const auto* wt = ctx.world.Get<WorldTransformComponent>(entity))
            return wt->scale;
        return {1.f, 1.f, 1.f};
    }
    if (!t->inheritParentScale)
        return t->localScale;
    if (const auto* p = ctx.world.Get<ParentComponent>(entity))
    {
        if (p->parent.IsValid() && ctx.world.IsAlive(p->parent))
        {
            if (const auto* parentWtc = ctx.world.Get<WorldTransformComponent>(p->parent))
            {
                const math::Vec3 ps = parentWtc->scale;
                return { ps.x * t->localScale.x,
                         ps.y * t->localScale.y,
                         ps.z * t->localScale.z };
            }
        }
    }
    return t->localScale;
}

static void SetWorldPosition(EditorFrameContext& ctx, EntityID entity, TransformComponent& transform, const math::Vec3& worldPosition)
{
    const math::Mat4 currentWorld = CurrentWorldMatrix(ctx, entity);
    const math::Mat4 desiredWorld = math::Mat4::TRS(
        worldPosition,
        WorldRotation(ctx, entity),
        MatrixScale(currentWorld));
    SetWorldTransform(ctx, entity, transform, desiredWorld);
}

static void SetWorldRotation(EditorFrameContext& ctx, EntityID entity, TransformComponent& transform, const math::Quat& worldRotation)
{
    const math::Mat4 currentWorld = CurrentWorldMatrix(ctx, entity);
    const math::Mat4 desiredWorld = math::Mat4::TRS(
        MatrixTranslation(currentWorld),
        worldRotation,
        MatrixScale(currentWorld));
    SetWorldTransform(ctx, entity, transform, desiredWorld);
}

// =============================================================================
// Viewport-Selektion: Kinder in World-Space fixieren
// =============================================================================

// Snapshot aller direkten Kinder einer Entity (einen Frame vor dem Drag-Start).
// Wird nur bei Viewport-Selektion aufgerufen.
static void SnapshotDirectChildren(EditorFrameContext& ctx, EntityID parentId)
{
    using namespace renderer::addons::editor;
    EditorState& st = ctx.state;
    st.gizmoDragChildren.clear();

    ctx.world.ForEachAlive([&](EntityID id) {
        const auto* pc = ctx.world.Get<ParentComponent>(id);
        if (!pc || pc->parent != parentId) return;

        GizmoChildData data;
        data.entity = id;
        data.worldPos = {0.f, 0.f, 0.f};
        data.worldRot = {0.f, 0.f, 0.f, 1.f};
        if (const auto* wtc = ctx.world.Get<WorldTransformComponent>(id))
        {
            data.worldPos = wtc->position;
            data.worldRot = wtc->rotation;
        }
        const auto* tc = ctx.world.Get<TransformComponent>(id);
        data.inheritScale = tc && tc->inheritParentScale;
        st.gizmoDragChildren.push_back(data);
    });

    // Parent-World-Transform einfrieren (wird in der Kompensation genutzt)
    st.gizmoDragParentWorldPos   = {};
    st.gizmoDragParentWorldRot   = {0.f, 0.f, 0.f, 1.f};
    st.gizmoDragParentWorldScale = {1.f, 1.f, 1.f};
    if (const auto* wtc = ctx.world.Get<WorldTransformComponent>(parentId))
    {
        st.gizmoDragParentWorldPos   = wtc->position;
        st.gizmoDragParentWorldRot   = wtc->rotation;
        st.gizmoDragParentWorldScale = wtc->scale;
    }
}

// Nach dem Verschieben des Parents: Kinder auf ihre originale World-Position zurücksetzen.
// Nur Translation des Parents hat sich geändert — Rotation/Scale bleiben gleich.
static void CompensateChildrenAfterTranslate(EditorFrameContext& ctx,
                                             const math::Vec3& newParentWorldPos)
{
    const EditorState& st         = ctx.state;
    const math::Quat   invParentR = st.gizmoDragParentWorldRot.Conjugate().Normalized();
    const math::Vec3&  parentS    = st.gizmoDragParentWorldScale;

    for (const auto& child : st.gizmoDragChildren)
    {
        if (!child.entity.IsValid() || !ctx.world.IsAlive(child.entity)) continue;
        auto* tc = ctx.world.Get<TransformComponent>(child.entity);
        if (!tc) continue;

        // rel = inv(parentRot) * (childWorldPos - newParentWorldPos)
        math::Vec3 rel = invParentR.Rotate(child.worldPos - newParentWorldPos);
        if (child.inheritScale)
        {
            rel.x = std::abs(parentS.x) > math::EPSILON ? rel.x / parentS.x : rel.x;
            rel.y = std::abs(parentS.y) > math::EPSILON ? rel.y / parentS.y : rel.y;
            rel.z = std::abs(parentS.z) > math::EPSILON ? rel.z / parentS.z : rel.z;
        }
        tc->localPosition = rel;
        tc->dirty = true;
    }
}

// Nach dem Rotieren des Parents: Kinder auf ihre originale World-Pose zurücksetzen.
// Nur Rotation des Parents hat sich geändert — World-Position/Scale bleiben gleich.
static void CompensateChildrenAfterRotate(EditorFrameContext& ctx,
                                          const math::Quat& newParentWorldRot)
{
    const EditorState& st         = ctx.state;
    const math::Quat   invNewR    = newParentWorldRot.Conjugate().Normalized();
    const math::Vec3&  parentPos  = st.gizmoDragParentWorldPos;
    const math::Vec3&  parentS    = st.gizmoDragParentWorldScale;

    for (const auto& child : st.gizmoDragChildren)
    {
        if (!child.entity.IsValid() || !ctx.world.IsAlive(child.entity)) continue;
        auto* tc = ctx.world.Get<TransformComponent>(child.entity);
        if (!tc) continue;

        // Position: rel = inv(newParentRot) * (childWorldPos - parentWorldPos)
        math::Vec3 rel = invNewR.Rotate(child.worldPos - parentPos);
        if (child.inheritScale)
        {
            rel.x = std::abs(parentS.x) > math::EPSILON ? rel.x / parentS.x : rel.x;
            rel.y = std::abs(parentS.y) > math::EPSILON ? rel.y / parentS.y : rel.y;
            rel.z = std::abs(parentS.z) > math::EPSILON ? rel.z / parentS.z : rel.z;
        }
        tc->localPosition = rel;

        // Rotation: newLocalRot = inv(newParentWorldRot) * childWorldRot
        tc->localRotation = (invNewR * child.worldRot).Normalized();
        tc->dirty = true;
    }
}

static void SelectEntityUnderMouse(EditorFrameContext& ctx)
{
    // Gizmo- oder OBB-Drag hat Vorrang — Entity-Selektion überspringen.
    // Auch Hover reicht: beim nächsten Klick wird der Gizmo-Drag gestartet,
    // nicht eine andere Entity selektiert.
    if (ctx.state.gizmoDragAxis       >= 0 ||
        ctx.state.rotGizmoDragAxis    >= 0 ||
        ctx.state.sclGizmoDragAxis    >= 0 ||
        ctx.state.obbDragFaceIdx      >= 0)
        return;

    ImGuiIO& io = ImGui::GetIO();
    if (io.WantCaptureMouse || ImGui::IsAnyItemHovered() || !ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        return;

    if (ShouldCancelSelectionChangeForActiveInput())
        return;

    if (io.DisplaySize.x <= 1.f || io.DisplaySize.y <= 1.f ||
        io.MousePos.x < 0.f || io.MousePos.y < 0.f ||
        io.MousePos.x >= io.DisplaySize.x || io.MousePos.y >= io.DisplaySize.y)
    {
        return;
    }

    const EditorCameraState& cam = ctx.state.editorCamera;

    const float ndcX = (io.MousePos.x / io.DisplaySize.x) * 2.f - 1.f;
    const float ndcY = 1.f - (io.MousePos.y / io.DisplaySize.y) * 2.f;
    const float aspect = io.DisplaySize.x / io.DisplaySize.y;
    const float tanHalfFov = std::tan((cam.fovDeg * math::DEG_TO_RAD) * 0.5f);
    const math::Quat rot = math::Quat::FromEulerDeg(cam.pitchDeg, cam.yawDeg, 0.f);
    const math::Vec3 rayDir = rot.Rotate({
        ndcX * aspect * tanHalfFov,
        ndcY * tanHalfFov,
        -1.f
    }).Normalized();

    const collision::Ray ray{ cam.position, rayDir };
    const float kMaxDist = EditorPickMaxDistance(cam);

    collision::RaycastHit hit{};
    mesh_renderer::MeshCollisionPipeline pickPipeline;
    pickPipeline.Build(ctx.world);
    mesh_renderer::MeshCollisionRaycastOptions pickOptions{};
    pickOptions.includeEntity = [&](EntityID entity) {
        return IsScenePickEntity(ctx.world, entity);
    };
    bool found = pickPipeline.Raycast(ctx.world, ctx.registry, ray, kMaxDist, hit, pickOptions);

    // Kamera-Gizmo Picking: Kugeltest pro Gizmo (CPU-Mesh meist nicht verfügbar)
    if (!found)
    {
        constexpr float kGizmoPickRadius = 0.5f;
        float bestDist = kMaxDist;
        ctx.world.View<EditorCameraGizmoComponent, WorldTransformComponent>(
            [&](EntityID id,
                const EditorCameraGizmoComponent&,
                const WorldTransformComponent& wt)
        {
            // Abstand Punkt-zu-Strahl berechnen
            const math::Vec3 oc     = wt.position - ray.origin;
            const float      tProj  = oc.x * ray.direction.x +
                                      oc.y * ray.direction.y +
                                      oc.z * ray.direction.z;
            if (tProj < 0.f) return;
            const math::Vec3 closest = ray.origin + ray.direction * tProj;
            const math::Vec3 diff    = wt.position - closest;
            const float      dist2   = diff.x*diff.x + diff.y*diff.y + diff.z*diff.z;
            if (dist2 < kGizmoPickRadius * kGizmoPickRadius && tProj < bestDist)
            {
                bestDist   = tProj;
                hit.entity = id;
                hit.distance = tProj;
                found = true;
            }
        });
    }

    if (found && hit.entity.IsValid() && ctx.world.IsAlive(hit.entity))
    {
        EntityID pickedEntity = hit.entity;
        // Kamera-Gizmo angeklickt → zur zugehörigen Kamera-Entity umleiten.
        // Variante A: modernes Gizmo mit EditorCameraGizmoComponent
        if (ctx.world.Get<EditorMaterialPreviewComponent>(hit.entity) != nullptr)
        {
            return;
        }
        else if (ctx.world.Get<EditorDpadGizmoComponent>(hit.entity) != nullptr)
        {
            return;
        }
        else if (ctx.world.Get<EditorRotateGizmoComponent>(hit.entity) != nullptr)
        {
            return;
        }
        else if (ctx.world.Get<EditorScaleGizmoComponent>(hit.entity) != nullptr)
        {
            return;
        }
        else if (ctx.world.Get<EditorAssetThumbnailComponent>(hit.entity) != nullptr)
        {
            return;
        }
        else if (const auto* gizmo = ctx.world.Get<EditorCameraGizmoComponent>(hit.entity))
        {
            if (gizmo->cameraEntity.IsValid() && ctx.world.IsAlive(gizmo->cameraEntity))
                pickedEntity = gizmo->cameraEntity;
        }
        // Variante B: Legacy-Gizmo (kein Component-Tag) — nächste lebende Kamera suchen
        else if (const auto* mc = ctx.world.Get<MeshComponent>(hit.entity))
        {
            if (mc->layerMask == renderer::LAYER_EDITOR_GIZMO)
            {
                // Nächste Kamera-Entity an derselben Position finden
                const auto* hitWtc = ctx.world.Get<WorldTransformComponent>(hit.entity);
                if (hitWtc)
                {
                    float    bestDist = 1e9f;
                    EntityID bestCam  = NULL_ENTITY;
                    ctx.world.ForEachAlive([&](EntityID id) {
                        if (!ctx.world.Has<CameraComponent>(id)) return;
                        const auto* wtc = ctx.world.Get<WorldTransformComponent>(id);
                        if (!wtc) return;
                        const math::Vec3 d = wtc->position - hitWtc->position;
                        const float dist = d.x*d.x + d.y*d.y + d.z*d.z;
                        if (dist < bestDist) { bestDist = dist; bestCam = id; }
                    });
                    if (bestCam.IsValid()) pickedEntity = bestCam;
                }
            }
        }

        if (ImGui::GetIO().KeyCtrl)
            AddToMultiSelection(ctx, pickedEntity);
        else
        {
            ClearMultiSelection(ctx);
            ctx.state.selectedEntity  = pickedEntity;
            ctx.state.selectionSource = SelectionSource::Viewport;

            // Kamera per Viewport angeklickt → Vorschau einmalig öffnen.
            // Guard zurücksetzen damit DrawCameraPreviewWindow das Fenster aufmacht,
            // auch wenn es vorher manuell geschlossen wurde.
            // (Einmaliger Reset hier statt dauerhaft in DrawCameraPreviewWindow —
            //  sonst kann der User das Fenster nicht schließen.)
            if (ctx.world.Has<CameraComponent>(pickedEntity))
                ctx.state.cameraPreviewLastAutoOpenSelection = NULL_ENTITY;
        }
        return;
    }

    if (!ImGui::GetIO().KeyCtrl)
    {
        ClearMultiSelection(ctx);
        ctx.state.selectedEntity = NULL_ENTITY;
    }
}

static void UpdateEditorCamera(EditorFrameContext& ctx)
{
    ImGuiIO& io = ImGui::GetIO();

    EditorCameraState& cam = ctx.state.editorCamera;

    const math::Quat rot     = math::Quat::FromEulerDeg(cam.pitchDeg, cam.yawDeg, 0.f);
    const math::Vec3 forward = rot.Rotate({0.f, 0.f, -1.f});
    const math::Vec3 right   = rot.Rotate({1.f, 0.f,  0.f});

    if (!io.WantCaptureMouse)
    {
        if (io.MouseWheel != 0.f)
        {
            const float boost = ImGui::IsKeyDown(ImGuiKey_LeftShift) || ImGui::IsKeyDown(ImGuiKey_RightShift) ? 5.f : 1.f;
            cam.position += forward * (io.MouseWheel * cam.moveSpeed * 0.1f * boost);
        }

        if (io.MouseDown[1])
        {
            cam.yawDeg   -= io.MouseDelta.x * 0.25f;
            cam.pitchDeg -= io.MouseDelta.y * 0.25f;
        }
    }

    // WASD + Pfeiltasten: gesperrt nur wenn ein Texteingabefeld aktiv ist
    if (!io.WantTextInput)
    {
        const float dt    = ctx.deltaSeconds;
        const float boost = ImGui::IsKeyDown(ImGuiKey_LeftShift) || ImGui::IsKeyDown(ImGuiKey_RightShift) ? 5.f : 1.f;
        const float speed = cam.moveSpeed * boost;

        if (ImGui::IsKeyDown(ImGuiKey_W)) cam.position += forward * (speed * dt);
        if (ImGui::IsKeyDown(ImGuiKey_S)) cam.position -= forward * (speed * dt);
        if (ImGui::IsKeyDown(ImGuiKey_A)) cam.position -= right   * (speed * dt);
        if (ImGui::IsKeyDown(ImGuiKey_D)) cam.position += right   * (speed * dt);
        if (ImGui::IsKeyDown(ImGuiKey_E)) cam.position.y += speed * dt;
        if (ImGui::IsKeyDown(ImGuiKey_Q)) cam.position.y -= speed * dt;

        const float rotSpeed = 60.f * dt * boost;
        if (ImGui::IsKeyDown(ImGuiKey_LeftArrow))  cam.yawDeg   += rotSpeed;
        if (ImGui::IsKeyDown(ImGuiKey_RightArrow)) cam.yawDeg   -= rotSpeed;
        if (ImGui::IsKeyDown(ImGuiKey_UpArrow))    cam.pitchDeg += rotSpeed;
        if (ImGui::IsKeyDown(ImGuiKey_DownArrow))  cam.pitchDeg -= rotSpeed;
    }

    cam.pitchDeg = std::clamp(cam.pitchDeg, -89.f, 89.f);
}

static void DrawCameraPreviewWindow(EditorFrameContext& ctx)
{
    const EntityID selected = ctx.state.selectedEntity;
    if (selected.IsValid() && ctx.world.Has<CameraComponent>(selected))
    {
        if (ctx.state.cameraPreviewLastAutoOpenSelection != selected)
        {
            ctx.state.cameraPreviewWindowOpen = true;
            ctx.state.cameraPreviewLastAutoOpenSelection = selected;
        }
    }
    else
    {
        ctx.state.cameraPreviewWindowOpen = false;
        ctx.state.cameraPreviewLastAutoOpenSelection = NULL_ENTITY;
    }

    if (!ctx.state.cameraPreviewWindowOpen)
        return;

    // Selektierte Kamera für Vorschau nutzen — fallback auf Main-Kamera
    EntityID previewCameraEntity = selected;
    if (!previewCameraEntity.IsValid() || !ctx.world.Has<CameraComponent>(previewCameraEntity))
        return;

    const auto* camera = ctx.world.Get<CameraComponent>(previewCameraEntity);
    if (!camera)
        return;

    ImGui::SetNextWindowPos(ImVec2(290.f, 10.f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(340.f, 230.f), ImGuiCond_FirstUseEver);
    const bool wasOpen = ctx.state.cameraPreviewWindowOpen;
    if (!ImGui::Begin("Camera Preview##editor", &ctx.state.cameraPreviewWindowOpen))
    {
        ImGui::End();
        return;
    }
    if (wasOpen && !ctx.state.cameraPreviewWindowOpen)
        ctx.state.cameraPreviewLastAutoOpenSelection = ctx.state.selectedEntity;

    ImGui::Separator();

    if (ctx.cameraPreviewTexture)
    {
        void* texId = ctx.cameraPreviewTexture(previewCameraEntity);
        if (texId)
        {
            const ImVec2 avail  = ImGui::GetContentRegionAvail();
            const float  aspect = camera->aspectRatio > 1e-6f ? camera->aspectRatio : (16.f / 9.f);
            const float  w      = avail.x;
            const float  h      = w / aspect;
            const ImVec2 uv0 = ctx.previewFlipY ? ImVec2(0.f, 1.f) : ImVec2(0.f, 0.f);
            const ImVec2 uv1 = ctx.previewFlipY ? ImVec2(1.f, 0.f) : ImVec2(1.f, 1.f);
            ImGui::Image(reinterpret_cast<ImTextureID>(texId), ImVec2(w, h), uv0, uv1);
        }
        else
        {
            ImGui::TextDisabled("(kein Preview verfuegbar)");
        }
    }
    else
    {
        ImGui::TextDisabled("(Preview: cameraPreviewTexture-Callback nicht gesetzt)");
    }

    ImGui::End();
}

// ─── Script-Section ──────────────────────────────────────────────────────────

static void DrawScriptSection(EditorFrameContext& ctx, EntityID entity)
{
    using namespace engine::script;

    auto* sl = ctx.world.Get<ScriptList>(entity);
    std::vector<std::string> scriptClassNames;
    if (ctx.scriptRegistry)
        scriptClassNames = ctx.scriptRegistry->GetClassNames();
    for (const std::string& markerClassName : FindCppScriptClassNames(ctx))
    {
        if (std::find(scriptClassNames.begin(), scriptClassNames.end(), markerClassName) ==
            scriptClassNames.end())
        {
            scriptClassNames.push_back(markerClassName);
        }
    }
    std::sort(scriptClassNames.begin(), scriptClassNames.end());

    if (!sl && scriptClassNames.empty())
        return;  // kein Script-System aktiv
    const auto* runtimeGuid = ctx.world.Get<GuidComponent>(entity);
    const std::string runtimeEntityGuid = runtimeGuid ? runtimeGuid->guid : std::string{};

    const bool open = ImGui::CollapsingHeader("Scripts##scripts_hdr",
                                              ImGuiTreeNodeFlags_DefaultOpen);
    if (!open) return;

    ImGui::PushID("ScriptSection");

    if (sl)
    {
        auto& instances = sl->Instances_Mutable();
        int toRemove = -1;

        for (int i = 0; i < (int)instances.size(); ++i)
        {
            ScriptInstance& inst = instances[i];
            ImGui::PushID(i);

            // Kopfzeile: fetter Instanzname + X-Button
            char headerBuf[256];
            std::snprintf(headerBuf, sizeof(headerBuf),
                "%s##si%d", inst.instanceName.c_str(), i);
            const bool instOpen = ImGui::TreeNodeEx(headerBuf,
                ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap);

            // X-Button rechts
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - 18.f +
                            ImGui::GetCursorPosX());
            if (ImGui::SmallButton("X"))
                toRemove = i;

            if (instOpen)
            {
                // Klassenname (read-only)
                ImGui::TextDisabled("Klasse: %s", inst.className.c_str());

                // Instanzname editierbar
                char buf[128];
                std::snprintf(buf, sizeof(buf), "%s", inst.instanceName.c_str());
                ImGui::SetNextItemWidth(-1.f);
                if (ImGui::InputText("##instname", buf, sizeof(buf)))
                {
                    inst.instanceName = buf;
                    // Änderung als Undo-Snapshot markieren
                    if (ctx.saveScene) {} // Snapshot wird beim nächsten Save erfasst
                }

                if (ctx.scriptRegistry)
                {
                    std::unordered_set<std::string> displayedFields;
                    const std::vector<EditorScriptFieldMarker> markerFields =
                        FindCppScriptFields(ctx, inst.className);
                    std::unordered_set<std::string> markerFieldNames;
                    for (const EditorScriptFieldMarker& markerField : markerFields)
                        markerFieldNames.insert(markerField.name);
                    const bool markersAreAuthoritative = !markerFieldNames.empty();

                    if (const auto* fields = ctx.scriptRegistry->GetFields(inst.className))
                    {
                        for (const ScriptFieldMeta& field : *fields)
                        {
                            if (markersAreAuthoritative &&
                                markerFieldNames.count(field.name) == 0u)
                            {
                                inst.fieldValues.erase(field.name);
                                continue;
                            }
                            displayedFields.insert(field.name);
                            ScriptFieldValue value{};
                            const auto storedIt = inst.fieldValues.find(field.name);
                            if (storedIt != inst.fieldValues.end())
                            {
                                value = storedIt->second;
                            }
                            else if (inst.script)
                            {
                                value.type = field.type;
                                (void)ctx.scriptRegistry->ReadField(*inst.script, inst.className, field.name, value);
                            }
                            else
                            {
                                value.type = field.type;
                            }

                            bool hasLiveValue = false;
                            if (!runtimeEntityGuid.empty())
                            {
                                const std::string liveKey = RuntimeScriptFieldKey(
                                    runtimeEntityGuid,
                                    static_cast<uint32_t>(i),
                                    field.name);
                                const auto liveIt = ctx.state.runtimeScriptFieldValues.find(liveKey);
                                if (liveIt != ctx.state.runtimeScriptFieldValues.end())
                                {
                                    value = liveIt->second;
                                    hasLiveValue = true;
                                }
                            }

                            if (DrawScriptFieldEditor(field.name.c_str(), ctx, field.type, value, hasLiveValue))
                            {
                                value.type = field.type;
                                inst.fieldValues[field.name] = value;
                                if (inst.script)
                                    (void)ctx.scriptRegistry->WriteField(*inst.script, inst.className, field.name, value);
                            }
                        }
                    }
                    for (const EditorScriptFieldMarker& markerField : markerFields)
                    {
                        if (displayedFields.count(markerField.name) > 0u)
                            continue;
                        displayedFields.insert(markerField.name);

                        ScriptFieldValue value{};
                        const auto storedIt = inst.fieldValues.find(markerField.name);
                        if (storedIt != inst.fieldValues.end())
                            value = storedIt->second;
                        else if (markerField.hasDefaultValue)
                            value = markerField.defaultValue;
                        else
                            value.type = markerField.type;

                        bool hasLiveValue = false;
                        if (!runtimeEntityGuid.empty())
                        {
                            const std::string liveKey = RuntimeScriptFieldKey(
                                runtimeEntityGuid,
                                static_cast<uint32_t>(i),
                                markerField.name);
                            const auto liveIt = ctx.state.runtimeScriptFieldValues.find(liveKey);
                            if (liveIt != ctx.state.runtimeScriptFieldValues.end())
                            {
                                value = liveIt->second;
                                hasLiveValue = true;
                            }
                        }

                        if (DrawScriptFieldEditor(markerField.name.c_str(), ctx, markerField.type, value, hasLiveValue))
                        {
                            value.type = markerField.type;
                            inst.fieldValues[markerField.name] = value;
                        }
                    }
                    if (!runtimeEntityGuid.empty())
                    {
                        const std::string livePrefix =
                            runtimeEntityGuid + "|" + std::to_string(static_cast<uint32_t>(i)) + "|";
                        for (const auto& [key, liveValue] : ctx.state.runtimeScriptFieldValues)
                        {
                            if (key.rfind(livePrefix, 0u) != 0u)
                                continue;
                            const std::string fieldName = key.substr(livePrefix.size());
                            if (displayedFields.count(fieldName) > 0u)
                                continue;
                            const std::string liveText =
                                "Live: " + fieldName + " = " + RuntimeScriptFieldValueText(liveValue);
                            ImGui::TextDisabled("%s", liveText.c_str());
                        }
                    }
                }

                ImGui::TreePop();
            }

            ImGui::PopID();
        }

        if (toRemove >= 0)
        {
            sl->RemoveAt(static_cast<size_t>(toRemove), entity);
        }
    }

    // "Script hinzufügen" Button
    if (!scriptClassNames.empty())
    {
        ImGui::Spacing();
        if (ImGui::Button("+ Script hinzufuegen", ImVec2(-1.f, 0.f)))
            ImGui::OpenPopup("AddScriptPopup");

        if (ImGui::BeginPopup("AddScriptPopup"))
        {
            ImGui::TextDisabled("Script-Klasse waehlen");
            ImGui::Separator();

            for (const auto& name : scriptClassNames)
            {
                if (ImGui::MenuItem(name.c_str()))
                {
                    if (!sl)
                    {
                        engine::script::ScriptList newList;
                        newList.SetOwnerEntity(entity);
                        ctx.world.Add<ScriptList>(entity, std::move(newList));
                        sl = ctx.world.Get<ScriptList>(entity);
                    }
                    if (sl)
                    {
                        const bool hasLiveFactory = ctx.scriptRegistry && ctx.scriptRegistry->Has(name);
                        if (hasLiveFactory)
                        {
                            sl->Add(name, entity, *ctx.scriptRegistry);
                        }
                        else
                        {
                            sl->AddNameOnly(name);
                            ScriptInstance& inst = sl->Instances_Mutable().back();
                            for (const EditorScriptFieldMarker& field : FindCppScriptFields(ctx, name))
                            {
                                ScriptFieldValue value{};
                                value.type = field.type;
                                if (field.hasDefaultValue)
                                    value = field.defaultValue;
                                inst.fieldValues[field.name] = std::move(value);
                            }
                            ctx.lastFileMessage =
                                "Script hinzugefuegt: " + name + " (Build noetig fuer Live-Code).";
                        }
                    }
                }
            }
            ImGui::EndPopup();
        }
    }

    ImGui::PopID();
}

// ─────────────────────────────────────────────────────────────────────────────

static void DrawTransformSection(EditorFrameContext& ctx, EntityID entity)
{
    auto* t = ctx.world.Get<TransformComponent>(entity);
    if (!t)
        return;

    if (!ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen))
        return;

    int space = ctx.state.transformSpace == EditorTransformSpace::World ? 1 : 0;
    if (ImGui::RadioButton("Local", space == 0))
        space = 0;
    if (ImGui::IsItemHovered())
        ImGui::SetItemTooltip("Local Space: Werte relativ zum Parent (bzw. World, wenn kein Parent vorhanden).");
    ImGui::SameLine();
    if (ImGui::RadioButton("World", space == 1))
        space = 1;
    if (ImGui::IsItemHovered())
        ImGui::SetItemTooltip("World Space: globale Achsen und globale Position/Rotation.");
    ctx.state.transformSpace = space == 1 ? EditorTransformSpace::World : EditorTransformSpace::Local;

    // Modus-Anzeige: Viewport-Selektion = "Nur Parent", Outliner = "Mit Kindern"
    {
        const bool isViewport = ctx.state.selectionSource == SelectionSource::Viewport;
        ImGui::SameLine();
        ImGui::Spacing(); ImGui::SameLine();
        if (isViewport)
        {
            ImGui::TextColored({0.4f, 0.9f, 1.0f, 1.f}, "* Nur Parent");
            if (ImGui::IsItemHovered())
                ImGui::SetItemTooltip(
                    "Viewport-Klick: nur diese Entity wird transformiert.\n"
                    "Kinder bleiben an ihrer World-Position.\n"
                    "Outliner-Klick: Parent + Kinder bewegen sich gemeinsam.");
        }
        else
        {
            ImGui::TextDisabled("* Mit Kindern");
            if (ImGui::IsItemHovered())
                ImGui::SetItemTooltip(
                    "Outliner-Klick: Parent + Kinder bewegen sich gemeinsam.\n"
                    "Viewport-Klick: nur diese Entity wird transformiert,\n"
                    "Kinder bleiben an ihrer World-Position.");
        }
    }

    // Hilfsfunktion: Label links (klickbar für Gizmo-Modus), DragFloat3 rechts
    constexpr float kLabelWidth = 72.f;

    // Gibt {valueChanged, labelClicked} zurück.
    // labelColor: wenn gesetzt, wird das Label in dieser Farbe gezeichnet (Modus-Anzeige).
    struct DragResult { bool changed; bool labelClicked; };
    auto LabeledDragFloat3 = [&](const char* label, const char* id,
                                 float* v, float speed,
                                 const ImVec4* labelColor = nullptr,
                                 float vmin = 0.f, float vmax = 0.f) -> DragResult
    {
        if (labelColor) ImGui::PushStyleColor(ImGuiCol_Text, *labelColor);
        ImGui::Text("%s", label);
        if (labelColor) ImGui::PopStyleColor();
        const bool labelClicked = ImGui::IsItemClicked();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Klicken um Gizmo-Modus zu wechseln");
        ImGui::SameLine(kLabelWidth);
        ImGui::SetNextItemWidth(-1.f);
        const ImGuiSliderFlags clampFlag = (vmin < vmax) ? ImGuiSliderFlags_AlwaysClamp : ImGuiSliderFlags_None;
        return { ImGui::DragFloat3(id, v, speed, vmin, vmax, "%.3f", clampFlag), labelClicked };
    };

    if (ctx.state.transformSpace == EditorTransformSpace::Local)
    {
        // sodass Ziehen an Z die Entity tatsächlich in ihrer lokalen Z-Richtung bewegt.
        const ImVec4 kPosActive{0.4f, 1.0f, 0.4f, 1.f};
        const ImVec4 kRotActive{0.4f, 0.6f, 1.0f, 1.f};
        const ImVec4 kSclActive{1.0f, 0.8f, 0.2f, 1.f};
        const bool posMode = ctx.state.gizmoMode == GizmoMode::Position;
        const bool rotMode = ctx.state.gizmoMode == GizmoMode::Rotation;
        const bool sclMode = ctx.state.gizmoMode == GizmoMode::Scale;

        const std::string localPositionKey = MakeEditHistoryKey(entity, "transform.local.position");
        auto posResult = LabeledDragFloat3("Position", "##pos", &t->localPosition.x, 0.1f,
                                           posMode ? &kPosActive : nullptr);
        if (posResult.labelClicked) ctx.state.gizmoMode = GizmoMode::Position;
        if (posResult.changed)
        {
            BeginPendingSceneEdit(ctx, localPositionKey, "Position geaendert");
            t->dirty = true;
        }
        if (ImGui::IsItemDeactivatedAfterEdit())
            CommitPendingSceneEdit(ctx, localPositionKey);

        math::Quat localRotation = t->localRotation;
        math::Vec3 euler = PrepareRotationEditEuler(
            ctx, entity, EditorTransformSpace::Local, localRotation);
        const std::string localRotationKey = MakeEditHistoryKey(entity, "transform.local.rotation");
        auto rotResult = LabeledDragFloat3("Rotation", "##rot", &euler.x, 1.0f,
                                           rotMode ? &kRotActive : nullptr, 0.f, 360.f);
        if (rotResult.labelClicked) ctx.state.gizmoMode = GizmoMode::Rotation;
        if (rotResult.changed)
        {
            BeginPendingSceneEdit(ctx, localRotationKey, "Rotation geaendert");
            localRotation = math::Quat::FromEulerDeg(euler.x, euler.y, euler.z);
            t->localRotation = localRotation;
            t->dirty = true;
        }
        if (ImGui::IsItemDeactivatedAfterEdit())
            CommitPendingSceneEdit(ctx, localRotationKey);
        CommitRotationEditState(ctx, entity, EditorTransformSpace::Local, euler, localRotation);

        const std::string localScaleKey = MakeEditHistoryKey(entity, "transform.local.scale");
        auto sclResult = LabeledDragFloat3("Scale", "##scl", &t->localScale.x, 0.01f,
                                           sclMode ? &kSclActive : nullptr, 0.0001f, 1000.f);
        if (sclResult.labelClicked) ctx.state.gizmoMode = GizmoMode::Scale;
        if (sclResult.changed)
        {
            BeginPendingSceneEdit(ctx, localScaleKey, "Skalierung geaendert");
            t->dirty = true;
        }
        if (ImGui::IsItemDeactivatedAfterEdit())
            CommitPendingSceneEdit(ctx, localScaleKey);

        // Checkbox "Inherit Parent Scale" — nur sichtbar wenn Entity einen Parent hat
        if (ctx.world.Has<ParentComponent>(entity))
        {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + kLabelWidth + 4.f);
            bool inherit = t->inheritParentScale;
            if (ImGui::Checkbox("Scale erben", &inherit))
            {
                const std::string key = MakeEditHistoryKey(entity, "transform.inheritParentScale");
                BeginPendingSceneEdit(ctx, key, "Scale-Vererbung geaendert");
                t->inheritParentScale = inherit;
                t->dirty = true;
                CommitPendingSceneEdit(ctx, key);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetItemTooltip(
                    "An: Dieses Objekt skaliert mit dem Parent mit.\n"
                    "Aus: Dieses Objekt behaelt seine eigene Weltgroesse.");
        }
    }
    else
    {
        const ImVec4 kPosActive{0.4f, 1.0f, 0.4f, 1.f};
        const ImVec4 kRotActive{0.4f, 0.6f, 1.0f, 1.f};
        const ImVec4 kSclActive{1.0f, 0.8f, 0.2f, 1.f};
        const bool posMode = ctx.state.gizmoMode == GizmoMode::Position;
        const bool rotMode = ctx.state.gizmoMode == GizmoMode::Rotation;
        const bool sclMode = ctx.state.gizmoMode == GizmoMode::Scale;

        math::Vec3 position = WorldPosition(ctx, entity);
        const std::string worldPositionKey = MakeEditHistoryKey(entity, "transform.world.position");
        auto posResult = LabeledDragFloat3("Position", "##pos", &position.x, 0.1f,
                                           posMode ? &kPosActive : nullptr);
        if (posResult.labelClicked) ctx.state.gizmoMode = GizmoMode::Position;
        if (posResult.changed)
        {
            BeginPendingSceneEdit(ctx, worldPositionKey, "Position geaendert");
            SetWorldPosition(ctx, entity, *t, position);
        }
        if (ImGui::IsItemDeactivatedAfterEdit())
            CommitPendingSceneEdit(ctx, worldPositionKey);

        const bool worldRotationBlocked = HasNonUniformScaledParent(ctx, entity);
        math::Quat worldRotation = WorldRotation(ctx, entity);
        math::Vec3 euler = PrepareRotationEditEuler(
            ctx, entity, EditorTransformSpace::World, worldRotation);
        const std::string worldRotationKey = MakeEditHistoryKey(entity, "transform.world.rotation");
        if (worldRotationBlocked)
            ImGui::BeginDisabled();
        auto rotResult = LabeledDragFloat3("Rotation", "##rot", &euler.x, 1.0f,
                                           rotMode ? &kRotActive : nullptr, 0.f, 360.f);
        if (rotResult.labelClicked) ctx.state.gizmoMode = GizmoMode::Rotation;
        if (rotResult.changed && !worldRotationBlocked)
        {
            BeginPendingSceneEdit(ctx, worldRotationKey, "Rotation geaendert");
            worldRotation = math::Quat::FromEulerDeg(euler.x, euler.y, euler.z);
            SetWorldRotation(ctx, entity, *t, worldRotation);
        }
        if (ImGui::IsItemDeactivatedAfterEdit())
            CommitPendingSceneEdit(ctx, worldRotationKey);
        CommitRotationEditState(ctx, entity, EditorTransformSpace::World, euler, worldRotation);
        if (worldRotationBlocked)
        {
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered())
                ImGui::SetItemTooltip("WORLD-Rotation ist bei nicht-uniform skalierten Parents deaktiviert, weil das Child sonst in einem reinen TRS-System geschert/verzerrt wird.");
        }

        // Scale-Gizmo arbeitet immer im Local-Space — World-Scale ist nur Anzeige,
        // aber das Label ist anklickbar um das Gizmo zu aktivieren.
        math::Vec3 scale = WorldScale(ctx, entity);
        {
            auto sclResult = LabeledDragFloat3("Scale", "##scl", &scale.x, 0.01f,
                                               sclMode ? &kSclActive : nullptr);
            if (sclResult.labelClicked) ctx.state.gizmoMode = GizmoMode::Scale;
        }
    }
}

static uint32_t VisibleUserLayerMask(const EditorFrameContext& ctx)
{
    uint32_t mask = 0u;
    for (int b = 0; b < 32; ++b)
    {
        if (b == 1)
            continue;
        const std::string& name = ctx.state.layerNames[b];
        if (name.empty() && b >= 8)
            continue;
        mask |= (1u << b);
    }
    return mask;
}

static uint32_t NormalizeSingleUserLayerMask(const EditorFrameContext& ctx, uint32_t mask)
{
    const uint32_t selected = mask & VisibleUserLayerMask(ctx);
    for (int b = 0; b < 32; ++b)
    {
        if ((selected & (1u << b)) != 0u)
            return (1u << b);
    }
    return renderer::LAYER_DEFAULT;
}

static int LayerIndexFromSingleMask(uint32_t mask)
{
    for (int b = 0; b < 32; ++b)
        if ((mask & (1u << b)) != 0u)
            return b;
    return 0;
}

static std::string BuildLayerMaskPreview(const EditorFrameContext& ctx, uint32_t mask)
{
    const uint32_t visibleMask = VisibleUserLayerMask(ctx);
    const uint32_t selected = mask & visibleMask;
    if (selected == 0u)
        return "Nothing";
    if ((selected & visibleMask) == visibleMask)
        return "Everything";

    std::string preview;
    int listed = 0;
    int total = 0;
    for (int b = 0; b < 32; ++b)
    {
        if ((selected & (1u << b)) == 0u)
            continue;
        if (b == 1)
            continue;
        const std::string& name = ctx.state.layerNames[b];
        if (name.empty() && b >= 8)
            continue;

        ++total;
        if (listed >= 3)
            continue;
        if (!preview.empty())
            preview += ", ";
        preview += name.empty() ? ("Layer " + std::to_string(b)) : name;
        ++listed;
    }

    if (total > 3)
        return std::to_string(total) + " Layers";
    return preview;
}

static bool DrawLayerMaskCombo(EditorFrameContext& ctx,
                               const char* label,
                               const char* id,
                               uint32_t& mask)
{
    const uint32_t visibleMask = VisibleUserLayerMask(ctx);
    uint32_t nextMask = mask & ~renderer::LAYER_EDITOR_GIZMO;
    bool changed = false;

    const std::string preview = BuildLayerMaskPreview(ctx, nextMask);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    if (ImGui::BeginCombo(label, preview.c_str()))
    {
        if (ImGui::Selectable(("Everything##" + std::string(id)).c_str(),
                              (nextMask & visibleMask) == visibleMask))
        {
            nextMask = visibleMask;
            changed = true;
        }
        if (ImGui::Selectable(("Nothing##" + std::string(id)).c_str(),
                              (nextMask & visibleMask) == 0u))
        {
            nextMask = renderer::LAYER_NONE;
            changed = true;
        }

        ImGui::Separator();
        for (int b = 0; b < 32; ++b)
        {
            if (b == 1)
                continue;
            const std::string& name = ctx.state.layerNames[b];
            if (name.empty() && b >= 8)
                continue;

            bool checked = (nextMask & (1u << b)) != 0u;
            const std::string itemLabel =
                (name.empty() ? ("Layer " + std::to_string(b)) : name) +
                "##" + id + std::to_string(b);
            if (ImGui::Checkbox(itemLabel.c_str(), &checked))
            {
                if (checked)
                    nextMask |= (1u << b);
                else
                    nextMask &= ~(1u << b);
                changed = true;
            }
        }
        ImGui::EndCombo();
    }

    nextMask &= ~renderer::LAYER_EDITOR_GIZMO;
    if (changed && nextMask != mask)
        mask = nextMask;
    return changed;
}

static void DrawCameraSection(EditorFrameContext& ctx, EntityID entity)
{
    auto* camera = ctx.world.Get<CameraComponent>(entity);
    if (!camera)
        return;

    const bool camOpen = ImGui::CollapsingHeader("Camera", ImGuiTreeNodeFlags_DefaultOpen);
    if (ImGui::BeginPopupContextItem("##cam_hdr_ctx"))
    {
        if (ImGui::MenuItem("Camera entfernen"))
            ctx.world.Remove<CameraComponent>(entity);
        ImGui::EndPopup();
    }
    if (!camOpen)
        return;

    int projection = camera->projection == ProjectionType::Orthographic ? 1 : 0;
    const std::string projectionKey = MakeEditHistoryKey(entity, "camera.projection");
    if (ImGui::Combo("Projection", &projection, "Perspective\0Orthographic\0"))
    {
        BeginPendingSceneEdit(ctx, projectionKey, "Kamera-Projektion geaendert");
        camera->projection = projection == 1 ? ProjectionType::Orthographic : ProjectionType::Perspective;
    }
    if (ImGui::IsItemDeactivatedAfterEdit())
        CommitPendingSceneEdit(ctx, projectionKey);

    const std::string primaryValueKey = camera->projection == ProjectionType::Perspective
        ? MakeEditHistoryKey(entity, "camera.fov")
        : MakeEditHistoryKey(entity, "camera.orthoSize");
    if (camera->projection == ProjectionType::Perspective)
    {
        if (ImGui::DragFloat("FOV", &camera->fovYDeg, 0.25f, 1.f, 179.f))
            BeginPendingSceneEdit(ctx, primaryValueKey, "Kamera-FOV geaendert");
        if (ImGui::IsItemDeactivatedAfterEdit())
            CommitPendingSceneEdit(ctx, primaryValueKey);
    }
    else
    {
        if (ImGui::DragFloat("Ortho Size", &camera->orthoSize, 0.05f, 0.01f, 10000.f))
            BeginPendingSceneEdit(ctx, primaryValueKey, "Kamera-Orthogroesse geaendert");
        if (ImGui::IsItemDeactivatedAfterEdit())
            CommitPendingSceneEdit(ctx, primaryValueKey);
    }

    const std::string nearKey = MakeEditHistoryKey(entity, "camera.near");
    if (ImGui::DragFloat("Near", &camera->nearPlane, 0.01f, 0.001f, camera->farPlane - 0.001f))
        BeginPendingSceneEdit(ctx, nearKey, "Near Plane geaendert");
    if (ImGui::IsItemDeactivatedAfterEdit())
        CommitPendingSceneEdit(ctx, nearKey);

    const std::string farKey = MakeEditHistoryKey(entity, "camera.far");
    if (ImGui::DragFloat("Far", &camera->farPlane, 1.0f, camera->nearPlane + 0.001f, 100000.f))
        BeginPendingSceneEdit(ctx, farKey, "Far Plane geaendert");
    if (ImGui::IsItemDeactivatedAfterEdit())
        CommitPendingSceneEdit(ctx, farKey);


    bool activeCamera = camera->isMainCamera;
    const std::string mainKey = MakeEditHistoryKey(entity, "camera.main");
    if (ImGui::Checkbox("Active Camera", &activeCamera))
    {
        BeginPendingSceneEdit(ctx, mainKey, "Aktive Kamera geaendert");
        if (activeCamera)
            SetExclusiveMainCamera(ctx, entity);
        else
            camera->isMainCamera = false;
    }
    if (ImGui::IsItemDeactivatedAfterEdit())
        CommitPendingSceneEdit(ctx, mainKey);

    {
        uint32_t cm = camera->cullingMask;
        const std::string cmKey = MakeEditHistoryKey(entity, "camera.cullingMask");
        if (DrawLayerMaskCombo(ctx, "Culling Mask", "cameraCulling", cm))
        {
            BeginPendingSceneEdit(ctx, cmKey, "Culling Mask geaendert");
            camera->cullingMask = cm;
            CommitPendingSceneEdit(ctx, cmKey);
        }
    }

    ImGui::Separator();
    ImGui::TextDisabled("Hintergrund");

    {
        int bgMode = static_cast<int>(camera->backgroundMode);
        const std::string bgModeKey = MakeEditHistoryKey(entity, "camera.backgroundMode");
        if (ImGui::Combo("##bgMode", &bgMode, "Solid\0Skybox\0"))
        {
            BeginPendingSceneEdit(ctx, bgModeKey, "Hintergrundmodus geaendert");
            camera->backgroundMode = static_cast<BackgroundMode>(bgMode);
            CommitPendingSceneEdit(ctx, bgModeKey);
        }
    }

    if (camera->backgroundMode == BackgroundMode::ClearColor)
    {
        const std::string colorKey = MakeEditHistoryKey(entity, "camera.clearColor");
        if (ImGui::ColorEdit4("Farbe##bgColor", camera->clearColor.data()))
            BeginPendingSceneEdit(ctx, colorKey, "Hintergrundfarbe geaendert");
        if (ImGui::IsItemDeactivatedAfterEdit())
            CommitPendingSceneEdit(ctx, colorKey);
    }

}

static void DrawLightSection(EditorFrameContext& ctx, EntityID entity)
{
    auto* light = ctx.world.Get<LightComponent>(entity);
    if (!light)
        return;

    const bool lightOpen = ImGui::CollapsingHeader("Light", ImGuiTreeNodeFlags_DefaultOpen);
    if (ImGui::BeginPopupContextItem("##light_hdr_ctx"))
    {
        if (ImGui::MenuItem("Light entfernen"))
            ctx.world.Remove<LightComponent>(entity);
        ImGui::EndPopup();
    }
    if (!lightOpen)
        return;

    int type = static_cast<int>(light->type);
    const std::string typeKey = MakeEditHistoryKey(entity, "light.type");
    if (ImGui::Combo("Type", &type, "Directional\0Point\0Spot\0"))
    {
        BeginPendingSceneEdit(ctx, typeKey, "Lichttyp geaendert");
        const LightType previousType = light->type;
        light->type = static_cast<LightType>(std::clamp(type, 0, 2));
        if (light->type != previousType)
        {
            if (light->type == LightType::Directional)
            {
                light->range = 100.f;
                light->castShadows = true;
                light->shadowSettings.enabled = true;
                light->shadowSettings.resolution = 2048u;
                light->shadowSettings.maxDistance = 100.f;
            }
            else if (light->type == LightType::Spot)
            {
                light->range = 12.f;
                light->spotInnerDeg = 18.f;
                light->spotOuterDeg = 35.f;
                light->castShadows = true;
                light->shadowSettings.enabled = true;
                light->shadowSettings.resolution = 1024u;
                light->shadowSettings.maxDistance = light->range;
            }
            else
            {
                light->range = 12.f;
                light->castShadows = true;
                light->shadowSettings.enabled = true;
                light->shadowSettings.resolution = 1024u;
                light->shadowSettings.maxDistance = light->range;
            }
        }
    }
    if (ImGui::IsItemDeactivatedAfterEdit())
        CommitPendingSceneEdit(ctx, typeKey);

    const std::string colorKey = MakeEditHistoryKey(entity, "light.color");
    if (ImGui::ColorEdit3("Color", &light->color.x))
        BeginPendingSceneEdit(ctx, colorKey, "Lichtfarbe geaendert");
    if (ImGui::IsItemDeactivatedAfterEdit())
        CommitPendingSceneEdit(ctx, colorKey);

    const std::string intensityKey = MakeEditHistoryKey(entity, "light.intensity");
    if (ImGui::DragFloat("Intensity", &light->intensity, 0.05f, 0.f, 100000.f))
        BeginPendingSceneEdit(ctx, intensityKey, "Lichtintensitaet geaendert");
    if (ImGui::IsItemDeactivatedAfterEdit())
        CommitPendingSceneEdit(ctx, intensityKey);

    if (light->type != LightType::Directional)
    {
        const float oldRange = light->range;
        const std::string rangeKey = MakeEditHistoryKey(entity, "light.range");
        if (ImGui::DragFloat("Range", &light->range, 0.1f, 0.01f, 100000.f))
        {
            const bool shadowDistanceFollowedRange =
                std::fabs(light->shadowSettings.maxDistance - oldRange) <= 0.01f ||
                std::fabs(light->shadowSettings.maxDistance - 100.f) <= 0.01f;
            if (shadowDistanceFollowedRange)
                light->shadowSettings.maxDistance = light->range;
            BeginPendingSceneEdit(ctx, rangeKey, "Lichtreichweite geaendert");
        }
        if (ImGui::IsItemDeactivatedAfterEdit())
            CommitPendingSceneEdit(ctx, rangeKey);
    }

    if (light->type == LightType::Spot)
    {
        const std::string innerKey = MakeEditHistoryKey(entity, "light.innerAngle");
        if (ImGui::DragFloat("Inner Angle", &light->spotInnerDeg, 0.25f, 0.f, 179.f))
            BeginPendingSceneEdit(ctx, innerKey, "Spot-Innenwinkel geaendert");
        if (ImGui::IsItemDeactivatedAfterEdit())
            CommitPendingSceneEdit(ctx, innerKey);

        const std::string outerKey = MakeEditHistoryKey(entity, "light.outerAngle");
        if (ImGui::DragFloat("Outer Angle", &light->spotOuterDeg, 0.25f, light->spotInnerDeg, 179.f))
            BeginPendingSceneEdit(ctx, outerKey, "Spot-Aussenwinkel geaendert");
        if (ImGui::IsItemDeactivatedAfterEdit())
            CommitPendingSceneEdit(ctx, outerKey);
    }

    bool shadows = light->castShadows || light->shadowSettings.enabled;
    const std::string castShadowsKey = MakeEditHistoryKey(entity, "light.castShadows");
    if (ImGui::Checkbox("Cast Shadows", &shadows))
    {
        BeginPendingSceneEdit(ctx, castShadowsKey, "Schattenoption geaendert");
        light->castShadows = shadows;
        light->shadowSettings.enabled = shadows;
        if (shadows && light->type != LightType::Directional)
        {
            const float range = std::max(light->range, 0.1f);
            if (light->shadowSettings.maxDistance <= 0.1f ||
                light->shadowSettings.maxDistance > range * 1.5f)
            {
                light->shadowSettings.maxDistance = range;
            }
        }
    }
    if (ImGui::IsItemDeactivatedAfterEdit())
        CommitPendingSceneEdit(ctx, castShadowsKey);

    if (shadows)
    {
        int resolution = static_cast<int>(light->shadowSettings.resolution);
        const std::string resolutionKey = MakeEditHistoryKey(entity, "light.shadowResolution");
        if (ImGui::DragInt("Shadow Resolution", &resolution, 16.f, 128, 8192))
        {
            BeginPendingSceneEdit(ctx, resolutionKey, "Schattenaufloesung geaendert");
            light->shadowSettings.resolution = static_cast<uint32_t>(std::max(resolution, 128));
        }
        if (ImGui::IsItemDeactivatedAfterEdit())
            CommitPendingSceneEdit(ctx, resolutionKey);

        const std::string biasKey = MakeEditHistoryKey(entity, "light.shadowBias");
        if (ImGui::DragFloat("Shadow Bias", &light->shadowSettings.bias, 0.0001f, 0.f, 1.f, "%.5f"))
            BeginPendingSceneEdit(ctx, biasKey, "Shadow Bias geaendert");
        if (ImGui::IsItemDeactivatedAfterEdit())
            CommitPendingSceneEdit(ctx, biasKey);

        const std::string normalBiasKey = MakeEditHistoryKey(entity, "light.normalBias");
        if (ImGui::DragFloat("Normal Bias", &light->shadowSettings.normalBias, 0.0001f, 0.f, 1.f, "%.5f"))
            BeginPendingSceneEdit(ctx, normalBiasKey, "Normal Bias geaendert");
        if (ImGui::IsItemDeactivatedAfterEdit())
            CommitPendingSceneEdit(ctx, normalBiasKey);

        const std::string strengthKey = MakeEditHistoryKey(entity, "light.shadowStrength");
        if (ImGui::DragFloat("Shadow Strength", &light->shadowSettings.strength, 0.01f, 0.f, 1.f))
            BeginPendingSceneEdit(ctx, strengthKey, "Shadow Strength geaendert");
        if (ImGui::IsItemDeactivatedAfterEdit())
            CommitPendingSceneEdit(ctx, strengthKey);

        const std::string distanceKey = MakeEditHistoryKey(entity, "light.shadowDistance");
        if (ImGui::DragFloat("Shadow Distanz", &light->shadowSettings.maxDistance, 0.5f, 1.f, 10000.f, "%.1f"))
            BeginPendingSceneEdit(ctx, distanceKey, "Schatten-Distanz geaendert");
        if (ImGui::IsItemDeactivatedAfterEdit())
            CommitPendingSceneEdit(ctx, distanceKey);
        if (ImGui::IsItemHovered())
            ImGui::SetItemTooltip(
                "Maximale Entfernung, bis zu der Schatten berechnet werden.\n"
                "Unabhaengig von 'Range' (Licht-Attenuation).\n"
                "Groessere Werte = weniger Tiefenpraezision nahe der Lichtquelle.");

        if (light->type == LightType::Directional)
        {
            int cascades = static_cast<int>(std::max<uint32_t>(1u, light->shadowSettings.cascadeCount));
            const std::string cascadeKey = MakeEditHistoryKey(entity, "light.cascadeCount");
            if (ImGui::SliderInt("Cascades", &cascades, 1, 4))
            {
                BeginPendingSceneEdit(ctx, cascadeKey, "Cascade-Anzahl geaendert");
                light->shadowSettings.cascadeCount = static_cast<uint32_t>(std::clamp(cascades, 1, 4));
            }
            if (ImGui::IsItemDeactivatedAfterEdit())
                CommitPendingSceneEdit(ctx, cascadeKey);
            if (ImGui::IsItemHovered())
                ImGui::SetItemTooltip(
                    "Cascaded Shadow Maps: Anzahl der Schatten-Stufen.\n"
                    "1 = eine einzelne Map ueber die ganze Distanz (nah = unscharf,\n"
                    "wenn die Distanz gross ist). 2-4 = nahe Bereiche bekommen eine\n"
                    "eigene, scharfe Map, ferne eine groebere. Behebt unscharfe\n"
                    "Schatten in Kameranaehe bei grosser Shadow-Distanz.");

            if (light->shadowSettings.cascadeCount > 1u)
            {
                const std::string lambdaKey = MakeEditHistoryKey(entity, "light.cascadeLambda");
                if (ImGui::SliderFloat("Cascade-Verteilung", &light->shadowSettings.cascadeLambda, 0.f, 1.f, "%.2f"))
                    BeginPendingSceneEdit(ctx, lambdaKey, "Cascade-Verteilung geaendert");
                if (ImGui::IsItemDeactivatedAfterEdit())
                    CommitPendingSceneEdit(ctx, lambdaKey);
                if (ImGui::IsItemHovered())
                    ImGui::SetItemTooltip(
                        "Verteilung der Cascade-Grenzen ueber die Shadow-Distanz.\n"
                        "0.0 = gleichmaessig (uniform) — ferne Cascades scharf, nahe groeber.\n"
                        "1.0 = logarithmisch — nahe Cascades sehr scharf (klein), ferne groeber.\n"
                        "Hoeher stellen, wenn Schatten direkt vor der Kamera noch unscharf sind.");
            }
        }
    }
}

static void DrawMeshSection(EditorFrameContext& ctx, EntityID entity)
{
    auto* meshComp = ctx.world.Get<MeshComponent>(entity);
    if (!meshComp)
        return;

    if (!ImGui::CollapsingHeader("Mesh", ImGuiTreeNodeFlags_DefaultOpen))
        return;

    const bool handleValid = meshComp->mesh.IsValid() &&
                             ctx.registry.meshes.Get(meshComp->mesh) != nullptr;

    if (!handleValid)
    {
        // Kein gueltiger Mesh-Handle — unterscheide zwei Faelle:
        if (!meshComp->meshAssetPath.empty())
        {
            // Pfad vorhanden aber Handle ungueltig → Scene wurde mit altem Code gespeichert
            // oder ResolveMeshAssetBindings ist noch nicht gelaufen.
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.4f, 0.1f, 1.f));
            ImGui::TextWrapped("Mesh nicht gebunden!");
            ImGui::PopStyleColor();
            ImGui::TextDisabled("Pfad: %s", meshComp->meshAssetPath.c_str());
            ImGui::TextDisabled("Scene neu speichern oder Modell erneut aus dem");
            ImGui::TextDisabled("Asset Browser in die Scene ziehen.");
            if (ctx.syncSceneState && ImGui::Button("Erneut binden##mesh"))
                ctx.syncSceneState();
        }
        else
        {
            ImGui::TextDisabled("(Kein Mesh zugewiesen)");
        }
        return;
    }

    const auto* mesh = ctx.registry.meshes.Get(meshComp->mesh);
    ImGui::Text("Name: %s", mesh->debugName.c_str());
    ImGui::Text("Submeshes: %u", static_cast<uint32_t>(mesh->submeshes.size()));
    if (!meshComp->meshAssetPath.empty())
        ImGui::TextDisabled("Pfad: %s", meshComp->meshAssetPath.c_str());

}

static void DrawMaterialSlotsSection(EditorFrameContext& ctx, EntityID entity)
{
    auto* meshComp = ctx.world.Get<MeshComponent>(entity);
    if (!meshComp)
    {
        EntityID meshChild = NULL_ENTITY;
        ctx.world.ForEachAlive([&](EntityID id)
        {
            if (meshChild.IsValid() || !ctx.world.Get<MeshComponent>(id))
                return;

            EntityID cur = id;
            while (cur.IsValid())
            {
                if (cur == entity)
                {
                    meshChild = id;
                    return;
                }
                const auto* parent = ctx.world.Get<ParentComponent>(cur);
                cur = parent ? parent->parent : NULL_ENTITY;
            }
        });

        if (!meshChild.IsValid())
            return;

        entity = meshChild;
        meshComp = ctx.world.Get<MeshComponent>(entity);
        if (!meshComp)
            return;
    }

    assets::MeshAsset* mesh = ctx.registry.meshes.Get(meshComp->mesh);
    if (!mesh)
        return;

    if (!ImGui::CollapsingHeader("Materials", ImGuiTreeNodeFlags_DefaultOpen))
        return;

    const MaterialComponent* overrideMaterial = ctx.world.Get<MaterialComponent>(entity);
    const bool hasEntityOverride = overrideMaterial &&
        (overrideMaterial->material.IsValid() ||
         !overrideMaterial->materialAssetPath.empty() ||
         !overrideMaterial->baseColorTexturePath.empty());

    const uint32_t submeshCount = static_cast<uint32_t>(mesh->submeshes.size());
    if (submeshCount == 0u)
    {
        ImGui::TextDisabled("(No material slots)");
        return;
    }

    // Element 0 automatisch vorauswählen wenn diese Entity selektiert wird
    // und noch kein Slot für sie aktiv ist.
    if (ctx.state.inspectorMaterialEntity != entity)
    {
        const uint32_t materialIndex0 = mesh->submeshes[0].materialIndex;
        std::string autoPath;
        if (overrideMaterial)
        {
            if (const MaterialComponent::SlotOverride* s = overrideMaterial->FindSlotOverride(0u))
                autoPath = s->materialAssetPath;
            if (autoPath.empty() && !overrideMaterial->materialAssetPath.empty())
                autoPath = overrideMaterial->materialAssetPath;
        }
        if (autoPath.empty() && materialIndex0 < mesh->materialHandles.size())
        {
            const assets::MaterialAsset* ma = GetMaterialAsset(ctx, mesh->materialHandles[materialIndex0]);
            if (ma) autoPath = ma->path;
        }
        if (CanOpenMaterialAssetPath(autoPath))
            SelectInspectorMaterial(ctx, entity, 0u, autoPath);
    }

    const float rowH = ImGui::GetFrameHeightWithSpacing();
    ImGui::BeginChild("##material_slots",
                      ImVec2(0.f, std::min(160.f, rowH * submeshCount + 8.f)),
                      true);

    for (uint32_t si = 0u; si < submeshCount; ++si)
    {
        const uint32_t materialIndex = mesh->submeshes[si].materialIndex;
        MaterialHandle slotHandle = MaterialHandle::Invalid();
        if (materialIndex < mesh->materialHandles.size())
            slotHandle = mesh->materialHandles[materialIndex];

        const MaterialComponent::SlotOverride* slotOverride =
            overrideMaterial ? overrideMaterial->FindSlotOverride(si) : nullptr;
        const bool hasSlotOverride = slotOverride &&
            (slotOverride->material.IsValid() || !slotOverride->materialAssetPath.empty());

        const MaterialHandle visibleHandle = hasSlotOverride
            ? slotOverride->material
            : (hasEntityOverride ? overrideMaterial->material : slotHandle);
        const assets::MaterialAsset* visibleMaterial = GetMaterialAsset(ctx, visibleHandle);
        std::string visibleName = MaterialDisplayName(visibleMaterial, visibleHandle);
        std::string visiblePath = (visibleMaterial && !visibleMaterial->path.empty())
            ? visibleMaterial->path
            : std::string{};

        if (hasSlotOverride && !slotOverride->materialAssetPath.empty())
        {
            visiblePath = slotOverride->materialAssetPath;
            visibleName = std::filesystem::path(visiblePath).filename().string();
        }
        else if (hasEntityOverride && !overrideMaterial->materialAssetPath.empty())
        {
            visiblePath = overrideMaterial->materialAssetPath;
            visibleName = std::filesystem::path(visiblePath).filename().string();
        }
        else if (hasEntityOverride && !overrideMaterial->baseColorTexturePath.empty())
        {
            visiblePath = overrideMaterial->baseColorTexturePath;
            visibleName = std::filesystem::path(visiblePath).filename().string();
        }

        const float buttonW = 54.f;
        const float rowW = std::max(1.f, ImGui::GetContentRegionAvail().x - buttonW - 8.f);
        std::string rowLabel = "Element " + std::to_string(si) + "  [" +
            (hasSlotOverride ? "Slot" : (hasEntityOverride ? "Override" : "Mesh")) +
            "]  " + visibleName;

        ImGui::PushID(static_cast<int>(si));
        const bool selectable = CanOpenMaterialAssetPath(visiblePath);
        const bool selected = selectable && IsInspectorMaterialSelection(ctx, entity, si, visiblePath);
        if (!selectable)
            ImGui::BeginDisabled();
        if (ImGui::Selectable(rowLabel.c_str(), selected, ImGuiSelectableFlags_AllowOverlap, ImVec2(rowW, 0.f)))
            SelectInspectorMaterial(ctx, entity, si, visiblePath);
        if (!selectable)
            ImGui::EndDisabled();

        if (ImGui::IsItemHovered() && !visiblePath.empty())
            ImGui::SetItemTooltip("%s", visiblePath.c_str());
        if (ImGui::BeginPopupContextItem("##material_slot_ctx"))
        {
            if (hasSlotOverride && ImGui::MenuItem("Remove Slot Override"))
            {
                ExecuteSceneMutation(ctx, "Material-Slot entfernt", [&]() {
                    if (MaterialComponent* material = ctx.world.Get<MaterialComponent>(entity))
                    {
                        material->slotOverrides.erase(
                            std::remove_if(material->slotOverrides.begin(),
                                           material->slotOverrides.end(),
                                           [&](const MaterialComponent::SlotOverride& slot) {
                                               return slot.submeshIndex == si;
                                           }),
                            material->slotOverrides.end());
                    }
                });
            }
            ImGui::EndPopup();
        }
        if (ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("KROM_ASSET_MATERIAL"))
            {
                const char* matPath = static_cast<const char*>(payload->Data);
                if (matPath && matPath[0] != '\0')
                {
                    ExecuteSceneMutation(ctx, "Material-Slot zugewiesen", [&]() {
                        SelectMaterialAsset(ctx, std::filesystem::path(matPath));
                        ApplyMaterialAssetToEntitySlot(ctx, entity, si, std::filesystem::path(matPath));
                    });
                }
            }
            ImGui::EndDragDropTarget();
        }

        ImGui::SameLine();
        const bool canOpen = CanOpenMaterialAssetPath(visiblePath);
        if (!canOpen)
            ImGui::BeginDisabled();
        if (ImGui::SmallButton("Open"))
            OpenMaterialAsset(ctx, visiblePath);
        if (!canOpen)
            ImGui::EndDisabled();

        if (!hasSlotOverride && !hasEntityOverride && materialIndex >= mesh->materialHandles.size())
        {
            ImGui::SameLine();
            ImGui::TextDisabled("(missing)");
        }

        ImGui::PopID();
    }

    ImGui::EndChild();

    if (hasEntityOverride && !overrideMaterial->materialAssetPath.empty())
        ImGui::TextDisabled("Entity override: %s", overrideMaterial->materialAssetPath.c_str());
    if (overrideMaterial && !overrideMaterial->slotOverrides.empty())
        ImGui::TextDisabled("Slot overrides: %u", static_cast<uint32_t>(overrideMaterial->slotOverrides.size()));
}

static void DrawMaterialSection(EditorFrameContext& ctx, EntityID entity)
{
    const bool hasMesh = ctx.world.Get<MeshComponent>(entity) != nullptr;

    MaterialComponent* materialComp = ctx.world.Get<MaterialComponent>(entity);
    const MaterialHandle matHandle = GetEntityMaterial(ctx, entity);
    const bool hasSlotSelection =
        ctx.state.inspectorMaterialEntity == entity &&
        ctx.state.inspectorMaterialSlot >= 0 &&
        !ctx.state.inspectorMaterialAssetPath.empty();
    // Fuer Entities OHNE Mesh den Material-Bereich nur anzeigen wenn ein Material
    // zugewiesen ist. Fuer Mesh-Entities immer anzeigen (Drag-Drop-Ziel muss sichtbar
    // bleiben, auch nachdem das Material entfernt wurde).
    if (!hasMesh && !hasSlotSelection && !matHandle.IsValid() &&
        (!materialComp || materialComp->materialAssetPath.empty()))
        return;

    const bool matOpen = ImGui::CollapsingHeader("Material", ImGuiTreeNodeFlags_DefaultOpen);
    if (ImGui::BeginPopupContextItem("##mat_hdr_ctx"))
    {
        if (materialComp && !materialComp->materialAssetPath.empty())
        {
            if (ImGui::MenuItem("In eigenem Fenster oeffnen"))
                OpenMaterialAsset(ctx, materialComp->materialAssetPath);
            ImGui::Separator();
            if (ImGui::MenuItem("Material entfernen"))
            {
                ExecuteSceneMutation(ctx, "Material entfernt", [&]() {
                    if (auto* mc = ctx.world.Get<MaterialComponent>(entity))
                    {
                        mc->material = MaterialHandle{};
                        mc->materialAssetPath.clear();
                    }
                });
            }
        }
        ImGui::EndPopup();
    }
    if (!matOpen)
        return;

    // Materialpfad anzeigen — die gesamte Zeile ist Drag-Drop-Ziel
    if (hasSlotSelection)
    {
        ImGui::TextDisabled("Element %d: %s",
                            ctx.state.inspectorMaterialSlot,
                            ctx.state.inspectorMaterialAssetPath.c_str());

        if (std::filesystem::path(ctx.state.inspectorMaterialAssetPath).generic_string() !=
            std::filesystem::path(ctx.state.selectedMaterialAssetPath).generic_string())
        {
            ctx.state.inspectorMaterialEntity = NULL_ENTITY;
            ctx.state.inspectorMaterialSlot = -1;
            ctx.state.inspectorMaterialAssetPath.clear();
            return;
        }

        ImGui::Separator();
        DrawSelectedMaterialInspector(ctx);
        return;
    }

    if (materialComp && !materialComp->materialAssetPath.empty())
    {
        ImGui::TextDisabled("%s", materialComp->materialAssetPath.c_str());
    }
    else
    {
        ImGui::TextDisabled("Material hierher ziehen");
    }

    // Drag-Drop auf den Materialpfad / Hinweistext
    if (ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("KROM_ASSET_MATERIAL"))
        {
            const char* matPath = static_cast<const char*>(payload->Data);
            if (matPath && matPath[0] != '\0')
            {
                ExecuteSceneMutation(ctx, "Material zugewiesen", [&]() {
                    SelectMaterialAsset(ctx, std::filesystem::path(matPath));
                    ApplyMaterialAssetToEntity(ctx, entity, std::filesystem::path(matPath));
                });
            }
        }
        ImGui::EndDragDropTarget();
    }

    // Inline-Material-Editor (nur wenn ein Asset-Material zugewiesen ist)
    if (materialComp && !materialComp->materialAssetPath.empty())
    {
        if (std::filesystem::path(materialComp->materialAssetPath).generic_string() ==
            std::filesystem::path(ctx.state.selectedMaterialAssetPath).generic_string())
        {
            ImGui::Separator();
            DrawSelectedMaterialInspector(ctx);
        }
    }
}

static void DrawProjectSettingsDialog(EditorFrameContext& ctx)
{
    // ── Edit Layers Fenster ───────────────────────────────────────────────────
    if (ctx.state.layerNamesWindowOpen)
    {
        ImGui::SetNextWindowSize(ImVec2(300.f, 380.f), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Layer bearbeiten##layerEdit", &ctx.state.layerNamesWindowOpen))
        {
            ImGui::TextDisabled("Index 0 (Default) und 1 (EditorGizmo) sind reserviert.");
            ImGui::Separator();
            for (int b = 0; b < 32; ++b)
            {
                char label[16];
                std::snprintf(label, sizeof(label), "%d", b);
                ImGui::SetNextItemWidth(40.f);
                ImGui::TextDisabled("%s", label);
                ImGui::SameLine();
                char buf[32]{};
                std::snprintf(buf, sizeof(buf), "%s", ctx.state.layerNames[b].c_str());
                char inputId[16];
                std::snprintf(inputId, sizeof(inputId), "##ln%d", b);
                const bool isReserved = (b == 0 || b == 1); // Default + EditorGizmo gesperrt
                if (isReserved) ImGui::BeginDisabled();
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
                if (ImGui::InputText(inputId, buf, sizeof(buf)))
                    ctx.state.layerNames[b] = buf;
                if (ImGui::IsItemDeactivatedAfterEdit() && ctx.saveProject)
                    ctx.saveProject();
                if (isReserved) ImGui::EndDisabled();
            }
        }
        ImGui::End();
    }

    if (ctx.state.settingsWindowOpen)
    {
        ImGui::OpenPopup("Projekt-Einstellungen##settings");
        ctx.state.settingsWindowOpen = false;
    }

    if (!ImGui::BeginPopupModal("Projekt-Einstellungen##settings",
                                nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize))
        return;

    ImGui::TextDisabled("Runtime:");
    const char* backendItems[] = { "Vulkan", "DirectX11", "OpenGL" };
    int backendSelection = 0;
    switch (ctx.projectBackend)
    {
    case renderer::DeviceFactory::BackendType::DirectX11: backendSelection = 1; break;
    case renderer::DeviceFactory::BackendType::OpenGL: backendSelection = 2; break;
    case renderer::DeviceFactory::BackendType::Vulkan:
    default: backendSelection = 0; break;
    }
    ImGui::SetNextItemWidth(220.f);
    if (ImGui::Combo("Backend", &backendSelection, backendItems, IM_ARRAYSIZE(backendItems)))
    {
        const renderer::DeviceFactory::BackendType nextBackend =
            EditorProjectBackendFromSelection(backendSelection);
        ctx.projectBackend = nextBackend;
        if (ctx.setProjectBackend)
            ctx.setProjectBackend(nextBackend);
    }
    ImGui::TextDisabled("Play baut die Projekt-Runtime fuer dieses Backend.");
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextDisabled("Szenenreihenfolge (Startszene = 1):");
    ImGui::Spacing();

    std::vector<std::string> order = ctx.sceneOrder;
    bool orderChanged = false;
    int moveFrom = -1, moveTo = -1;

    for (int i = 0; i < static_cast<int>(order.size()); ++i)
    {
        ImGui::PushID(i);

        // Hoch-Button zuerst (links), dann Szenenname — so bleiben sie immer sichtbar.
        const bool canMoveUp = (i > 0);
        if (!canMoveUp) ImGui::BeginDisabled();
        if (ImGui::SmallButton(" ^ ")) { moveFrom = i; moveTo = i - 1; orderChanged = true; }
        if (!canMoveUp) ImGui::EndDisabled();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Nach oben");
        ImGui::SameLine();

        // Runter-Button
        const bool canMoveDown = (i < static_cast<int>(order.size()) - 1);
        if (!canMoveDown) ImGui::BeginDisabled();
        if (ImGui::SmallButton(" v ")) { moveFrom = i; moveTo = i + 1; orderChanged = true; }
        if (!canMoveDown) ImGui::EndDisabled();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Nach unten");
        ImGui::SameLine();

        // Index + Name
        ImGui::TextDisabled("%d.", i + 1);
        ImGui::SameLine();
        ImGui::TextUnformatted(order[i].c_str());
        ImGui::SameLine();
        if (i == 0)
        {
            ImGui::TextDisabled("Start");
        }
        else
        {
            if (ImGui::SmallButton("Start"))
            {
                moveFrom = i;
                moveTo = 0;
                orderChanged = true;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Als Startszene setzen");
        }

        ImGui::PopID();
    }

    if (orderChanged && moveFrom >= 0 && moveTo >= 0 &&
        moveTo < static_cast<int>(order.size()))
    {
        std::swap(order[moveFrom], order[moveTo]);
        if (ctx.setSceneOrder)
            ctx.setSceneOrder(order);
        ctx.sceneOrder = order; // sofort im UI reflektieren
    }

    if (!order.empty())
    {
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextDisabled("Index 1 = Startszene (LoadScene(1))");
        ImGui::TextDisabled("Index 2 = LoadScene(2), usw.");
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Build-Reihenfolge (Checkboxen)
    ImGui::TextDisabled("Build-Einschluss:");
    const auto& excluded = ctx.buildExcludedScenes;
    for (const std::string& scene : ctx.sceneOrder)
    {
        const bool isExcluded = std::find(excluded.begin(), excluded.end(), scene) != excluded.end();
        bool included = !isExcluded;
        if (ImGui::Checkbox(scene.c_str(), &included))
        {
            if (ctx.toggleBuildExclude)
                ctx.toggleBuildExclude(scene, !included);
        }
    }

    ImGui::Spacing();
    const bool canPlay = static_cast<bool>(ctx.playGame) && !ctx.sceneOrder.empty();
    if (!canPlay) ImGui::BeginDisabled();
    if (ImGui::Button("Play", ImVec2(-1.f, 0.f)))
    {
        const bool ok = ctx.playGame ? ctx.playGame() : false;
        if (!ok && ctx.lastFileMessage.empty())
            ctx.lastFileMessage = "Play fehlgeschlagen.";
    }
    if (!canPlay) ImGui::EndDisabled();

    ImGui::Spacing();
    const bool canBuild = static_cast<bool>(ctx.buildAllScenes) && !ctx.sceneOrder.empty();
    if (!canBuild) ImGui::BeginDisabled();
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.12f, 0.40f, 0.12f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.18f, 0.56f, 0.18f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.22f, 0.65f, 0.22f, 1.f));
    if (ImGui::Button("Spiel erstellen", ImVec2(-1.f, 0.f)))
    {
        const bool ok = ctx.buildAllScenes ? ctx.buildAllScenes() : false;
        ctx.lastFileMessage = ok ? "Build erfolgreich." : "Build fehlgeschlagen.";
    }
    ImGui::PopStyleColor(3);
    if (!canBuild) ImGui::EndDisabled();

    ImGui::Spacing();
    if (ImGui::Button("Schliessen", ImVec2(-1.f, 0.f)) ||
        ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        ImGui::CloseCurrentPopup();

    ImGui::EndPopup();
}

static void DrawSceneManager(EditorFrameContext& ctx)
{
    ImGui::SetNextWindowPos(ImVec2(10.f, 30.f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(230.f, 240.f), ImGuiCond_FirstUseEver);

    if (!ImGui::Begin("Szenen##scenemanager"))
    {
        ImGui::End();
        return;
    }

    // ── Aktive Szenen ────────────────────────────────────────────────────────
    // Primäre Szene + alle additiv geladenen, jeweils mit ×-Button zum Entladen.
    if (ctx.loadedEditorScenes.empty())
    {
        ImGui::TextDisabled("(keine Szene geladen)");
    }
    else
    {
        for (size_t i = 0; i < ctx.loadedEditorScenes.size(); ++i)
        {
            const std::string& scene = ctx.loadedEditorScenes[i];
            const bool isPrimary    = (i == 0);

            ImGui::PushID(("loaded##" + scene).c_str());

            // Entladen-Button — nur für additiv geladene Szenen
            if (!isPrimary && ctx.unloadEditorScene)
            {
                ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.40f, 0.12f, 0.10f, 1.f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.58f, 0.18f, 0.14f, 1.f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.72f, 0.22f, 0.17f, 1.f));
                if (ImGui::SmallButton("×"))
                    ctx.unloadEditorScene(scene);
                ImGui::PopStyleColor(3);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Szene '%s' entladen", scene.c_str());
                ImGui::SameLine();
            }
            else if (isPrimary)
            {
                // Kleines Platzhalter-Padding damit Szenenname auf gleicher Höhe bleibt
                ImGui::TextDisabled("  ");
                ImGui::SameLine();
            }

            // Szenenname — Primäre farbig, additive gedimmt
            if (isPrimary)
                ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "● %s", scene.c_str());
            else
                ImGui::TextColored(ImVec4(0.6f, 0.8f, 0.6f, 0.8f), "+ %s", scene.c_str());

            ImGui::PopID();
        }
    }

    ImGui::Separator();

    // ── Neue Szene erstellen ──────────────────────────────────────────────────
    static char s_newName[64] = {};
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 60.f);
    ImGui::InputTextWithHint("##newscene", "Szenenname...", s_newName, sizeof(s_newName));
    ImGui::SameLine();
    const bool canCreate = s_newName[0] != '\0' && ctx.newEditorScene;
    if (!canCreate) ImGui::BeginDisabled();
    if (ImGui::SmallButton("+Neu"))
    {
        ctx.newEditorScene(s_newName);
        s_newName[0] = '\0';
    }
    if (!canCreate) ImGui::EndDisabled();

    ImGui::Separator();

    // ── Szenenliste ──────────────────────────────────────────────────────────
    if (ctx.availableEditorScenes.empty())
    {
        ImGui::TextDisabled("Keine Szenen in Assets/Scenes/");
    }
    else
    {
        const auto& loaded = ctx.loadedEditorScenes;
        for (const std::string& scene : ctx.availableEditorScenes)
        {
            ImGui::PushID(scene.c_str());

            const bool isLoaded = std::find(loaded.begin(), loaded.end(), scene) != loaded.end();

            if (isLoaded)
            {
                // Bereits geladene Szene: nur Name ausgegraut anzeigen (kein Laden-Button)
                ImGui::TextDisabled("  %s", scene.c_str());
            }
            else
            {
                ImGui::TextUnformatted(scene.c_str());
                const float btnX = ImGui::GetContentRegionAvail().x;

                // Abstand berechnen damit Buttons rechtsbündig sind
                const float ladenW  = ImGui::CalcTextSize("Laden").x + ImGui::GetStyle().FramePadding.x * 2.f;
                const float additW  = ImGui::CalcTextSize("+").x    + ImGui::GetStyle().FramePadding.x * 2.f;
                const float spacing = ImGui::GetStyle().ItemSpacing.x;
                ImGui::SameLine(btnX - ladenW - additW - spacing);

                if (ImGui::SmallButton("Laden") && ctx.switchEditorScene)
                    ctx.switchEditorScene(scene, /*additive=*/false);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Primäre Szene wechseln — nicht-persistente\nEntities werden vorher entfernt.");

                ImGui::SameLine();
                if (ImGui::SmallButton("+") && ctx.switchEditorScene)
                    ctx.switchEditorScene(scene, /*additive=*/true);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Additiv laden — bestehende Entities bleiben\nerhalten, neue kommen dazu.");
            }
            ImGui::PopID();
        }
    }

    ImGui::Separator();

    // ── Build ─────────────────────────────────────────────────────────────────
    ImGui::TextDisabled("Build:");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "Haken = Szene wird beim Build exportiert.\n"
            "Kein Haken = Szene wird beim Build uebersprungen\n"
            "(z.B. Testlevels, Dev-Szenen).");

    if (!ctx.availableEditorScenes.empty())
    {
        for (const std::string& scene : ctx.availableEditorScenes)
        {
            // Kombination aus Scope-ID und Prefix, damit kein Clash mit der Szenenliste oben
            ImGui::PushID(("build##" + scene).c_str());

            const bool isExcluded = std::find(
                ctx.buildExcludedScenes.begin(),
                ctx.buildExcludedScenes.end(),
                scene) != ctx.buildExcludedScenes.end();
            bool included = !isExcluded;

            if (ImGui::Checkbox(scene.c_str(), &included))
            {
                if (ctx.toggleBuildExclude)
                    ctx.toggleBuildExclude(scene, /*excluded=*/!included);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    included ? "Im Build enthalten — Haken entfernen zum Ausschliessen."
                             : "Vom Build ausgeschlossen — Haken setzen zum Einschliessen.");

            ImGui::PopID();
        }
    }

    ImGui::Spacing();

    // "Spiel erstellen"-Button — nur aktiv wenn Callback gesetzt und Szenen vorhanden
    const bool canPlay = static_cast<bool>(ctx.playGame) &&
                          !ctx.availableEditorScenes.empty();
    if (!canPlay)
        ImGui::BeginDisabled();
    if (ImGui::Button("Play", ImVec2(-1.f, 0.f)))
    {
        const bool ok = ctx.playGame ? ctx.playGame() : false;
        if (!ok && ctx.lastFileMessage.empty())
            ctx.lastFileMessage = "Play fehlgeschlagen.";
    }
    if (!canPlay)
        ImGui::EndDisabled();
    if (ImGui::IsItemHovered() && canPlay)
        ImGui::SetTooltip("Speichert, exportiert, baut die Runtime und startet das Spiel.");

    ImGui::Spacing();

    const bool canBuild = static_cast<bool>(ctx.buildAllScenes) &&
                          !ctx.availableEditorScenes.empty();
    if (!canBuild)
        ImGui::BeginDisabled();
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.12f, 0.40f, 0.12f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.18f, 0.56f, 0.18f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.22f, 0.65f, 0.22f, 1.f));
    if (ImGui::Button("Spiel erstellen", ImVec2(-1.f, 0.f)))
    {
        const bool ok = ctx.buildAllScenes ? ctx.buildAllScenes() : false;
        ctx.lastFileMessage = ok ? "Build erfolgreich." : "Build fehlgeschlagen.";
    }
    ImGui::PopStyleColor(3);
    if (!canBuild)
        ImGui::EndDisabled();
    if (ImGui::IsItemHovered() && canBuild)
        ImGui::SetTooltip(
            "Exportiert alle nicht-ausgeschlossenen Szenen\n"
            "als .kscene (Runtime-Format fuer Krom::LoadScene()).");

    ImGui::Spacing();
    const bool canExport = static_cast<bool>(ctx.exportProject) && !ctx.sceneOrder.empty();
    if (!canExport) ImGui::BeginDisabled();
    if (ImGui::Button("Projekt exportieren", ImVec2(-1.f, 0.f)))
    {
        const bool ok = ctx.exportProject ? ctx.exportProject() : false;
        if (!ok && ctx.lastFileMessage.empty())
            ctx.lastFileMessage = "Export fehlgeschlagen.";
    }
    if (!canExport) ImGui::EndDisabled();
    if (ImGui::IsItemHovered() && canExport)
        ImGui::SetTooltip("Erstellt eine spielbare Ausgabe unter Build/<Config>-<Backend>.");

    ImGui::End();
}

// =============================================================================
// OBB – Oriented Bounding Box
// =============================================================================

static void BuildBasis(const math::Vec3& n, math::Vec3& outRight, math::Vec3& outUp);

static math::Mat4 BuildEditorViewMatrix(const EditorCameraState& cam)
{
    const math::Quat rot     = math::Quat::FromEulerDeg(cam.pitchDeg, cam.yawDeg, 0.f);
    const math::Vec3 forward = rot.Rotate({0.f, 0.f, -1.f});
    const math::Vec3 up      = rot.Rotate({0.f, 1.f,  0.f});
    return math::Mat4::LookAtRH(cam.position, cam.position + forward, up);
}

// Projiziert einen Weltpunkt auf Bildschirm-Pixel. Gibt false zurück wenn hinter der Kamera.
static bool WorldToScreenPx(const math::Vec3& worldPos,
                             const math::Mat4& vp,
                             const ImVec2& displaySize,
                             ImVec2& outScreen)
{
    const math::Vec4 clip = vp * math::Vec4{worldPos, 1.f};
    if (clip.w < 0.01f)
        return false;
    outScreen = {
        (clip.x / clip.w * 0.5f + 0.5f)       * displaySize.x,
        (1.f - (clip.y / clip.w * 0.5f + 0.5f)) * displaySize.y
    };
    return true;
}

// 8 World-Space-Ecken der OBB (Reihenfolge passend zu DebugDrawRenderer::Box).
static std::array<math::Vec3, 8> ComputeOBBCorners(const math::Mat4& entityWorld,
                                                    const OBBComponent& obb)
{
    const math::Mat4 obbMat = entityWorld *
        math::Mat4::TRS(obb.centerOffset, obb.orientation, {1.f, 1.f, 1.f});
    const float hx = obb.halfExtents.x, hy = obb.halfExtents.y, hz = obb.halfExtents.z;
    const math::Vec3 lc[8] = {
        {-hx,-hy,-hz}, { hx,-hy,-hz}, { hx, hy,-hz}, {-hx, hy,-hz},
        {-hx,-hy, hz}, { hx,-hy, hz}, { hx, hy, hz}, {-hx, hy, hz}
    };
    std::array<math::Vec3, 8> r;
    for (int i = 0; i < 8; ++i)
        r[i] = obbMat.TransformPoint(lc[i]);
    return r;
}

struct OBBFace
{
    math::Vec3 worldCenter;
    math::Vec3 worldNormal;
    int        axis;  // 0=X, 1=Y, 2=Z
    float      sign;  // +1 oder -1
};

static std::array<OBBFace, 6> ComputeOBBFaces(const math::Mat4& entityWorld,
                                               const OBBComponent& obb)
{
    const math::Mat4 obbMat = entityWorld *
        math::Mat4::TRS(obb.centerOffset, obb.orientation, {1.f, 1.f, 1.f});
    const math::Vec3 worldCenter = obbMat.TransformPoint({0.f, 0.f, 0.f});
    const float h[3] = { obb.halfExtents.x, obb.halfExtents.y, obb.halfExtents.z };

    std::array<OBBFace, 6> faces;
    for (int ax = 0; ax < 3; ++ax)
    {
        math::Vec3 localAxis{};
        (&localAxis.x)[ax] = 1.f;
        for (int s = 0; s < 2; ++s)
        {
            const float sg = (s == 0) ? 1.f : -1.f;
            // Face-Center: Offset in World-Space unter Berücksichtigung der Entity-Scale.
            // TransformDirection(axis * halfExtent) skaliert korrekt mit; normalize erst
            // danach für die Normale verwenden.
            const math::Vec3 faceOffset = obbMat.TransformDirection(localAxis * (sg * h[ax]));
            const math::Vec3 worldNormal = obbMat.TransformDirection(localAxis * sg).Normalized();
            faces[ax * 2 + s] = { worldCenter + faceOffset, worldNormal, ax, sg };
        }
    }
    return faces;
}

// Ray vs World-Space AABB (BoundsComponent). Gibt Eintrittsdistanz t zurück.
// Nur t > 0 ist ein Vorwärts-Treffer; -1 = kein Treffer.
static float RayVsWorldAABB(const math::Vec3& rayOrigin,
                             const math::Vec3& rayDir,
                             const BoundsComponent& bounds)
{
    const float cx = bounds.centerWorld.x, cy = bounds.centerWorld.y, cz = bounds.centerWorld.z;
    const float ex = bounds.extentsWorld.x, ey = bounds.extentsWorld.y, ez = bounds.extentsWorld.z;
    const float o[3] = { rayOrigin.x, rayOrigin.y, rayOrigin.z };
    const float d[3] = { rayDir.x,    rayDir.y,    rayDir.z    };
    const float c[3] = { cx, cy, cz };
    const float e[3] = { ex, ey, ez };

    float tMin = -1e30f, tMax = 1e30f;
    for (int i = 0; i < 3; ++i)
    {
        if (std::abs(d[i]) < 1e-8f)
        {
            if (o[i] < c[i] - e[i] || o[i] > c[i] + e[i]) return -1.f;
        }
        else
        {
            float t1 = (c[i] - e[i] - o[i]) / d[i];
            float t2 = (c[i] + e[i] - o[i]) / d[i];
            if (t1 > t2) std::swap(t1, t2);
            tMin = std::max(tMin, t1);
            tMax = std::min(tMax, t2);
            if (tMin > tMax) return -1.f;
        }
    }
    return (tMin > 0.f) ? tMin : -1.f;
}

// Trackt die Bewegungsachse des selektierten Entitys anhand des Weltpositions-Deltas.
// Muss jeden Frame vor SnapEntityToSurface aufgerufen werden.
static void UpdateSnapAxis(EditorFrameContext& ctx)
{
    EditorState& st = ctx.state;
    const EntityID selected = st.selectedEntity;

    if (!selected.IsValid())
    {
        st.snapAxis = -1;
        return;
    }

    const auto* wtc = ctx.world.Get<WorldTransformComponent>(selected);
    if (!wtc) { st.snapAxis = -1; return; }

    const math::Vec3 delta = {
        std::abs(wtc->position.x - st.snapLastPos.x),
        std::abs(wtc->position.y - st.snapLastPos.y),
        std::abs(wtc->position.z - st.snapLastPos.z)
    };
    st.snapLastPos = wtc->position;

    constexpr float kThreshold = 0.0005f; // unter diesem Wert: keine Bewegung erkannt
    const float maxDelta = std::max({delta.x, delta.y, delta.z});
    if (maxDelta < kThreshold)
        return; // Achse beibehalten wenn keine Bewegung

    if (delta.x >= delta.y && delta.x >= delta.z)      st.snapAxis = 0;
    else if (delta.y >= delta.x && delta.y >= delta.z) st.snapAxis = 1;
    else                                                st.snapAxis = 2;
}

// Rastet das selektierte Entity an der nächsten Oberfläche entlang der erkannten Achse ein.
// snapAxis == -1: alle 6 Richtungen; 0/1/2: nur ±X/±Y/±Z.
static void SnapEntityToSurface(EditorFrameContext& ctx)
{
    const EntityID selected = ctx.state.selectedEntity;
    if (!selected.IsValid()) return;

    const auto* bounds = ctx.world.Get<BoundsComponent>(selected);
    const auto* wtc    = ctx.world.Get<WorldTransformComponent>(selected);
    auto*       tc     = ctx.world.Get<TransformComponent>(selected);
    if (!bounds || !wtc || !tc) return;

    const math::Vec3 cx = bounds->centerWorld;
    const math::Vec3 ex = bounds->extentsWorld;

    struct SnapCandidate { math::Vec3 origin; math::Vec3 dir; };
    const SnapCandidate kAll[6] = {
        { {cx.x - ex.x, cx.y, cx.z}, {-1.f,  0.f,  0.f} },
        { {cx.x + ex.x, cx.y, cx.z}, { 1.f,  0.f,  0.f} },
        { {cx.x, cx.y - ex.y, cx.z}, { 0.f, -1.f,  0.f} },
        { {cx.x, cx.y + ex.y, cx.z}, { 0.f,  1.f,  0.f} },
        { {cx.x, cx.y, cx.z - ex.z}, { 0.f,  0.f, -1.f} },
        { {cx.x, cx.y, cx.z + ex.z}, { 0.f,  0.f,  1.f} },
    };

    // Kandidaten nach erkannter Achse filtern
    const int axis = ctx.state.snapAxis;
    int first = 0, last = 6;
    if (axis == 0) { first = 0; last = 2; } // ±X
    if (axis == 1) { first = 2; last = 4; } // ±Y
    if (axis == 2) { first = 4; last = 6; } // ±Z

    constexpr float kMaxDist = 50.f;
    float      bestT   = kMaxDist;
    math::Vec3 bestDir = {0.f, -1.f, 0.f};
    bool       found   = false;

    for (int ci = first; ci < last; ++ci)
    {
        const auto& cand = kAll[ci];
        ctx.world.ForEachAlive([&](EntityID e)
        {
            if (e == selected) return;
            const auto* ob = ctx.world.Get<BoundsComponent>(e);
            if (!ob) return;
            const float t = RayVsWorldAABB(cand.origin, cand.dir, *ob);
            if (t > 0.f && t < bestT)
            {
                bestT   = t;
                bestDir = cand.dir;
                found   = true;
            }
        });
    }

    if (!found) return;

    const math::Vec3 newWorldPos = wtc->position + bestDir * bestT;

    // Viewport-Selektion: Kinder vor dem Snap snapshotten, danach fixieren
    if (ctx.state.selectionSource == SelectionSource::Viewport)
        SnapshotDirectChildren(ctx, selected);

    SetWorldPosition(ctx, selected, *tc, newWorldPos);

    if (ctx.state.selectionSource == SelectionSource::Viewport &&
        !ctx.state.gizmoDragChildren.empty())
        CompensateChildrenAfterTranslate(ctx, newWorldPos);
}

static void DrawOBBSection(EditorFrameContext& ctx, EntityID entity)
{
    // Sektion nur anzeigen wenn eine OBB-Komponente vorhanden ist.
    // Hinzufügen läuft über Rechtsklick im Inspector → "+ Komponente".
    auto* obb = ctx.world.Get<OBBComponent>(entity);
    if (!obb)
        return;

    const bool obbOpen = ImGui::CollapsingHeader("Kollision##obb_hdr");
    if (ImGui::BeginPopupContextItem("##obb_hdr_ctx"))
    {
        if (ImGui::MenuItem("Kollision entfernen"))
        {
            ctx.world.Remove<OBBComponent>(entity);
            ctx.state.obbDragFaceIdx = -1;
        }
        ImGui::EndPopup();
    }
    if (!obbOpen)
        return;

    ImGui::Checkbox("Sichtbar (OBB anzeigen)##obb_vis", &obb->showInEditor);
    ImGui::Separator();

    // Vollgroesse anzeigen, intern als halfExtents speichern
    math::Vec3 size = obb->halfExtents * 2.f;
    ImGui::TextDisabled("Groesse  X / Y / Z");
    if (ImGui::DragFloat3("##obb_size", &size.x, 0.01f, 0.001f, 10000.f, "%.3f"))
    {
        obb->halfExtents.x = std::max(0.001f, size.x * 0.5f);
        obb->halfExtents.y = std::max(0.001f, size.y * 0.5f);
        obb->halfExtents.z = std::max(0.001f, size.z * 0.5f);
    }

    ImGui::TextDisabled("Mittelpunkt-Offset");
    ImGui::DragFloat3("##obb_center", &obb->centerOffset.x, 0.01f, -10000.f, 10000.f, "%.3f");

    ImGui::Separator();
    if (ImGui::Button("Aus Mesh berechnen##obb_auto"))
    {
        if (const auto* b = ctx.world.Get<BoundsComponent>(entity))
        {
            obb->centerOffset = b->centerLocal;
            obb->halfExtents  = b->extentsLocal;
            obb->orientation  = math::Quat::Identity();
        }
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Berechnet OBB aus der Mesh-AABB.\nKollisionsdaten bleiben erhalten.");
}

// ── Phase 1: DebugDraw-Rendering ─────────────────────────────────────────────
// Wird aus DrawLightDebugLines (Pre-Render-Phase, vor ImGui::NewFrame) aufgerufen.
// Liest obbHoveredFace aus dem letzten ImGui-Frame für Hover-Highlighting.
static void DrawOBBDebugLines(EditorFrameContext& ctx)
{
    if (!ctx.debugDraw) return;
    const EntityID selected = ctx.state.selectedEntity;
    if (!selected.IsValid()) return;
    const auto* obb = ctx.world.Get<OBBComponent>(selected);
    if (!obb || !obb->showInEditor) return;
    const auto* wtc = ctx.world.Get<WorldTransformComponent>(selected);
    if (!wtc) return;

    using namespace engine::addons::debug_draw;
    DebugDrawRenderer& dd = *ctx.debugDraw;

    dd.Box(ComputeOBBCorners(wtc->matrix, *obb), {0.f, 1.f, 0.2f, 1.f});

    const auto faces = ComputeOBBFaces(wtc->matrix, *obb);

    // Handle-Kreise: screen-space konstant (~2% Kamera-Distanz), in der Flächen-Ebene.
    constexpr float kScreenFraction = 0.02f;

    for (int i = 0; i < 6; ++i)
    {
        const auto& f = faces[i];

        const float camDist = (f.worldCenter - ctx.state.editorCamera.position).Length();
        const float radius  = std::max(0.03f, camDist * kScreenFraction);

        const bool       hovered = (ctx.state.obbHoveredFace == i);
        const float      drawR   = hovered ? radius * 1.6f : radius;
        const math::Vec4 col     = hovered
            ? math::Vec4{1.f, 0.4f, 0.f, 1.f}
            : math::Vec4{1.f, 1.f, 0.f, 1.f};

        dd.Circle(f.worldCenter, f.worldNormal, drawR, 16, col);
    }
}

// ── Phase 2: ImGui-Input-Handling ────────────────────────────────────────────
// Wird aus DrawEditorPanels aufgerufen (nach ImGui::NewFrame, vor SelectEntityUnderMouse).
// Schreibt obbHoveredFace / obbDragFaceIdx in EditorState.
static void UpdateOBBHandles(EditorFrameContext& ctx)
{
    EditorState& st = ctx.state;

    // Drag beenden (immer zuerst prüfen)
    if (st.obbDragFaceIdx >= 0 && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
    {
        st.obbDragFaceIdx = -1;
        st.obbHoveredFace = -1;
    }

    // Aktiver Gizmo-Drag (Position oder Rotation) oder Snap-Lock → OBB-Handles sperren
    if (st.gizmoDragAxis >= 0 || st.rotGizmoDragAxis >= 0 || st.sclGizmoDragAxis >= 0 || st.snapMoveLocked)
    {
        st.obbHoveredFace = -1;
        return;
    }

    const EntityID selected = st.selectedEntity;
    if (!selected.IsValid()) { st.obbHoveredFace = -1; return; }
    auto* obb = ctx.world.Get<OBBComponent>(selected);
    if (!obb || !obb->showInEditor) { st.obbHoveredFace = -1; st.obbDragFaceIdx = -1; return; }
    const auto* wtc = ctx.world.Get<WorldTransformComponent>(selected);
    if (!wtc) { st.obbHoveredFace = -1; return; }

    ImGuiIO& io = ImGui::GetIO();
    if (io.WantCaptureMouse) { st.obbHoveredFace = -1; return; }

    const auto faces = ComputeOBBFaces(wtc->matrix, *obb);

    const math::Mat4 view = BuildEditorViewMatrix(st.editorCamera);
    const float aspect    = (io.DisplaySize.y > 0.f)
                            ? io.DisplaySize.x / io.DisplaySize.y : 1.f;
    const math::Mat4 vp   = math::Mat4::PerspectiveFovRH(
        st.editorCamera.fovDeg * math::DEG_TO_RAD, aspect,
        st.editorCamera.nearPlane, st.editorCamera.farPlane) * view;

    // ── Hover-Erkennung (nur wenn kein Drag läuft) ──
    st.obbHoveredFace = -1;
    if (st.obbDragFaceIdx < 0)
    {
        constexpr float kPickRadiusSq = 10.f * 10.f;
        for (int i = 0; i < 6; ++i)
        {
            ImVec2 sp;
            if (!WorldToScreenPx(faces[i].worldCenter, vp, io.DisplaySize, sp))
                continue;
            const float dx = io.MousePos.x - sp.x;
            const float dy = io.MousePos.y - sp.y;
            if (dx * dx + dy * dy < kPickRadiusSq)
            {
                st.obbHoveredFace = i;
                break;
            }
        }
    }

    // ── Drag starten ──
    if (st.obbHoveredFace >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
    {
        const OBBFace& f  = faces[st.obbHoveredFace];
        st.obbDragFaceIdx     = st.obbHoveredFace;
        st.obbDragDepth       = (f.worldCenter - st.editorCamera.position).Length();
        st.obbDragStartMouse  = { io.MousePos.x, io.MousePos.y };
        st.obbDragStartExtent = (&obb->halfExtents.x)[f.axis];
        st.obbDragStartCenter = (&obb->centerOffset.x)[f.axis];
    }

    // ── Drag fortführen ──
    if (st.obbDragFaceIdx >= 0 && ImGui::IsMouseDown(ImGuiMouseButton_Left))
    {
        const OBBFace& f = faces[st.obbDragFaceIdx];

        const float tanHalfFov    = std::tan(30.f * math::DEG_TO_RAD);
        const float unitsPerPixel = 2.f * st.obbDragDepth * tanHalfFov /
                                    std::max(1.f, io.DisplaySize.y);

        // Face-Normale in Kamera-Raum → Bildschirm-Richtung
        const math::Vec3 camNormal = view.TransformDirection(f.worldNormal);
        const float snx =  camNormal.x;
        const float sny = -camNormal.y; // Kamera-Y nach oben, Screen-Y nach unten
        const float len = std::sqrt(snx * snx + sny * sny);

        if (len > 1e-4f)
        {
            const float nx = snx / len;
            const float ny = sny / len;
            const float dx = io.MousePos.x - st.obbDragStartMouse.x;
            const float dy = io.MousePos.y - st.obbDragStartMouse.y;
            const float worldDelta = (nx * dx + ny * dy) * unitsPerPixel;

            float& he = (&obb->halfExtents.x)[f.axis];
            float& co = (&obb->centerOffset.x)[f.axis];

            const bool ctrl = ImGui::IsKeyDown(ImGuiKey_LeftCtrl) ||
                              ImGui::IsKeyDown(ImGuiKey_RightCtrl);
            if (ctrl)
            {
                // Symmetrisch: beide Flächen wachsen, Zentrum bleibt
                he = std::max(0.001f, st.obbDragStartExtent + worldDelta);
                co = st.obbDragStartCenter;
            }
            else
            {
                // Einseitig: nur die gezogene Fläche bewegt sich
                he = std::max(0.001f, st.obbDragStartExtent + worldDelta * 0.5f);
                co = st.obbDragStartCenter + f.sign * worldDelta * 0.5f;
            }
        }
    }
}

struct InspectorDragResult { bool changed; bool labelClicked; };
static InspectorDragResult InspectorDragFloat3(const char* label, const char* id,
                                      float* v, float speed,
                                      const ImVec4* labelColor = nullptr,
                                      float vmin = 0.f, float vmax = 0.f)
{
    constexpr float kLabelWidth = 72.f;
    if (labelColor) ImGui::PushStyleColor(ImGuiCol_Text, *labelColor);
    ImGui::Text("%s", label);
    if (labelColor) ImGui::PopStyleColor();
    const bool labelClicked = ImGui::IsItemClicked();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Klicken um Gizmo-Modus zu wechseln");
    ImGui::SameLine(kLabelWidth);
    ImGui::SetNextItemWidth(-1.f);
    const ImGuiSliderFlags clampFlag = (vmin < vmax) ? ImGuiSliderFlags_AlwaysClamp : ImGuiSliderFlags_None;
    return InspectorDragResult{ ImGui::DragFloat3(id, v, speed, vmin, vmax, "%.3f", clampFlag), labelClicked };
}

static bool MultiSelectionContains(EditorState& state, EntityID entity)
{
    return std::find(state.multiSelection.begin(), state.multiSelection.end(), entity) !=
           state.multiSelection.end();
}

static bool HasSelectedAncestor(EditorFrameContext& ctx, EntityID entity)
{
    const auto* parent = ctx.world.Get<ParentComponent>(entity);
    uint32_t depth = 0u;
    while (parent && parent->parent.IsValid() && ctx.world.IsAlive(parent->parent) && depth++ < 1024u)
    {
        if (MultiSelectionContains(ctx.state, parent->parent))
            return true;
        parent = ctx.world.Get<ParentComponent>(parent->parent);
    }
    return false;
}

static bool IsMultiTransformTarget(EditorFrameContext& ctx, EntityID entity)
{
    return entity.IsValid() && ctx.world.IsAlive(entity) && !HasSelectedAncestor(ctx, entity);
}

static void CaptureMultiSelectionScaleStart(EditorFrameContext& ctx)
{
    EditorState& st = ctx.state;
    st.multiSelectionDragStartPositions.clear();
    st.multiSelectionDragStartScales.clear();
    st.multiSelectionDragStartPositions.reserve(st.multiSelection.size());
    st.multiSelectionDragStartScales.reserve(st.multiSelection.size());
    for (EntityID e : st.multiSelection)
    {
        st.multiSelectionDragStartPositions.push_back(WorldPosition(ctx, e));
        st.multiSelectionDragStartScales.push_back(WorldScale(ctx, e));
    }
}

static void ApplyEnvironmentFromState(EditorFrameContext& ctx)
{
    if (ctx.state.environmentTexturePath.empty() || !ctx.setEditorEnvironment)
        return;

    if (ctx.setEditorEnvironment(ctx.state.environmentTexturePath,
                                 ctx.state.environmentIntensity,
                                 ctx.state.environmentEnableIBL))
    {
        ctx.lastFileMessage = "Environment aktualisiert.";
    }
    else
    {
        ctx.lastFileMessage = "Environment konnte nicht geladen werden.";
    }
}

static bool IsHdrEnvironmentTexturePath(const std::filesystem::path& path)
{
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return ext == ".hdr";
}

static void DrawSkyboxDropZone(EditorFrameContext& ctx)
{
    const std::string preview = ctx.state.environmentTexturePath.empty()
        ? std::string("<keine HDR-Textur>")
        : ctx.state.environmentTexturePath;

    char pathBuffer[512]{};
    std::snprintf(pathBuffer, sizeof(pathBuffer), "%s", preview.c_str());
    ImGui::SetNextItemWidth(-1.f);
    ImGui::InputText("##EnvironmentTexturePath", pathBuffer, sizeof(pathBuffer),
                     ImGuiInputTextFlags_ReadOnly);

    const ImVec2 dropMin = ImGui::GetCursorScreenPos();
    const float dropWidth = std::max(1.f, ImGui::GetContentRegionAvail().x);
    const ImVec2 dropSize(dropWidth, 44.f);
    ImGui::InvisibleButton("##EnvironmentHdrDropZone", dropSize);
    const ImVec2 dropMax(dropMin.x + dropSize.x, dropMin.y + dropSize.y);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    dl->AddRectFilled(dropMin, dropMax, hovered ? IM_COL32(70,92,112,255) : IM_COL32(48,56,64,255), 4.f);
    dl->AddRect      (dropMin, dropMax, IM_COL32(130,154,176,255), 4.f, 0, 1.5f);
    const char* dropText = "HDR-Textur hier ablegen";
    const ImVec2 ts = ImGui::CalcTextSize(dropText);
    dl->AddText(ImVec2(dropMin.x + (dropSize.x - ts.x) * 0.5f,
                       dropMin.y + (dropSize.y - ts.y) * 0.5f),
                ImGui::GetColorU32(ImGuiCol_Text), dropText);

    if (ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("KROM_ASSET_TEXTURE"))
        {
            const char* texPath = static_cast<const char*>(p->Data);
            if (texPath && texPath[0] != '\0')
            {
                const std::filesystem::path dropped = std::filesystem::path(texPath).lexically_normal();
                if (IsHdrEnvironmentTexturePath(dropped))
                {
                    ctx.state.environmentTexturePath = dropped.string();
                    ApplyEnvironmentFromState(ctx);
                }
            }
        }
        ImGui::EndDragDropTarget();
    }

    if (!ctx.state.environmentTexturePath.empty())
    {
        if (ImGui::SmallButton("Entfernen##skybox"))
        {
            ctx.state.environmentTexturePath.clear();
            if (ctx.clearEditorEnvironment)
                ctx.clearEditorEnvironment();
        }
    }
}

static void DrawEnvironmentInspector(EditorFrameContext& ctx)
{
    using AmbientSource = EditorState::AmbientSource;

    // ── Environment ───────────────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("Environment", ImGuiTreeNodeFlags_DefaultOpen))
    {
        DrawSkyboxDropZone(ctx);
    }

    // ── Environment Lighting ──────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("Environment Lighting", ImGuiTreeNodeFlags_DefaultOpen))
    {
        int src = static_cast<int>(ctx.state.ambientSource);
        if (ImGui::Combo("Source", &src, "Skybox\0Gradient\0Color\0"))
        {
            ctx.state.ambientSource        = static_cast<AmbientSource>(src);
            ctx.state.environmentEnableIBL = (ctx.state.ambientSource == AmbientSource::Skybox);
            ApplyEnvironmentFromState(ctx);
        }

        // IBL Intensity – nur wenn Skybox-Quelle aktiv; Rebake nur beim Loslassen der Maus
        if (ctx.state.ambientSource == AmbientSource::Skybox)
        {
            float envIntensity = ctx.state.environmentIntensity;
            ImGui::TextUnformatted("IBL Intensity");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1.f);
            ImGui::DragFloat("##iblIntensity", &envIntensity, 0.01f, 0.0f, 4.0f, "%.2f",
                             ImGuiSliderFlags_AlwaysClamp);
            ctx.state.environmentIntensity = envIntensity;   // immer State aktualisieren
            if (ImGui::IsItemDeactivatedAfterEdit())          // Rebake nur beim Loslassen
                ApplyEnvironmentFromState(ctx);
        }

        bool ambientChanged = false;

        // Farbfelder + Fallback-Intensität – nur relevant wenn IBL NICHT aktiv.
        // Bei Skybox (IBL aktiv): IBL ist das Ambient-Licht → ambientIntensity hat keine Wirkung.
        // Die IBL-Helligkeit wird ausschliesslich durch environmentIntensity (IBL Intensity, oben) gesteuert.
        if (ctx.state.ambientSource == AmbientSource::Gradient)
        {
            ImGui::TextUnformatted("Sky");
            ImGui::SameLine();
            ambientChanged |= ImGui::ColorEdit3("##ambSky", &ctx.state.ambientColor.x,
                                                ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel);
            ImGui::SameLine();
            ImGui::TextUnformatted("Ground");
            ImGui::SameLine();
            ambientChanged |= ImGui::ColorEdit3("##ambGnd", &ctx.state.ambientColorGround.x,
                                                ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel);

            float ambientIntensity = ctx.state.ambientIntensity;
            ImGui::TextUnformatted("Helligkeit");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1.f);
            if (ImGui::DragFloat("##ambientIntensity", &ambientIntensity, 0.02f, 0.0f, 10.0f, "%.2f",
                                 ImGuiSliderFlags_AlwaysClamp))
            {
                ctx.state.ambientIntensity = ambientIntensity;
                ambientChanged = true;
            }
        }
        else if (ctx.state.ambientSource == AmbientSource::Color)
        {
            ImGui::TextUnformatted("Farbe");
            ImGui::SameLine();
            ambientChanged |= ImGui::ColorEdit3("##ambColor", &ctx.state.ambientColor.x,
                                                ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel);

            float ambientIntensity = ctx.state.ambientIntensity;
            ImGui::TextUnformatted("Helligkeit");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1.f);
            if (ImGui::DragFloat("##ambientIntensity", &ambientIntensity, 0.02f, 0.0f, 10.0f, "%.2f",
                                 ImGuiSliderFlags_AlwaysClamp))
            {
                ctx.state.ambientIntensity = ambientIntensity;
                ambientChanged = true;
            }
        }
        // AmbientSource::Skybox → IBL aktiv: kein Fallback-Slider.
        // Helligkeit wird ausschliesslich durch IBL Intensity (oben) gesteuert.

        if (ambientChanged && ctx.setEditorAmbientLight)
        {
            // Gradient: Durchschnitt aus Sky und Ground als Näherung
            const math::Vec3 effectiveColor =
                (ctx.state.ambientSource == AmbientSource::Gradient)
                ? math::Vec3{
                    (ctx.state.ambientColor.x + ctx.state.ambientColorGround.x) * 0.5f,
                    (ctx.state.ambientColor.y + ctx.state.ambientColorGround.y) * 0.5f,
                    (ctx.state.ambientColor.z + ctx.state.ambientColorGround.z) * 0.5f }
                : ctx.state.ambientColor;
            ctx.setEditorAmbientLight(effectiveColor, ctx.state.ambientIntensity);
        }
    }

    if (!ctx.lastFileMessage.empty())
    {
        ImGui::Separator();
        ImGui::TextWrapped("%s", ctx.lastFileMessage.c_str());
    }
}

void DrawInspector(EditorFrameContext& ctx)
{
    ImGui::SetNextWindowPos(ImVec2(10.f, 400.f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(260.f, 490.f), ImGuiCond_FirstUseEver);
    ImGui::Begin("Inspector##editor");

    bool drawEntityTab = true;
    bool drawEnvironmentTab = false;
    if (ImGui::BeginTabBar("##InspectorTabs"))
    {
        drawEntityTab = false;
        if (ImGui::BeginTabItem("Entity"))
        {
            drawEntityTab = true;
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Szene"))
        {
            drawEnvironmentTab = true;
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    if (drawEnvironmentTab)
    {
        DrawEnvironmentInspector(ctx);
        ImGui::End();
        return;
    }

    if (!drawEntityTab)
    {
        ImGui::End();
        return;
    }

    if (!ctx.state.selectedEntity.IsValid())
    {
        if (!ctx.state.selectedPrefabAssetPath.empty())
        {
            const std::filesystem::path prefabPath = ResolveSelectedPrefabPath(ctx);
            engine::addons::prefab::PrefabAsset prefab;
            std::string error;
            const bool loaded =
                engine::addons::prefab::LoadPrefabFromFile(prefabPath, ctx.registry, prefab, &error);

            ImGui::TextDisabled("Prefab");
            ImGui::TextWrapped("%s", ctx.state.selectedPrefabAssetPath.c_str());
            ImGui::Separator();

            if (loaded)
            {
                ImGui::Text("Name: %s", prefab.name.c_str());
                ImGui::Text("Entities: %zu", prefab.records.size());
                ImGui::Text("Root Index: %u", prefab.rootIndex);
                ImGui::Separator();
                if (ImGui::Button("In Szene laden", ImVec2(-1.f, 0.f)))
                    SpawnPrefabAsset(ctx, prefabPath);
            }
            else
            {
                ImGui::TextWrapped("%s", error.empty() ? "Prefab konnte nicht gelesen werden." : error.c_str());
            }

            ImGui::End();
            return;
        }

        ImGui::TextDisabled("Keine Entity ausgewaehlt");
        ImGui::End();
        return;
    }

    const EntityID selected = ctx.state.selectedEntity;

    // Kamera-Gizmo-Entities können nicht ausgewählt werden (Entity-Liste filtert sie aus),
    // aber sicherheitshalber auch hier abfangen.
    if (ctx.world.Get<EditorCameraGizmoComponent>(selected) != nullptr)
    {
        ImGui::TextDisabled("(Editor-internes Kamera-Gizmo)");
        ImGui::End();
        return;
    }
    if (ctx.world.Get<EditorRuntimeGizmoTag>(selected) != nullptr)
    {
        ImGui::TextDisabled("(Editor-internes Gizmo)");
        ImGui::End();
        return;
    }
    if (ctx.world.Get<EditorMaterialPreviewComponent>(selected) != nullptr)
    {
        ImGui::TextDisabled("(Editor-interne Materialvorschau)");
        ImGui::End();
        return;
    }
    if (ctx.world.Get<EditorAssetThumbnailComponent>(selected) != nullptr)
    {
        ImGui::TextDisabled("(Editor-internes Asset-Thumbnail)");
        ImGui::End();
        return;
    }

    if (ImGui::Button("Als Prefab speichern", ImVec2(-1.f, 0.f)))
        SaveEntityAsPrefab(ctx, selected);
    ImGui::Separator();

    // Multi-Selektion: Inspector zeigt Gruppeninfo + editierbare Transform
    if (ctx.state.multiSelection.size() >= 2)
    {
        ImGui::TextDisabled("Gruppe (%zu Objekte)", ctx.state.multiSelection.size());
        ImGui::Separator();

        // ── Position (Pivot verschieben → alle Entities um Delta bewegen) ────
        const bool mPosMode = ctx.state.gizmoMode == GizmoMode::Position;
        const bool mRotMode = ctx.state.gizmoMode == GizmoMode::Rotation;
        const bool mSclMode = ctx.state.gizmoMode == GizmoMode::Scale;
        const ImVec4 kPosActive{0.4f, 1.0f, 0.4f, 1.f};
        const ImVec4 kRotActive{0.4f, 0.6f, 1.0f, 1.f};
        const ImVec4 kSclActive{1.0f, 0.8f, 0.2f, 1.f};

        math::Vec3 pivot = ctx.state.multiSelectionPivot;
        auto mPosResult = InspectorDragFloat3("Position", "##mpos", &pivot.x, 0.1f,
                                              mPosMode ? &kPosActive : nullptr);
        if (mPosResult.labelClicked) ctx.state.gizmoMode = GizmoMode::Position;
        if (mPosResult.changed)
        {
            BeginPendingSceneEdit(ctx, "multisel.pivot.pos", "Gruppe bewegt");
            const math::Vec3 delta = pivot - ctx.state.multiSelectionPivot;
            for (EntityID e : ctx.state.multiSelection)
            {
                if (!IsMultiTransformTarget(ctx, e)) continue;
                auto* tc = ctx.world.Get<TransformComponent>(e);
                if (!tc) continue;
                SetWorldPosition(ctx, e, *tc, WorldPosition(ctx, e) + delta);
            }
            ctx.state.multiSelectionPivot = pivot;
        }
        if (ImGui::IsItemDeactivatedAfterEdit())
            CommitPendingSceneEdit(ctx, "multisel.pivot.pos");

        // ── Rotation (Delta um Pivot) — zeigt immer 0/0/0 (relativer Delta) ──
        math::Vec3 eulerDelta{0.f, 0.f, 0.f};
        auto mRotResult = InspectorDragFloat3("Rotation", "##mrot", &eulerDelta.x, 1.0f,
                                              mRotMode ? &kRotActive : nullptr);
        if (mRotResult.labelClicked) ctx.state.gizmoMode = GizmoMode::Rotation;
        if (mRotResult.changed)
        {
            BeginPendingSceneEdit(ctx, "multisel.pivot.rot", "Gruppe rotiert");
            const math::Quat deltaRot = math::Quat::FromEulerDeg(
                eulerDelta.x, eulerDelta.y, eulerDelta.z);
            for (EntityID e : ctx.state.multiSelection)
            {
                if (!IsMultiTransformTarget(ctx, e)) continue;
                auto* tc = ctx.world.Get<TransformComponent>(e);
                if (!tc) continue;
                const math::Vec3 offset   = WorldPosition(ctx, e) - ctx.state.multiSelectionPivot;
                const math::Vec3 newPos   = ctx.state.multiSelectionPivot + deltaRot.Rotate(offset);
                const math::Quat newRot   = (deltaRot * WorldRotation(ctx, e)).Normalized();
                const math::Mat4 world    = math::Mat4::TRS(newPos, newRot, WorldScale(ctx, e));
                SetWorldTransform(ctx, e, *tc, world);
            }
        }
        if (ImGui::IsItemDeactivatedAfterEdit())
            CommitPendingSceneEdit(ctx, "multisel.pivot.rot");

        // ── Scale — zeigt den Wert der ersten Entity; Änderungen werden als
        //           absoluter Delta auf alle Objekte angewendet. So verhält
        //           sich die Eingabe konsistent mit der Einzelauswahl:
        //           "5 eingeben" setzt alle auf 5, Drag-Bewegungen sind relativ.
        {
            // Anzeigewert: Scale der ersten Entity in der Auswahl
            math::Vec3 displayScale{1.f, 1.f, 1.f};
            const EntityID first = ctx.state.multiSelection.empty()
                ? NULL_ENTITY : ctx.state.multiSelection[0];
            if (first.IsValid() && ctx.world.IsAlive(first))
                displayScale = WorldScale(ctx, first);

            const math::Vec3 displayScaleBefore = displayScale;
            auto mSclResult = InspectorDragFloat3("Scale", "##mscl", &displayScale.x, 0.01f,
                                                  mSclMode ? &kSclActive : nullptr, 0.001f, 1000.f);
            if (mSclResult.labelClicked) ctx.state.gizmoMode = GizmoMode::Scale;
            if (mSclResult.changed)
            {
                BeginPendingSceneEdit(ctx, "multisel.pivot.scl", "Gruppe skaliert");

                // Delta = Differenz zwischen neuem und altem Anzeigewert
                const math::Vec3 delta{
                    displayScale.x - displayScaleBefore.x,
                    displayScale.y - displayScaleBefore.y,
                    displayScale.z - displayScaleBefore.z };

                for (EntityID e : ctx.state.multiSelection)
                {
                    if (!IsMultiTransformTarget(ctx, e)) continue;
                    if (!e.IsValid() || !ctx.world.IsAlive(e)) continue;
                    auto* tc = ctx.world.Get<TransformComponent>(e);
                    if (!tc) continue;
                    const math::Vec3 oldScale = WorldScale(ctx, e);
                    const math::Vec3 newScale{
                        std::max(0.0001f, oldScale.x + delta.x),
                        std::max(0.0001f, oldScale.y + delta.y),
                        std::max(0.0001f, oldScale.z + delta.z) };
                    const math::Mat4 world = math::Mat4::TRS(
                        WorldPosition(ctx, e), WorldRotation(ctx, e), newScale);
                    SetWorldTransform(ctx, e, *tc, world);
                }
            }
            if (ImGui::IsItemDeactivatedAfterEdit())
                CommitPendingSceneEdit(ctx, "multisel.pivot.scl");
        }

        ImGui::Separator();

        // ── Active / Visible / Persistent ─────────────────────────────────────
        // Zeigt den Wert der ersten Entity; Änderung wird auf alle angewendet.
        {
            const EntityID first = ctx.state.multiSelection.empty()
                ? NULL_ENTITY : ctx.state.multiSelection[0];

            // Active
            bool activeVal = true;
            if (first.IsValid() && ctx.world.IsAlive(first))
                if (const auto* ac = ctx.world.Get<ActiveComponent>(first))
                    activeVal = ac->active;
            if (ImGui::Checkbox("Active", &activeVal))
            {
                BeginPendingSceneEdit(ctx, "multisel.active", "Active geaendert");
                for (EntityID e : ctx.state.multiSelection)
                {
                    if (!e.IsValid() || !ctx.world.IsAlive(e)) continue;
                    if (auto* ac = ctx.world.Get<ActiveComponent>(e))
                        ac->active = activeVal;
                }
                CommitPendingSceneEdit(ctx, "multisel.active");
            }

            ImGui::SameLine();

            // Visible (nur wenn erste Entity ein Mesh hat)
            const bool firstHasMesh = first.IsValid() && ctx.world.IsAlive(first)
                                   && ctx.world.Get<MeshComponent>(first) != nullptr;
            bool visibleVal = true;
            if (first.IsValid() && ctx.world.IsAlive(first))
                if (const auto* mc = ctx.world.Get<MeshComponent>(first))
                    visibleVal = mc->visible;
            if (!firstHasMesh) ImGui::BeginDisabled();
            if (ImGui::Checkbox("Visible", &visibleVal))
            {
                BeginPendingSceneEdit(ctx, "multisel.visible", "Visible geaendert");
                for (EntityID e : ctx.state.multiSelection)
                {
                    if (!e.IsValid() || !ctx.world.IsAlive(e)) continue;
                    if (auto* mc = ctx.world.Get<MeshComponent>(e))
                        mc->visible = visibleVal;
                }
                CommitPendingSceneEdit(ctx, "multisel.visible");
            }
            if (!firstHasMesh) ImGui::EndDisabled();

            // Persistent
            bool persistVal = false;
            if (first.IsValid() && ctx.world.IsAlive(first))
                persistVal = ctx.world.Has<EditorPersistentComponent>(first);
            if (ImGui::Checkbox("Persistent", &persistVal))
            {
                BeginPendingSceneEdit(ctx, "multisel.persistent", "Persistent geaendert");
                for (EntityID e : ctx.state.multiSelection)
                {
                    if (!e.IsValid() || !ctx.world.IsAlive(e)) continue;
                    if (persistVal && !ctx.world.Has<EditorPersistentComponent>(e))
                        ctx.world.Add<EditorPersistentComponent>(e);
                    else if (!persistVal && ctx.world.Has<EditorPersistentComponent>(e))
                        ctx.world.Remove<EditorPersistentComponent>(e);
                }
                CommitPendingSceneEdit(ctx, "multisel.persistent");
            }

            // Scale erben (nur relevant wenn mind. eine Entity einen Parent hat)
            bool anyHasParent = false;
            for (EntityID e : ctx.state.multiSelection)
                if (e.IsValid() && ctx.world.IsAlive(e) && ctx.world.Has<ParentComponent>(e))
                    { anyHasParent = true; break; }

            if (anyHasParent)
            {
                bool inheritVal = true;
                if (first.IsValid() && ctx.world.IsAlive(first))
                    if (const auto* tc = ctx.world.Get<TransformComponent>(first))
                        inheritVal = tc->inheritParentScale;
                if (ImGui::Checkbox("Scale erben", &inheritVal))
                {
                    BeginPendingSceneEdit(ctx, "multisel.inheritScale", "Scale-Vererbung geaendert");
                    for (EntityID e : ctx.state.multiSelection)
                    {
                        if (!e.IsValid() || !ctx.world.IsAlive(e)) continue;
                        if (!ctx.world.Has<ParentComponent>(e)) continue;
                        if (auto* tc = ctx.world.Get<TransformComponent>(e))
                        {
                            tc->inheritParentScale = inheritVal;
                            tc->dirty = true;
                        }
                    }
                    CommitPendingSceneEdit(ctx, "multisel.inheritScale");
                }
            }
        }

        ImGui::Separator();
        ImGui::TextDisabled("STRG+Klick zum Abwaehlen.\nKlick ohne STRG hebt Gruppenauswahl auf.");
        ImGui::End();
        return;
    }
    ImGui::PushID(static_cast<int>(selected.value));

    ImGui::TextDisabled("Name");
    const float deleteWidth = ImGui::CalcTextSize("Delete").x +
                              ImGui::GetStyle().FramePadding.x * 2.f;
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float nameWidth = std::clamp(
        ImGui::GetContentRegionAvail().x - deleteWidth - spacing,
        140.f,
        280.f);

    ImGui::SetNextItemWidth(nameWidth);
    if (auto* nc = ctx.world.Get<NameComponent>(selected))
    {
        char nameBuffer[128]{};
        std::snprintf(nameBuffer, sizeof(nameBuffer), "%s", nc->name.c_str());
        const std::string nameKey = MakeEditHistoryKey(selected, "name");
        if (ImGui::InputText("##EntityName", nameBuffer, sizeof(nameBuffer)))
        {
            BeginPendingSceneEdit(ctx, nameKey, "Name geaendert");
            nc->name = nameBuffer;
        }
        if (ImGui::IsItemDeactivatedAfterEdit())
            CommitPendingSceneEdit(ctx, nameKey);
    }
    else
    {
        char nameBuffer[128]{};
        std::snprintf(nameBuffer, sizeof(nameBuffer), "Entity %u", selected.Index());
        const std::string nameKey = MakeEditHistoryKey(selected, "name");
        if (ImGui::InputText("##EntityName", nameBuffer, sizeof(nameBuffer)))
        {
            BeginPendingSceneEdit(ctx, nameKey, "Name geaendert");
            ctx.world.Add<NameComponent>(selected, NameComponent{nameBuffer});
        }
        if (ImGui::IsItemDeactivatedAfterEdit())
            CommitPendingSceneEdit(ctx, nameKey);
    }
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.46f, 0.12f, 0.10f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.65f, 0.18f, 0.14f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.78f, 0.22f, 0.17f, 1.0f));
    const bool deleteClicked = ImGui::SmallButton("Delete");
    ImGui::PopStyleColor(3);
    ImGui::TextDisabled("Type: %s", PrimaryEntityType(ctx, selected));
    ImGui::TextDisabled("Components: %s", ComponentSummary(ctx, selected).c_str());

    // ── Tag | Layer  (eine Zeile, wie Unity) ─────────────────────────────────
    {
        auto* mesh = ctx.world.Get<MeshComponent>(selected);

        // Aktuellen Layer-Index bestimmen
        int layerIdx = 0;
        if (mesh)
        {
            mesh->layerMask = NormalizeSingleUserLayerMask(ctx, mesh->layerMask);
            layerIdx = LayerIndexFromSingleMask(mesh->layerMask);
        }

        const float totalW = ImGui::GetContentRegionAvail().x;
        const float tagLabelW   = ImGui::CalcTextSize("Tag").x   + ImGui::GetStyle().ItemSpacing.x;
        const float layerLabelW = ImGui::CalcTextSize("Layer").x + ImGui::GetStyle().ItemSpacing.x;
        const float halfW = (totalW - tagLabelW - layerLabelW) * 0.5f;

        // ── Tag: Dropdown mit verwalteter Liste ──────────────────────────────
        ImGui::Text("Tag");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(halfW);
        const auto* tc = ctx.world.Get<TagComponent>(selected);
        const std::string currentTag = tc ? tc->tag : "";
        const std::string tagKey = MakeEditHistoryKey(selected, "tag");

        if (ImGui::BeginCombo("##TagCombo", currentTag.empty() ? "Untagged" : currentTag.c_str()))
        {
            // Bestehende Tags — Klick = zuweisen, Rechtsklick = löschen
            std::string tagToDelete;
            for (const auto& t : ctx.state.knownTags)
            {
                const bool sel = (currentTag == t || (currentTag.empty() && t == "Untagged"));
                if (ImGui::Selectable(t.c_str(), sel))
                {
                    BeginPendingSceneEdit(ctx, tagKey, "Tag geaendert");
                    const std::string newTag = (t == "Untagged") ? "" : t;
                    if (!ctx.world.Has<TagComponent>(selected))
                        ctx.world.Add<TagComponent>(selected, TagComponent{newTag});
                    else
                        ctx.world.Get<TagComponent>(selected)->tag = newTag;
                    CommitPendingSceneEdit(ctx, tagKey);
                }
                if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
                    ImGui::OpenPopup(("##del_" + t).c_str());
                if (ImGui::BeginPopup(("##del_" + t).c_str()))
                {
                    if (t != "Untagged" && ImGui::MenuItem(("Loeschen: " + t).c_str()))
                        tagToDelete = t;
                    ImGui::EndPopup();
                }
                if (sel) ImGui::SetItemDefaultFocus();
            }
            if (!tagToDelete.empty())
            {
                auto& tags = ctx.state.knownTags;
                tags.erase(std::remove(tags.begin(), tags.end(), tagToDelete), tags.end());
            }

            // Neuen Tag hinzufügen
            ImGui::Separator();
            static char newTagBuf[64]{};
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 26.f);
            ImGui::InputTextWithHint("##NewTagInput", "Neuer Tag...", newTagBuf, sizeof(newTagBuf));
            ImGui::SameLine();
            if (ImGui::SmallButton("+") && newTagBuf[0] != '\0')
            {
                const std::string newT = newTagBuf;
                if (std::find(ctx.state.knownTags.begin(), ctx.state.knownTags.end(), newT)
                    == ctx.state.knownTags.end())
                    ctx.state.knownTags.push_back(newT);
                BeginPendingSceneEdit(ctx, tagKey, "Tag geaendert");
                if (!ctx.world.Has<TagComponent>(selected))
                    ctx.world.Add<TagComponent>(selected, TagComponent{newT});
                else
                    ctx.world.Get<TagComponent>(selected)->tag = newT;
                CommitPendingSceneEdit(ctx, tagKey);
                newTagBuf[0] = '\0';
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndCombo();
        }

        ImGui::SameLine();

        // ── Layer: Dropdown mit editierbaren Namen ───────────────────────────
        ImGui::Text("Layer");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        const std::string& layerPreview = ctx.state.layerNames[layerIdx];
        const std::string layerKey = MakeEditHistoryKey(selected, "layer");
        const bool noLayerSupport = (mesh == nullptr);
        if (noLayerSupport) ImGui::BeginDisabled();
        if (ImGui::BeginCombo("##LayerCombo", layerPreview.c_str()))
        {
            for (int b = 0; b < 32; ++b)
            {
                if (b == 1) continue; // EditorGizmo — niemals für User-Objekte
                const std::string& n = ctx.state.layerNames[b];
                if (n.empty() && b >= 8) continue;
                char label[48];
                std::snprintf(label, sizeof(label), "%d: %s", b, n.c_str());
                if (ImGui::Selectable(label, b == layerIdx))
                {
                    BeginPendingSceneEdit(ctx, layerKey, "Layer geaendert");
                    if (mesh) mesh->layerMask = (1u << b);
                    CommitPendingSceneEdit(ctx, layerKey);
                }
                if (b == layerIdx) ImGui::SetItemDefaultFocus();
            }
            ImGui::Separator();
            if (ImGui::Selectable("Edit Layers..."))
                ctx.state.layerNamesWindowOpen = true;
            ImGui::EndCombo();
        }
        if (noLayerSupport) ImGui::EndDisabled();
    }
    ImGui::Separator();

    if (deleteClicked)
    {
        DestroyEntityHierarchy(ctx, selected);
        ImGui::PopID();
        ImGui::End();
        return;
    }

    // ── Active / Visible ─────────────────────────────────────────────────────
    {
        const bool isCamera = ctx.world.Has<CameraComponent>(selected);

        if (!isCamera)
        {
            auto* activeComp = ctx.world.Get<ActiveComponent>(selected);
            bool isActive = (activeComp == nullptr) || activeComp->active;
            if (ImGui::Checkbox("Active", &isActive))
            {
                BeginPendingSceneEdit(ctx, MakeEditHistoryKey(selected, "active"), "Active geaendert");
                if (activeComp)
                    activeComp->active = isActive;
                else
                    ctx.world.Add<ActiveComponent>(selected, ActiveComponent{isActive});
                CommitPendingSceneEdit(ctx, MakeEditHistoryKey(selected, "active"));
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "Entity komplett deaktivieren — wird aus\n"
                    "Rendering UND Systemen entfernt.\n"
                    "Entspricht Unity's SetActive().");

            ImGui::SameLine();

            auto* meshComp = ctx.world.Get<MeshComponent>(selected);
            bool isVisible = (meshComp == nullptr) || meshComp->visible;
            const bool hasNoMesh = (meshComp == nullptr);
            if (hasNoMesh) ImGui::BeginDisabled();
            if (ImGui::Checkbox("Visible", &isVisible))
            {
                BeginPendingSceneEdit(ctx, MakeEditHistoryKey(selected, "visible"), "Visible geaendert");
                if (meshComp)
                    meshComp->visible = isVisible;
                CommitPendingSceneEdit(ctx, MakeEditHistoryKey(selected, "visible"));
            }
            if (hasNoMesh) ImGui::EndDisabled();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "Nur Rendering an/aus — Entity laeuft weiter\n"
                    "durch Systeme und Transform.\n"
                    "Nur verfuegbar wenn Entity ein Mesh hat.");
            ImGui::Separator();
        }
    }

    // ── Persistent-Flag ──────────────────────────────────────────────────────
    {
        bool isPersistent = ctx.world.Has<EditorPersistentComponent>(selected);
        if (ImGui::Checkbox("Persistent", &isPersistent))
        {
            if (isPersistent)
                ctx.world.Add<EditorPersistentComponent>(selected, EditorPersistentComponent{});
            else
                ctx.world.Remove<EditorPersistentComponent>(selected);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Entity ueberlebt LoadScene() / UnloadScene().\n"
                "Gilt nur fuer diese Entity — nicht fuer Kinder.");
        ImGui::Separator();
    }

    // Alle Sektionen deaktivieren wenn Entity inaktiv ist (Kameras ausgenommen)
    {
        const bool isCamera = ctx.world.Has<CameraComponent>(selected);
        const auto* activeComp = ctx.world.Get<ActiveComponent>(selected);
        const bool isInactive = !isCamera && activeComp && !activeComp->active;
        if (isInactive) ImGui::BeginDisabled();

        DrawTransformSection(ctx, selected);
        DrawCameraSection(ctx, selected);
        DrawLightSection(ctx, selected);
        DrawMeshSection(ctx, selected);
        DrawMaterialSlotsSection(ctx, selected);
        DrawOBBSection(ctx, selected);
        DrawMaterialSection(ctx, selected);
        DrawScriptSection(ctx, selected);

        if (isInactive) ImGui::EndDisabled();
    }

    // ── Drag-Drop-Zone für Material ───────────────────────────────────────────
    // Füllt den restlichen freien Raum oberhalb des "+ Komponente"-Buttons.
    {
        const float remainingH = ImGui::GetContentRegionAvail().y - 28.f;
        if (remainingH > 4.f)
        {
            ImGui::InvisibleButton("##InspectorFreeArea",
                ImVec2(std::max(1.f, ImGui::GetContentRegionAvail().x), remainingH));
            if (ImGui::BeginDragDropTarget())
            {
                if (const ImGuiPayload* payload =
                        ImGui::AcceptDragDropPayload("KROM_ASSET_MATERIAL"))
                {
                    const char* matPath = static_cast<const char*>(payload->Data);
                    if (matPath && matPath[0] != '\0')
                    {
                        ExecuteSceneMutation(ctx, "Material zugewiesen", [&]() {
                            SelectMaterialAsset(ctx, std::filesystem::path(matPath));
                            ApplyMaterialAssetToEntity(ctx, selected, std::filesystem::path(matPath));
                        });
                    }
                }
                ImGui::EndDragDropTarget();
            }
            // Rechtsklick auf freien Bereich → Popup öffnen
            if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
                ImGui::OpenPopup("AddComponentPopup##inspector");
        }
    }

    // "+" Button — am unteren Rand wenn Platz ist, aber niemals ueber Inhalt zeichnen.
    // Bei langen Inspector-Inhalten bleibt er im normalen Scroll-Flow unter den Controls.
    const float addButtonY = ImGui::GetWindowHeight() - 28.f;
    if (ImGui::GetCursorPosY() < addButtonY)
        ImGui::SetCursorPosY(addButtonY);
    ImGui::Separator();
    if (ImGui::Button("+ Komponente", ImVec2(-1.f, 0.f)))
        ImGui::OpenPopup("AddComponentPopup##inspector");

    // ── "Komponente hinzufügen"-Popup (Rechtsklick oder "+" Button) ───────────
    if (ImGui::BeginPopup("AddComponentPopup##inspector"))
    {
        ImGui::TextDisabled("Komponente hinzufuegen");
        ImGui::Separator();

        const bool hasMesh     = ctx.world.Has<MeshComponent>(selected);
        const bool hasOBB      = ctx.world.Has<OBBComponent>(selected);
        const bool hasMaterial = [&]() {
            auto* mc = ctx.world.Get<MaterialComponent>(selected);
            return mc && !mc->materialAssetPath.empty();
        }();

        // Kollision (OBB)
        const bool obbDisabled = !hasMesh || hasOBB;
        if (obbDisabled) ImGui::BeginDisabled();
        if (ImGui::MenuItem("Kollision (Box)"))
        {
            OBBComponent fresh;
            if (const auto* b = ctx.world.Get<BoundsComponent>(selected))
            {
                fresh.centerOffset = b->centerLocal;
                fresh.halfExtents  = b->extentsLocal;
            }
            ctx.world.Add<OBBComponent>(selected, fresh);
        }
        if (obbDisabled) ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        {
            if (!hasMesh)    ImGui::SetTooltip("Nur fuer Mesh-Entities verfuegbar.");
            else if (hasOBB) ImGui::SetTooltip("Kollision ist bereits vorhanden.");
            else             ImGui::SetTooltip("Erstellt eine Box-Kollision aus der Mesh-AABB.");
        }

        // Material
        if (hasMaterial) ImGui::BeginDisabled();
        if (ImGui::MenuItem("Material"))
            AssignDefaultMaterialToEntity(ctx, selected);
        if (hasMaterial) ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && hasMaterial)
            ImGui::SetTooltip("Material ist bereits zugewiesen.\nMaterial-Datei in den Inspector\noder auf die Entity ziehen um es zu wechseln.");

        ImGui::EndPopup();
    }

    ImGui::PopID();
    ImGui::End();
}

static void BuildBasis(const math::Vec3& n, math::Vec3& outRight, math::Vec3& outUp)
{
    const math::Vec3 ref = (std::abs(n.x) <= std::abs(n.y) && std::abs(n.x) <= std::abs(n.z))
        ? math::Vec3{1.f, 0.f, 0.f}
        : (std::abs(n.y) <= std::abs(n.z) ? math::Vec3{0.f, 1.f, 0.f} : math::Vec3{0.f, 0.f, 1.f});
    outRight = math::Vec3::Cross(n, ref).Normalized();
    outUp    = math::Vec3::Cross(n, outRight);
}

static void DrawLightGizmos(EditorFrameContext& ctx)
{
    if (!ctx.debugDraw)
        return;
    const EntityID selected = ctx.state.selectedEntity;
    if (!selected.IsValid())
        return;
    const auto* light = ctx.world.Get<LightComponent>(selected);
    if (!light)
        return;

    using namespace engine::addons::debug_draw;
    DebugDrawRenderer& dd = *ctx.debugDraw;

    const math::Vec3 pos     = WorldPosition(ctx, selected);
    const math::Quat rot     = WorldRotation(ctx, selected);
    const math::Vec4 col     = {light->color.x, light->color.y, light->color.z, 1.f};
    const math::Vec3 forward = rot.Rotate({0.f, 0.f, -1.f});

    switch (light->type)
    {
    case LightType::Spot:
    {
        const float outerR = light->range * std::tan(light->spotOuterDeg * math::DEG_TO_RAD);
        const float innerR = light->range * std::tan(light->spotInnerDeg * math::DEG_TO_RAD);
        const math::Vec3 tip = pos + forward * light->range;

        dd.Circle(tip, forward, outerR, 32, col);
        dd.Circle(tip, forward, innerR, 32, {col.x, col.y, col.z, 0.5f});

        math::Vec3 right, up;
        BuildBasis(forward, right, up);
        constexpr int kRays = 6;
        for (int i = 0; i < kRays; ++i)
        {
            const float a   = (static_cast<float>(i) / kRays) * math::TWO_PI;
            const math::Vec3 rim = tip + (right * std::cos(a) + up * std::sin(a)) * outerR;
            dd.Line(pos, rim, col);
        }
        break;
    }
    case LightType::Directional:
    {
        constexpr float kLen       = 2.f;
        constexpr float kHeadLen   = 0.35f;
        constexpr float kHeadWidth = 0.12f;
        constexpr float kSpacing   = 0.25f;

        const math::Vec3 end = pos + forward * kLen;
        dd.Line(pos, end, col);

        math::Vec3 right, up;
        BuildBasis(forward, right, up);

        const math::Vec3 headBase = end - forward * kHeadLen;
        dd.Line(end, headBase + right * kHeadWidth, col);
        dd.Line(end, headBase - right * kHeadWidth, col);
        dd.Line(end, headBase + up    * kHeadWidth, col);
        dd.Line(end, headBase - up    * kHeadWidth, col);

        dd.Line(pos + right * kSpacing, end + right * kSpacing, col);
        dd.Line(pos - right * kSpacing, end - right * kSpacing, col);
        dd.Line(pos + up    * kSpacing, end + up    * kSpacing, col);
        dd.Line(pos - up    * kSpacing, end - up    * kSpacing, col);
        break;
    }
    case LightType::Point:
    {
        dd.Circle(pos, {0.f, 1.f, 0.f}, light->range, 32, col);
        dd.Circle(pos, {1.f, 0.f, 0.f}, light->range, 32, col);
        dd.Circle(pos, {0.f, 0.f, 1.f}, light->range, 32, col);
        break;
    }
    }
}

static void CancelEditorInteraction(EditorFrameContext& ctx)
{
    EditorState& st = ctx.state;

    st.selectedEntity = NULL_ENTITY;
    st.multiSelection.clear();
    st.multiSelectionDragStartPositions.clear();
    st.multiSelectionDragStartRotations.clear();
    st.multiSelectionDragStartScales.clear();
    st.inspectorMaterialEntity = NULL_ENTITY;
    st.inspectorMaterialSlot = -1;
    st.inspectorMaterialAssetPath.clear();

    st.gizmoHoveredAxis = -1;
    st.gizmoDragAxis = -1;
    st.rotGizmoHoveredAxis = -1;
    st.rotGizmoDragAxis = -1;
    st.sclGizmoHoveredAxis = -1;
    st.sclGizmoDragAxis = -1;
    st.obbHoveredFace = -1;
    st.obbDragFaceIdx = -1;
    st.snapMoveLocked = false;
    st.gizmoDragChildren.clear();

    st.deleteConfirmOpen = false;
    st.closeConfirmOpen = false;

    // Keine ImGui-Internals hier verwenden: ESC kann aus verschiedenen
    // Fenster-/Popup-Kontexten kommen, und Fokus-Internals assertieren dann.
}

static void RunFileShortcutActions(EditorFrameContext& ctx)
{
    ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
    {
        CancelEditorInteraction(ctx);
        return;
    }

    if (io.WantTextInput)
        return;

    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false))
    {
        UndoEditorHistory(ctx);
    }
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false))
    {
        RedoEditorHistory(ctx);
    }

    // Gizmo-Modus Tastaturkürzel (nur ohne Ctrl, nicht bei Kamera-Steuerung)
    if (!io.KeyCtrl && !ImGui::IsMouseDown(ImGuiMouseButton_Right))
    {
        if (ImGui::IsKeyPressed(ImGuiKey_1, false))
            ctx.state.gizmoMode = GizmoMode::Position;
        if (ImGui::IsKeyPressed(ImGuiKey_2, false))
            ctx.state.gizmoMode = GizmoMode::Rotation;
        if (ImGui::IsKeyPressed(ImGuiKey_3, false))
            ctx.state.gizmoMode = GizmoMode::Scale;
    }

    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false))
    {
        // Projektdatei + Scene in einem Schritt speichern (saveProject ruft intern SaveEditorScene auf)
        const bool sceneOk = ctx.saveProject ? ctx.saveProject() : false;

        // Material mitsparen wenn das Material-Editor-Fenster offen und ein Asset geladen ist
        const bool materialSaved = ctx.state.materialWindowOpen &&
                                   ctx.state.selectedMaterialAssetLoaded
                                       ? SaveSelectedMaterialAsset(ctx)
                                       : false;

        if (materialSaved && !sceneOk)
            ctx.lastFileMessage = "Material gespeichert.";
        else if (sceneOk && materialSaved)
            ctx.lastFileMessage = "Gespeichert + Material gespeichert.";
        else if (sceneOk)
            ctx.lastFileMessage = "Gespeichert.";
        else
            ctx.lastFileMessage = "Speichern fehlgeschlagen.";
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Delete, false))
    {
        if (ctx.state.selectedEntity.IsValid() && ctx.world.IsAlive(ctx.state.selectedEntity))
            ctx.state.deleteConfirmOpen = true;
    }
}

static void DrawDeleteConfirmDialog(EditorFrameContext& ctx)
{
    if (ctx.state.deleteConfirmOpen)
    {
        ImGui::OpenPopup("Entity loeschen?##editor");
        ctx.state.deleteConfirmOpen = false;
    }

    if (ImGui::BeginPopupModal("Entity loeschen?##editor", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        const char* name = "(unbenannt)";
        if (ctx.state.selectedEntity.IsValid())
            if (const auto* nc = ctx.world.Get<NameComponent>(ctx.state.selectedEntity))
                if (!nc->name.empty())
                    name = nc->name.c_str();

        ImGui::Text("Entity \"%s\" loeschen?", name);
        ImGui::Separator();

        if (ImGui::Button("Loeschen", ImVec2(120.f, 0.f)))
        {
            DestroyEntityHierarchy(ctx, ctx.state.selectedEntity);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Abbrechen", ImVec2(120.f, 0.f)) ||
            ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

static void DrawCreateProjectDialog(EditorFrameContext& ctx)
{
    if (ctx.state.createProjectDialogOpen)
    {
        ImGui::OpenPopup("Projekt erstellen##editor");
        ctx.state.createProjectDialogOpen = false;
    }

    if (ImGui::BeginPopupModal("Projekt erstellen##editor",
                               nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::InputText("Projektname", ctx.state.projectNameBuffer.data(), ctx.state.projectNameBuffer.size());

        // Projektordner mit Browse-Button
        {
            const bool hasBrowse = static_cast<bool>(ctx.browseForProjectFolder);
            const float browseButtonWidth = hasBrowse ? 28.f : 0.f;
            const float spacing = hasBrowse ? ImGui::GetStyle().ItemSpacing.x : 0.f;
            ImGui::Text("Projektordner");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - browseButtonWidth - spacing);
            ImGui::InputText("##ProjectParentDir",
                             ctx.state.projectParentDirBuffer.data(),
                             ctx.state.projectParentDirBuffer.size());
            if (hasBrowse)
            {
                ImGui::SameLine();
                if (ImGui::Button("...##BrowseFolder", ImVec2(browseButtonWidth, 0.f)))
                {
                    const std::string picked = ctx.browseForProjectFolder();
                    if (!picked.empty())
                        std::snprintf(ctx.state.projectParentDirBuffer.data(),
                                      ctx.state.projectParentDirBuffer.size(),
                                      "%s", picked.c_str());
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetItemTooltip("Uebergeordneten Ordner auswaehlen");
            }
        }

        const char* backendItems = "Vulkan\0DirectX11\0OpenGL\0";
        ImGui::Combo("Backend", &ctx.state.projectBackendSelection, backendItems);

        const renderer::DeviceFactory::BackendType selectedBackend =
            EditorProjectBackendFromSelection(ctx.state.projectBackendSelection);
        ImGui::Separator();
        ImGui::Text("Aktuell: %s", ctx.currentProjectName.empty() ? "(kein Projekt)" : ctx.currentProjectName.c_str());
        ImGui::Text("Neues Projekt-Backend: %s", EditorProjectBackendLabel(selectedBackend));

        if (ImGui::Button("Erstellen", ImVec2(120.f, 0.f)))
        {
            const bool ok = ctx.createProject
                ? ctx.createProject(ctx.state.projectNameBuffer.data(),
                                    ctx.state.projectParentDirBuffer.data(),
                                    selectedBackend)
                : false;
            if (ok)
                ImGui::CloseCurrentPopup();
            else if (ctx.lastFileMessage.empty())
                ctx.lastFileMessage = "Projekt konnte nicht erstellt werden.";
        }
        ImGui::SameLine();
        if (ImGui::Button("Abbrechen", ImVec2(120.f, 0.f)) ||
            ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

static void DrawOpenProjectDialog(EditorFrameContext& ctx)
{
    if (ctx.state.openProjectDialogOpen)
    {
        ImGui::OpenPopup("Projekt oeffnen##editor");
        ctx.state.openProjectDialogOpen = false;
    }

    if (ImGui::BeginPopupModal("Projekt oeffnen##editor",
                               nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize))
    {
        // Pfad-Eingabe mit optionalem Browse-Button
        const bool hasBrowse = static_cast<bool>(ctx.browseForProjectFile);
        const float browseButtonWidth = hasBrowse ? 28.f : 0.f;
        const float spacing = hasBrowse ? ImGui::GetStyle().ItemSpacing.x : 0.f;
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - browseButtonWidth - spacing);
        ImGui::InputText("##ProjectOpenPath",
                         ctx.state.projectOpenPathBuffer.data(),
                         ctx.state.projectOpenPathBuffer.size());
        if (hasBrowse)
        {
            ImGui::SameLine();
            if (ImGui::Button("...##BrowseProject", ImVec2(browseButtonWidth, 0.f)))
            {
                const std::string picked = ctx.browseForProjectFile();
                if (!picked.empty())
                    std::snprintf(ctx.state.projectOpenPathBuffer.data(),
                                  ctx.state.projectOpenPathBuffer.size(),
                                  "%s", picked.c_str());
            }
            if (ImGui::IsItemHovered())
                ImGui::SetItemTooltip("Projektdatei auswaehlen (krom-project.json)");
        }
        ImGui::TextDisabled("Ordner oder direkte krom-project.json angeben.");

        if (ImGui::Button("Oeffnen", ImVec2(120.f, 0.f)))
        {
            const bool ok = ctx.loadProject
                ? ctx.loadProject(ctx.state.projectOpenPathBuffer.data())
                : false;
            if (ok)
                ImGui::CloseCurrentPopup();
            else if (ctx.lastFileMessage.empty())
                ctx.lastFileMessage = "Projekt konnte nicht geladen werden.";
        }
        ImGui::SameLine();
        if (ImGui::Button("Abbrechen", ImVec2(120.f, 0.f)) ||
            ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

// ---------------------------------------------------------------------------
// Close-Confirm-Dialog — wird von DrawFileMenuBar UND DrawNoProjectScreen benutzt
// ---------------------------------------------------------------------------
static void DrawCloseConfirmDialog(EditorFrameContext& ctx)
{
    if (ctx.state.closeConfirmOpen)
    {
        ImGui::OpenPopup("Anwendung schliessen?##editor");
        ctx.state.closeConfirmOpen = false;
    }

    if (ImGui::BeginPopupModal("Anwendung schliessen?##editor",
                               nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextUnformatted("Editor wirklich schliessen?");
        ImGui::Separator();
        if (ImGui::Button("Schliessen", ImVec2(120.f, 0.f)))
        {
            if (ctx.requestClose)
                ctx.requestClose();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Abbrechen", ImVec2(120.f, 0.f)) ||
            ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

// ---------------------------------------------------------------------------
// Startbildschirm — wird gezeigt solange kein Projekt geladen ist
// ---------------------------------------------------------------------------
static void DrawNoProjectScreen(EditorFrameContext& ctx)
{
    // Modals + Close-Confirm auch ohne volles Menueband verarbeiten
    DrawCreateProjectDialog(ctx);
    DrawOpenProjectDialog(ctx);
    DrawCloseConfirmDialog(ctx);

    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 windowSize{
        std::min(1030.f, io.DisplaySize.x - 12.f),
        std::min(560.f, io.DisplaySize.y - 12.f)
    };
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                            ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(windowSize, ImGuiCond_Always);
    constexpr ImGuiWindowFlags kScreenFlags =
        ImGuiWindowFlags_NoTitleBar   |
        ImGuiWindowFlags_NoResize     |
        ImGuiWindowFlags_NoMove       |
        ImGuiWindowFlags_NoCollapse   |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoSavedSettings;

    if (!ImGui::Begin("##KromStartScreen", nullptr, kScreenFlags))
    {
        ImGui::End();
        return;
    }

    constexpr float kLauncherW = 640.f;
    constexpr float kLeftW = 290.f;
    constexpr float kHeaderH = 150.f;
    constexpr float kButtonW = 190.f;
    constexpr float kButtonH = 28.f;
    const float previewW = std::max(320.f, ImGui::GetContentRegionAvail().x - kLauncherW - ImGui::GetStyle().ItemSpacing.x);

    ImGui::BeginChild("##StartLauncher", ImVec2(kLauncherW, 0.f), false);
    DrawStartLogo(ctx, ImVec2(kLauncherW, kHeaderH));

    ImGui::Spacing();

    const float lowerH = ImGui::GetContentRegionAvail().y;
    ImGui::BeginChild("##StartActions", ImVec2(kLeftW, lowerH), false);
    if (ImGui::Button("Open Project", ImVec2(kButtonW, kButtonH)))
    {
        if (ctx.browseForProjectFile)
        {
            const std::string picked = ctx.browseForProjectFile();
            if (!picked.empty())
            {
                const bool ok = ctx.loadProject ? ctx.loadProject(picked) : false;
                if (!ok && ctx.lastFileMessage.empty())
                    ctx.lastFileMessage = "Projekt konnte nicht geladen werden.";
            }
        }
        else
        {
            ctx.state.openProjectDialogOpen = true;
        }
    }

    ImGui::Spacing();
    if (ImGui::Button("New Project", ImVec2(kButtonW, kButtonH)))
        ctx.state.createProjectDialogOpen = true;

    if (ctx.browseForProjectFolder)
    {
        ImGui::Spacing();
        if (ImGui::Button("Change Folder", ImVec2(kButtonW, kButtonH)))
        {
            const std::string picked = ctx.browseForProjectFolder();
            if (!picked.empty())
                std::snprintf(ctx.state.projectParentDirBuffer.data(),
                              ctx.state.projectParentDirBuffer.size(),
                              "%s", picked.c_str());
        }
    }

    if (!ctx.lastFileMessage.empty())
    {
        ImGui::Dummy(ImVec2(1.f, 24.f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.96f, 0.55f, 0.35f, 1.f));
        ImGui::TextWrapped("%s", ctx.lastFileMessage.c_str());
            ImGui::PopStyleColor();
    }

    ImGui::Dummy(ImVec2(1.f, 24.f));
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.30f, 0.10f, 0.10f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.50f, 0.15f, 0.15f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.65f, 0.20f, 0.20f, 1.f));
    if (ImGui::Button("Close", ImVec2(kButtonW, kButtonH)))
        ctx.state.closeConfirmOpen = true;
    ImGui::PopStyleColor(3);

    const char* copyright = "KROM Editor Version 0.3";
    const float copyrightY = ImGui::GetWindowHeight() - ImGui::GetTextLineHeightWithSpacing() - 4.f;
    if (ImGui::GetCursorPosY() < copyrightY)
        ImGui::SetCursorPosY(copyrightY);
    ImGui::TextDisabled("%s", copyright);
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##StartProjects", ImVec2(0.f, lowerH), false);
    ImGui::TextUnformatted("Projekte");
    ImGui::TextDisabled("%s", ctx.state.projectParentDirBuffer.data());
    ImGui::Separator();

    const std::vector<StartProjectEntry> projects = ScanStartProjects(ctx);
    const float listHeight = std::max(160.f, ImGui::GetContentRegionAvail().y - 2.f);
    ImGui::BeginChild("##StartProjectList", ImVec2(0.f, listHeight), false);
    if (projects.empty())
    {
        ImGui::Spacing();
        ImGui::TextDisabled("Keine Projekte gefunden.");
        ImGui::TextDisabled("Erstelle ein neues Projekt oder waehle einen anderen Projektordner.");
    }
    else
    {
        for (const StartProjectEntry& project : projects)
        {
            ImGui::PushID(project.projectFile.c_str());
            const bool selected = false;
            if (ImGui::Selectable(project.name.c_str(), selected, ImGuiSelectableFlags_AllowDoubleClick,
                                  ImVec2(0.f, 42.f)))
            {
                const bool ok = ctx.loadProject ? ctx.loadProject(project.projectFile) : false;
                if (!ok && ctx.lastFileMessage.empty())
                    ctx.lastFileMessage = "Projekt konnte nicht geladen werden.";
            }
            const ImVec2 itemMin = ImGui::GetItemRectMin();
            ImGui::SetCursorScreenPos(ImVec2(itemMin.x + 8.f, itemMin.y + 22.f));
            ImGui::TextDisabled("%s", project.path.c_str());
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    ImGui::EndChild();
    ImGui::EndChild();

    ImGui::SameLine();
    DrawStartEnginePreview(ctx, ImVec2(previewW, ImGui::GetContentRegionAvail().y));

    ImGui::End();
}

static void DrawFileMenuBar(EditorFrameContext& ctx)
{
    RunFileShortcutActions(ctx);
    const bool canUndo = CanUndoEditorHistory(ctx.state);
    const bool canRedo = CanRedoEditorHistory(ctx.state);
    const std::string undoActionLabel = GetUndoEditorHistoryLabel(ctx.state);
    const std::string redoActionLabel = GetRedoEditorHistoryLabel(ctx.state);
    const std::string undoLabel = canUndo ? ("Undo " + undoActionLabel) : "Undo";
    const std::string redoLabel = canRedo ? ("Redo " + redoActionLabel) : "Redo";

    if (ImGui::BeginMainMenuBar())
    {
        if (ImGui::BeginMenu("Datei"))
        {
            if (ImGui::MenuItem("Neues Projekt...", nullptr, false, static_cast<bool>(ctx.createProject)))
                ctx.state.createProjectDialogOpen = true;
            if (ImGui::MenuItem("Projekt oeffnen...", nullptr, false, static_cast<bool>(ctx.loadProject)))
                ctx.state.openProjectDialogOpen = true;
            if (ImGui::MenuItem("Einstellungen...", nullptr, false, !ctx.currentProjectName.empty()))
                ctx.state.settingsWindowOpen = true;
            ImGui::Separator();
            // Speichert Projektdatei (krom-project.json) UND Scene in einem Schritt.
            // saveProject ruft intern SaveEditorScene() auf — kein separater Eintrag noetig.
            if (ImGui::MenuItem("Speichern", "Ctrl+S", false, static_cast<bool>(ctx.saveProject)))
            {
                const bool ok = ctx.saveProject ? ctx.saveProject() : false;
                ctx.lastFileMessage = ok ? "Gespeichert." : "Speichern fehlgeschlagen.";
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Schliessen", "Esc"))
                ctx.state.closeConfirmOpen = true;
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Edit"))
        {
            if (ImGui::MenuItem(undoLabel.c_str(), "Ctrl+Z", false, canUndo))
                UndoEditorHistory(ctx);
            if (ImGui::MenuItem(redoLabel.c_str(), "Ctrl+Y", false, canRedo))
                RedoEditorHistory(ctx);
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Ansicht"))
        {
            if (ImGui::MenuItem("Editor-Einstellungen..."))
                ctx.state.cameraSettingsOpen = true;
            ImGui::EndMenu();
        }

        const bool canPlay = static_cast<bool>(ctx.playGame) && !ctx.currentProjectName.empty();
        if (!canPlay)
            ImGui::BeginDisabled();
        if (ImGui::MenuItem("Play"))
        {
            const bool ok = ctx.playGame ? ctx.playGame() : false;
            if (!ok && ctx.lastFileMessage.empty())
                ctx.lastFileMessage = "Play fehlgeschlagen.";
        }
        if (!canPlay)
            ImGui::EndDisabled();

        ImGui::EndMainMenuBar();
    }

    // ── Kamera-Einstellungen Dialog ───────────────────────────────────────────
    if (ctx.state.cameraSettingsOpen)
    {
        ImGui::SetNextWindowSize(ImVec2(300.f, 240.f), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Editor-Einstellungen##camset", &ctx.state.cameraSettingsOpen))
        {
            EditorCameraState& cam = ctx.state.editorCamera;

            ImGui::CollapsingHeader("Kamera", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_Leaf);
            ImGui::DragFloat("FOV",               &cam.fovDeg,    0.5f,   10.f,    170.f, "%.1f°");
            ImGui::DragFloat("Near",              &cam.nearPlane, 0.001f, 0.001f,  10.f,  "%.3f m");
            ImGui::DragFloat("Far",               &cam.farPlane,  10.f,   10.f,    100000.f, "%.0f m");
            ImGui::DragFloat("Geschwindigkeit",   &cam.moveSpeed, 1.f,    0.1f,    10000.f, "%.1f");

            cam.farPlane  = std::max(cam.nearPlane + 1.f, cam.farPlane);
            cam.nearPlane = std::clamp(cam.nearPlane, 0.001f, cam.farPlane - 1.f);
            cam.fovDeg    = std::clamp(cam.fovDeg, 10.f, 170.f);
            cam.moveSpeed = std::max(0.1f, cam.moveSpeed);

            ImGui::Separator();

            ImGui::CollapsingHeader("Viewport", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_Leaf);
            if (ImGui::Checkbox("IBL aktiv", &ctx.state.environmentEnableIBL))
            {
                ctx.state.ambientSource = ctx.state.environmentEnableIBL
                    ? EditorState::AmbientSource::Skybox : EditorState::AmbientSource::Color;
                ApplyEnvironmentFromState(ctx);
                if (ctx.setEditorAmbientLight)
                    ctx.setEditorAmbientLight(ctx.state.ambientColor, ctx.state.ambientIntensity);
            }
            ImGui::Checkbox("Skybox anzeigen", &ctx.state.showSkyboxBackground);
            if (ImGui::ColorEdit3("Hintergrund", ctx.state.backgroundColor.data()))
            {
                if (ctx.setEditorBackgroundColor)
                    ctx.setEditorBackgroundColor(ctx.state.backgroundColor);
            }

            ImGui::Separator();

            ImGui::CollapsingHeader("Code Editor", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_Leaf);
            constexpr float kCodeEditorLabelWidth = 82.f;
            ImGui::TextUnformatted("Executable");
            ImGui::SameLine(kCodeEditorLabelWidth);
            ImGui::SetNextItemWidth(-1.f);
            ImGui::InputText("##code_editor_executable", ctx.state.codeEditorExecutableBuffer.data(),
                             ctx.state.codeEditorExecutableBuffer.size());
            ImGui::TextUnformatted("Arguments");
            ImGui::SameLine(kCodeEditorLabelWidth);
            ImGui::SetNextItemWidth(-1.f);
            ImGui::InputText("##code_editor_arguments", ctx.state.codeEditorArgumentsBuffer.data(),
                             ctx.state.codeEditorArgumentsBuffer.size());
            if (ImGui::Button("VS Code"))
            {
                std::snprintf(ctx.state.codeEditorExecutableBuffer.data(),
                              ctx.state.codeEditorExecutableBuffer.size(), "%s", "code");
                std::snprintf(ctx.state.codeEditorArgumentsBuffer.data(),
                              ctx.state.codeEditorArgumentsBuffer.size(), "%s", "\"{workspace}\" -g \"{file}:{line}\"");
            }
            ImGui::SameLine();
            if (ImGui::Button("System Default"))
            {
                ctx.state.codeEditorExecutableBuffer[0] = '\0';
                std::snprintf(ctx.state.codeEditorArgumentsBuffer.data(),
                              ctx.state.codeEditorArgumentsBuffer.size(), "%s", "\"{file}\"");
            }

            ImGui::Separator();

            if (ImGui::Button("Zuruecksetzen"))
            {
                cam.fovDeg    = 60.f;
                cam.nearPlane = 0.05f;
                cam.farPlane  = 2000.f;
                cam.moveSpeed = 50.f;
            }
            ImGui::SameLine();
            if (ImGui::Button("Schliessen"))
                ctx.state.cameraSettingsOpen = false;
        }
        ImGui::End();
    }

    DrawCreateProjectDialog(ctx);
    DrawOpenProjectDialog(ctx);
    DrawCloseConfirmDialog(ctx);
}

} // namespace

// =============================================================================
// Transform-Gizmo – 3D-Pfeile zum Verschieben der selektierten Entity
// =============================================================================

// 2D-Abstand von Punkt P zum Liniensegment AB (Pixel)
static float DistPointToSegment2D(ImVec2 p, ImVec2 a, ImVec2 b)
{
    const float dx = b.x - a.x, dy = b.y - a.y;
    const float lenSq = dx * dx + dy * dy;
    if (lenSq < 1e-6f)
        return std::sqrt((p.x-a.x)*(p.x-a.x) + (p.y-a.y)*(p.y-a.y));
    const float t  = std::clamp(((p.x-a.x)*dx + (p.y-a.y)*dy) / lenSq, 0.f, 1.f);
    const float cx = a.x + t*dx, cy = a.y + t*dy;
    return std::sqrt((p.x-cx)*(p.x-cx) + (p.y-cy)*(p.y-cy));
}

// ── Phase 1: DebugDraw ────────────────────────────────────────────────────────
struct DpadGizmoRuntime
{
    MeshHandle arrowMesh;
    MeshHandle sphereMesh;
    EntityID arrows[3] = {NULL_ENTITY, NULL_ENTITY, NULL_ENTITY};
    EntityID center = NULL_ENTITY;
    bool attemptedLoad = false;
};

static DpadGizmoRuntime& GetDpadGizmoRuntime()
{
    static DpadGizmoRuntime runtime;
    return runtime;
}

static std::filesystem::path EditorDpadAssetPath(const EditorFrameContext& ctx, const char* filename)
{
    if (!ctx.engineEditorDir.empty())
        return std::filesystem::path(ctx.engineEditorDir) / filename;
    return std::filesystem::path("editor") / filename;
}

static MeshHandle LoadEditorDpadMesh(EditorFrameContext& ctx, const std::filesystem::path& path)
{
    if (!ctx.assetPipeline)
        return MeshHandle::Invalid();

    const std::string meshKey = path.generic_string() + "#mesh/0";
    assets::ImportedAssetBundle bundle = ctx.assetPipeline->ImportBundle(path.string());
    if (!bundle.Ok() || bundle.meshes.empty())
    {
        Debug::LogWarning("Transform-DPad: Mesh konnte nicht geladen werden: %s", path.string().c_str());
        return MeshHandle::Invalid();
    }

    for (assets::SubMeshData& sub : bundle.meshes[0].submeshes)
        assets::EnsureTangents(sub);

    auto meshAsset = std::make_unique<assets::MeshAsset>(std::move(bundle.meshes[0]));
    meshAsset->path = meshKey;
    meshAsset->state = assets::AssetState::Loaded;
    meshAsset->gpuStatus.dirty = true;
    meshAsset->gpuStatus.uploaded = false;
    meshAsset->materialHandles.clear();
    return ctx.registry.GetOrAddMesh(meshKey, std::move(meshAsset));
}

static EntityID CreateDpadGizmoEntity(EditorFrameContext& ctx,
                                      const char* name,
                                      MeshHandle mesh,
                                      int axis)
{
    const EntityID entity = ctx.world.CreateEntity();
    ctx.world.Add<ActiveComponent>(entity, ActiveComponent{false});
    ctx.world.Add<NameComponent>(entity, name);
    ctx.world.Add<TransformComponent>(entity);
    ctx.world.Add<WorldTransformComponent>(entity);
    ctx.world.Add<EditorRuntimeGizmoTag>(entity);
    ctx.world.Add<EditorDpadGizmoComponent>(entity, EditorDpadGizmoComponent{axis});

    MeshComponent meshComp{};
    meshComp.mesh = mesh;
    meshComp.castShadows = false;
    meshComp.receiveShadows = false;
    meshComp.layerMask = renderer::LAYER_EDITOR_GIZMO;
    if (const assets::MeshAsset* meshAsset = ctx.registry.meshes.Get(mesh))
        meshComp.meshAssetPath = meshAsset->path;
    ctx.world.Add<MeshComponent>(entity, meshComp);
    ctx.world.Add<MaterialComponent>(entity);
    return entity;
}

static void SetDpadGizmoActive(EditorFrameContext& ctx, bool active)
{
    DpadGizmoRuntime& runtime = GetDpadGizmoRuntime();
    auto setOne = [&](EntityID entity)
    {
        if (!entity.IsValid() || !ctx.world.IsAlive(entity))
            return;
        if (auto* activeComp = ctx.world.Get<ActiveComponent>(entity))
            activeComp->active = active;
    };
    for (EntityID arrow : runtime.arrows)
        setOne(arrow);
    setOne(runtime.center);
}

static bool EnsureDpadGizmo(EditorFrameContext& ctx)
{
    DpadGizmoRuntime& runtime = GetDpadGizmoRuntime();
    const bool alive =
        runtime.arrows[0].IsValid() && ctx.world.IsAlive(runtime.arrows[0]) &&
        runtime.arrows[1].IsValid() && ctx.world.IsAlive(runtime.arrows[1]) &&
        runtime.arrows[2].IsValid() && ctx.world.IsAlive(runtime.arrows[2]) &&
        runtime.center.IsValid() && ctx.world.IsAlive(runtime.center);
    if (alive)
        return true;

    if (!ctx.assetPipeline)
        return false;
    if (runtime.attemptedLoad && (!runtime.arrowMesh.IsValid() || !runtime.sphereMesh.IsValid()))
        return false;

    runtime.attemptedLoad = true;
    if (!runtime.arrowMesh.IsValid())
        runtime.arrowMesh = LoadEditorDpadMesh(ctx, EditorDpadAssetPath(ctx, "arrow.glb"));
    if (!runtime.sphereMesh.IsValid())
        runtime.sphereMesh = LoadEditorDpadMesh(ctx, EditorDpadAssetPath(ctx, "dpad_sphere.glb"));
    if (!runtime.arrowMesh.IsValid() || !runtime.sphereMesh.IsValid())
        return false;

    runtime.arrows[0] = CreateDpadGizmoEntity(ctx, "__editor_dpad_x", runtime.arrowMesh, 0);
    runtime.arrows[1] = CreateDpadGizmoEntity(ctx, "__editor_dpad_y", runtime.arrowMesh, 1);
    runtime.arrows[2] = CreateDpadGizmoEntity(ctx, "__editor_dpad_z", runtime.arrowMesh, 2);
    runtime.center = CreateDpadGizmoEntity(ctx, "__editor_dpad_center", runtime.sphereMesh, -1);
    ctx.assetPipeline->UploadPendingGpuAssets();
    return true;
}

static bool UpdateDpadGizmo(EditorFrameContext& ctx,
                            const math::Vec3& center,
                            const math::Quat& basisRotation,
                            float size)
{
    if (!EnsureDpadGizmo(ctx))
        return false;

    SetDpadGizmoActive(ctx, true);
    DpadGizmoRuntime& runtime = GetDpadGizmoRuntime();

    const std::string normalMat[3] = {
        EditorDpadAssetPath(ctx, "dpad_rot.mat").generic_string(),
        EditorDpadAssetPath(ctx, "dpad_gruen.mat").generic_string(),
        EditorDpadAssetPath(ctx, "dpad_blau.mat").generic_string()
    };
    const std::string yellowMat = EditorDpadAssetPath(ctx, "dpad_gelb.mat").generic_string();
    const std::string centerMat = EditorDpadAssetPath(ctx, "dpad_weiss.mat").generic_string();

    const math::Vec3 axes[3] = {
        basisRotation.Rotate({1.f, 0.f, 0.f}),
        basisRotation.Rotate({0.f, 1.f, 0.f}),
        basisRotation.Rotate({0.f, 0.f, 1.f})
    };
    const math::Quat rotations[3] = {
        basisRotation * math::Quat::FromAxisAngleDeg({0.f, 1.f, 0.f}, 90.f),
        basisRotation * math::Quat::FromAxisAngleDeg({1.f, 0.f, 0.f}, -90.f),
        basisRotation
    };
    const float scale = std::max(0.001f, size / 8.0f);
    const float offsets[3] = {2.50218f, 2.11085f, 2.21877f};
    bool materialChanged = false;

    auto applyTransform = [&](EntityID entity,
                              const math::Vec3& position,
                              const math::Quat& rotation,
                              const math::Vec3& localScale)
    {
        if (auto* transform = ctx.world.Get<TransformComponent>(entity))
        {
            transform->localPosition = position;
            transform->localRotation = rotation;
            transform->localScale = localScale;
            transform->dirty = false;
            ++transform->localVersion;
            ++transform->worldVersion;
            transform->parentWorldVersion = 0u;
        }
        if (auto* worldTransform = ctx.world.Get<WorldTransformComponent>(entity))
        {
            worldTransform->position = position;
            worldTransform->rotation = rotation;
            worldTransform->scale = localScale;
            worldTransform->matrix = math::Mat4::TRS(position, rotation, localScale);
            worldTransform->inverse = worldTransform->matrix.InverseAffine();
        }
    };

    for (int i = 0; i < 3; ++i)
    {
        const EntityID entity = runtime.arrows[i];
        if (!entity.IsValid() || !ctx.world.IsAlive(entity))
            continue;
        applyTransform(entity,
                       center + axes[i] * (offsets[i] * scale),
                       rotations[i],
                       {scale, scale, scale});
        const bool highlighted = ctx.state.gizmoHoveredAxis == i || ctx.state.gizmoDragAxis == i;
        const std::string& materialPath = highlighted ? yellowMat : normalMat[i];
        if (auto* material = ctx.world.Get<MaterialComponent>(entity))
        {
            if (material->materialAssetPath != materialPath)
            {
                material->slotOverrides.clear();
                material->baseColorTexturePath.clear();
                material->materialAssetPath = materialPath;
                material->material = MaterialHandle::Invalid();
                materialChanged = true;
            }
        }
    }

    if (runtime.center.IsValid() && ctx.world.IsAlive(runtime.center))
    {
        applyTransform(runtime.center, center, basisRotation, {scale, scale, scale});
        const bool centerHov = (ctx.state.gizmoHoveredAxis == 3 ||
                                ctx.state.gizmoDragAxis    == 3);
        const std::string& cMat = centerHov ? yellowMat : centerMat;
        if (auto* material = ctx.world.Get<MaterialComponent>(runtime.center))
        {
            if (material->materialAssetPath != cMat)
            {
                material->slotOverrides.clear();
                material->baseColorTexturePath.clear();
                material->materialAssetPath = cMat;
                material->material = MaterialHandle::Invalid();
                materialChanged = true;
            }
        }
    }

    if (materialChanged)
        ResolveMaterialAssetBindings(ctx);
    return true;
}

struct RotateGizmoRuntime
{
    MeshHandle mesh;
    MeshHandle sphereMesh;
    EntityID rings[3]    = {NULL_ENTITY, NULL_ENTITY, NULL_ENTITY};
    EntityID center      = NULL_ENTITY;
    bool attemptedLoad   = false;
};

static RotateGizmoRuntime& GetRotateGizmoRuntime()
{
    static RotateGizmoRuntime runtime;
    return runtime;
}

static EntityID CreateRotateGizmoEntity(EditorFrameContext& ctx,
                                        const char* name,
                                        MeshHandle mesh,
                                        int axis)
{
    const EntityID entity = ctx.world.CreateEntity();
    ctx.world.Add<ActiveComponent>(entity, ActiveComponent{false});
    ctx.world.Add<NameComponent>(entity, name);
    ctx.world.Add<TransformComponent>(entity);
    ctx.world.Add<WorldTransformComponent>(entity);
    ctx.world.Add<EditorRuntimeGizmoTag>(entity);
    ctx.world.Add<EditorRotateGizmoComponent>(entity, EditorRotateGizmoComponent{axis});

    MeshComponent meshComp{};
    meshComp.mesh = mesh;
    meshComp.castShadows = false;
    meshComp.receiveShadows = false;
    meshComp.layerMask = renderer::LAYER_EDITOR_GIZMO;
    if (const assets::MeshAsset* meshAsset = ctx.registry.meshes.Get(mesh))
        meshComp.meshAssetPath = meshAsset->path;
    ctx.world.Add<MeshComponent>(entity, meshComp);
    ctx.world.Add<MaterialComponent>(entity);
    return entity;
}

static void SetRotateGizmoActive(EditorFrameContext& ctx, bool active)
{
    RotateGizmoRuntime& runtime = GetRotateGizmoRuntime();
    auto setOne = [&](EntityID entity)
    {
        if (!entity.IsValid() || !ctx.world.IsAlive(entity)) return;
        if (auto* ac = ctx.world.Get<ActiveComponent>(entity))
            ac->active = active;
    };
    for (EntityID ring : runtime.rings)
        setOne(ring);
    setOne(runtime.center);
}

static bool EnsureRotateGizmo(EditorFrameContext& ctx)
{
    RotateGizmoRuntime& runtime = GetRotateGizmoRuntime();
    const bool alive =
        runtime.rings[0].IsValid() && ctx.world.IsAlive(runtime.rings[0]) &&
        runtime.rings[1].IsValid() && ctx.world.IsAlive(runtime.rings[1]) &&
        runtime.rings[2].IsValid() && ctx.world.IsAlive(runtime.rings[2]) &&
        runtime.center.IsValid()   && ctx.world.IsAlive(runtime.center);
    if (alive)
        return true;

    if (!ctx.assetPipeline)
        return false;
    if (runtime.attemptedLoad && (!runtime.mesh.IsValid() || !runtime.sphereMesh.IsValid()))
        return false;

    runtime.attemptedLoad = true;
    if (!runtime.mesh.IsValid())
        runtime.mesh = LoadEditorDpadMesh(ctx, EditorDpadAssetPath(ctx, "rotate.glb"));
    if (!runtime.sphereMesh.IsValid())
        runtime.sphereMesh = LoadEditorDpadMesh(ctx, EditorDpadAssetPath(ctx, "dpad_sphere.glb"));
    if (!runtime.mesh.IsValid() || !runtime.sphereMesh.IsValid())
        return false;

    runtime.rings[0] = CreateRotateGizmoEntity(ctx, "__editor_rotate_x",      runtime.mesh,       0);
    runtime.rings[1] = CreateRotateGizmoEntity(ctx, "__editor_rotate_y",      runtime.mesh,       1);
    runtime.rings[2] = CreateRotateGizmoEntity(ctx, "__editor_rotate_z",      runtime.mesh,       2);
    runtime.center   = CreateRotateGizmoEntity(ctx, "__editor_rotate_center", runtime.sphereMesh, -1);
    ctx.assetPipeline->UploadPendingGpuAssets();
    return true;
}

static void BuildRotateGizmoVisualRotations(const math::Quat& basisRotation,
                                            math::Quat (&rotations)[3])
{
    // rotate.glb braucht eine feste Y-Korrektur. In World bleibt basisRotation
    // Identity; in Local kommt die Objektrotation aus DrawRotationGizmo dazu.
    const math::Quat visualBasis =
        basisRotation * math::Quat::FromAxisAngleDeg({0.f, 1.f, 0.f}, 90.f);
    rotations[0] = visualBasis * math::Quat::FromAxisAngleDeg({1.f, 0.f, 0.f}, -90.f);
    rotations[1] = visualBasis;
    rotations[2] = visualBasis * math::Quat::FromAxisAngleDeg({0.f, 0.f, 1.f}, -90.f);
}

static bool UpdateRotateGizmo(EditorFrameContext& ctx,
                              const math::Vec3& center,
                              const math::Quat& basisRotation,
                              float radius)
{
    if (!EnsureRotateGizmo(ctx))
        return false;

    SetRotateGizmoActive(ctx, true);
    RotateGizmoRuntime& runtime = GetRotateGizmoRuntime();

    const std::string normalMat[3] = {
        EditorDpadAssetPath(ctx, "dpad_rot.mat").generic_string(),
        EditorDpadAssetPath(ctx, "dpad_gruen.mat").generic_string(),
        EditorDpadAssetPath(ctx, "dpad_blau.mat").generic_string()
    };
    const std::string yellowMat = EditorDpadAssetPath(ctx, "dpad_gelb.mat").generic_string();

    math::Quat rotations[3];
    BuildRotateGizmoVisualRotations(basisRotation, rotations);
    constexpr float kRotateMeshRadius = 1.3088281f;
    const float scale = std::max(0.001f, radius / kRotateMeshRadius);
    bool materialChanged = false;

    auto applyTransform = [&](EntityID entity,
                              const math::Quat& rotation,
                              const math::Vec3& localScale)
    {
        if (auto* transform = ctx.world.Get<TransformComponent>(entity))
        {
            transform->localPosition = center;
            transform->localRotation = rotation;
            transform->localScale = localScale;
            transform->dirty = false;
            ++transform->localVersion;
            ++transform->worldVersion;
            transform->parentWorldVersion = 0u;
        }
        if (auto* worldTransform = ctx.world.Get<WorldTransformComponent>(entity))
        {
            worldTransform->position = center;
            worldTransform->rotation = rotation;
            worldTransform->scale = localScale;
            worldTransform->matrix = math::Mat4::TRS(center, rotation, localScale);
            worldTransform->inverse = worldTransform->matrix.InverseAffine();
        }
    };

    for (int i = 0; i < 3; ++i)
    {
        const EntityID entity = runtime.rings[i];
        if (!entity.IsValid() || !ctx.world.IsAlive(entity))
            continue;

        applyTransform(entity, rotations[i], {scale, scale, scale});
        const bool highlighted = ctx.state.rotGizmoHoveredAxis == i ||
                                 ctx.state.rotGizmoDragAxis == i;
        const std::string& materialPath = highlighted ? yellowMat : normalMat[i];
        if (auto* material = ctx.world.Get<MaterialComponent>(entity))
        {
            if (material->materialAssetPath != materialPath)
            {
                material->slotOverrides.clear();
                material->baseColorTexturePath.clear();
                material->materialAssetPath = materialPath;
                material->material = MaterialHandle::Invalid();
                materialChanged = true;
            }
        }
    }

    // Center-Sphere (Trackball, axis 3)
    if (runtime.center.IsValid() && ctx.world.IsAlive(runtime.center))
    {
        const float sphereScale = scale * 0.16f;  // gleiche Sichtgröße wie Positions-Sphere
        if (auto* transform = ctx.world.Get<TransformComponent>(runtime.center))
        {
            transform->localPosition = center;
            transform->localRotation = math::Quat::Identity();
            transform->localScale    = {sphereScale, sphereScale, sphereScale};
            transform->dirty = false;
            ++transform->localVersion; ++transform->worldVersion;
            transform->parentWorldVersion = 0u;
        }
        if (auto* wt = ctx.world.Get<WorldTransformComponent>(runtime.center))
        {
            const math::Vec3 ls{sphereScale, sphereScale, sphereScale};
            wt->position = center;
            wt->rotation = math::Quat::Identity();
            wt->scale    = ls;
            wt->matrix   = math::Mat4::TRS(center, math::Quat::Identity(), ls);
            wt->inverse  = wt->matrix.InverseAffine();
        }
        const bool hov = (ctx.state.rotGizmoHoveredAxis == 3 ||
                          ctx.state.rotGizmoDragAxis    == 3);
        const std::string matPath = hov
            ? yellowMat
            : EditorDpadAssetPath(ctx, "dpad_weiss.mat").generic_string();
        if (auto* material = ctx.world.Get<MaterialComponent>(runtime.center))
        {
            if (material->materialAssetPath != matPath)
            {
                material->slotOverrides.clear();
                material->baseColorTexturePath.clear();
                material->materialAssetPath = matPath;
                material->material          = MaterialHandle::Invalid();
                materialChanged             = true;
            }
        }
    }

    if (materialChanged)
        ResolveMaterialAssetBindings(ctx);
    return true;
}

// =============================================================================
// Scale-Gizmo — drei farbige Pfeile (scale.glb) entlang X / Y / Z
// =============================================================================

struct ScaleGizmoRuntime
{
    MeshHandle scaleMesh;
    MeshHandle sphereMesh;
    EntityID   arrows[3]     = {NULL_ENTITY, NULL_ENTITY, NULL_ENTITY};
    EntityID   center        = NULL_ENTITY;
    bool       attemptedLoad = false;
};

static ScaleGizmoRuntime& GetScaleGizmoRuntime()
{
    static ScaleGizmoRuntime runtime;
    return runtime;
}

static EntityID CreateScaleGizmoEntity(EditorFrameContext& ctx,
                                       const char* name,
                                       MeshHandle mesh,
                                       int axis)
{
    const EntityID entity = ctx.world.CreateEntity();
    ctx.world.Add<ActiveComponent>(entity, ActiveComponent{false});
    ctx.world.Add<NameComponent>(entity, name);
    ctx.world.Add<TransformComponent>(entity);
    ctx.world.Add<WorldTransformComponent>(entity);
    ctx.world.Add<EditorRuntimeGizmoTag>(entity);
    ctx.world.Add<EditorScaleGizmoComponent>(entity, EditorScaleGizmoComponent{axis});

    MeshComponent meshComp{};
    meshComp.mesh         = mesh;
    meshComp.castShadows  = false;
    meshComp.receiveShadows = false;
    meshComp.layerMask    = renderer::LAYER_EDITOR_GIZMO;
    if (const assets::MeshAsset* meshAsset = ctx.registry.meshes.Get(mesh))
        meshComp.meshAssetPath = meshAsset->path;
    ctx.world.Add<MeshComponent>(entity, meshComp);
    ctx.world.Add<MaterialComponent>(entity);
    return entity;
}

static void SetScaleGizmoActive(EditorFrameContext& ctx, bool active)
{
    ScaleGizmoRuntime& runtime = GetScaleGizmoRuntime();
    auto setOne = [&](EntityID entity)
    {
        if (!entity.IsValid() || !ctx.world.IsAlive(entity)) return;
        if (auto* ac = ctx.world.Get<ActiveComponent>(entity))
            ac->active = active;
    };
    for (EntityID arrow : runtime.arrows)
        setOne(arrow);
    setOne(runtime.center);
}

static bool EnsureScaleGizmo(EditorFrameContext& ctx)
{
    ScaleGizmoRuntime& runtime = GetScaleGizmoRuntime();
    const bool alive =
        runtime.arrows[0].IsValid() && ctx.world.IsAlive(runtime.arrows[0]) &&
        runtime.arrows[1].IsValid() && ctx.world.IsAlive(runtime.arrows[1]) &&
        runtime.arrows[2].IsValid() && ctx.world.IsAlive(runtime.arrows[2]) &&
        runtime.center.IsValid()   && ctx.world.IsAlive(runtime.center);
    if (alive)
        return true;

    if (!ctx.assetPipeline)
        return false;
    if (runtime.attemptedLoad && (!runtime.scaleMesh.IsValid() || !runtime.sphereMesh.IsValid()))
        return false;

    runtime.attemptedLoad = true;
    if (!runtime.scaleMesh.IsValid())
        runtime.scaleMesh  = LoadEditorDpadMesh(ctx, EditorDpadAssetPath(ctx, "scale.glb"));
    if (!runtime.sphereMesh.IsValid())
        runtime.sphereMesh = LoadEditorDpadMesh(ctx, EditorDpadAssetPath(ctx, "dpad_sphere.glb"));
    if (!runtime.scaleMesh.IsValid() || !runtime.sphereMesh.IsValid())
        return false;

    runtime.arrows[0] = CreateScaleGizmoEntity(ctx, "__editor_scale_x",      runtime.scaleMesh,  0);
    runtime.arrows[1] = CreateScaleGizmoEntity(ctx, "__editor_scale_y",      runtime.scaleMesh,  1);
    runtime.arrows[2] = CreateScaleGizmoEntity(ctx, "__editor_scale_z",      runtime.scaleMesh,  2);
    runtime.center    = CreateScaleGizmoEntity(ctx, "__editor_scale_center", runtime.sphereMesh, -1);
    ctx.assetPipeline->UploadPendingGpuAssets();
    return true;
}

static bool UpdateScaleGizmo(EditorFrameContext& ctx,
                             const math::Vec3& center,
                             const math::Quat& basisRotation,
                             float size)
{
    if (!EnsureScaleGizmo(ctx))
        return false;

    SetScaleGizmoActive(ctx, true);
    ScaleGizmoRuntime& runtime = GetScaleGizmoRuntime();

    const std::string normalMat[3] = {
        EditorDpadAssetPath(ctx, "dpad_rot.mat").generic_string(),
        EditorDpadAssetPath(ctx, "dpad_gruen.mat").generic_string(),
        EditorDpadAssetPath(ctx, "dpad_blau.mat").generic_string()
    };
    const std::string yellowMat = EditorDpadAssetPath(ctx, "dpad_gelb.mat").generic_string();
    const std::string centerMat = EditorDpadAssetPath(ctx, "dpad_weiss.mat").generic_string();

    const math::Vec3 axes[3] = {
        basisRotation.Rotate({1.f, 0.f, 0.f}),
        basisRotation.Rotate({0.f, 1.f, 0.f}),
        basisRotation.Rotate({0.f, 0.f, 1.f})
    };
    const math::Quat rotations[3] = {
        basisRotation * math::Quat::FromAxisAngleDeg({0.f, 1.f, 0.f},  90.f),
        basisRotation * math::Quat::FromAxisAngleDeg({1.f, 0.f, 0.f}, -90.f),
        basisRotation
    };
    const float scale = std::max(0.001f, size / 8.0f);
    const float offsets[3] = {2.50218f, 2.11085f, 2.21877f};
    bool materialChanged = false;

    auto applyTransform = [&](EntityID entity,
                              const math::Vec3& position,
                              const math::Quat& rotation,
                              const math::Vec3& localScale)
    {
        if (auto* transform = ctx.world.Get<TransformComponent>(entity))
        {
            transform->localPosition = position;
            transform->localRotation = rotation;
            transform->localScale    = localScale;
            transform->dirty         = false;
            ++transform->localVersion;
            ++transform->worldVersion;
            transform->parentWorldVersion = 0u;
        }
        if (auto* worldTransform = ctx.world.Get<WorldTransformComponent>(entity))
        {
            worldTransform->position = position;
            worldTransform->rotation = rotation;
            worldTransform->scale    = localScale;
            worldTransform->matrix   = math::Mat4::TRS(position, rotation, localScale);
            worldTransform->inverse  = worldTransform->matrix.InverseAffine();
        }
    };

    for (int i = 0; i < 3; ++i)
    {
        const EntityID entity = runtime.arrows[i];
        if (!entity.IsValid() || !ctx.world.IsAlive(entity))
            continue;
        applyTransform(entity,
                       center + axes[i] * (offsets[i] * scale),
                       rotations[i],
                       {scale, scale, scale});
        const bool highlighted = ctx.state.sclGizmoHoveredAxis == i ||
                                 ctx.state.sclGizmoDragAxis    == i;
        const std::string& materialPath = highlighted ? yellowMat : normalMat[i];
        if (auto* material = ctx.world.Get<MaterialComponent>(entity))
        {
            if (material->materialAssetPath != materialPath)
            {
                material->slotOverrides.clear();
                material->baseColorTexturePath.clear();
                material->materialAssetPath = materialPath;
                material->material          = MaterialHandle::Invalid();
                materialChanged             = true;
            }
        }
    }

    // Center-Sphere (axis 3 = uniform)
    if (runtime.center.IsValid() && ctx.world.IsAlive(runtime.center))
    {
        applyTransform(runtime.center, center, basisRotation, {scale, scale, scale});
        const bool highlighted = ctx.state.sclGizmoHoveredAxis == 3 ||
                                 ctx.state.sclGizmoDragAxis    == 3;
        const std::string& matPath = highlighted ? yellowMat : centerMat;
        if (auto* material = ctx.world.Get<MaterialComponent>(runtime.center))
        {
            if (material->materialAssetPath != matPath)
            {
                material->slotOverrides.clear();
                material->baseColorTexturePath.clear();
                material->materialAssetPath = matPath;
                material->material          = MaterialHandle::Invalid();
                materialChanged             = true;
            }
        }
    }

    if (materialChanged)
        ResolveMaterialAssetBindings(ctx);
    return true;
}

static void DrawScaleGizmo(EditorFrameContext& ctx)
{
    if (ctx.state.gizmoMode != GizmoMode::Scale)
    {
        SetScaleGizmoActive(ctx, false);
        return;
    }

    EditorState& st = ctx.state;
    const bool isMulti = st.multiSelection.size() >= 2;

    math::Vec3 center;
    math::Quat basisRotation = math::Quat::Identity();
    math::Vec3 axes[3] = {{1,0,0},{0,1,0},{0,0,1}};

    if (isMulti)
    {
        center = st.multiSelectionPivot;
        // Multi-Selektion: immer World-Space-Achsen
    }
    else
    {
        const EntityID selected = st.selectedEntity;
        if (!selected.IsValid())
        {
            SetScaleGizmoActive(ctx, false);
            st.sclGizmoHoveredAxis = -1;
            return;
        }
        const auto* wtc = ctx.world.Get<WorldTransformComponent>(selected);
        if (!wtc)
        {
            SetScaleGizmoActive(ctx, false);
            st.sclGizmoHoveredAxis = -1;
            return;
        }
        center = wtc->position;
        basisRotation = wtc->rotation;
        axes[0] = wtc->rotation.Rotate({1.f, 0.f, 0.f});
        axes[1] = wtc->rotation.Rotate({0.f, 1.f, 0.f});
        axes[2] = wtc->rotation.Rotate({0.f, 0.f, 1.f});
    }

    const float camDist = (center - st.editorCamera.position).Length();
    const float sz      = std::max(0.15f, camDist * 0.10f);
    const bool meshOk   = UpdateScaleGizmo(ctx, center, basisRotation, sz);

    // Drag beenden
    if (st.sclGizmoDragAxis >= 0 && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
    {
        CommitPendingSceneEdit(ctx, "gizmo.scale");
        st.sclGizmoDragAxis = -1;
    }

    ImGuiIO& io = ImGui::GetIO();
    if (io.WantCaptureMouse) { st.sclGizmoHoveredAxis = -1; return; }

    const math::Mat4 view   = BuildEditorViewMatrix(st.editorCamera);
    const float      aspect = (io.DisplaySize.y > 0.f) ? io.DisplaySize.x / io.DisplaySize.y : 1.f;
    const math::Mat4 vp     = math::Mat4::PerspectiveFovRH(
        st.editorCamera.fovDeg * math::DEG_TO_RAD, aspect,
        st.editorCamera.nearPlane, st.editorCamera.farPlane) * view;

    ImVec2 screenCenter;
    if (!WorldToScreenPx(center, vp, io.DisplaySize, screenCenter))
    {
        st.sclGizmoHoveredAxis = -1;
        return;
    }

    if (!meshOk) { st.sclGizmoHoveredAxis = -1; return; }

    // Hover-Erkennung
    st.sclGizmoHoveredAxis = -1;
    if (st.sclGizmoDragAxis < 0)
    {
        constexpr float kPickDist       = 22.f;
        constexpr float kVisualTipScale = 1.45f;
        constexpr float kCenterRadius   = 20.f;  // Pixel-Radius für Center-Sphere
        float bestDist = kPickDist;

        // Center-Sphere zuerst prüfen (hat Vorrang)
        const float dcx = io.MousePos.x - screenCenter.x;
        const float dcy = io.MousePos.y - screenCenter.y;
        const float distCenter = std::sqrt(dcx * dcx + dcy * dcy);
        if (distCenter < kCenterRadius)
        {
            st.sclGizmoHoveredAxis = 3;
        }
        else
        {
            for (int i = 0; i < 3; ++i)
            {
                ImVec2 screenTip;
                if (!WorldToScreenPx(center + axes[i] * (sz * kVisualTipScale), vp, io.DisplaySize, screenTip))
                    continue;
                const float d = DistPointToSegment2D(io.MousePos, screenCenter, screenTip);
                if (d < bestDist) { bestDist = d; st.sclGizmoHoveredAxis = i; }
            }
        }
    }
    else
    {
        st.sclGizmoHoveredAxis = st.sclGizmoDragAxis;
    }

    // Drag starten
    if (st.sclGizmoHoveredAxis >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
    {
        // Für Einzel-Achsen: Screen-Space-Richtung einfrieren
        // Für Center (axis 3): diagonal nach rechts-oben als Drag-Richtung
        float snx = 1.f, sny = -1.f;
        if (st.sclGizmoHoveredAxis < 3)
        {
            const math::Vec3& axisDir = axes[st.sclGizmoHoveredAxis];
            ImVec2 screenTip;
            if (WorldToScreenPx(center + axisDir, vp, io.DisplaySize, screenTip))
            {
                const float sdx  = screenTip.x - screenCenter.x;
                const float sdy  = screenTip.y - screenCenter.y;
                const float sLen = std::sqrt(sdx * sdx + sdy * sdy);
                if (sLen > 1e-4f) { snx = sdx / sLen; sny = sdy / sLen; }
            }
        }
        else
        {
            // uniform: diagonal (rechts = größer, links = kleiner)
            const float diag = std::sqrt(2.f);
            snx = 1.f / diag;
            sny = -1.f / diag;
        }

        BeginPendingSceneEdit(ctx, "gizmo.scale", "Objekt skaliert");
        st.sclGizmoDragAxis       = st.sclGizmoHoveredAxis;
        st.sclGizmoDragStartMouse = {io.MousePos.x, io.MousePos.y};
        st.sclGizmoDragSnx        = snx;
        st.sclGizmoDragSny        = sny;

        // Start-Scales sichern
        if (isMulti)
        {
            st.multiSelectionDragStartScales.clear();
            for (EntityID e : st.multiSelection)
            {
                math::Vec3 s{1.f, 1.f, 1.f};
                if (e.IsValid() && ctx.world.IsAlive(e))
                    if (const auto* tc = ctx.world.Get<TransformComponent>(e))
                        s = tc->localScale;
                st.multiSelectionDragStartScales.push_back(s);
            }
            st.sclGizmoDragStartScale = {1.f, 1.f, 1.f};
        }
        else
        {
            if (const auto* tc = ctx.world.Get<TransformComponent>(st.selectedEntity))
                st.sclGizmoDragStartScale = tc->localScale;
        }
    }

    // Drag fortführen
    if (st.sclGizmoDragAxis >= 0 && ImGui::IsMouseDown(ImGuiMouseButton_Left))
    {
        const float mouseDx    = io.MousePos.x - st.sclGizmoDragStartMouse.x;
        const float mouseDy    = io.MousePos.y - st.sclGizmoDragStartMouse.y;
        const float projPixels = st.sclGizmoDragSnx * mouseDx + st.sclGizmoDragSny * mouseDy;

        constexpr float kSensitivity = 1.f / 200.f;
        const float scaleFactor = 1.f + projPixels * kSensitivity;

        auto applyScale = [&](math::Vec3 s0, TransformComponent* tc)
        {
            if (!tc) return;
            auto clampV = [](float v, float f) -> float
            {
                const float r = v * f;
                return v >= 0.f ? std::max(0.0001f, r) : std::min(-0.0001f, r);
            };
            math::Vec3 newScale = s0;
            switch (st.sclGizmoDragAxis)
            {
                case 0: newScale.x = clampV(s0.x, scaleFactor); break;
                case 1: newScale.y = clampV(s0.y, scaleFactor); break;
                case 2: newScale.z = clampV(s0.z, scaleFactor); break;
                case 3:
                    newScale.x = clampV(s0.x, scaleFactor);
                    newScale.y = clampV(s0.y, scaleFactor);
                    newScale.z = clampV(s0.z, scaleFactor);
                    break;
                default: break;
            }
            tc->localScale = newScale;
            tc->dirty      = true;
        };

        if (isMulti)
        {
            for (size_t i = 0; i < st.multiSelection.size(); ++i)
            {
                const EntityID e = st.multiSelection[i];
                if (!e.IsValid() || !ctx.world.IsAlive(e)) continue;
                const math::Vec3 s0 = (i < st.multiSelectionDragStartScales.size())
                    ? st.multiSelectionDragStartScales[i] : math::Vec3{1.f, 1.f, 1.f};
                applyScale(s0, ctx.world.Get<TransformComponent>(e));
            }
        }
        else
        {
            applyScale(st.sclGizmoDragStartScale,
                       ctx.world.Get<TransformComponent>(st.selectedEntity));
        }
    }
}

// =============================================================================

static void DrawTransformGizmo(EditorFrameContext& ctx)
{
    if (ctx.state.gizmoMode != GizmoMode::Position)
    {
        SetDpadGizmoActive(ctx, false);
        return;
    }

    math::Vec3 center;
    math::Vec3 axes[3];
    math::Quat basisRotation = math::Quat::Identity();

    const bool isMulti = ctx.state.multiSelection.size() >= 2;
    if (isMulti)
    {
        center  = ctx.state.multiSelectionPivot;
        axes[0] = {1.f, 0.f, 0.f};
        axes[1] = {0.f, 1.f, 0.f};
        axes[2] = {0.f, 0.f, 1.f};
    }
    else
    {
        const EntityID selected = ctx.state.selectedEntity;
        if (!selected.IsValid())
        {
            SetDpadGizmoActive(ctx, false);
            return;
        }
        const auto* wtc = ctx.world.Get<WorldTransformComponent>(selected);
        if (!wtc)
        {
            SetDpadGizmoActive(ctx, false);
            return;
        }
        center = wtc->position;
        if (ctx.state.transformSpace == EditorTransformSpace::Local)
        {
            basisRotation = wtc->rotation;
            axes[0] = wtc->rotation.Rotate({1.f, 0.f, 0.f});
            axes[1] = wtc->rotation.Rotate({0.f, 1.f, 0.f});
            axes[2] = wtc->rotation.Rotate({0.f, 0.f, 1.f});
        }
        else
        {
            axes[0] = {1.f, 0.f, 0.f};
            axes[1] = {0.f, 1.f, 0.f};
            axes[2] = {0.f, 0.f, 1.f};
        }
    }

    const float camDist = (center - ctx.state.editorCamera.position).Length();
    const float sz      = std::max(0.15f, camDist * 0.10f);
    if (UpdateDpadGizmo(ctx, center, basisRotation, sz))
        return;

    if (!ctx.debugDraw) return;
    using namespace engine::addons::debug_draw;
    DebugDrawRenderer& dd = *ctx.debugDraw;

    const math::Vec4 kAxisCol[3] = {
        {1.0f, 0.20f, 0.20f, 1.f},   // X – Rot
        {0.2f, 1.00f, 0.20f, 1.f},   // Y – Grün
        {0.2f, 0.50f, 1.00f, 1.f},   // Z – Blau
    };
    const math::Vec4 kHover{1.f, 1.f, 0.f, 1.f};  // Gelb beim Hover

    const float shaftLen = sz * 0.82f;
    const float headW    = sz * 0.07f;

    for (int i = 0; i < 3; ++i)
    {
        const bool       hovered  = (ctx.state.gizmoHoveredAxis == i);
        const bool       dragging = (ctx.state.gizmoDragAxis    == i);
        const math::Vec4 col      = (hovered || dragging) ? kHover : kAxisCol[i];

        const math::Vec3 tip      = center + axes[i] * sz;
        const math::Vec3 headBase = center + axes[i] * shaftLen;

        // Schaft
        dd.Line(center, headBase, col);

        // Pfeilkopf: 4 Linien von Spitze zur Basis
        math::Vec3 t, bu;
        BuildBasis(axes[i], t, bu);
        dd.Line(tip, headBase + t  * headW, col);
        dd.Line(tip, headBase - t  * headW, col);
        dd.Line(tip, headBase + bu * headW, col);
        dd.Line(tip, headBase - bu * headW, col);
    }

    // Mittelpunkt-Marker (kleines weißes Kreuz)
    const float  cSz = sz * 0.06f;
    const math::Vec4 cCol{1.f, 1.f, 1.f, 0.7f};
    dd.Line(center - math::Vec3{cSz, 0.f, 0.f}, center + math::Vec3{cSz, 0.f, 0.f}, cCol);
    dd.Line(center - math::Vec3{0.f, cSz, 0.f}, center + math::Vec3{0.f, cSz, 0.f}, cCol);
    dd.Line(center - math::Vec3{0.f, 0.f, cSz}, center + math::Vec3{0.f, 0.f, cSz}, cCol);
}

// ── Phase 2: ImGui-Input ──────────────────────────────────────────────────────
static void UpdateTransformGizmoInput(EditorFrameContext& ctx)
{
    EditorState& st = ctx.state;

    // Drag beenden
    if (st.gizmoDragAxis >= 0 && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
    {
        CommitPendingSceneEdit(ctx, "gizmo.translate");
        st.gizmoDragAxis = -1;
    }

    if (st.gizmoMode != GizmoMode::Position) { st.gizmoHoveredAxis = -1; return; }

    // Snap-Lock: nach G-Snap keine Bewegung bis Maus vollständig losgelassen
    if (st.snapMoveLocked)
    {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
            st.snapMoveLocked = false;  // freigeben sobald Maus los
        else
        {
            st.gizmoHoveredAxis = -1;
            return;                     // noch gedrückt → alles sperren
        }
    }

    const bool isMulti = st.multiSelection.size() >= 2;

    math::Vec3 center;
    math::Vec3 axes[3];

    if (isMulti)
    {
        center  = st.multiSelectionPivot;
        axes[0] = {1.f, 0.f, 0.f};
        axes[1] = {0.f, 1.f, 0.f};
        axes[2] = {0.f, 0.f, 1.f};
    }
    else
    {
        const EntityID selected = st.selectedEntity;
        if (!selected.IsValid()) { st.gizmoHoveredAxis = -1; return; }
        const auto* wtc = ctx.world.Get<WorldTransformComponent>(selected);
        if (!wtc) { st.gizmoHoveredAxis = -1; return; }
        center = wtc->position;
        if (st.transformSpace == EditorTransformSpace::Local)
        {
            axes[0] = wtc->rotation.Rotate({1.f, 0.f, 0.f});
            axes[1] = wtc->rotation.Rotate({0.f, 1.f, 0.f});
            axes[2] = wtc->rotation.Rotate({0.f, 0.f, 1.f});
        }
        else
        {
            axes[0] = {1.f, 0.f, 0.f};
            axes[1] = {0.f, 1.f, 0.f};
            axes[2] = {0.f, 0.f, 1.f};
        }
    }

    ImGuiIO& io = ImGui::GetIO();
    if (io.WantCaptureMouse) { st.gizmoHoveredAxis = -1; return; }

    const float camDist = (center - st.editorCamera.position).Length();
    const float sz      = std::max(0.15f, camDist * 0.10f);

    // VP-Matrix
    const math::Mat4 view   = BuildEditorViewMatrix(st.editorCamera);
    const float      aspect = (io.DisplaySize.y > 0.f) ? io.DisplaySize.x / io.DisplaySize.y : 1.f;
    const math::Mat4 vp     = math::Mat4::PerspectiveFovRH(
        st.editorCamera.fovDeg * math::DEG_TO_RAD, aspect,
        st.editorCamera.nearPlane, st.editorCamera.farPlane) * view;

    ImVec2 screenCenter;
    if (!WorldToScreenPx(center, vp, io.DisplaySize, screenCenter))
    {
        st.gizmoHoveredAxis = -1;
        return;
    }

    // Hover-Erkennung (nur wenn kein Drag läuft)
    st.gizmoHoveredAxis = -1;
    if (st.gizmoDragAxis < 0)
    {
        // Center-Sphere zuerst (hat Vorrang)
        constexpr float kCenterRadius = 18.f;
        const float dcx = io.MousePos.x - screenCenter.x;
        const float dcy = io.MousePos.y - screenCenter.y;
        if (std::sqrt(dcx * dcx + dcy * dcy) < kCenterRadius)
        {
            st.gizmoHoveredAxis = 3;
        }
        else
        {
            constexpr float kPickDist = 22.f;
            constexpr float kVisualTipScale = 1.45f;
            float bestDist = kPickDist;
            for (int i = 0; i < 3; ++i)
            {
                ImVec2 screenTip;
                if (!WorldToScreenPx(center + axes[i] * (sz * kVisualTipScale), vp, io.DisplaySize, screenTip))
                    continue;
                const float d = DistPointToSegment2D(io.MousePos, screenCenter, screenTip);
                if (d < bestDist) { bestDist = d; st.gizmoHoveredAxis = i; }
            }
        }
    }
    else
    {
        st.gizmoHoveredAxis = st.gizmoDragAxis;
    }

    // Drag starten
    if (st.gizmoHoveredAxis >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
    {
        float snx = 1.f, sny = 0.f;
        if (st.gizmoHoveredAxis < 3)
        {
            const math::Vec3& axisDir = axes[st.gizmoHoveredAxis];
            ImVec2 screenTip;
            if (WorldToScreenPx(center + axisDir, vp, io.DisplaySize, screenTip))
            {
                const float sdx = screenTip.x - screenCenter.x;
                const float sdy = screenTip.y - screenCenter.y;
                const float sLen = std::sqrt(sdx * sdx + sdy * sdy);
                if (sLen > 1e-4f) { snx = sdx / sLen; sny = sdy / sLen; }
            }
        }
        // Achse 3 (Screen-Plane): snx/sny werden beim Drag nicht genutzt

        BeginPendingSceneEdit(ctx, "gizmo.translate", "Objekt bewegt");
        st.gizmoDragAxis       = st.gizmoHoveredAxis;
        st.gizmoDragStartPos   = center;
        st.gizmoDragStartMouse = {io.MousePos.x, io.MousePos.y};
        st.gizmoDragDepth      = camDist;
        st.gizmoDragSnx        = snx;
        st.gizmoDragSny        = sny;

        // Start-Positionen aller Entities sichern (für stabilen Multi-Selektion-Drag)
        if (isMulti)
        {
            st.multiSelectionDragStartPositions.clear();
            for (EntityID e : st.multiSelection)
            {
                math::Vec3 pos{0.f, 0.f, 0.f};
                if (e.IsValid() && ctx.world.IsAlive(e))
                    if (const auto* wtc = ctx.world.Get<WorldTransformComponent>(e))
                        pos = wtc->position;
                st.multiSelectionDragStartPositions.push_back(pos);
            }
            st.gizmoDragChildren.clear();   // Multi-Selektion: keine Kind-Kompensation
        }
        else if (st.selectionSource == SelectionSource::Viewport)
        {
            // Viewport-Selektion: Kinder snapshotten — sie bleiben in World-Space stehen
            SnapshotDirectChildren(ctx, st.selectedEntity);
        }
        else
        {
            st.gizmoDragChildren.clear();
        }
    }

    // Drag fortführen
    if (st.gizmoDragAxis >= 0 && ImGui::IsMouseDown(ImGuiMouseButton_Left))
    {
        const float mouseDx    = io.MousePos.x - st.gizmoDragStartMouse.x;
        const float mouseDy    = io.MousePos.y - st.gizmoDragStartMouse.y;

        const float tanHalfFov = std::tan(30.f * math::DEG_TO_RAD);
        const float unitsPerPx = 2.f * st.gizmoDragDepth * tanHalfFov /
                                 std::max(1.f, io.DisplaySize.y);

        // Achse 3: freies Bewegen in der Kamera-Ebene (Screen-Plane)
        if (st.gizmoDragAxis == 3)
        {
            const math::Mat4 viewMat = BuildEditorViewMatrix(st.editorCamera);
            const math::Vec3 camRight = { viewMat.m[0][0], viewMat.m[1][0], viewMat.m[2][0] };
            const math::Vec3 camUp    = { viewMat.m[0][1], viewMat.m[1][1], viewMat.m[2][1] };
            const math::Vec3 newPos   = st.gizmoDragStartPos
                + camRight * ( mouseDx * unitsPerPx)
                + camUp    * (-mouseDy * unitsPerPx);  // Y invertiert (Screen nach unten)

            if (isMulti)
            {
                for (size_t i = 0; i < st.multiSelection.size(); ++i)
                {
                    const EntityID e = st.multiSelection[i];
                    if (!e.IsValid() || !ctx.world.IsAlive(e)) continue;
                    auto* tc = ctx.world.Get<TransformComponent>(e);
                    if (!tc) continue;
                    const math::Vec3 startPos = (i < st.multiSelectionDragStartPositions.size())
                        ? st.multiSelectionDragStartPositions[i] : math::Vec3{0.f, 0.f, 0.f};
                    const math::Vec3 delta = newPos - st.gizmoDragStartPos;
                    SetWorldPosition(ctx, e, *tc, startPos + delta);
                }
                st.multiSelectionPivot = newPos;
            }
            else
            {
                const EntityID selected = st.selectedEntity;
                auto* tc = ctx.world.Get<TransformComponent>(selected);
                if (tc)
                {
                    SetWorldPosition(ctx, selected, *tc, newPos);
                    if (st.selectionSource == SelectionSource::Viewport && !st.gizmoDragChildren.empty())
                        CompensateChildrenAfterTranslate(ctx, newPos);
                }
            }
            return;  // nicht in Achsen-Drag-Code fallen
        }

        const math::Vec3& axisDir = axes[st.gizmoDragAxis];
        const float projPixels = st.gizmoDragSnx * mouseDx + st.gizmoDragSny * mouseDy;
        const float displacement = projPixels * unitsPerPx;

        if (isMulti)
        {
            for (size_t i = 0; i < st.multiSelection.size(); ++i)
            {
                const EntityID e = st.multiSelection[i];
                if (!e.IsValid() || !ctx.world.IsAlive(e)) continue;
                auto* tc = ctx.world.Get<TransformComponent>(e);
                if (!tc) continue;
                const math::Vec3 startPos = (i < st.multiSelectionDragStartPositions.size())
                    ? st.multiSelectionDragStartPositions[i]
                    : math::Vec3{0.f, 0.f, 0.f};
                SetWorldPosition(ctx, e, *tc, startPos + axisDir * displacement);
            }
            // Pivot mitbewegen
            st.multiSelectionPivot = st.gizmoDragStartPos + axisDir * displacement;
        }
        else
        {
            const EntityID   selected     = st.selectedEntity;
            auto*            tc           = ctx.world.Get<TransformComponent>(selected);
            const math::Vec3 newParentPos = st.gizmoDragStartPos + axisDir * displacement;
            if (tc)
            {
                SetWorldPosition(ctx, selected, *tc, newParentPos);
                // Viewport-Selektion: Kinder in World-Space fixieren
                if (st.selectionSource == SelectionSource::Viewport &&
                    !st.gizmoDragChildren.empty())
                    CompensateChildrenAfterTranslate(ctx, newParentPos);
            }
        }
    }
}

// =============================================================================
// Rotations-Gizmo – drei farbige Ringe um alle drei Achsen
// =============================================================================

static void DrawRotationGizmo(EditorFrameContext& ctx)
{
    if (ctx.state.gizmoMode != GizmoMode::Rotation)
    {
        SetRotateGizmoActive(ctx, false);
        return;
    }

    math::Vec3 center;
    math::Vec3 axes[3];
    math::Quat basisRotation = math::Quat::Identity();
    const bool isMulti = ctx.state.multiSelection.size() >= 2;

    if (isMulti)
    {
        center  = ctx.state.multiSelectionPivot;
        axes[0] = {1.f, 0.f, 0.f};
        axes[1] = {0.f, 1.f, 0.f};
        axes[2] = {0.f, 0.f, 1.f};
    }
    else
    {
        const EntityID selected = ctx.state.selectedEntity;
        if (!selected.IsValid())
        {
            SetRotateGizmoActive(ctx, false);
            return;
        }
        const auto* wtc = ctx.world.Get<WorldTransformComponent>(selected);
        if (!wtc)
        {
            SetRotateGizmoActive(ctx, false);
            return;
        }
        center = wtc->position;
        if (ctx.state.transformSpace == EditorTransformSpace::Local)
        {
            basisRotation = wtc->rotation;
            axes[0] = wtc->rotation.Rotate({1.f, 0.f, 0.f});
            axes[1] = wtc->rotation.Rotate({0.f, 1.f, 0.f});
            axes[2] = wtc->rotation.Rotate({0.f, 0.f, 1.f});
        }
        else
        {
            axes[0] = {1.f, 0.f, 0.f};
            axes[1] = {0.f, 1.f, 0.f};
            axes[2] = {0.f, 0.f, 1.f};
        }
    }

    const float camDist = (center - ctx.state.editorCamera.position).Length();
    const float radius  = std::max(0.15f, camDist * 0.10f);

    if (UpdateRotateGizmo(ctx, center, basisRotation, radius))
        return;

    if (!ctx.debugDraw) return;
    using namespace engine::addons::debug_draw;
    DebugDrawRenderer& dd = *ctx.debugDraw;

    const math::Vec4 kColors[3] = {
        {1.0f, 0.20f, 0.20f, 1.f},
        {0.2f, 1.00f, 0.20f, 1.f},
        {0.2f, 0.50f, 1.00f, 1.f},
    };
    const math::Vec4 kHover{1.f, 1.f, 0.f, 1.f};

    for (int i = 0; i < 3; ++i)
    {
        const bool hov = (ctx.state.rotGizmoHoveredAxis == i ||
                          ctx.state.rotGizmoDragAxis    == i);
        dd.Circle(center, axes[i], radius, 48, hov ? kHover : kColors[i]);
    }
}

static void UpdateRotationGizmoInput(EditorFrameContext& ctx)
{
    EditorState& st = ctx.state;
    if (st.gizmoMode != GizmoMode::Rotation) { st.rotGizmoHoveredAxis = -1; return; }

    // Drag beenden
    if (st.rotGizmoDragAxis >= 0 && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
    {
        CommitPendingSceneEdit(ctx, "gizmo.rotate");
        st.rotGizmoDragAxis = -1;
    }

    // Snap-Lock
    if (st.snapMoveLocked) { st.rotGizmoHoveredAxis = -1; return; }

    const bool isMulti = st.multiSelection.size() >= 2;

    math::Vec3 center;
    if (isMulti)
    {
        center = st.multiSelectionPivot;
    }
    else
    {
        const EntityID selected = st.selectedEntity;
        if (!selected.IsValid()) { st.rotGizmoHoveredAxis = -1; return; }
        const auto* wtc = ctx.world.Get<WorldTransformComponent>(selected);
        const auto* tc  = ctx.world.Get<TransformComponent>(selected);
        if (!wtc || !tc) { st.rotGizmoHoveredAxis = -1; return; }
        center = wtc->position;
    }

    ImGuiIO& io = ImGui::GetIO();
    if (io.WantCaptureMouse) { st.rotGizmoHoveredAxis = -1; return; }

    // center bereits gesetzt oben
    const float      camDist = (center - st.editorCamera.position).Length();
    const float      radius  = std::max(0.15f, camDist * 0.10f);

    // Bei Single-Selection: Local-Space-Achsen aus Entity-Rotation
    math::Vec3 axes[3];
    math::Quat basisRotation = math::Quat::Identity();
    if (!isMulti && st.transformSpace == EditorTransformSpace::Local)
    {
        const auto* wtcSingle = ctx.world.Get<WorldTransformComponent>(st.selectedEntity);
        if (wtcSingle)
        {
            basisRotation = wtcSingle->rotation;
            axes[0] = wtcSingle->rotation.Rotate({1.f, 0.f, 0.f});
            axes[1] = wtcSingle->rotation.Rotate({0.f, 1.f, 0.f});
            axes[2] = wtcSingle->rotation.Rotate({0.f, 0.f, 1.f});
        }
        else
        {
            axes[0] = {1.f, 0.f, 0.f};
            axes[1] = {0.f, 1.f, 0.f};
            axes[2] = {0.f, 0.f, 1.f};
        }
    }
    else
    {
        axes[0] = {1.f, 0.f, 0.f};
        axes[1] = {0.f, 1.f, 0.f};
        axes[2] = {0.f, 0.f, 1.f};
    }

    const math::Mat4 view   = BuildEditorViewMatrix(st.editorCamera);
    const float      aspect = (io.DisplaySize.y > 0.f) ? io.DisplaySize.x / io.DisplaySize.y : 1.f;
    const math::Mat4 vp     = math::Mat4::PerspectiveFovRH(
        st.editorCamera.fovDeg * math::DEG_TO_RAD, aspect,
        st.editorCamera.nearPlane, st.editorCamera.farPlane) * view;

    ImVec2 screenCenter;
    const bool screenCenterValid = WorldToScreenPx(center, vp, io.DisplaySize, screenCenter);

    // Hover: zuerst Center-Sphere prüfen, dann Ringe
    st.rotGizmoHoveredAxis = -1;
    if (st.rotGizmoDragAxis < 0)
    {
        constexpr float kCenterRadius = 18.f;
        if (screenCenterValid)
        {
            const float dcx = io.MousePos.x - screenCenter.x;
            const float dcy = io.MousePos.y - screenCenter.y;
            if (std::sqrt(dcx * dcx + dcy * dcy) < kCenterRadius)
                st.rotGizmoHoveredAxis = 3;
        }

        if (st.rotGizmoHoveredAxis < 0)  // Ringe nur prüfen wenn Sphere nicht getroffen
        {
        constexpr int   kSamples  = 18;
        constexpr float kPickDist = 18.f;
        float bestDist = kPickDist;
        math::Quat visualRotations[3];
        BuildRotateGizmoVisualRotations(basisRotation, visualRotations);

        for (int i = 0; i < 3; ++i)
        {
            ImVec2 prev{};
            bool prevValid = false;
            for (int s = 0; s <= kSamples; ++s)
            {
                const float a = (static_cast<float>(s) / kSamples) * math::TWO_PI;
                const math::Vec3 localPoint{
                    -std::cos(a) * radius,
                    0.f,
                    std::sin(a) * radius
                };
                const math::Vec3 wp = center + visualRotations[i].Rotate(localPoint);
                ImVec2 sp;
                const bool valid = WorldToScreenPx(wp, vp, io.DisplaySize, sp);
                if (valid && prevValid)
                {
                    const float d = DistPointToSegment2D(io.MousePos, prev, sp);
                    if (d < bestDist) { bestDist = d; st.rotGizmoHoveredAxis = i; }
                }
                prev = sp;
                prevValid = valid;
            }
        }
        } // end if (rotGizmoHoveredAxis < 0)
    }
    else
    {
        st.rotGizmoHoveredAxis = st.rotGizmoDragAxis;
    }

    // Drag starten — Achse + Startwinkel einfrieren (oder Trackball)
    if (st.rotGizmoHoveredAxis >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
    {
        ImVec2 sc;
        if (!WorldToScreenPx(center, vp, io.DisplaySize, sc)) return;

        BeginPendingSceneEdit(ctx, "gizmo.rotate", "Objekt rotiert");
        st.rotGizmoDragAxis         = st.rotGizmoHoveredAxis;
        st.rotGizmoDragStartMouse   = {io.MousePos.x, io.MousePos.y};
        st.rotGizmoDragScreenCenter = {sc.x, sc.y};

        if (st.rotGizmoHoveredAxis < 3)
        {
            // Normaler Ring-Drag: Winkel einfrieren
            st.rotGizmoDragWorldAxis  = axes[st.rotGizmoHoveredAxis];
            st.rotGizmoDragStartAngle = std::atan2(-(io.MousePos.y - sc.y), io.MousePos.x - sc.x);
        }
        else
        {
            // Trackball: Kamera-Right/Up beim Drag-Start einfrieren
            const math::Mat4 viewMat = BuildEditorViewMatrix(st.editorCamera);
            // Right = erste Zeile (X), Up = zweite Zeile (Y) der View-Matrix (transponiert = Kamera-Weltachsen)
            st.rotGizmoDragCamRight = { viewMat.m[0][0], viewMat.m[1][0], viewMat.m[2][0] };
            st.rotGizmoDragCamUp    = { viewMat.m[0][1], viewMat.m[1][1], viewMat.m[2][1] };
        }

        if (isMulti)
        {
            // Start-Quats + Positionen aller Entities sichern
            st.multiSelectionDragStartPositions.clear();
            st.multiSelectionDragStartRotations.clear();
            for (EntityID e : st.multiSelection)
            {
                math::Vec3 pos{0.f, 0.f, 0.f};
                math::Quat rot = math::Quat::Identity();
                if (e.IsValid() && ctx.world.IsAlive(e))
                    if (const auto* wtc = ctx.world.Get<WorldTransformComponent>(e))
                    {
                        pos = wtc->position;
                        rot = wtc->rotation;
                    }
                st.multiSelectionDragStartPositions.push_back(pos);
                st.multiSelectionDragStartRotations.push_back(rot);
            }
        }
        else
        {
            st.rotGizmoDragStartQuat = WorldRotation(ctx, st.selectedEntity);
            // Viewport-Selektion: Kinder snapshotten — sie bleiben in World-Space stehen
            if (st.selectionSource == SelectionSource::Viewport)
                SnapshotDirectChildren(ctx, st.selectedEntity);
            else
                st.gizmoDragChildren.clear();
        }
    }

    // Drag fortführen
    if (st.rotGizmoDragAxis >= 0 && ImGui::IsMouseDown(ImGuiMouseButton_Left))
    {
        math::Quat deltaRot;

        if (st.rotGizmoDragAxis < 3)
        {
            // Ring-Drag: Winkel-Delta um eingefrorene Weltachse
            float currentAngle = std::atan2(
                -(io.MousePos.y - st.rotGizmoDragScreenCenter.y),
                  (io.MousePos.x - st.rotGizmoDragScreenCenter.x));
            float deltaAngle = currentAngle - st.rotGizmoDragStartAngle;
            while (deltaAngle >  math::PI)  deltaAngle -= math::TWO_PI;
            while (deltaAngle < -math::PI)  deltaAngle += math::TWO_PI;
            deltaRot = math::Quat::FromAxisAngleDeg(
                st.rotGizmoDragWorldAxis, deltaAngle * math::RAD_TO_DEG);
        }
        else
        {
            // Trackball: Maus-Delta → Rotation um senkrechte Kamera-Achse
            const float mouseDx = io.MousePos.x - st.rotGizmoDragStartMouse.x;
            const float mouseDy = io.MousePos.y - st.rotGizmoDragStartMouse.y;
            constexpr float kSensitivity = 0.4f;  // Grad pro Pixel
            // Rechts ziehen = um Kamera-Up drehen (Yaw),
            // Hoch ziehen   = um Kamera-Right drehen (Pitch, negiert weil Y nach unten)
            const float angleYaw   = mouseDx * kSensitivity;
            const float anglePitch = -mouseDy * kSensitivity;
            const math::Quat yawRot   = math::Quat::FromAxisAngleDeg(st.rotGizmoDragCamUp,    angleYaw);
            const math::Quat pitchRot = math::Quat::FromAxisAngleDeg(st.rotGizmoDragCamRight, anglePitch);
            deltaRot = (yawRot * pitchRot).Normalized();
        }

        if (isMulti)
        {
            // Jede Entity: Rotation um Pivot
            // newPos  = pivot + deltaRot * (startPos - pivot)
            // newQuat = deltaRot * startQuat
            for (size_t i = 0; i < st.multiSelection.size(); ++i)
            {
                const EntityID e = st.multiSelection[i];
                if (!e.IsValid() || !ctx.world.IsAlive(e)) continue;
                auto* tc = ctx.world.Get<TransformComponent>(e);
                if (!tc) continue;

                const math::Vec3 startPos = (i < st.multiSelectionDragStartPositions.size())
                    ? st.multiSelectionDragStartPositions[i] : math::Vec3{0.f, 0.f, 0.f};
                const math::Quat startQuat = (i < st.multiSelectionDragStartRotations.size())
                    ? st.multiSelectionDragStartRotations[i] : math::Quat::Identity();

                const math::Vec3 offset    = startPos - st.multiSelectionPivot;
                const math::Vec3 newOffset = deltaRot.Rotate(offset);
                const math::Vec3 newPos    = st.multiSelectionPivot + newOffset;
                const math::Quat newQuat   = (deltaRot * startQuat).Normalized();

                const math::Mat4 desiredWorld = math::Mat4::TRS(newPos, newQuat,
                    WorldScale(ctx, e));
                SetWorldTransform(ctx, e, *tc, desiredWorld);
            }
        }
        else
        {
            auto* tc = ctx.world.Get<TransformComponent>(st.selectedEntity);
            if (tc)
            {
                const math::Quat newParentRot = (deltaRot * st.rotGizmoDragStartQuat).Normalized();
                SetWorldRotation(ctx, st.selectedEntity, *tc, newParentRot);
                // Viewport-Selektion: Kinder in World-Space fixieren
                if (st.selectionSource == SelectionSource::Viewport &&
                    !st.gizmoDragChildren.empty())
                    CompensateChildrenAfterRotate(ctx, newParentRot);
            }
        }
    }
}

void DrawLightDebugLines(EditorFrameContext& ctx)
{
#ifdef KROM_EDITOR_HAS_IMGUI
    DrawLightGizmos(ctx);
    DrawOBBDebugLines(ctx);
    DrawTransformGizmo(ctx);
    DrawRotationGizmo(ctx);
    DrawScaleGizmo(ctx);
#else
    (void)ctx;
#endif
}

void DrawEditorPanels(EditorFrameContext& ctx)
{
    // Kein Projekt geladen → nur Startbildschirm zeigen, keinen Editor-Panel-Inhalt.
    if (ctx.currentProjectName.empty())
    {
        DrawNoProjectScreen(ctx);
        return;
    }

    FlushPendingMaterialOpen(ctx);
    TickMaterialTextureSync(ctx);

    DrawFileMenuBar(ctx);
    DrawDeleteConfirmDialog(ctx);
    UpdateSnapAxis(ctx);
    UpdateTransformGizmoInput(ctx);   // Position-Gizmo hat Vorrang
    UpdateRotationGizmoInput(ctx);    // Rotations-Gizmo ebenfalls vor Selektion
    UpdateOBBHandles(ctx);
    SelectEntityUnderMouse(ctx);
    UpdateEditorCamera(ctx);

    // G: Snap to Surface — danach Bewegung sperren bis Maus losgelassen
    if (!ImGui::GetIO().WantTextInput &&
        ImGui::IsKeyPressed(ImGuiKey_G, /*repeat=*/false))
    {
        SnapEntityToSurface(ctx);
        ctx.state.gizmoDragAxis   = -1;   // laufenden Drag abbrechen
        ctx.state.snapMoveLocked  = true; // keine neue Bewegung bis Maus los
    }

    DrawProjectSettingsDialog(ctx);
    DrawMaterialLibrary(ctx);

    if (ctx.state.assetBrowser)
        DrawAssetBrowser(ctx, *ctx.state.assetBrowser);

    DrawEntityList(ctx);
    DrawInspector(ctx);
    DrawCameraPreviewWindow(ctx);
    engine::addons::editor::DrawPrefabEditorWindow(ctx);
}

} // namespace engine::renderer::addons::editor

#else // KROM_EDITOR_HAS_IMGUI

namespace engine::renderer::addons::editor {
void DrawEditorPanels(EditorFrameContext&) {}
} // namespace engine::renderer::addons::editor

#endif // KROM_EDITOR_HAS_IMGUI
