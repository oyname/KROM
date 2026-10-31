// =============================================================================
// KROM Engine - addons/editor/EditorComponentClipboard.cpp
// Zwischenablage fuer Komponenten im Inspector (siehe Header).
// =============================================================================
#ifdef KROM_EDITOR_HAS_IMGUI

#include "addons/editor/EditorComponentClipboard.hpp"
#include "addons/script/ScriptList.hpp"
#include "addons/script/ScriptRegistry.hpp"

#include <unordered_map>
#include <utility>

namespace engine::renderer::addons::editor {
namespace {

struct TransformClip
{
    bool       hasPosition = false;
    bool       hasRotation = false;
    bool       hasScale    = false;
    math::Vec3 position{};
    math::Quat rotation = math::Quat::Identity();
    math::Vec3 scale{1.f, 1.f, 1.f};
};

struct ScriptClip
{
    bool        valid = false;
    std::string className;
    std::string instanceName;
    std::unordered_map<std::string, script::ScriptFieldValue> fieldValues;
};

TransformClip& TransformClipboard()
{
    static TransformClip clip;
    return clip;
}

ScriptClip& ScriptClipboard()
{
    static ScriptClip clip;
    return clip;
}

void MarkTransformChanged(TransformComponent& t)
{
    t.dirty = true;
    ++t.localVersion;
}

} // namespace

ComponentClip& ComponentClipboard()
{
    static ComponentClip clip;
    return clip;
}

void DrawPasteComponentAsNewItem(EditorFrameContext& ctx, EntityID entity)
{
    const ComponentClip& clip = ComponentClipboard();
    if (!clip.apply || !clip.presentOn || clip.presentOn(ctx, entity))
        return;
    const std::string label = "Einfuegen: " + clip.label;
    if (ImGui::MenuItem(label.c_str()))
    {
        const ComponentClip copy = clip;
        ExecuteSceneMutation(ctx, "Komponente eingefuegt", [&]() { copy.apply(ctx, entity, true); });
    }
}

void DrawTransformClipboardMenuItems(EditorFrameContext& ctx, EntityID entity)
{
    TransformComponent* t = ctx.world.Get<TransformComponent>(entity);
    if (!t)
        return;
    TransformClip& clip = TransformClipboard();

    DrawComponentClipboardMenuItems<TransformComponent>(ctx, entity, "Transform");
    ImGui::Separator();
    if (ImGui::MenuItem("Position kopieren"))
    {
        clip.position = t->localPosition;
        clip.hasPosition = true;
    }
    if (ImGui::MenuItem("Rotation kopieren"))
    {
        clip.rotation = t->localRotation;
        clip.hasRotation = true;
    }
    if (ImGui::MenuItem("Skalierung kopieren"))
    {
        clip.scale = t->localScale;
        clip.hasScale = true;
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Position einfuegen", nullptr, false, clip.hasPosition))
    {
        ExecuteSceneMutation(ctx, "Position eingefuegt", [&]() {
            if (auto* target = ctx.world.Get<TransformComponent>(entity))
            {
                target->localPosition = clip.position;
                MarkTransformChanged(*target);
            }
        });
    }
    if (ImGui::MenuItem("Rotation einfuegen", nullptr, false, clip.hasRotation))
    {
        ExecuteSceneMutation(ctx, "Rotation eingefuegt", [&]() {
            if (auto* target = ctx.world.Get<TransformComponent>(entity))
            {
                target->localRotation = clip.rotation;
                MarkTransformChanged(*target);
            }
        });
    }
    if (ImGui::MenuItem("Skalierung einfuegen", nullptr, false, clip.hasScale))
    {
        ExecuteSceneMutation(ctx, "Skalierung eingefuegt", [&]() {
            if (auto* target = ctx.world.Get<TransformComponent>(entity))
            {
                target->localScale = clip.scale;
                MarkTransformChanged(*target);
            }
        });
    }
}

void DrawScriptClipboardMenuItems(EditorFrameContext& ctx, EntityID entity, size_t index)
{
    auto* list = ctx.world.Get<script::ScriptList>(entity);
    if (!list || index >= list->Instances_Mutable().size())
        return;
    ScriptClip& clip = ScriptClipboard();
    const script::ScriptInstance& instance = list->Instances_Mutable()[index];

    if (ImGui::MenuItem("Script kopieren"))
    {
        clip.valid = true;
        clip.className = instance.className;
        clip.instanceName = instance.instanceName;
        clip.fieldValues = instance.fieldValues;
        ctx.lastFileMessage = "Kopiert: Script " + instance.instanceName;
    }
    const bool canPaste = clip.valid && clip.className == instance.className;
    if (ImGui::MenuItem("Werte einfuegen", nullptr, false, canPaste))
    {
        const ScriptClip copy = clip;
        ExecuteSceneMutation(ctx, "Script-Werte eingefuegt", [&]() {
            auto* target = ctx.world.Get<script::ScriptList>(entity);
            if (!target || index >= target->Instances_Mutable().size())
                return;
            target->Instances_Mutable()[index].fieldValues = copy.fieldValues;
            if (ctx.scriptRegistry && ctx.scriptRegistry->Has(copy.className))
                target->ApplyStoredFieldValues(index, *ctx.scriptRegistry);
        });
    }
}

void DrawPasteScriptAsNewItem(EditorFrameContext& ctx, EntityID entity)
{
    const ScriptClip& clip = ScriptClipboard();
    if (!clip.valid)
        return;
    const std::string label = "Einfuegen: Script " + clip.instanceName;
    if (!ImGui::MenuItem(label.c_str()))
        return;
    const ScriptClip copy = clip;
    ExecuteSceneMutation(ctx, "Script eingefuegt", [&]() {
        auto* list = ctx.world.Get<script::ScriptList>(entity);
        if (!list)
        {
            script::ScriptList newList;
            newList.SetOwnerEntity(entity);
            ctx.world.Add<script::ScriptList>(entity, std::move(newList));
            list = ctx.world.Get<script::ScriptList>(entity);
        }
        if (!list)
            return;
        const bool live = ctx.scriptRegistry && ctx.scriptRegistry->Has(copy.className);
        if (live)
            list->Add(copy.className, entity, *ctx.scriptRegistry);
        else
            list->AddNameOnly(copy.className, copy.instanceName);
        const size_t index = list->Instances_Mutable().size() - 1u;
        script::ScriptInstance& added = list->Instances_Mutable()[index];
        added.instanceName = copy.instanceName;
        added.fieldValues = copy.fieldValues;
        if (live)
            list->ApplyStoredFieldValues(index, *ctx.scriptRegistry);
    });
}

} // namespace engine::renderer::addons::editor

#endif // KROM_EDITOR_HAS_IMGUI
