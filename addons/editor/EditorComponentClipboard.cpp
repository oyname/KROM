// =============================================================================
// KROM Engine - addons/editor/EditorComponentClipboard.cpp
// Zwischenablage fuer Komponenten im Inspector (siehe Header).
// =============================================================================
#ifdef KROM_EDITOR_HAS_IMGUI

#include "addons/editor/EditorComponentClipboard.hpp"
#include "addons/editor/EditorValueClipboard.hpp"
#include "addons/script/ScriptList.hpp"
#include "addons/script/ScriptRegistry.hpp"

#include <unordered_map>
#include <utility>
#include <vector>
#include <cmath>

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
    EditorTransformSpace positionSpace = EditorTransformSpace::Local;
    EditorTransformSpace rotationSpace = EditorTransformSpace::Local;
    EditorTransformSpace scaleSpace = EditorTransformSpace::Local;
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

struct TransformPose
{
    math::Vec3 position{};
    math::Quat rotation = math::Quat::Identity();
    math::Vec3 scale{1.f, 1.f, 1.f};
};

math::Vec3 Multiply(const math::Vec3& a, const math::Vec3& b)
{
    return {a.x * b.x, a.y * b.y, a.z * b.z};
}

math::Vec3 Divide(const math::Vec3& value, const math::Vec3& scale)
{
    return {std::abs(scale.x) > math::EPSILON ? value.x / scale.x : value.x,
            std::abs(scale.y) > math::EPSILON ? value.y / scale.y : value.y,
            std::abs(scale.z) > math::EPSILON ? value.z / scale.z : value.z};
}

// Read current local data rather than a possibly stale render-frame world cache.
bool ReadWorldPose(const ecs::World& world, EntityID entity, TransformPose& pose)
{
    std::vector<TransformComponent> chain;
    for (auto cursor = entity; world.IsAlive(cursor);)
    {
        const auto* local = world.Get<TransformComponent>(cursor);
        if (!local) break;
        chain.push_back(*local);
        if (chain.size() > world.EntityCount()) return false;
        const auto* parent = world.Get<ParentComponent>(cursor);
        if (!parent) break;
        cursor = parent->parent;
    }
    if (chain.empty()) return false;
    bool root = true;
    for (auto it = chain.rbegin(); it != chain.rend(); ++it)
    {
        if (root)
        {
            pose = {it->localPosition, it->localRotation, it->localScale};
            root = false;
            continue;
        }
        const auto offset = it->inheritParentScale ? Multiply(pose.scale, it->localPosition) : it->localPosition;
        pose.position += pose.rotation.Rotate(offset);
        pose.rotation = (pose.rotation * it->localRotation).Normalized();
        pose.scale = it->inheritParentScale ? Multiply(pose.scale, it->localScale) : it->localScale;
    }
    return true;
}

bool ApplyTransformValue(EditorFrameContext& ctx, EntityID entity, TransformClipboardPart part,
                         EditorTransformSpace space, const TransformPose& pose,
                         const TransformComponent* entire = nullptr, bool asNew = false)
{
    if (!ctx.world.IsAlive(entity)) return false;
    TransformPose parentPose;
    if (space == EditorTransformSpace::World)
        if (const auto* parent = ctx.world.Get<ParentComponent>(entity))
            if (ctx.world.IsAlive(parent->parent) && !ReadWorldPose(ctx.world, parent->parent, parentPose)) return false;
    if (!ctx.world.Has<TransformComponent>(entity))
    {
        if (!asNew) return false;
        ctx.world.Add<TransformComponent>(entity);
    }
    auto* target = ctx.world.Get<TransformComponent>(entity);
    if (entire) target->inheritParentScale = entire->inheritParentScale;
    const auto inverseRotation = parentPose.rotation.Conjugate().Normalized();
    if (part == TransformClipboardPart::All || part == TransformClipboardPart::Position)
    {
        target->localPosition = space == EditorTransformSpace::World
            ? inverseRotation.Rotate(pose.position - parentPose.position) : pose.position;
        if (space == EditorTransformSpace::World && target->inheritParentScale)
            target->localPosition = Divide(target->localPosition, parentPose.scale);
    }
    if (part == TransformClipboardPart::All || part == TransformClipboardPart::Rotation)
        target->localRotation = space == EditorTransformSpace::World
            ? (inverseRotation * pose.rotation).Normalized() : pose.rotation;
    if (part == TransformClipboardPart::All || part == TransformClipboardPart::Scale)
        target->localScale = space == EditorTransformSpace::World && target->inheritParentScale
            ? Divide(pose.scale, parentPose.scale) : pose.scale;
    MarkTransformChanged(*target);
    return true;
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

bool CopyTransformToClipboard(EditorFrameContext& ctx, EntityID entity, TransformClipboardPart part)
{
    const auto* component = ctx.world.Get<TransformComponent>(entity);
    if (!component) return false;
    const TransformComponent source = *component;
    const auto space = ctx.state.transformSpace;
    TransformPose pose{source.localPosition, source.localRotation, source.localScale};
    if (space == EditorTransformSpace::World && !ReadWorldPose(ctx.world, entity, pose)) return false;
    if (part == TransformClipboardPart::Position)
        value_ui::Copy("float3", &pose.position, sizeof(pose.position), static_cast<int>(space));
    else if (part == TransformClipboardPart::Scale)
        value_ui::Copy("float3", &pose.scale, sizeof(pose.scale), static_cast<int>(space));
    else if (part == TransformClipboardPart::Rotation)
        value_ui::Copy("quaternion", &pose.rotation, sizeof(pose.rotation), static_cast<int>(space));
    if (part == TransformClipboardPart::All)
    {
        auto& clip = ComponentClipboard();
        clip.typeId = ComponentTypeID<TransformComponent>::value;
        clip.label = space == EditorTransformSpace::World ? "Transform (World)" : "Transform (Local)";
        clip.apply = [source, pose, space](EditorFrameContext& target, EntityID e, bool asNew) {
            return ApplyTransformValue(target, e, TransformClipboardPart::All, space, pose, &source, asNew);
        };
        clip.presentOn = [](EditorFrameContext& target, EntityID e) { return target.world.Has<TransformComponent>(e); };
        ctx.lastFileMessage = "Kopiert: " + clip.label;
        return true;
    }
    auto& clip = TransformClipboard();
    if (part == TransformClipboardPart::Position)
    { clip.position = pose.position; clip.positionSpace = space; clip.hasPosition = true; }
    if (part == TransformClipboardPart::Rotation)
    { clip.rotation = pose.rotation; clip.rotationSpace = space; clip.hasRotation = true; }
    if (part == TransformClipboardPart::Scale)
    { clip.scale = pose.scale; clip.scaleSpace = space; clip.hasScale = true; }
    return true;
}

bool PasteTransformFromClipboard(EditorFrameContext& ctx, EntityID entity, TransformClipboardPart part)
{
    if (part == TransformClipboardPart::All)
    {
        if (!ClipboardHolds<TransformComponent>()) return false;
        const auto clip = ComponentClipboard();
        return ExecuteSceneMutation(ctx, "Transform eingefuegt", [&]() { clip.apply(ctx, entity, false); });
    }
    const auto clip = TransformClipboard();
    TransformPose pose;
    EditorTransformSpace space;
    if (part == TransformClipboardPart::Position && clip.hasPosition)
    { pose.position = clip.position; space = clip.positionSpace; }
    else if (part == TransformClipboardPart::Rotation && clip.hasRotation)
    { pose.rotation = clip.rotation; space = clip.rotationSpace; }
    else if (part == TransformClipboardPart::Scale && clip.hasScale)
    { pose.scale = clip.scale; space = clip.scaleSpace; }
    else return false;
    if (!ctx.world.Has<TransformComponent>(entity)) return false;
    return ExecuteSceneMutation(ctx, "Transformwert eingefuegt", [&]() { ApplyTransformValue(ctx, entity, part, space, pose); });
}

void DrawTransformValueClipboardMenu(EditorFrameContext& ctx, EntityID entity, TransformClipboardPart part)
{
    const bool rotation = part == TransformClipboardPart::Rotation;
    const char* type = rotation ? "quaternion" : "float3";
    const size_t bytes = rotation ? sizeof(math::Quat) : sizeof(math::Vec3);
    ImGui::PushID(static_cast<int>(part));
    value_ui::Menu("##transform_value", [&] { (void)CopyTransformToClipboard(ctx, entity, part); },
        value_ui::Compatible(type, bytes), [&] {
            TransformPose pose;
            if (rotation) std::memcpy(&pose.rotation, value_ui::Clipboard().bytes.data(), bytes);
            else if (part == TransformClipboardPart::Position) std::memcpy(&pose.position, value_ui::Clipboard().bytes.data(), bytes);
            else std::memcpy(&pose.scale, value_ui::Clipboard().bytes.data(), bytes);
            const auto space = static_cast<EditorTransformSpace>(value_ui::Clipboard().coordinateSpace);
            return ExecuteSceneMutation(ctx, "Transformwert eingefuegt", [&] {
                ApplyTransformValue(ctx, entity, part, space, pose);
            });
        });
    ImGui::PopID();
    value_ui::ForgetLabel();
}

void DrawTransformClipboardMenuItems(EditorFrameContext& ctx, EntityID entity)
{
    if (!ctx.world.Has<TransformComponent>(entity)) return;
    DrawComponentClipboardMenuItems<TransformComponent>(ctx, entity, "Transform");
    ImGui::Separator();
    const auto& clip = TransformClipboard();
    const char* copySpace = ctx.state.transformSpace == EditorTransformSpace::World ? "World" : "Local";
    auto item = [&](const char* name, TransformClipboardPart part, bool available, EditorTransformSpace pasteSpace) {
        const auto copyLabel = std::string(name) + " kopieren (" + copySpace + ")";
        if (ImGui::MenuItem(copyLabel.c_str())) (void)CopyTransformToClipboard(ctx, entity, part);
        const auto pasteLabel = std::string(name) + " einfuegen (" + (pasteSpace == EditorTransformSpace::World ? "World" : "Local") + ")";
        if (ImGui::MenuItem(pasteLabel.c_str(), nullptr, false, available)) (void)PasteTransformFromClipboard(ctx, entity, part);
    };
    item("Position", TransformClipboardPart::Position, clip.hasPosition, clip.positionSpace);
    item("Rotation", TransformClipboardPart::Rotation, clip.hasRotation, clip.rotationSpace);
    item("Skalierung", TransformClipboardPart::Scale, clip.hasScale, clip.scaleSpace);
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
