#include "addons/editor/EditorAnimationWindow.hpp"
#include "addons/editor/EditorUIInternal.hpp"
#include "addons/editor/EditorFileNaming.hpp"

#ifdef KROM_EDITOR_HAS_IMGUI

#include "addons/animation/AnimationComponents.hpp"
#include "addons/animation/AnimationSystem.hpp"
#include "addons/editor/EditorAnimationControllers.hpp"
#include "addons/editor/EditorControllerGraph.hpp"
#include "addons/mesh_renderer/MeshRendererComponents.hpp"
#include "addons/particles/ParticleComponents.hpp"
#include "addons/mesh_renderer/MeshRendererSerialization.hpp"
#include "addons/debug_draw/DebugDraw.hpp"
#include "renderer/RenderLayers.hpp"
#include "addons/editor/EditorCard.hpp"
#include "addons/editor/EditorCommands.hpp"
#include "addons/editor/EditorPreferences.hpp"
#include "addons/editor/EditorIcons.hpp"
#include "addons/editor/EditorModelClips.hpp"
#include "addons/editor/EditorTheme.hpp"
#include "addons/editor/EditorValueClipboard.hpp"
#include "animation/AnimationClipEditing.hpp"
#include "animation/AnimationClipFile.hpp"
#include "animation/AnimationEvaluator.hpp"
#include "animation/AnimationSampler.hpp"
#include "animation/ModelClipSettings.hpp"
#include "animation/SkeletonPoseEvaluator.hpp"
#include "assets/AssetPipeline.hpp"
#include "assets/AssetRegistry.hpp"
#include "core/PathUtf8.hpp"
#include "ecs/Components.hpp"
#include "imgui_internal.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cfloat>
#include <cstring>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine::renderer::addons::editor {

namespace {

namespace anim = ::engine::animation;
using assets::AnimationChannel;
using assets::AnimationClip;
using assets::AnimationInterpolation;
using assets::AnimationTargetProperty;

constexpr float kSnapRate = 30.f;

struct Pose
{
    math::Vec3 position;
    math::Quat rotation;
    math::Vec3 scale;
};

// The editor renders in linear space; raw sRGB values come out washed out.
ImU32 LinearColor(int r, int g, int b, float alpha = 1.f)
{
    return ImGui::GetColorU32(ImVec4(std::pow(r / 255.f, 2.2f), std::pow(g / 255.f, 2.2f), std::pow(b / 255.f, 2.2f), alpha));
}

struct WindowState
{
    bool session = false;
    bool dirty = false;
    std::string clipPath;                // the .anim file, relative to Assets
    std::vector<AnimationClip> clips;    // every clip of the animation
    int activeClip = 0;
    AnimationClip clip;                  // working copy of clips[activeClip]
    EntityID target = NULL_ENTITY;       // scene object the animation is assigned to on save
    std::string targetGuid;
    std::string targetName;
    EntityID copyRoot = NULL_ENTITY;     // hidden copy that is posed and shown in the window
    EntityID copyParent = NULL_ENTITY;   // stands in for the original's parent chain
    int copyAge = 0;                     // frames since the copy was built

    // Model mode: clipPath is a model file (*.glb/*.gltf). Its clips are shown
    // read-only; what is edited are their settings ("<model>.clips").
    bool modelMode = false;
    anim::ModelClipSettings modelSettings;  // clips[i] belongs to modelSettings.clips[i]
    bool modelClipsStale = false;           // rebuild the shown clips from the settings
    bool modelGestureActive = false;
    anim::ModelClipSettings modelGestureBefore;

    // Controller mode: the copy plays the controller open in the graph window
    // with the runtime's own AnimationSystem; nothing is edited or saved here.
    bool controllerMode = false;
    std::string controllerPath;          // the .animctrl, relative to Assets
    std::unique_ptr<::engine::addons::animation::AnimationSystem> controllerSystem;
    const assets::AssetRegistry* controllerRegistry = nullptr;
    ::engine::addons::animation::AnimationControllerAsset controllerBound; // last compiled, without graph positions
    bool controllerHasBound = false;
    uint64_t controllerVersion = ~0ull;  // graph version controllerBound was taken from
    bool controllerPaused = false;
    std::string controllerAsked;         // preview start already requested for this path + object
    EntityID followSelected = NULL_ENTITY; // selection FollowSelection reacted to last
    // Clips opened on purpose (inspector, asset browser) win over the controller
    // preview until the selection changes or the graph's Preview button is used.
    bool controllerSuppressed = false;
    // The graph's Preview button asked for the controller preview; it runs only
    // then, so clips stay editable (with gizmos) otherwise. Stop, closing the
    // graph or closing this window clear it.
    bool controllerRequested = false;
    EntityID suppressedSelection = NULL_ENTITY;
    int keepControllerFocus = 0;         // frames: opened from the graph, which keeps the focus
    std::string previewEventName;        // last event the controller preview fired
    double previewEventTime = -100.0;

    // Clip events (timeline band, inspector card)
    int selectedEvent = -1;              // index into clip.events
    bool eventDragging = false;
    float eventPopupTime = 0.f;
    bool eventPopupOnMarker = false;
    std::vector<std::pair<float, float>> eventMarkers; // screen positions of the last frame (tests)

    std::function<void()> pendingAction; // waits for the unsaved-changes answer
    bool askUnsaved = false;
    char renameBuffer[128]{};

    float time = 0.f;
    bool playing = false;
    bool recording = false;
    bool poseDirty = false;

    float pixelsPerSecond = 160.f;
    float viewStart = 0.f;
    std::set<std::string> expanded;

    std::vector<anim::KeyRef> selection;
    bool dragging = false;
    bool scrubbing = false;
    float dragMouseX = 0.f;
    AnimationClip dragStartClip;
    std::vector<anim::KeyRef> dragStartKeys;

    bool gestureActive = false;
    AnimationClip gestureBefore;

    int popupChannelGroup = -1;
    int popupChannel = -1;
    float popupTime = 0.f;

    std::unordered_map<std::string, Pose> baseline; // rest pose of the copy
    std::unordered_map<std::string, Pose> applied;

    // 3D preview
    bool framed = false;
    float yawDeg = 30.f;
    float pitchDeg = 12.f;
    float distance = 3.f;
    math::Vec3 center{0.f, 1.f, 0.f};
    float radius = 1.f;
    float groundY = 0.f;
    std::string selectedNode;
    bool orthographic = false;
    const char* viewName = nullptr;      // locale key of a fixed axis view
    bool previewHovered = false;
    bool previewVisible = false;  // the 3D view was drawn this frame
    bool ownsGizmos = false;      // the shared gizmos currently work in the preview
    bool timelineShown = false;   // the Timeline window was drawn since the editor opened
    ImVec2 previewMin{};
    ImVec2 previewSize{1.f, 1.f};
};

WindowState& W()
{
    static WindowState state;
    return state;
}

struct TargetNodes
{
    std::vector<EntityID> all;
    std::unordered_map<std::string, EntityID> byName;
    EntityID skinEntity = NULL_ENTITY;
    const assets::SkeletonAsset* skeleton = nullptr;
};

TargetNodes CollectNodes(EditorFrameContext& ctx, EntityID root)
{
    TargetNodes nodes;
    nodes.all.push_back(root);
    ctx.world.View<ParentComponent>([&](EntityID id, ParentComponent& parent) {
        EntityID current = parent.parent;
        for (uint32_t depth = 0u; current.IsValid() && depth < 1024u; ++depth)
        {
            if (current == root)
            {
                nodes.all.push_back(id);
                return;
            }
            const auto* next = ctx.world.Get<ParentComponent>(current);
            current = next ? next->parent : NULL_ENTITY;
        }
    });
    for (const EntityID id : nodes.all)
    {
        if (const auto* name = ctx.world.Get<NameComponent>(id); name && !name->name.empty())
            nodes.byName.emplace(name->name, id);
        if (!nodes.skeleton)
            if (const auto* skin = ctx.world.Get<SkinComponent>(id))
                if (const auto* skeleton = ctx.registry.skeletons.Get(skin->skeleton))
                {
                    nodes.skinEntity = id;
                    nodes.skeleton = skeleton;
                }
    }

    // Imported rigs can keep the skinned mesh beside the bones instead of below
    // them. Such a mesh belongs here if this hierarchy holds all of its bones.
    if (!nodes.skeleton)
    {
        const bool rootIsCopy = ctx.world.Has<EditorRuntimeGizmoTag>(root);
        const auto* rootWorld = ctx.world.Get<WorldTransformComponent>(root);
        float bestDistance = FLT_MAX;
        ctx.world.View<SkinComponent>([&](EntityID id, SkinComponent& skin) {
            if (ctx.world.Has<EditorRuntimeGizmoTag>(id) != rootIsCopy)
                return;
            const auto* skeleton = ctx.registry.skeletons.Get(skin.skeleton);
            if (!skeleton || skeleton->bones.empty())
                return;
            for (const assets::Bone& bone : skeleton->bones)
                if (!nodes.byName.count(bone.name))
                    return;
            const auto* skinWorld = ctx.world.Get<WorldTransformComponent>(id);
            const float distance = rootWorld && skinWorld ? (skinWorld->position - rootWorld->position).Length() : 0.f;
            if (distance < bestDistance) // several instances of one rig: the nearest one
            {
                bestDistance = distance;
                nodes.skinEntity = id;
                nodes.skeleton = skeleton;
            }
        });
        if (nodes.skeleton)
            nodes.all.push_back(nodes.skinEntity);
    }
    return nodes;
}

int BoneIndex(const assets::SkeletonAsset& skeleton, const std::string& name)
{
    for (size_t i = 0; i < skeleton.bones.size(); ++i)
        if (skeleton.bones[i].name == name)
            return static_cast<int>(i);
    return -1;
}

bool ReadPose(EditorFrameContext& ctx, EntityID id, Pose& out)
{
    const auto* transform = ctx.world.Get<TransformComponent>(id);
    if (!transform)
        return false;
    out = {transform->localPosition, transform->localRotation, transform->localScale};
    return true;
}

void WritePose(EditorFrameContext& ctx, EntityID id, const Pose& pose)
{
    if (auto* transform = ctx.world.Get<TransformComponent>(id))
    {
        transform->localPosition = pose.position;
        transform->localRotation = pose.rotation;
        transform->localScale = pose.scale;
        transform->dirty = true;
        ++transform->localVersion;
    }
}

bool SameVec(const math::Vec3& a, const math::Vec3& b)
{
    constexpr float eps = 1e-4f;
    return std::fabs(a.x - b.x) <= eps && std::fabs(a.y - b.y) <= eps && std::fabs(a.z - b.z) <= eps;
}

bool SameQuat(const math::Quat& a, const math::Quat& b)
{
    return std::fabs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w) >= 1.f - 1e-6f;
}

std::filesystem::path AbsoluteClipPath(EditorFrameContext& ctx, const std::string& relative)
{
    return ctx.assetPipeline->GetAssetRoot() / PathFromUtf8(relative);
}

std::string ClipAssetPath(const std::string& file, const std::string& clip)
{
    return file + "#" + clip;
}

void RegisterClip(EditorFrameContext& ctx, const std::string& key, const AnimationClip& clip)
{
    // Keeps the registry current so Play uses the latest save without a reload.
    if (auto* existing = ctx.registry.animations.Get(ctx.registry.FindAnimationByPath(key)))
    {
        *existing = clip;
        existing->path = key;
        existing->state = assets::AssetState::Loaded;
    }
    else
    {
        auto copy = std::make_unique<AnimationClip>(clip);
        copy->path = key;
        copy->state = assets::AssetState::Loaded;
        (void)ctx.registry.GetOrAddAnimation(key, std::move(copy));
    }
}

bool WriteAnimationFile(EditorFrameContext& ctx, const std::string& relative, const std::vector<AnimationClip>& clips)
{
    const std::filesystem::path path = AbsoluteClipPath(ctx, relative);
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << anim::SerializeAnimationSet(clips);
    if (!out.good())
    {
        ctx.lastFileMessage = Translate("status.save_failed");
        return false;
    }
    for (const AnimationClip& clip : clips)
        RegisterClip(ctx, ClipAssetPath(relative, clip.name), clip);
    if (!clips.empty())
        RegisterClip(ctx, relative, clips.front()); // older scenes reference the bare file
    return true;
}

void DestroyCopy(EditorFrameContext& ctx)
{
    WindowState& w = W();
    if (w.copyRoot.IsValid() && ctx.world.IsAlive(w.copyRoot) && ctx.world.Has<EditorRuntimeGizmoTag>(w.copyRoot))
        for (const EntityID id : CollectNodes(ctx, w.copyRoot).all)
            ctx.world.DestroyEntity(id);
    if (w.copyParent.IsValid() && ctx.world.IsAlive(w.copyParent) && ctx.world.Has<EditorRuntimeGizmoTag>(w.copyParent))
        ctx.world.DestroyEntity(w.copyParent);
    w.copyParent = NULL_ENTITY;
    w.copyRoot = NULL_ENTITY;
}

bool ResolveTarget(EditorFrameContext& ctx)
{
    WindowState& w = W();
    if (w.target.IsValid() && ctx.world.IsAlive(w.target) && ctx.world.Has<TransformComponent>(w.target))
        return true;
    // Undo and Stop rebuild entities with new ids.
    EntityID found = NULL_ENTITY;
    if (!w.targetGuid.empty())
        ctx.world.View<GuidComponent>([&](EntityID id, GuidComponent& guid) {
            if (guid.guid == w.targetGuid)
                found = id;
        });
    if (!found.IsValid() && !w.targetName.empty())
        ctx.world.View<NameComponent>([&](EntityID id, NameComponent& name) {
            if (name.name == w.targetName && !ctx.world.Has<EditorRuntimeGizmoTag>(id))
                found = id;
        });
    if (!found.IsValid())
        return false;
    w.target = found;
    w.poseDirty = true;
    return true;
}

void EndSession(EditorFrameContext& ctx)
{
    WindowState& w = W();
    DestroyCopy(ctx);
    w.session = false;
    w.dirty = false;
    w.clips.clear();
    w.activeClip = 0;
    w.playing = false;
    w.recording = false;
    w.dragging = false;
    w.gestureActive = false;
    w.selection.clear();
    w.baseline.clear();
    w.applied.clear();
    w.target = NULL_ENTITY;
    w.targetGuid.clear();
    w.targetName.clear();
    w.clipPath.clear();
    w.clip = {};
    w.modelMode = false;
    w.modelSettings = {};
    w.modelClipsStale = false;
    w.modelGestureActive = false;
    w.controllerMode = false;
    w.controllerPath.clear();
    w.controllerHasBound = false;
    w.controllerVersion = ~0ull;
    w.controllerPaused = false;
    w.selectedEvent = -1;
    w.eventDragging = false;
    w.previewEventName.clear();
    ctx.state.animationPreviewActive = false;
}

void BeginSession(EditorFrameContext& ctx, EntityID target, std::string relative, std::vector<AnimationClip> clips, int active)
{
    EndSession(ctx);
    WindowState& w = W();
    w.session = true;
    w.framed = false;
    w.selectedNode.clear();
    w.target = target;
    if (const auto* guid = ctx.world.Get<GuidComponent>(target))
        w.targetGuid = guid->guid;
    if (const auto* name = ctx.world.Get<NameComponent>(target))
        w.targetName = name->name;
    w.clipPath = std::move(relative);
    w.clips = std::move(clips);
    // A clip made for one object also fits another one, as it does at runtime.
    const TargetNodes targetNodes = CollectNodes(ctx, target);
    if (const auto* targetName = ctx.world.Get<NameComponent>(target); targetName && !targetNodes.skeleton)
        for (AnimationClip& clip : w.clips)
        {
            if (clip.channels.empty() || targetNodes.byName.count(clip.channels.front().targetName))
                continue;
            const std::string& only = clip.channels.front().targetName;
            if (std::all_of(clip.channels.begin(), clip.channels.end(), [&](const AnimationChannel& c) { return c.targetName == only; }))
            {
                const std::string original = only;
                for (AnimationChannel& channel : clip.channels)
                    if (channel.targetName == original)
                        channel.targetName = targetName->name;
            }
        }
    w.activeClip = std::clamp(active, 0, static_cast<int>(w.clips.size()) - 1);
    w.clip = w.clips[static_cast<size_t>(w.activeClip)];
    w.time = 0.f;
    w.viewStart = 0.f;
    w.poseDirty = true;
    w.recording = true;
}

bool SeedClip(const assets::SkeletonAsset* skeleton,
              const std::unordered_map<std::string, Pose>& rest, AnimationClip& clip, std::string& missing);

// A model clip as the editor shows it: one value per key, and for rigs a channel
// for every bone (the skeleton sampler treats a bone without channel as identity).
AnimationClip ToEditableModelClip(const AnimationClip& source, const TargetNodes& nodes,
                                  const std::unordered_map<std::string, Pose>& rest)
{
    AnimationClip clip;
    clip.name = source.name;
    clip.name.erase(std::remove(clip.name.begin(), clip.name.end(), '#'), clip.name.end());
    clip.duration = source.duration;
    clip.looping = source.looping;
    clip.rootMotion = source.rootMotion;
    clip.events = source.events;
    for (const AnimationChannel& from : source.channels)
    {
        if (nodes.skeleton && (from.targetIndex < 0 || from.targetIndex >= static_cast<int32_t>(nodes.skeleton->bones.size())))
            continue;
        AnimationChannel channel;
        channel.targetIndex = from.targetIndex;
        channel.targetName = nodes.skeleton ? nodes.skeleton->bones[static_cast<size_t>(from.targetIndex)].name : from.targetName;
        channel.property = from.property;
        const bool cubic = from.interpolation == AnimationInterpolation::CubicSpline;
        channel.interpolation = cubic ? AnimationInterpolation::Linear : from.interpolation;
        channel.times = from.times;
        const size_t stride = cubic ? 3u : 1u;
        const size_t offset = cubic ? 1u : 0u; // the value sits between the two tangents
        for (size_t k = 0; k < from.times.size(); ++k)
        {
            if (anim::IsRotationChannel(from)) channel.quatValues.push_back(from.quatValues[k * stride + offset]);
            else                               channel.vec3Values.push_back(from.vec3Values[k * stride + offset]);
        }
        clip.channels.push_back(std::move(channel));
    }
    if (!nodes.skeleton)
        return clip;
    for (size_t bone = 0; bone < nodes.skeleton->bones.size(); ++bone)
    {
        const std::string& boneName = nodes.skeleton->bones[bone].name;
        const auto restPose = rest.find(boneName);
        if (restPose == rest.end())
            continue;
        for (const AnimationTargetProperty property : {AnimationTargetProperty::Translation,
                                                       AnimationTargetProperty::Rotation,
                                                       AnimationTargetProperty::Scale})
        {
            if (anim::FindChannel(clip, boneName, property) >= 0)
                continue;
            AnimationChannel channel;
            channel.targetName = boneName;
            channel.targetIndex = static_cast<int32_t>(bone);
            channel.property = property;
            anim::SetKey(channel, 0.f,
                         property == AnimationTargetProperty::Scale ? restPose->second.scale : restPose->second.position,
                         restPose->second.rotation);
            clip.channels.push_back(std::move(channel));
        }
    }
    return clip;
}

// Model file of a rig, from its skeleton reference.
std::string RigModelPath(EditorFrameContext& ctx, const TargetNodes& nodes)
{
    const auto* skin = nodes.skinEntity.IsValid() ? ctx.world.Get<SkinComponent>(nodes.skinEntity) : nullptr;
    if (!skin || skin->skeletonAssetPath.empty())
        return {};
    return skin->skeletonAssetPath.substr(0, skin->skeletonAssetPath.rfind('#'));
}

// The model's animations as registered ("<model>#animation/<i>"), imported on first use.
std::vector<const AnimationClip*> ModelSourceClips(EditorFrameContext& ctx, const std::string& model)
{
    std::vector<const AnimationClip*> sources;
    if (!ctx.assetPipeline || model.empty())
        return sources;
    RefreshModelClipAssets(*ctx.assetPipeline, ctx.registry, model,
                           GetModelClips(ctx.assetPipeline->GetAssetRoot(), model).settings);
    for (int index = 0;; ++index)
    {
        const auto* source = ctx.registry.animations.Get(
            ctx.registry.FindAnimationByPath(model + "#animation/" + std::to_string(index)));
        if (!source)
            break;
        sources.push_back(source);
    }
    return sources;
}

// Clips the rig's model file ships with, as editable keyframe copies for a .anim.
std::vector<AnimationClip> LoadModelClips(EditorFrameContext& ctx, const TargetNodes& nodes,
                                          const std::unordered_map<std::string, Pose>& rest)
{
    std::vector<AnimationClip> result;
    if (!nodes.skeleton)
        return result;
    int index = 0;
    for (const AnimationClip* source : ModelSourceClips(ctx, RigModelPath(ctx, nodes)))
    {
        AnimationClip clip = ToEditableModelClip(*source, nodes, rest);
        if (clip.name.empty())
            clip.name = "Clip " + std::to_string(index + 1);
        clip.looping = true;
        result.push_back(std::move(clip));
        ++index;
    }
    return result;
}

// Shown clips of the model mode, built from the edited settings (not the saved ones).
void RebuildModelClips(EditorFrameContext& ctx, const TargetNodes& nodes)
{
    WindowState& w = W();
    w.modelClipsStale = false;
    const std::vector<const AnimationClip*> sources = ModelSourceClips(ctx, w.clipPath);
    w.clips.clear();
    for (const anim::ModelClipEntry& entry : w.modelSettings.clips)
    {
        std::optional<AnimationClip> built = anim::BuildModelClip(entry, w.modelSettings.fps, sources);
        AnimationClip clip = built ? ToEditableModelClip(*built, nodes, w.baseline) : AnimationClip{};
        clip.name = entry.name;
        clip.looping = entry.looping;
        clip.rootMotion = entry.rootMotion;
        w.clips.push_back(std::move(clip));
    }
    if (w.clips.empty())
    {
        AnimationClip none;
        none.name = Translate("animation.no_clip");
        w.clips.push_back(std::move(none));
    }
    w.activeClip = std::clamp(w.activeClip, 0, static_cast<int>(w.clips.size()) - 1);
    w.clip = w.clips[static_cast<size_t>(w.activeClip)];
    w.time = std::min(w.time, w.clip.duration);
    w.selection.clear();
    w.poseDirty = true;
}

// The window poses a hidden copy, so the scene stays untouched while animating.
bool EnsureCopy(EditorFrameContext& ctx)
{
    WindowState& w = W();
    if (w.copyRoot.IsValid() && ctx.world.IsAlive(w.copyRoot) && ctx.world.Has<EditorRuntimeGizmoTag>(w.copyRoot))
        return true;
    if (!ResolveTarget(ctx))
        return false;

    const TargetNodes source = CollectNodes(ctx, w.target);
    std::unordered_map<uint32_t, EntityID> copies;
    for (const EntityID id : source.all)
    {
        const auto* transform = ctx.world.Get<TransformComponent>(id);
        if (!transform)
            continue;
        const EntityID copy = ctx.world.CreateEntity();
        copies[id.value] = copy;
        ctx.world.Add<EditorRuntimeGizmoTag>(copy);
        ctx.world.Add<ActiveComponent>(copy, ActiveComponent{true});
        ctx.world.Add<TransformComponent>(copy, *transform);
        ctx.world.Get<TransformComponent>(copy)->dirty = true;
        ctx.world.Add<WorldTransformComponent>(copy);
        if (const auto* name = ctx.world.Get<NameComponent>(id))
            ctx.world.Add<NameComponent>(copy, *name);
        if (const auto* mesh = ctx.world.Get<MeshComponent>(id))
        {
            MeshComponent copiedMesh = *mesh;
            copiedMesh.visible = true;
            copiedMesh.castShadows = false;
            copiedMesh.layerMask = renderer::LAYER_EDITOR_ANIMATION_PREVIEW;
            ctx.world.Add<MeshComponent>(copy, std::move(copiedMesh));
        }
        if (const auto* material = ctx.world.Get<MaterialComponent>(id))
            ctx.world.Add<MaterialComponent>(copy, *material);
        if (const auto* bounds = ctx.world.Get<BoundsComponent>(id))
            ctx.world.Add<BoundsComponent>(copy, *bounds);
        if (const auto* emitter = ctx.world.Get<engine::ParticleEmitterComponent>(id))
            ctx.world.Add<engine::ParticleEmitterComponent>(copy, *emitter);
        if (const auto* skin = ctx.world.Get<SkinComponent>(id))
        {
            SkinComponent copiedSkin{};
            copiedSkin.skeleton = skin->skeleton;
            copiedSkin.skeletonAssetPath = skin->skeletonAssetPath;
            ctx.world.Add<SkinComponent>(copy, std::move(copiedSkin));
        }
    }
    for (const EntityID id : source.all)
    {
        const auto* parent = ctx.world.Get<ParentComponent>(id);
        const auto copy = copies.find(id.value);
        if (!parent || copy == copies.end() || id == w.target)
            continue;
        const auto copiedParent = copies.find(parent->parent.value);
        if (copiedParent == copies.end())
            continue;
        ctx.world.Add<ParentComponent>(copy->second, ParentComponent{copiedParent->second});
        if (!ctx.world.Has<ChildrenComponent>(copiedParent->second))
            ctx.world.Add<ChildrenComponent>(copiedParent->second);
        ctx.world.Get<ChildrenComponent>(copiedParent->second)->Add(copy->second);
    }
    const auto root = copies.find(w.target.value);
    if (root == copies.end())
        return false;
    w.copyRoot = root->second;
    w.framed = false;
    w.copyAge = 0;
    w.poseDirty = true;

    // Keys are local to the parent, so the copy needs the same parent space: a
    // stand-in entity carries the world transform of the original's parent.
    if (const auto* parent = ctx.world.Get<ParentComponent>(w.target))
        if (const auto* parentWorld = ctx.world.Get<WorldTransformComponent>(parent->parent))
        {
            w.copyParent = ctx.world.CreateEntity();
            ctx.world.Add<EditorRuntimeGizmoTag>(w.copyParent);
            ctx.world.Add<ActiveComponent>(w.copyParent, ActiveComponent{true});
            TransformComponent parentTransform;
            parentTransform.localPosition = parentWorld->position;
            parentTransform.localRotation = parentWorld->rotation;
            parentTransform.localScale = parentWorld->scale;
            parentTransform.dirty = true;
            ctx.world.Add<TransformComponent>(w.copyParent, parentTransform);
            ctx.world.Add<WorldTransformComponent>(w.copyParent);
            ctx.world.Add<ParentComponent>(w.copyRoot, ParentComponent{w.copyParent});
            ctx.world.Add<ChildrenComponent>(w.copyParent);
            ctx.world.Get<ChildrenComponent>(w.copyParent)->Add(w.copyRoot);
        }

    // A single object is selected right away so its gizmo is visible.
    if (const auto* rootName = ctx.world.Get<NameComponent>(w.copyRoot); rootName && w.selectedNode.empty() &&
        !w.controllerMode && !CollectNodes(ctx, w.copyRoot).skeleton)
        w.selectedNode = rootName->name;

    w.baseline.clear();
    for (const auto& [name, id] : CollectNodes(ctx, w.copyRoot).byName)
    {
        Pose pose;
        if (ReadPose(ctx, id, pose))
            w.baseline[name] = pose;
    }

    if (w.controllerMode)
    {
        // The copy gets an animator of its own that only the preview steps
        // (playing stays off, so a running game's AnimationSystem leaves it alone).
        AnimationPlayerComponent preview;
        preview.controllerAssetPath = w.controllerPath;
        if (const auto* original = ctx.world.Get<AnimationPlayerComponent>(w.target))
            preview.speed = original->speed;
        preview.playing = false;
        ctx.world.Add<AnimationPlayerComponent>(w.copyRoot, std::move(preview));
        w.controllerHasBound = false;
        return true;
    }

    if (w.modelMode)
    {
        RebuildModelClips(ctx, CollectNodes(ctx, w.copyRoot));
        return true;
    }
    // A rig clip without keys (older files) shows the model's own motion, or the rest pose.
    if (const TargetNodes copied = CollectNodes(ctx, w.copyRoot); copied.skeleton)
    {
        const std::vector<AnimationClip> modelClips = LoadModelClips(ctx, copied, w.baseline);
        for (AnimationClip& clip : w.clips)
            if (clip.channels.empty())
            {
                if (!modelClips.empty())
                {
                    clip.channels = modelClips.front().channels;
                    clip.duration = modelClips.front().duration;
                }
                else
                {
                    std::string missing;
                    SeedClip(copied.skeleton, w.baseline, clip, missing);
                }
                w.dirty = true;
            }
        w.clip = w.clips[static_cast<size_t>(w.activeClip)];
    }
    return true;
}

// Model mode: the clip the animator should play after a save. Keeps its clip of
// this model while that still exists (an older "#animation/<n>" becomes the whole
// clip of that animation), otherwise the clip shown in the window.
const anim::ModelClipEntry* ModelClipToAssign(EditorFrameContext& ctx, const std::string& currentPath)
{
    const WindowState& w = W();
    std::string model, id;
    if (anim::SplitModelClipAssetPath(currentPath, model, id) && model == w.clipPath)
        if (const anim::ModelClipEntry* kept = w.modelSettings.Find(id))
            return kept;
    const std::string legacy = w.clipPath + "#animation/";
    if (ctx.assetPipeline && currentPath.rfind(legacy, 0) == 0)
    {
        const size_t index = static_cast<size_t>(std::strtoul(currentPath.c_str() + legacy.size(), nullptr, 10));
        const ModelClipList& list = GetModelClips(ctx.assetPipeline->GetAssetRoot(), w.clipPath);
        if (index < list.sources.size())
            for (const anim::ModelClipEntry& entry : w.modelSettings.clips)
                if (!entry.hasRange && entry.source == list.sources[index])
                    return &entry;
    }
    if (w.activeClip >= 0 && w.activeClip < static_cast<int>(w.modelSettings.clips.size()))
        return &w.modelSettings.clips[static_cast<size_t>(w.activeClip)];
    return nullptr;
}

// Lets editor Play and the exported game run the clip on this object.
void AssignClipToTarget(EditorFrameContext& ctx)
{
    WindowState& w = W();
    const TargetNodes nodes = CollectNodes(ctx, w.target);
    const EntityID owner = nodes.skeleton ? nodes.skinEntity : w.target;
    // Keep the start clip chosen on the animator while it still exists.
    std::string assetPath = ClipAssetPath(w.clipPath, w.clip.name);
    bool looping = w.clip.looping;
    if (w.modelMode)
    {
        const auto* current = ctx.world.Get<AnimationPlayerComponent>(owner);
        const anim::ModelClipEntry* entry = ModelClipToAssign(ctx, current ? current->clipAssetPath : std::string{});
        if (!entry)
            return;
        assetPath = anim::ModelClipAssetPath(w.clipPath, entry->id);
        looping = entry->looping;
    }
    else if (const auto* current = ctx.world.Get<AnimationPlayerComponent>(owner))
    {
        const size_t hash = current->clipAssetPath.rfind('#');
        if (hash != std::string::npos && current->clipAssetPath.substr(0, hash) == w.clipPath)
        {
            const std::string startClip = current->clipAssetPath.substr(hash + 1u);
            if (std::any_of(w.clips.begin(), w.clips.end(), [&](const AnimationClip& c) { return c.name == startClip; }))
                assetPath = current->clipAssetPath;
        }
    }
    const AnimationHandle handle = ctx.registry.FindAnimationByPath(assetPath);
    if (const auto* current = ctx.world.Get<AnimationPlayerComponent>(owner);
        current && current->clipAssetPath == assetPath && current->clip == handle && current->looping == looping)
        return;
    ExecuteSceneMutation(ctx, "history.assign_animation", [&] {
        const auto assign = [&](EntityID id) {
            if (!ctx.world.Has<AnimationPlayerComponent>(id))
                ctx.world.Add<AnimationPlayerComponent>(id);
            auto* player = ctx.world.Get<AnimationPlayerComponent>(id);
            player->SetClip(handle);
            player->clipAssetPath = assetPath;
            player->playing = true;
            player->looping = looping;
            player->bindingsDirty = true;
        };
        assign(owner);
        // Imported rigs also carry a player on their root; keep it from running the old clip.
        if (owner != w.target && ctx.world.Has<AnimationPlayerComponent>(w.target))
            assign(w.target);
    });
}

// The skeleton sampler treats bones without channels as identity, which would
// collapse the rig, so every skinned clip starts from the rest pose of all bones.
bool SeedClip(const assets::SkeletonAsset* skeleton,
              const std::unordered_map<std::string, Pose>& rest, AnimationClip& clip, std::string& missing)
{
    if (!skeleton)
        return true;
    for (size_t i = 0; i < skeleton->bones.size(); ++i)
    {
        const std::string& boneName = skeleton->bones[i].name;
        const auto it = rest.find(boneName);
        if (it == rest.end())
        {
            missing = boneName;
            return false;
        }
        for (const AnimationTargetProperty property : {AnimationTargetProperty::Translation,
                                                       AnimationTargetProperty::Rotation,
                                                       AnimationTargetProperty::Scale})
        {
            AnimationChannel channel;
            channel.targetName = boneName;
            channel.targetIndex = static_cast<int32_t>(i);
            channel.property = property;
            anim::SetKey(channel, 0.f,
                         property == AnimationTargetProperty::Scale ? it->second.scale : it->second.position,
                         it->second.rotation);
            clip.channels.push_back(std::move(channel));
        }
    }
    return true;
}

// Starts a new, not yet saved animation; the file appears on the first save.
bool CreateAnimation(EditorFrameContext& ctx, EntityID target)
{
    const TargetNodes nodes = CollectNodes(ctx, target);
    std::unordered_map<std::string, Pose> rest;
    for (const auto& [name, id] : nodes.byName)
    {
        Pose pose;
        if (ReadPose(ctx, id, pose))
            rest[name] = pose;
    }

    AnimationClip clip;
    clip.name = "Clip";
    clip.duration = 1.f;
    clip.looping = true;
    std::string missing;
    if (!SeedClip(nodes.skeleton, rest, clip, missing))
    {
        ctx.lastFileMessage = TranslateLabel("animation.missing_bone", "Bone has no object in the hierarchy: ") + missing;
        return false;
    }

    const auto* targetName = ctx.world.Get<NameComponent>(target);
    std::string stem = targetName ? targetName->name : std::string("Animation");
    for (char& c : stem)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_')
            c = '_';
    const std::filesystem::path root = ctx.assetPipeline->GetAssetRoot();
    const std::filesystem::path file = MakeUniqueFilesystemPath(root / "Animations", stem, ".anim");
    std::vector<AnimationClip> clips = LoadModelClips(ctx, nodes, rest);
    if (clips.empty())
        clips.push_back(std::move(clip));
    BeginSession(ctx, target, PathToGenericUtf8(file.lexically_relative(root)), std::move(clips), 0);
    W().dirty = true;
    return true;
}

bool OpenAnimation(EditorFrameContext& ctx, const std::string& relative, EntityID requestedTarget = NULL_ENTITY)
{
    std::ifstream in(AbsoluteClipPath(ctx, relative), std::ios::binary);
    const std::string source((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<AnimationClip> clips;
    if (source.empty() || !anim::ParseAnimationSet(source, clips) || clips.empty())
    {
        ctx.lastFileMessage = TranslateLabel("animation.load_failed", "Could not read clip: ") + relative;
        return false;
    }
    for (AnimationClip& clip : clips)
        if (clip.name.empty())
            clip.name = "Clip";
    WindowState& w = W();
    EntityID target = requestedTarget.IsValid() ? requestedTarget
        : (w.session && ResolveTarget(ctx)) ? w.target : ctx.state.selectedEntity;
    if (!target.IsValid() || !ctx.world.IsAlive(target))
    {
        ctx.lastFileMessage = Translate("animation.select_object");
        return false;
    }
    BeginSession(ctx, target, relative, std::move(clips), 0);
    return true;
}

// Model mode on a model file: its clips with their settings, the given one shown.
bool OpenModelClips(EditorFrameContext& ctx, const std::string& model, EntityID target, const std::string& clipAssetPath)
{
    if (!ctx.assetPipeline || !target.IsValid() || !ctx.world.IsAlive(target))
    {
        ctx.lastFileMessage = Translate("animation.select_object");
        return false;
    }
    // The window works with clip ids (undo, saving, assigning): keep them stable.
    (void)PersistModelClips(ctx.assetPipeline->GetAssetRoot(), model);
    const ModelClipList& list = GetModelClips(ctx.assetPipeline->GetAssetRoot(), model);
    if (!list.error.empty())
    {
        ctx.lastFileMessage = TranslateLabel("animation.load_failed", "Could not read clip: ") +
                              anim::ModelClipSettingsPath(model) + " (" + list.error + ")";
        return false;
    }
    anim::ModelClipSettings settings = list.settings;
    int active = 0;
    std::string ignoredModel, id;
    if (anim::SplitModelClipAssetPath(clipAssetPath, ignoredModel, id))
        for (size_t i = 0; i < settings.clips.size(); ++i)
            if (settings.clips[i].id == id)
                active = static_cast<int>(i);
    // Shown clips are built once the preview copy (and its rest pose) exists.
    std::vector<AnimationClip> placeholders(std::max<size_t>(settings.clips.size(), 1u));
    for (size_t i = 0; i < settings.clips.size(); ++i)
        placeholders[i].name = settings.clips[i].name;
    BeginSession(ctx, target, model, std::move(placeholders), active);
    WindowState& w = W();
    w.modelMode = true;
    w.modelSettings = std::move(settings);
    w.modelClipsStale = true;
    w.recording = false;
    return true;
}

bool SaveAnimation(EditorFrameContext& ctx);

// Runs an action that would replace the open animation. Unsaved changes are
// written first with auto-save on; otherwise the window asks.
void RunAfterUnsavedCheck(EditorFrameContext& ctx, std::function<void()> action)
{
    WindowState& w = W();
    if (w.session && w.dirty && GetEditorPreferences().autoSaveEditors)
        SaveAnimation(ctx);
    if (w.session && w.dirty)
    {
        w.pendingAction = std::move(action);
        w.askUnsaved = true;
        return;
    }
    action();
}

void SyncActiveClip()
{
    WindowState& w = W();
    if (w.activeClip >= 0 && w.activeClip < static_cast<int>(w.clips.size()))
        w.clips[static_cast<size_t>(w.activeClip)] = w.clip;
}

bool SaveAnimation(EditorFrameContext& ctx)
{
    WindowState& w = W();
    if (!w.session)
        return false;
    if (w.controllerMode)
        return true; // the controller is saved by the graph window
    if (w.modelMode)
    {
        if (!SaveModelClipSettings(ctx.assetPipeline->GetAssetRoot(), w.clipPath, w.modelSettings))
        {
            ctx.lastFileMessage = Translate("status.save_failed");
            return false;
        }
        RefreshModelClipAssets(*ctx.assetPipeline, ctx.registry, w.clipPath, w.modelSettings);
        w.dirty = false;
        if (ResolveTarget(ctx))
            AssignClipToTarget(ctx);
        return true;
    }
    SyncActiveClip();
    if (!WriteAnimationFile(ctx, w.clipPath, w.clips))
        return false;
    w.dirty = false;
    if (ResolveTarget(ctx))
        AssignClipToTarget(ctx);
    return true;
}

void SwitchClip(int index)
{
    WindowState& w = W();
    SyncActiveClip();
    w.activeClip = index;
    w.clip = w.clips[static_cast<size_t>(index)];
    w.selectedEvent = -1;
    w.time = 0.f;
    w.playing = false;
    w.selection.clear();
    w.gestureActive = false;
    w.poseDirty = true;
}

std::string UniqueClipName(const std::string& base)
{
    const WindowState& w = W();
    const auto taken = [&](const std::string& name) {
        return std::any_of(w.clips.begin(), w.clips.end(), [&](const AnimationClip& c) { return c.name == name; });
    };
    if (!taken(base))
        return base;
    for (int i = 2;; ++i)
    {
        std::string candidate = base + " " + std::to_string(i);
        if (!taken(candidate))
            return candidate;
    }
}

void BeginGesture()
{
    WindowState& w = W();
    if (!w.gestureActive)
    {
        w.gestureBefore = w.clip;
        w.gestureActive = true;
    }
}

void ApplyClipSnapshot(const std::string& path, const AnimationClip& clip)
{
    WindowState& w = W();
    if (!w.session || w.clipPath != path || w.clip.name != clip.name)
        return;
    w.clip = clip;
    w.selection.clear();
    w.poseDirty = true;
    w.dirty = true;
}

// Model mode: settings changes, one undo step per finished edit.
void BeginModelEdit()
{
    WindowState& w = W();
    if (!w.modelGestureActive)
    {
        w.modelGestureBefore = w.modelSettings;
        w.modelGestureActive = true;
    }
    w.modelClipsStale = true;
}

void ApplyModelSettingsSnapshot(const std::string& model, const anim::ModelClipSettings& settings)
{
    WindowState& w = W();
    if (!w.session || !w.modelMode || w.clipPath != model)
        return;
    const std::string activeId = w.activeClip >= 0 && w.activeClip < static_cast<int>(w.modelSettings.clips.size())
        ? w.modelSettings.clips[static_cast<size_t>(w.activeClip)].id : std::string{};
    w.modelSettings = settings;
    for (size_t i = 0; i < w.modelSettings.clips.size(); ++i)
        if (w.modelSettings.clips[i].id == activeId)
            w.activeClip = static_cast<int>(i);
    w.modelGestureActive = false;
    w.modelClipsStale = true;
    w.dirty = true;
}

void CommitModelEdit(EditorFrameContext& ctx)
{
    WindowState& w = W();
    if (!w.modelGestureActive || ImGui::IsAnyMouseDown() || ImGui::IsAnyItemActive())
        return;
    w.modelGestureActive = false;
    if (w.modelGestureBefore == w.modelSettings)
        return;
    w.dirty = true;
    RecordEditorValueChange(ctx, "history.edit_animation",
        [path = w.clipPath, before = w.modelGestureBefore] { ApplyModelSettingsSnapshot(path, before); },
        [path = w.clipPath, after = w.modelSettings] { ApplyModelSettingsSnapshot(path, after); });
}

// A whole drag or recorded gizmo move becomes one undo step.
void CommitGesture(EditorFrameContext& ctx)
{
    WindowState& w = W();
    if (!w.gestureActive || ImGui::IsAnyMouseDown() || ImGui::IsAnyItemActive())
        return;
    w.gestureActive = false;
    if (anim::SerializeAnimationSet({w.gestureBefore}) == anim::SerializeAnimationSet({w.clip}))
        return;
    w.dirty = true;
    const auto before = std::make_shared<AnimationClip>(w.gestureBefore);
    const auto after = std::make_shared<AnimationClip>(w.clip);
    RecordEditorValueChange(ctx, "history.edit_animation",
        [path = w.clipPath, before] { ApplyClipSnapshot(path, *before); },
        [path = w.clipPath, after] { ApplyClipSnapshot(path, *after); });
}

void ApplyPose(EditorFrameContext& ctx, const TargetNodes& nodes)
{
    WindowState& w = W();
    // The playhead may sit behind the clip end: hold the last pose instead of wrapping.
    const float sampleTime = std::min(w.time, w.clip.duration);
    if (nodes.skeleton)
    {
        const bool looping = w.clip.looping;
        w.clip.looping = false;
        const std::vector<anim::BonePose> poses = anim::AnimationSampler::SampleForSkeleton(
            w.clip, sampleTime, nodes.skeleton->bones.size());
        w.clip.looping = looping;
        if (auto* skin = ctx.world.Get<SkinComponent>(nodes.skinEntity))
            skin->bonePalette = anim::SkeletonPoseEvaluator::Evaluate(*nodes.skeleton, poses);
        for (size_t i = 0; i < poses.size(); ++i)
            if (const auto it = nodes.byName.find(nodes.skeleton->bones[i].name); it != nodes.byName.end())
                WritePose(ctx, it->second, {poses[i].position, poses[i].rotation, poses[i].scale});
    }
    else
    {
        for (const AnimationChannel& channel : w.clip.channels)
        {
            const auto it = nodes.byName.find(channel.targetName);
            Pose pose;
            if (it == nodes.byName.end() || !ReadPose(ctx, it->second, pose))
                continue;
            if (anim::EvaluateChannel(channel, sampleTime, pose.position, pose.rotation, pose.scale))
                WritePose(ctx, it->second, pose);
        }
    }
    w.applied.clear();
    for (const auto& [name, id] : nodes.byName)
    {
        Pose pose;
        if (ReadPose(ctx, id, pose))
            w.applied[name] = pose;
    }
}

void WriteRecordedKey(const std::string& name, int targetIndex, AnimationTargetProperty property,
                      const math::Vec3& vec, const math::Quat& quat,
                      const math::Vec3& previousVec, const math::Quat& previousQuat)
{
    WindowState& w = W();
    int index = anim::FindChannel(w.clip, name, property);
    if (index < 0)
    {
        AnimationChannel channel;
        channel.targetName = name;
        channel.targetIndex = targetIndex;
        channel.property = property;
        // Hold the old value until this key, otherwise the object would jump at time 0.
        if (w.time > anim::kKeyTimeEpsilon)
            anim::SetKey(channel, 0.f, previousVec, previousQuat);
        w.clip.channels.push_back(std::move(channel));
        index = static_cast<int>(w.clip.channels.size()) - 1;
    }
    anim::SetKey(w.clip.channels[static_cast<size_t>(index)], w.time, vec, quat);
    w.clip.duration = std::max(w.clip.duration, w.time);
    w.selection.clear(); // a new key shifts the indices the selection refers to
}

int NodeTrackIndex(const std::string& name)
{
    int next = 0;
    for (const AnimationChannel& channel : W().clip.channels)
    {
        if (channel.targetName == name)
            return channel.targetIndex;
        next = std::max(next, channel.targetIndex + 1);
    }
    return next;
}

// While recording, whatever differs from the shown pose (gizmo, inspector) becomes a key.
void CaptureRecordedChanges(EditorFrameContext& ctx, const TargetNodes& nodes)
{
    WindowState& w = W();
    for (const auto& [name, id] : nodes.byName)
    {
        const int boneIndex = nodes.skeleton ? BoneIndex(*nodes.skeleton, name) : -1;
        if (nodes.skeleton && boneIndex < 0)
            continue;
        const auto applied = w.applied.find(name);
        Pose current;
        if (applied == w.applied.end() || !ReadPose(ctx, id, current))
            continue;
        const bool moved = !SameVec(current.position, applied->second.position);
        const bool rotated = !SameQuat(current.rotation, applied->second.rotation);
        const bool scaled = !SameVec(current.scale, applied->second.scale);
        if (!moved && !rotated && !scaled)
            continue;

        BeginGesture();
        const int targetIndex = nodes.skeleton ? boneIndex : NodeTrackIndex(name);
        const Pose& previous = applied->second;
        if (moved)
            WriteRecordedKey(name, targetIndex, AnimationTargetProperty::Translation,
                             current.position, {}, previous.position, {});
        if (rotated)
            WriteRecordedKey(name, targetIndex, AnimationTargetProperty::Rotation,
                             {}, current.rotation, {}, previous.rotation);
        if (scaled)
            WriteRecordedKey(name, targetIndex, AnimationTargetProperty::Scale,
                             current.scale, {}, previous.scale, {});
        w.poseDirty = true;
    }
}

// Stores the pose exactly as it is shown right now, at the playhead.
void KeyCurrentPose(EditorFrameContext& ctx, const std::string& name, int property)
{
    WindowState& w = W();
    if (w.modelMode) // model clips come from the model file
        return;
    const TargetNodes nodes = CollectNodes(ctx, w.copyRoot);
    const auto node = nodes.byName.find(name);
    Pose current;
    if (node == nodes.byName.end() || !ReadPose(ctx, node->second, current))
        return;
    const int boneIndex = nodes.skeleton ? BoneIndex(*nodes.skeleton, name) : -1;
    if (nodes.skeleton && boneIndex < 0)
        return;
    const int targetIndex = nodes.skeleton ? boneIndex : NodeTrackIndex(name);
    BeginGesture();
    const auto wanted = [&](AnimationTargetProperty p) { return property < 0 || property == static_cast<int>(p); };
    if (wanted(AnimationTargetProperty::Translation))
        WriteRecordedKey(name, targetIndex, AnimationTargetProperty::Translation, current.position, {}, current.position, {});
    if (wanted(AnimationTargetProperty::Rotation))
        WriteRecordedKey(name, targetIndex, AnimationTargetProperty::Rotation, {}, current.rotation, {}, current.rotation);
    if (wanted(AnimationTargetProperty::Scale))
        WriteRecordedKey(name, targetIndex, AnimationTargetProperty::Scale, current.scale, {}, current.scale, {});
    w.applied[name] = current;
    w.selection.clear();
    w.expanded.insert(name);
    w.poseDirty = true;
}

AnimationPlayerComponent* PreviewPlayer(EditorFrameContext& ctx)
{
    const WindowState& w = W();
    if (!w.session || !w.controllerMode || !w.copyRoot.IsValid() || !ctx.world.IsAlive(w.copyRoot))
        return nullptr;
    return ctx.world.Get<AnimationPlayerComponent>(w.copyRoot);
}

// Controller mode: compiles the graph's working copy (unsaved edits included)
// and steps the copy with the runtime's AnimationSystem, real frame delta.
void TickControllerPreview(EditorFrameContext& ctx, const TargetNodes& nodes)
{
    namespace ac = ::engine::addons::animation;
    WindowState& w = W();
    w.recording = false;
    if (!ctx.assetPipeline || ControllerGraphPath() != w.controllerPath)
        return; // SyncControllerPreview ends the session next frame
    const ac::AnimationControllerAsset* graph = ControllerGraphAsset();
    if (!graph)
        return;
    if (!w.controllerSystem || w.controllerRegistry != &ctx.registry)
    {
        w.controllerSystem = std::make_unique<ac::AnimationSystem>(ctx.registry);
        w.controllerRegistry = &ctx.registry;
        w.controllerHasBound = false;
    }
    ac::AnimationControllerLibrary& library = w.controllerSystem->Controllers();
    if (library.AssetRoot() != ctx.assetPipeline->GetAssetRoot())
    {
        library.SetAssetRoot(ctx.assetPipeline->GetAssetRoot());
        w.controllerHasBound = false;
    }

    // Rebind only when the logic changed: moving nodes in the graph keeps the preview running.
    if (!w.controllerHasBound || w.controllerVersion != ControllerGraphVersion())
    {
        w.controllerVersion = ControllerGraphVersion();
        ac::AnimationControllerAsset logic = *graph;
        for (ac::AnimationStateDesc& state : logic.states)
            state.posX = state.posY = 0.f;
        logic.entryPosX = logic.entryPosY = logic.anyPosX = logic.anyPosY = 0.f;
        if (!w.controllerHasBound || !(logic == w.controllerBound))
        {
            // The runtime takes the node pose it finds when binding as rest pose.
            for (const auto& [name, id] : nodes.byName)
                if (const auto rest = w.baseline.find(name); rest != w.baseline.end())
                    WritePose(ctx, id, rest->second);
            library.SetClipResolver(MakeEditorClipResolver(ctx));
            library.Set(w.controllerPath, logic);
            w.controllerBound = std::move(logic);
            w.controllerHasBound = true;
        }
    }

    w.controllerSystem->UpdatePlayer(ctx.world, w.copyRoot, w.controllerPaused ? 0.f : ImGui::GetIO().DeltaTime);
    if (const auto& fired = w.controllerSystem->FiredEvents(); !fired.empty())
    {
        w.previewEventName = fired.back().event->name;
        w.previewEventTime = ImGui::GetTime();
    }

    // A rig is posed through its bone palette; the bone objects follow for the gizmos.
    const AnimationPlayerComponent* player = PreviewPlayer(ctx);
    if (player && nodes.skeleton && player->controllerState.skinEntity.IsValid())
    {
        const auto& output = player->controllerState.output;
        for (size_t i = 0; i < output.size() && i < nodes.skeleton->bones.size(); ++i)
            if (const auto it = nodes.byName.find(nodes.skeleton->bones[i].name); it != nodes.byName.end())
                WritePose(ctx, it->second, {output[i].position, output[i].rotation, output[i].scale});
    }
}

void SortClipEvents();

void TickSession(EditorFrameContext& ctx)
{
    WindowState& w = W();
    ctx.state.animationPreviewActive = false;
    if (!w.session || ctx.playActive)
        return;
    if (!EnsureCopy(ctx))
    {
        EndSession(ctx);
        return;
    }
    const TargetNodes nodes = CollectNodes(ctx, w.copyRoot);
    ctx.state.animationPreviewActive = true;
    ++w.copyAge;
    if (w.controllerMode)
    {
        TickControllerPreview(ctx, nodes);
        return;
    }
    if (w.modelMode)
    {
        w.recording = false;
        if (w.modelClipsStale)
            RebuildModelClips(ctx, nodes);
    }
    if (w.recording && !w.playing && !w.poseDirty)
        CaptureRecordedChanges(ctx, nodes);

    if (w.playing)
    {
        const float duration = std::max(w.clip.duration, 0.001f);
        w.time += ImGui::GetIO().DeltaTime;
        if (w.time >= duration)
        {
            if (w.clip.looping)
                w.time = std::fmod(w.time, duration);
            else
            {
                w.time = duration;
                w.playing = false;
            }
        }
        w.poseDirty = true;
    }
    if (w.poseDirty)
    {
        ApplyPose(ctx, nodes);
        w.poseDirty = false;
    }
    SortClipEvents();
    CommitGesture(ctx);
    CommitModelEdit(ctx);
    // Auto-save writes once a change is complete, not during a drag.
    if (w.dirty && !w.gestureActive && !w.modelGestureActive && GetEditorPreferences().autoSaveEditors &&
        !ImGui::IsAnyMouseDown() && !ImGui::IsAnyItemActive())
        SaveAnimation(ctx);
}

struct TrackGroup
{
    std::string name;
    std::vector<int> channels;
    int depth = 0;
};

// The rig as a tree: every bone or object is listed, with or without keys.
std::vector<TrackGroup> BuildGroups(EditorFrameContext& ctx, const AnimationClip& clip, const TargetNodes& nodes)
{
    std::unordered_map<uint32_t, std::vector<EntityID>> children;
    for (const EntityID id : nodes.all)
        if (const auto* parent = ctx.world.Get<ParentComponent>(id); parent && id != nodes.all.front())
            children[parent->parent.value].push_back(id);

    std::vector<TrackGroup> groups;
    const auto addGroup = [&](const std::string& name, int depth) { groups.push_back({name, {}, depth}); };
    std::function<void(EntityID, int)> visit = [&](EntityID id, int depth) {
        int childDepth = depth;
        if (const auto* name = ctx.world.Get<NameComponent>(id);
            name && !name->name.empty() && (!nodes.skeleton || BoneIndex(*nodes.skeleton, name->name) >= 0))
        {
            addGroup(name->name, depth);
            childDepth = depth + 1;
        }
        if (const auto it = children.find(id.value); it != children.end())
            for (const EntityID child : it->second)
                visit(child, childDepth);
    };
    visit(nodes.all.front(), 0);

    for (size_t i = 0; i < clip.channels.size(); ++i)
    {
        const std::string& name = clip.channels[i].targetName;
        auto it = std::find_if(groups.begin(), groups.end(), [&](const TrackGroup& g) { return g.name == name; });
        if (it == groups.end())
        {
            addGroup(name, 0);
            it = groups.end() - 1;
        }
        it->channels.push_back(static_cast<int>(i));
    }
    for (TrackGroup& group : groups)
        std::stable_sort(group.channels.begin(), group.channels.end(), [&](int a, int b) {
            return clip.channels[static_cast<size_t>(a)].property < clip.channels[static_cast<size_t>(b)].property;
        });
    return groups;
}

struct TrackRow
{
    int group = -1;
    int channel = -1; // -1: the group's summary row
};

float SnapTime(float time)
{
    if (ImGui::GetIO().KeyAlt)
        return time;
    return std::round(time * kSnapRate) / kSnapRate;
}

void SetTime(float time)
{
    WindowState& w = W();
    w.time = std::max(0.f, time);
    w.poseDirty = true;
}

bool IsSelected(int channel, int key)
{
    const auto& selection = W().selection;
    return std::find(selection.begin(), selection.end(), anim::KeyRef{channel, key}) != selection.end();
}

void DrawDiamond(ImDrawList& drawList, const ImVec2& center, float radius, ImU32 color)
{
    drawList.AddQuadFilled(ImVec2(center.x, center.y - radius), ImVec2(center.x + radius, center.y),
                           ImVec2(center.x, center.y + radius), ImVec2(center.x - radius, center.y), color);
}

const char* PropertyLabel(AnimationTargetProperty property)
{
    switch (property)
    {
    case AnimationTargetProperty::Rotation: return Translate("transform.rotation");
    case AnimationTargetProperty::Scale:    return Translate("transform.scale");
    default:                                return "Position";
    }
}

void JumpToKey(int direction);
void DrawKeyDetails();

void DrawTransport(EditorFrameContext& ctx)
{
    WindowState& w = W();
    ImGui::BeginDisabled(!w.session);
    if (ImGui::Button("|<##anim_rewind"))
    {
        w.playing = false;
        SetTime(0.f);
    }
    ImGui::SameLine();
    if (ImGui::Button("<##anim_prev_key"))
        JumpToKey(-1);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", Translate("animation.prev_key"));
    ImGui::SameLine();
    if (ImGui::Button(w.playing ? ICON_LC_PAUSE "##anim_play" : ICON_LC_PLAY "##anim_play"))
    {
        w.playing = !w.playing;
        if (w.playing)
            w.recording = false;
    }
    ImGui::SameLine();
    if (ImGui::Button(">##anim_next_key"))
        JumpToKey(1);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", Translate("animation.next_key"));
    ImGui::SameLine();
    ImGui::BeginDisabled(w.modelMode);
    const bool wasRecording = w.recording;
    if (wasRecording)
        ImGui::PushStyleColor(ImGuiCol_Button, theme::kDanger);
    if (ImGui::Button("REC##anim_rec"))
    {
        w.recording = !w.recording;
        if (w.recording)
            w.playing = false;
    }
    if (wasRecording)
        ImGui::PopStyleColor();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", Translate("animation.rec_tip"));
    ImGui::SameLine();
    ImGui::BeginDisabled(w.selectedNode.empty());
    if (ImGui::Button("K##anim_key_current"))
        KeyCurrentPose(ctx, w.selectedNode, -1);
    ImGui::EndDisabled();
    ImGui::EndDisabled(); // model mode
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s\n%s", Translate("animation.key_current"), Translate("animation.key_current_tip"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN); // ends on the same edge as the rows below
    float time = w.time;
    if (ImGui::DragFloat("##anim_time", &time, 0.01f, 0.f, 3600.f, "%.2f s"))
    {
        w.playing = false;
        SetTime(time);
    }
    ImGui::EndDisabled();
}

// Width of the left column: the keyframe panel above and the track names below
// share it, so both halves of the window sit on one grid.
float AnimationColumnWidth(float available)
{
    return std::min(std::clamp(available * 0.32f, 280.f, 340.f), available * 0.5f);
}

bool CreateAnimation(EditorFrameContext& ctx, EntityID target);

// Model mode: clip choice and the settings of the clip (source, frame range,
// loop, root motion). The keys themselves come from the model.
void DrawModelClipRows(EditorFrameContext& ctx)
{
    WindowState& w = W();
    const ImGuiStyle& style = ImGui::GetStyle();
    anim::ModelClipSettings& settings = w.modelSettings;
    const bool hasEntry = w.activeClip >= 0 && w.activeClip < static_cast<int>(settings.clips.size());
    const ModelClipList& list = GetModelClips(ctx.assetPipeline->GetAssetRoot(), w.clipPath);
    const auto sourceMissing = [&](const anim::ModelClipEntry& entry) { return !list.SourceExists(entry.source); };

    value_ui::RowLabel("Clip", nullptr, false);
    const float menuWidth = ImGui::CalcTextSize("...").x + style.FramePadding.x * 2.f;
    ImGui::SetNextItemWidth(std::max(1.f, ImGui::GetContentRegionAvail().x - menuWidth - style.ItemSpacing.x));
    if (ImGui::BeginCombo("##anim_model_clip", w.clip.name.c_str()))
    {
        for (int i = 0; i < static_cast<int>(settings.clips.size()); ++i)
        {
            const anim::ModelClipEntry& entry = settings.clips[static_cast<size_t>(i)];
            ImGui::PushID(entry.id.c_str());
            const bool missing = sourceMissing(entry);
            if (missing)
                ImGui::PushStyleColor(ImGuiCol_Text, theme::kDanger);
            if (ImGui::Selectable(entry.name.c_str(), i == w.activeClip) && i != w.activeClip)
            {
                w.activeClip = i;
                w.clip = w.clips[static_cast<size_t>(std::min(i, static_cast<int>(w.clips.size()) - 1))];
                w.time = 0.f;
                w.playing = false;
                w.selection.clear();
                w.poseDirty = true;
            }
            if (missing)
                ImGui::PopStyleColor();
            if (entry.hasRange && ImGui::IsItemHovered())
                ImGui::SetTooltip("%s  [%d - %d]", entry.source.c_str(), entry.startFrame, entry.endFrame);
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("...##anim_model_clip_menu", ImVec2(menuWidth, 0.f)))
    {
        std::snprintf(w.renameBuffer, sizeof(w.renameBuffer), "%s", hasEntry ? settings.clips[static_cast<size_t>(w.activeClip)].name.c_str() : "");
        ImGui::OpenPopup("##anim_model_clip_popup");
    }
    if (ImGui::BeginPopup("##anim_model_clip_popup"))
    {
        ImGui::BeginDisabled(!hasEntry);
        if (ImGui::MenuItem(TranslateLabel("animation.cut_clip", "Cut Clip from Range").c_str()))
        {
            BeginModelEdit();
            anim::ModelClipEntry cut = settings.clips[static_cast<size_t>(w.activeClip)];
            const int sourceFrames = static_cast<int>(std::lround(w.clip.duration * settings.fps));
            cut.id = settings.NewId();
            cut.name = settings.UniqueName(cut.name);
            if (!cut.hasRange)
            {
                cut.hasRange = true;
                cut.startFrame = 0;
                cut.endFrame = sourceFrames;
            }
            settings.clips.push_back(std::move(cut));
            w.activeClip = static_cast<int>(settings.clips.size()) - 1;
            w.time = 0.f;
        }
        const bool removable = hasEntry && (settings.clips[static_cast<size_t>(w.activeClip)].hasRange ||
                                            sourceMissing(settings.clips[static_cast<size_t>(w.activeClip)]));
        ImGui::BeginDisabled(!removable);
        if (ImGui::MenuItem(TranslateLabel("animation.delete_clip", "Delete Clip").c_str()))
        {
            BeginModelEdit();
            settings.clips.erase(settings.clips.begin() + w.activeClip);
            w.activeClip = std::max(0, w.activeClip - 1);
        }
        ImGui::EndDisabled();
        if (!removable && hasEntry && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", Translate("animation.whole_clip_tip"));
        ImGui::Separator();
        ImGui::SetNextItemWidth(180.f);
        if (ImGui::InputText("##anim_model_clip_name", w.renameBuffer, sizeof(w.renameBuffer),
                             ImGuiInputTextFlags_EnterReturnsTrue))
        {
            const std::string name(w.renameBuffer);
            anim::ModelClipEntry& entry = settings.clips[static_cast<size_t>(w.activeClip)];
            if (!name.empty() && name != entry.name)
            {
                BeginModelEdit();
                entry.name = settings.UniqueName(name);
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::Separator();
        float fps = settings.fps;
        ImGui::SetNextItemWidth(180.f);
        if (ImGui::DragFloat(TranslateLabel("animation.fps", "Frames per Second##anim_model_fps").c_str(), &fps, 0.1f, 1.f, 240.f, "%.2f",
                             ImGuiSliderFlags_AlwaysClamp) && fps != settings.fps)
        {
            BeginModelEdit();
            settings.fps = fps;
        }
        ImGui::Separator();
        if (ImGui::MenuItem(TranslateLabel("animation.new_anim_file", "New Hand-made Animation (.anim)").c_str()))
        {
            const EntityID target = w.target;
            RunAfterUnsavedCheck(ctx, [&ctx, target] { CreateAnimation(ctx, target); });
        }
        ImGui::EndPopup();
    }
    if (!hasEntry)
        return;
    anim::ModelClipEntry& entry = settings.clips[static_cast<size_t>(w.activeClip)];

    // Source animation in the model and the frame range cut from it.
    value_ui::RowLabel(Translate("animation.source"), nullptr, false);
    if (sourceMissing(entry))
        ImGui::TextColored(theme::kDanger, "%s  %s",
                           entry.source.c_str(), Translate("animation.source_missing"));
    else
        ImGui::TextUnformatted(entry.source.c_str());

    const AnimationClip* source = nullptr;
    for (const AnimationClip* candidate : ModelSourceClips(ctx, w.clipPath))
        if (candidate->name == entry.source)
            source = candidate;
    const int sourceFrames = source ? static_cast<int>(std::lround(source->duration * settings.fps)) : 0;
    value_ui::RowLabel(Translate("animation.range"), nullptr, false);
    if (entry.hasRange)
    {
        int range[2] = {entry.startFrame, entry.endFrame};
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::DragInt2("##anim_model_range", range, 0.25f, 0, std::max(sourceFrames, 1), "%d", ImGuiSliderFlags_AlwaysClamp))
        {
            range[0] = std::clamp(range[0], 0, std::max(sourceFrames, 0));
            range[1] = std::clamp(range[1], range[0], std::max(sourceFrames, range[0]));
            if (range[0] != entry.startFrame || range[1] != entry.endFrame)
            {
                BeginModelEdit();
                entry.startFrame = range[0];
                entry.endFrame = range[1];
            }
        }
    }
    else
    {
        value_ui::TextDisabled("0 - %d  %s", sourceFrames, Translate("animation.whole_clip"));
    }

    // Length (read-only) and loop share a row, as for .anim clips.
    value_ui::RowLabel(Translate("animation.length"), nullptr, false);
    const std::string loopLabel = TranslateLabel("animation.loop", "Loop##anim_model_loop");
    const float loopWidth = ImGui::GetFrameHeight() + style.ItemInnerSpacing.x +
                            ImGui::CalcTextSize(loopLabel.c_str(), nullptr, true).x;
    char length[64];
    std::snprintf(length, sizeof(length), "%.2f s  (%d f)", w.clip.duration,
                  static_cast<int>(std::lround(w.clip.duration * settings.fps)));
    value_ui::TextDisabled("%s", length);
    ImGui::SameLine(0.f, style.ItemSpacing.x);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.f, ImGui::GetContentRegionAvail().x - loopWidth));
    bool looping = entry.looping;
    if (value_ui::Checkbox(loopLabel.c_str(), &looping))
    {
        BeginModelEdit();
        entry.looping = looping;
    }
    bool rootMotion = entry.rootMotion;
    if (value_ui::Checkbox(TranslateLabel("animation.root_motion", "Root Motion##anim_model_root").c_str(), &rootMotion))
    {
        BeginModelEdit();
        entry.rootMotion = rootMotion;
    }
}

// Clip choice, length and loop: label-left rows under the playback buttons.
void DrawClipRows(EditorFrameContext& ctx)
{
    WindowState& w = W();
    if (w.modelMode)
    {
        DrawModelClipRows(ctx);
        return;
    }
    const ImGuiStyle& style = ImGui::GetStyle();

    value_ui::RowLabel("Clip", nullptr, false);
    const char* menuLabel = "...##anim_clip_menu";
    const float menuWidth = ImGui::CalcTextSize("...").x + style.FramePadding.x * 2.f;
    ImGui::SetNextItemWidth(std::max(1.f, ImGui::GetContentRegionAvail().x - menuWidth - style.ItemSpacing.x));
    if (ImGui::BeginCombo("##anim_clip", w.clip.name.c_str()))
    {
        int chosenClip = -1;
        for (int i = 0; i < static_cast<int>(w.clips.size()); ++i)
        {
            ImGui::PushID(i);
            if (ImGui::Selectable(w.clips[static_cast<size_t>(i)].name.c_str(), i == w.activeClip))
                chosenClip = i;
            ImGui::PopID();
        }
        ImGui::EndCombo();
        if (chosenClip >= 0 && chosenClip != w.activeClip)
            SwitchClip(chosenClip);
    }
    ImGui::SameLine();
    if (ImGui::Button(menuLabel, ImVec2(menuWidth, 0.f)))
    {
        std::snprintf(w.renameBuffer, sizeof(w.renameBuffer), "%s", w.clip.name.c_str());
        ImGui::OpenPopup("##anim_clip_popup");
    }
    if (ImGui::BeginPopup("##anim_clip_popup"))
    {
        if (ImGui::MenuItem(TranslateLabel("animation.new_clip", "New Clip").c_str()))
        {
            const TargetNodes nodes = CollectNodes(ctx, w.copyRoot);
            AnimationClip clip;
            clip.name = UniqueClipName("Clip");
            clip.duration = 1.f;
            clip.looping = true;
            std::string missing;
            SeedClip(nodes.skeleton, w.baseline, clip, missing);
            SyncActiveClip();
            w.clips.push_back(std::move(clip));
            w.dirty = true;
            SwitchClip(static_cast<int>(w.clips.size()) - 1);
        }
        if (ImGui::MenuItem(TranslateLabel("animation.duplicate_clip", "Duplicate Clip").c_str()))
        {
            SyncActiveClip();
            AnimationClip clip = w.clip;
            clip.name = UniqueClipName(clip.name);
            w.clips.push_back(std::move(clip));
            w.dirty = true;
            SwitchClip(static_cast<int>(w.clips.size()) - 1);
        }
        const TargetNodes modelNodes = CollectNodes(ctx, w.copyRoot);
        const std::vector<AnimationClip> modelClips = LoadModelClips(ctx, modelNodes, w.baseline);
        if (!modelClips.empty() && ImGui::BeginMenu(TranslateLabel("animation.import_model", "Add from Model").c_str()))
        {
            for (const AnimationClip& modelClip : modelClips)
                if (ImGui::MenuItem(modelClip.name.c_str()))
                {
                    SyncActiveClip();
                    AnimationClip clip = modelClip;
                    clip.name = UniqueClipName(clip.name);
                    w.clips.push_back(std::move(clip));
                    w.dirty = true;
                    SwitchClip(static_cast<int>(w.clips.size()) - 1);
                }
            ImGui::EndMenu();
        }
        ImGui::BeginDisabled(w.clips.size() < 2u);
        if (ImGui::MenuItem(TranslateLabel("animation.delete_clip", "Delete Clip").c_str()))
        {
            const int removed = w.activeClip;
            w.clips.erase(w.clips.begin() + removed);
            w.activeClip = -1; // nothing to sync back
            w.dirty = true;
            SwitchClip(std::min(removed, static_cast<int>(w.clips.size()) - 1));
        }
        ImGui::EndDisabled();
        ImGui::Separator();
        ImGui::SetNextItemWidth(180.f);
        if (ImGui::InputText("##anim_clip_name", w.renameBuffer, sizeof(w.renameBuffer),
                             ImGuiInputTextFlags_EnterReturnsTrue))
        {
            std::string name(w.renameBuffer);
            name.erase(std::remove(name.begin(), name.end(), '#'), name.end());
            if (!name.empty() && name != w.clip.name)
            {
                w.clip.name = UniqueClipName(name);
                SyncActiveClip();
                w.dirty = true;
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // Length and the loop box share a row; the box ends on the same edge as the clip menu.
    value_ui::RowLabel(Translate("animation.length"), nullptr, false);
    const std::string loopLabel = TranslateLabel("animation.loop", "Loop##anim_loop");
    const float loopWidth = ImGui::GetFrameHeight() + style.ItemInnerSpacing.x +
                            ImGui::CalcTextSize(loopLabel.c_str(), nullptr, true).x;
    ImGui::SetNextItemWidth(std::max(1.f, ImGui::GetContentRegionAvail().x - loopWidth - style.ItemSpacing.x));
    float duration = w.clip.duration;
    if (ImGui::DragFloat("##anim_length", &duration, 0.01f, 0.1f, 3600.f, "%.2f s", ImGuiSliderFlags_AlwaysClamp))
    {
        BeginGesture();
        w.clip.duration = std::max(duration, anim::LastKeyTime(w.clip));
        w.poseDirty = true;
    }
    ImGui::SameLine(0.f, style.ItemSpacing.x);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.f, ImGui::GetContentRegionAvail().x - loopWidth));
    bool looping = w.clip.looping;
    if (value_ui::Checkbox(loopLabel.c_str(), &looping))
    {
        BeginGesture();
        w.clip.looping = looping;
    }
}

// Save line at the top, the same button as in the material editor.
void DrawAnimationActions(EditorFrameContext& ctx)
{
    WindowState& w = W();
    if (card::SaveButton(TranslateLabel("common.save", "Save##animation")))
        SaveAnimation(ctx);
    ImGui::SameLine();
    if (w.dirty)
        ImGui::TextColored(ImVec4(0.96f, 0.82f, 0.30f, 1.f), "%s", Translate("common.unsaved_changes"));
    else
        value_ui::TextDisabled(Translate(GetEditorPreferences().autoSaveEditors ? "material.autosaved" : "material.saved"));
    ImGui::Separator();
}

void SelectKeyAt(const std::vector<TrackGroup>& groups, const TrackRow& row, int key, float time, bool additive)
{
    WindowState& w = W();
    w.selectedEvent = -1;
    std::vector<anim::KeyRef> hit;
    if (row.channel >= 0)
        hit.push_back({row.channel, key});
    else
        for (const int channel : groups[static_cast<size_t>(row.group)].channels)
            if (const int found = anim::FindKey(w.clip.channels[static_cast<size_t>(channel)], time); found >= 0)
                hit.push_back({channel, found});

    const bool alreadySelected = !hit.empty() && IsSelected(hit[0].channel, hit[0].key);
    if (additive && alreadySelected)
    {
        for (const anim::KeyRef& ref : hit)
            w.selection.erase(std::remove(w.selection.begin(), w.selection.end(), ref), w.selection.end());
        return;
    }
    if (!additive && !alreadySelected)
        w.selection.clear();
    for (const anim::KeyRef& ref : hit)
        if (!IsSelected(ref.channel, ref.key))
            w.selection.push_back(ref);
}

void AddKeysAt(int group, int channel, float time, const std::vector<TrackGroup>& groups)
{
    WindowState& w = W();
    if (w.modelMode)
        return;
    BeginGesture();
    const auto addKey = [&](int index) {
        AnimationChannel& target = w.clip.channels[static_cast<size_t>(index)];
        math::Vec3 translation = math::Vec3::Zero();
        math::Quat rotation = math::Quat::Identity();
        math::Vec3 scale = math::Vec3::One();
        anim::EvaluateChannel(target, time, translation, rotation, scale);
        anim::SetKey(target, time,
                     target.property == AnimationTargetProperty::Scale ? scale : translation, rotation);
    };
    if (channel >= 0)
        addKey(channel);
    else
        for (const int index : groups[static_cast<size_t>(group)].channels)
            addKey(index);
    w.clip.duration = std::max(w.clip.duration, time);
    w.selection.clear();
    w.poseDirty = true;
}


// --- Clip events ---------------------------------------------------------------
// The timeline shows w.clip.events (seconds from the clip start). .anim clips keep
// them there; model clips keep them in their settings, in source frames, and the
// shown list is mirrored from those. w.selectedEvent indexes the shown list.

anim::ModelClipEntry* ActiveModelEntry()
{
    WindowState& w = W();
    if (!w.modelMode || w.activeClip < 0 || w.activeClip >= static_cast<int>(w.modelSettings.clips.size()))
        return nullptr;
    return &w.modelSettings.clips[static_cast<size_t>(w.activeClip)];
}

float ModelFps()
{
    return std::max(1.f, W().modelSettings.fps);
}

int FirstFrame(const anim::ModelClipEntry& entry)
{
    return entry.hasRange ? entry.startFrame : 0;
}

bool ModelEventShown(const anim::ModelClipEntry& entry, const anim::ModelClipEvent& event, float& time)
{
    time = static_cast<float>(event.frame - FirstFrame(entry)) / ModelFps();
    return time >= -1e-4f && time <= W().clip.duration + 1e-4f;
}

// The shown events of a model clip, right away (the full rebuild follows).
void MirrorModelEvents()
{
    WindowState& w = W();
    const anim::ModelClipEntry* entry = ActiveModelEntry();
    if (!entry)
        return;
    w.clip.events.clear();
    for (const anim::ModelClipEvent& event : entry->events)
        if (float time = 0.f; ModelEventShown(*entry, event, time))
            w.clip.events.push_back({std::clamp(time, 0.f, w.clip.duration), event.name, event.value, event.text});
    if (w.activeClip >= 0 && w.activeClip < static_cast<int>(w.clips.size()))
        w.clips[static_cast<size_t>(w.activeClip)].events = w.clip.events;
}

// Settings index of the n-th shown event of a model clip; -1 if none.
int ModelEventIndex(int shown)
{
    const anim::ModelClipEntry* entry = ActiveModelEntry();
    if (!entry || shown < 0)
        return -1;
    int seen = -1;
    for (size_t i = 0; i < entry->events.size(); ++i)
        if (float time = 0.f; ModelEventShown(*entry, entry->events[i], time) && ++seen == shown)
            return static_cast<int>(i);
    return -1;
}

int ShownEventIndex(int modelIndex)
{
    const anim::ModelClipEntry* entry = ActiveModelEntry();
    if (!entry || modelIndex < 0)
        return -1;
    int shown = -1;
    for (int i = 0; i <= modelIndex && i < static_cast<int>(entry->events.size()); ++i)
        if (float time = 0.f; ModelEventShown(*entry, entry->events[static_cast<size_t>(i)], time))
            ++shown;
    return shown;
}

bool EventSelected()
{
    const WindowState& w = W();
    return w.session && !w.controllerMode && w.selectedEvent >= 0 &&
           w.selectedEvent < static_cast<int>(w.clip.events.size());
}

std::string UniqueEventName()
{
    const auto& events = W().clip.events;
    const auto used = [&](const std::string& name) {
        return std::any_of(events.begin(), events.end(), [&](const assets::AnimationEvent& e) { return e.name == name; });
    };
    if (!used("Event"))
        return "Event";
    for (int i = 2;; ++i)
        if (const std::string name = "Event_" + std::to_string(i); !used(name))
            return name;
}

int AddClipEvent(float time)
{
    WindowState& w = W();
    if (!w.session || w.controllerMode)
        return -1;
    time = std::clamp(time, 0.f, std::max(0.f, w.clip.duration));
    const std::string name = UniqueEventName();
    if (w.modelMode)
    {
        anim::ModelClipEntry* entry = ActiveModelEntry();
        if (!entry)
            return -1;
        BeginModelEdit();
        anim::ModelClipEvent event;
        event.frame = FirstFrame(*entry) + static_cast<int>(std::lround(time * ModelFps()));
        event.name = name;
        const auto at = std::upper_bound(entry->events.begin(), entry->events.end(), event.frame,
            [](int frame, const anim::ModelClipEvent& e) { return frame < e.frame; });
        const int index = static_cast<int>(at - entry->events.begin());
        entry->events.insert(at, std::move(event));
        MirrorModelEvents();
        w.selectedEvent = ShownEventIndex(index);
    }
    else
    {
        BeginGesture();
        const auto at = std::upper_bound(w.clip.events.begin(), w.clip.events.end(), time,
            [](float t, const assets::AnimationEvent& e) { return t < e.time; });
        const int index = static_cast<int>(at - w.clip.events.begin());
        w.clip.events.insert(at, {time, name, 0.f, {}});
        w.selectedEvent = index;
    }
    w.selection.clear();
    return w.selectedEvent;
}

bool DeleteSelectedEvent()
{
    WindowState& w = W();
    if (!EventSelected())
        return false;
    if (w.modelMode)
    {
        anim::ModelClipEntry* entry = ActiveModelEntry();
        const int index = ModelEventIndex(w.selectedEvent);
        if (!entry || index < 0)
            return false;
        BeginModelEdit();
        entry->events.erase(entry->events.begin() + index);
        MirrorModelEvents();
    }
    else
    {
        BeginGesture();
        w.clip.events.erase(w.clip.events.begin() + w.selectedEvent);
    }
    w.selectedEvent = -1;
    return true;
}

// Writes the selected event back (name, value, text, time). The order is
// restored by SortClipEvents once the edit ends.
void SetSelectedEvent(const assets::AnimationEvent& changed)
{
    WindowState& w = W();
    if (!EventSelected())
        return;
    assets::AnimationEvent value = changed;
    value.name = anim::AnimationEventName(value.name.empty() ? std::string("Event") : value.name);
    value.time = std::clamp(value.time, 0.f, std::max(0.f, w.clip.duration));
    if (w.modelMode)
    {
        anim::ModelClipEntry* entry = ActiveModelEntry();
        const int index = ModelEventIndex(w.selectedEvent);
        if (!entry || index < 0)
            return;
        anim::ModelClipEvent& event = entry->events[static_cast<size_t>(index)];
        const int frame = FirstFrame(*entry) + static_cast<int>(std::lround(value.time * ModelFps()));
        if (event.frame == frame && event.name == value.name && event.value == value.value && event.text == value.text)
            return;
        BeginModelEdit();
        event.frame = frame;
        event.name = value.name;
        event.value = value.value;
        event.text = value.text;
        MirrorModelEvents();
    }
    else
    {
        if (w.clip.events[static_cast<size_t>(w.selectedEvent)] == value)
            return;
        BeginGesture();
        w.clip.events[static_cast<size_t>(w.selectedEvent)] = value;
    }
}

// Keeps the events sorted by time once a drag or an edit has ended; the selection follows.
void SortClipEvents()
{
    WindowState& w = W();
    if (!w.session || w.controllerMode || ImGui::IsAnyMouseDown() || ImGui::IsAnyItemActive())
        return;
    if (w.modelMode)
    {
        anim::ModelClipEntry* entry = ActiveModelEntry();
        if (!entry || std::is_sorted(entry->events.begin(), entry->events.end(),
                                     [](const anim::ModelClipEvent& a, const anim::ModelClipEvent& b) { return a.frame < b.frame; }))
            return;
        const int selected = ModelEventIndex(w.selectedEvent);
        std::vector<size_t> order(entry->events.size());
        for (size_t i = 0; i < order.size(); ++i)
            order[i] = i;
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return entry->events[a].frame < entry->events[b].frame; });
        std::vector<anim::ModelClipEvent> sorted;
        int newSelected = -1;
        for (size_t i = 0; i < order.size(); ++i)
        {
            if (static_cast<int>(order[i]) == selected)
                newSelected = static_cast<int>(i);
            sorted.push_back(entry->events[order[i]]);
        }
        entry->events = std::move(sorted);
        MirrorModelEvents();
        w.selectedEvent = newSelected >= 0 ? ShownEventIndex(newSelected) : -1;
        return;
    }
    auto& events = w.clip.events;
    if (std::is_sorted(events.begin(), events.end(), [](const assets::AnimationEvent& a, const assets::AnimationEvent& b) { return a.time < b.time; }))
        return;
    std::vector<size_t> order(events.size());
    for (size_t i = 0; i < order.size(); ++i)
        order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return events[a].time < events[b].time; });
    std::vector<assets::AnimationEvent> sorted;
    int newSelected = -1;
    for (size_t i = 0; i < order.size(); ++i)
    {
        if (static_cast<int>(order[i]) == w.selectedEvent)
            newSelected = static_cast<int>(i);
        sorted.push_back(events[order[i]]);
    }
    events = std::move(sorted);
    w.selectedEvent = newSelected;
}

// Inspector card of the selected event.
void DrawEventDetails()
{
    WindowState& w = W();
    assets::AnimationEvent event = w.clip.events[static_cast<size_t>(w.selectedEvent)];
    ImGui::PushID(w.selectedEvent);

    char name[64];
    std::snprintf(name, sizeof(name), "%s", event.name.c_str());
    value_ui::InputText((std::string(Translate("animation.event_name")) + "##event_name").c_str(), name, sizeof(name));
    if (ImGui::IsItemDeactivatedAfterEdit() && name[0] != '\0')
    {
        event.name = name;
        SetSelectedEvent(event);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", Translate("animation.event_name_tip"));

    if (w.modelMode)
    {
        // Model clips count in frames of the source animation, like start and end.
        const anim::ModelClipEntry* entry = ActiveModelEntry();
        const int index = ModelEventIndex(w.selectedEvent);
        int frame = entry && index >= 0 ? entry->events[static_cast<size_t>(index)].frame : 0;
        if (value_ui::DragInt((std::string(Translate("animation.event_frame")) + "##event_frame").c_str(), &frame, 0.2f) && entry)
        {
            event.time = static_cast<float>(frame - FirstFrame(*entry)) / ModelFps();
            SetSelectedEvent(event);
        }
    }
    else if (value_ui::DragFloat((std::string(Translate("animation.time")) + "##event_time").c_str(), &event.time,
                                 1.f / kSnapRate, 0.f, std::max(0.f, w.clip.duration), "%.2f s"))
    {
        SetSelectedEvent(event);
    }

    if (value_ui::DragFloat((std::string(Translate("animation.event_value")) + "##event_value").c_str(), &event.value, 0.05f, 0.f, 0.f, "%.2f"))
        SetSelectedEvent(event);

    char text[160];
    std::snprintf(text, sizeof(text), "%s", event.text.c_str());
    value_ui::InputText((std::string(Translate("animation.event_text")) + "##event_text").c_str(), text, sizeof(text));
    if (ImGui::IsItemDeactivatedAfterEdit())
    {
        event.text = text;
        SetSelectedEvent(event);
    }

    ImGui::PushStyleColor(ImGuiCol_Text, theme::kDanger);
    if (ImGui::Button(TranslateLabel("common.delete", "Delete##event_delete").c_str()))
        DeleteSelectedEvent();
    ImGui::PopStyleColor();
    ImGui::PopID();
}

// Event band at the top of the time ruler: one marker per event. Click selects,
// dragging moves, right-click adds (empty spot) or deletes (marker).
void DrawEventBand(const ImVec2& min, float width, float height,
                   const std::function<float(float)>& xOf, const std::function<float(float)>& timeOf)
{
    WindowState& w = W();
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetCursorScreenPos(min);
    ImGui::InvisibleButton("##anim_events", ImVec2(width, height), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool hovered = ImGui::IsItemHovered();
    const bool activated = ImGui::IsItemActivated() && ImGui::IsMouseDown(ImGuiMouseButton_Left);
    const ImVec2 max(min.x + width, min.y + height);
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(min, max, ImGui::GetColorU32(theme::kField));

    const auto& events = w.clip.events;
    int hoveredEvent = -1;
    float nearest = 6.f;
    w.eventMarkers.clear();
    w.eventMarkers.emplace_back(min.x, min.y + height * 0.5f); // index -1: the band itself
    for (size_t i = 0; i < events.size(); ++i)
    {
        const float x = xOf(events[i].time);
        w.eventMarkers.emplace_back(x, min.y + height * 0.5f);
        if (hovered && std::fabs(io.MousePos.x - x) < nearest)
        {
            nearest = std::fabs(io.MousePos.x - x);
            hoveredEvent = static_cast<int>(i);
        }
    }

    if (activated)
    {
        w.selectedEvent = hoveredEvent;
        w.eventDragging = hoveredEvent >= 0;
        if (hoveredEvent >= 0)
            w.selection.clear();
    }
    if (w.eventDragging)
    {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) || !EventSelected())
            w.eventDragging = false;
        else if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.f))
        {
            assets::AnimationEvent moved = events[static_cast<size_t>(w.selectedEvent)];
            moved.time = SnapTime(timeOf(io.MousePos.x));
            SetSelectedEvent(moved);
        }
    }
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
    {
        w.eventPopupTime = SnapTime(timeOf(io.MousePos.x));
        w.eventPopupOnMarker = hoveredEvent >= 0;
        if (hoveredEvent >= 0)
        {
            w.selectedEvent = hoveredEvent;
            w.selection.clear();
        }
        ImGui::OpenPopup("##anim_event_menu");
    }
    if (ImGui::BeginPopup("##anim_event_menu"))
    {
        if (!w.eventPopupOnMarker)
        {
            if (ImGui::MenuItem(TranslateLabel("animation.add_event", "Add Event").c_str()))
                AddClipEvent(w.eventPopupTime);
        }
        else
        {
            ImGui::PushStyleColor(ImGuiCol_Text, theme::kDanger);
            if (ImGui::MenuItem(TranslateLabel("common.delete", "Delete").c_str(), "Del"))
                DeleteSelectedEvent();
            ImGui::PopStyleColor();
        }
        ImGui::EndPopup();
    }

    drawList->PushClipRect(min, max, true);
    const ImU32 markerColor = ImGui::GetColorU32(theme::kWarning);
    const ImU32 selectedColor = ImGui::GetColorU32(theme::kAccentHover);
    const ImU32 nameColor = ImGui::GetColorU32(theme::kTextSecondary);
    if (events.empty())
        drawList->AddText(ImVec2(xOf(0.f) + 4.f, min.y + (height - ImGui::GetFontSize()) * 0.5f),
                          ImGui::GetColorU32(theme::kTextDisabled), Translate("animation.events_band"));
    for (size_t i = 0; i < events.size(); ++i)
    {
        const float x = xOf(events[i].time);
        const bool selected = static_cast<int>(i) == w.selectedEvent;
        const ImU32 color = selected || static_cast<int>(i) == hoveredEvent ? selectedColor : markerColor;
        // A small flag: pole and pennant, the name beside it.
        drawList->AddLine(ImVec2(x, min.y + 2.f), ImVec2(x, max.y - 2.f), color, selected ? 2.f : 1.5f);
        drawList->AddTriangleFilled(ImVec2(x, min.y + 2.f), ImVec2(x + 8.f, min.y + 5.f), ImVec2(x, min.y + 8.f), color);
        drawList->AddText(ImVec2(x + 10.f, min.y + (height - ImGui::GetFontSize()) * 0.5f), selected ? color : nameColor,
                          events[i].name.c_str());
    }
    drawList->PopClipRect();
    if (hoveredEvent >= 0)
    {
        const assets::AnimationEvent& e = events[static_cast<size_t>(hoveredEvent)];
        ImGui::SetTooltip("%s  (%.2f s)\n%s %.2f%s%s", e.name.c_str(), e.time, Translate("animation.event_value"), e.value,
                          e.text.empty() ? "" : "\n", e.text.c_str());
    }
    else if (hovered)
        ImGui::SetTooltip("%s", Translate("animation.events_tip"));
}

void DeleteSelectedKeys()
{
    WindowState& w = W();
    if (DeleteSelectedEvent())
        return;
    if (w.modelMode)
        return;
    if (w.selection.empty())
        return;
    BeginGesture();
    // A clip that ended on a deleted key ends on the key before it now.
    const float lastKeyBefore = anim::LastKeyTime(w.clip);
    anim::DeleteKeys(w.clip, w.selection);
    if (w.clip.duration <= lastKeyBefore + anim::kKeyTimeEpsilon)
        w.clip.duration = std::max(anim::LastKeyTime(w.clip), 0.1f);
    w.selection.clear();
    w.poseDirty = true;
}

math::Vec3 QuatToEulerDeg(const math::Quat& q)
{
    const float sinPitch = 2.f * (q.w * q.x - q.y * q.z);
    const float pitch = std::asin(std::clamp(sinPitch, -1.f, 1.f));
    float yaw, roll;
    if (std::fabs(sinPitch) < 0.9999f)
    {
        yaw = std::atan2(2.f * (q.x * q.z + q.w * q.y), 1.f - 2.f * (q.x * q.x + q.y * q.y));
        roll = std::atan2(2.f * (q.x * q.y + q.w * q.z), 1.f - 2.f * (q.x * q.x + q.z * q.z));
    }
    else
    {
        yaw = std::atan2(-2.f * (q.x * q.z - q.w * q.y), 1.f - 2.f * (q.y * q.y + q.z * q.z));
        roll = 0.f;
    }
    return {pitch * math::RAD_TO_DEG, yaw * math::RAD_TO_DEG, roll * math::RAD_TO_DEG};
}

// "seconds:frames", like the time ruler of other animation tools.
std::string FormatTime(float seconds)
{
    const int totalFrames = static_cast<int>(std::lround(seconds * kSnapRate));
    char text[32];
    std::snprintf(text, sizeof(text), "%d:%02d", totalFrames / static_cast<int>(kSnapRate), totalFrames % static_cast<int>(kSnapRate));
    return text;
}

// The value a channel shows, in the form people edit it (rotation as angles).
math::Vec3 DisplayValue(const AnimationChannel& channel, float time)
{
    math::Vec3 translation = math::Vec3::Zero();
    math::Quat rotation = math::Quat::Identity();
    math::Vec3 scale = math::Vec3::One();
    anim::EvaluateChannel(channel, time, translation, rotation, scale);
    if (anim::IsRotationChannel(channel))
        return QuatToEulerDeg(rotation);
    return channel.property == AnimationTargetProperty::Scale ? scale : translation;
}

// What the selected keyframe is: object, property, time, value and interpolation.
void DrawKeyDetails()
{
    WindowState& w = W();
    if (w.selection.empty())
    {
        ImGui::TextWrapped("%s", Translate("animation.key_hint"));
        return;
    }
    if (w.selection.size() > 1u)
    {
        ImGui::Text(Translate("animation.keys_selected"), static_cast<int>(w.selection.size()));
        return;
    }

    const anim::KeyRef ref = w.selection.front();
    AnimationChannel& channel = w.clip.channels[static_cast<size_t>(ref.channel)];
    const size_t key = static_cast<size_t>(ref.key);

    value_ui::RowLabel(Translate("animation.object"));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(channel.targetName.c_str());
    value_ui::RowLabel(Translate("animation.property"));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(PropertyLabel(channel.property));

    float time = channel.times[key];
    if (value_ui::DragFloat((std::string(Translate("animation.time")) + "##key_time").c_str(), &time, 1.f / kSnapRate, 0.f, 3600.f, "%.2f s"))
    {
        BeginGesture();
        w.selection = anim::MoveKeys(w.clip, {ref}, time - channel.times[key]);
        w.clip.duration = std::max(w.clip.duration, anim::LastKeyTime(w.clip));
        w.poseDirty = true;
        return; // the key moved inside the channel
    }

    // value_ui draws the coloured X/Y/Z fields of the inspector here.
    const bool rotation = anim::IsRotationChannel(channel);
    math::Vec3 value = rotation ? QuatToEulerDeg(channel.quatValues[key]) : channel.vec3Values[key];
    if (value_ui::DragFloat3((std::string(PropertyLabel(channel.property)) + "##key_value").c_str(), &value.x,
                             rotation ? 0.5f : 0.01f, 0.f, 0.f, rotation ? "%.1f" : "%.3f"))
    {
        BeginGesture();
        if (rotation) channel.quatValues[key] = math::Quat::FromEulerDeg(value.x, value.y, value.z);
        else          channel.vec3Values[key] = value;
        w.poseDirty = true;
    }

    int interpolation = channel.interpolation == AnimationInterpolation::Step ? 1 : 0;
    const std::string linearLabel = Translate("animation.interp_linear");
    const std::string stepLabel = Translate("animation.interp_step");
    const char* const items[] = {linearLabel.c_str(), stepLabel.c_str()};
    if (value_ui::Combo((std::string(Translate("animation.interpolation")) + "##key_interp").c_str(), &interpolation, items, 2))
    {
        BeginGesture();
        channel.interpolation = interpolation == 1 ? AnimationInterpolation::Step : AnimationInterpolation::Linear;
        w.poseDirty = true;
    }
}

void JumpToKey(int direction)
{
    WindowState& w = W();
    float best = -1.f;
    for (const AnimationChannel& channel : w.clip.channels)
        for (const float time : channel.times)
        {
            const bool ahead = direction > 0 ? time > w.time + anim::kKeyTimeEpsilon : time < w.time - anim::kKeyTimeEpsilon;
            if (ahead && (best < 0.f || (direction > 0 ? time < best : time > best)))
                best = time;
        }
    if (best >= 0.f)
    {
        w.playing = false;
        SetTime(best);
    }
}

void DrawTimeline(EditorFrameContext& ctx, float column)
{
    WindowState& w = W();
    const ImGuiIO& io = ImGui::GetIO();
    const ImGuiStyle& style = ImGui::GetStyle();
    // Track names use the panel's column; the keys start where the preview starts.
    const float labelWidth = column + style.ItemSpacing.x;
    // Playback buttons, clip, length and loop; model clips add source, range and root motion.
    const float headerRows = w.modelMode ? (w.modelSettings.clips.empty() ? 2.f : 6.f) : 3.f;
    const float headerHeight = headerRows * ImGui::GetFrameHeightWithSpacing() - style.ItemSpacing.y;
    const float rulerHeight = headerHeight + 6.f;
    const float rowHeight = ImGui::GetFrameHeight();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float timelineX = origin.x + labelWidth;
    const float timelineWidth = std::max(40.f, ImGui::GetContentRegionAvail().x - labelWidth);
    // Room left of time 0, so the playhead can be grabbed at the very start.
    constexpr float kTimelineMargin = 16.f;
    const auto xOf = [&](float t) { return timelineX + kTimelineMargin + (t - w.viewStart) * w.pixelsPerSecond; };
    const auto timeOf = [&](float x) { return w.viewStart + (x - timelineX - kTimelineMargin) / w.pixelsPerSecond; };
    const ImU32 textColor = ImGui::GetColorU32(ImGuiCol_Text);
    const ImU32 mutedColor = ImGui::GetColorU32(theme::kTextDisabled);
    const ImU32 lineColor = ImGui::GetColorU32(theme::kBorder);
    const ImU32 playheadColor = ImGui::GetColorU32(theme::kDanger);
    const ImU32 keyColor = ImGui::GetColorU32(theme::kTextSecondary);
    const ImU32 selectedKeyColor = ImGui::GetColorU32(theme::kAccentHover);

    // Playback and clip controls stand left of the time ruler, above the track names.
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + 3.f));
    if (ImGui::BeginChild("##anim_header", ImVec2(column, headerHeight), ImGuiChildFlags_None,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground))
    {
        value_ui::InlineLabelScope rows(value_ui::LabelColumnFor(column));
        DrawTransport(ctx);
        DrawClipRows(ctx);
    }
    ImGui::EndChild();

    // Ruler: the event band on top, the scrubbing area below it.
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const float bandHeight = rowHeight;
    ImGui::SetCursorScreenPos(ImVec2(timelineX, origin.y + bandHeight + 2.f));
    ImGui::InvisibleButton("##anim_ruler", ImVec2(timelineWidth, std::max(1.f, rulerHeight - bandHeight - 2.f)));
    if (ImGui::IsItemActive())
    {
        w.playing = false;
        SetTime(SnapTime(timeOf(io.MousePos.x)));
    }
    const ImVec2 rulerMin(timelineX, origin.y);
    const ImVec2 rulerMax(timelineX + timelineWidth, origin.y + rulerHeight);
    drawList->AddRectFilled(rulerMin, rulerMax, ImGui::GetColorU32(theme::kPanel));
    drawList->PushClipRect(rulerMin, rulerMax, true);
    float tickStep = 10.f;
    for (const float candidate : {0.1f, 0.25f, 0.5f, 1.f, 2.f, 5.f, 10.f})
        if (candidate * w.pixelsPerSecond >= 64.f)
        {
            tickStep = candidate;
            break;
        }
    const float endTime = timeOf(timelineX + timelineWidth);
    for (int i = static_cast<int>(std::floor(w.viewStart / tickStep)); i * tickStep <= endTime; ++i)
    {
        const float x = xOf(i * tickStep);
        drawList->AddLine(ImVec2(x, rulerMax.y - 7.f), ImVec2(x, rulerMax.y), lineColor);
        char label[24];
        std::snprintf(label, sizeof(label), "%s", FormatTime(i * tickStep).c_str());
        drawList->AddText(ImVec2(x + 3.f, rulerMax.y - 9.f - ImGui::GetFontSize()), mutedColor, label);
    }
    drawList->PopClipRect();
    DrawEventBand(ImVec2(timelineX, origin.y), timelineWidth, bandHeight, xOf, timeOf);

    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + rulerHeight));

    const std::vector<TrackGroup> groups = BuildGroups(ctx, w.clip, CollectNodes(ctx, w.copyRoot));
    std::vector<TrackRow> rows;
    for (size_t g = 0; g < groups.size(); ++g)
    {
        rows.push_back({static_cast<int>(g), -1});
        if (w.expanded.count(groups[g].name))
            for (const int channel : groups[g].channels)
                rows.push_back({static_cast<int>(g), channel});
    }

    const ImGuiWindowFlags childFlags = io.KeyCtrl ? ImGuiWindowFlags_NoScrollWithMouse : ImGuiWindowFlags_None;
    constexpr float kScrollbarHeight = 12.f;
    // The view follows the playhead while it plays or is dragged.
    const float visibleSeconds = timelineWidth / w.pixelsPerSecond;
    if (w.playing || w.scrubbing)
    {
        if (w.time > w.viewStart + visibleSeconds * 0.9f)
            w.viewStart = w.time - visibleSeconds * 0.9f;
        else if (w.time < w.viewStart)
            w.viewStart = std::max(0.f, w.time - visibleSeconds * 0.1f);
    }

    if (ImGui::BeginChild("##anim_tracks", ImVec2(0.f, -(kScrollbarHeight + 4.f)), ImGuiChildFlags_None, childFlags))
    {
        drawList = ImGui::GetWindowDrawList();
        const ImVec2 top = ImGui::GetCursorScreenPos();
        const float totalHeight = std::max(ImGui::GetContentRegionAvail().y, rows.size() * rowHeight);
        const float contentWidth = ImGui::GetContentRegionAvail().x;
        const float areaWidth = std::max(40.f, contentWidth - labelWidth);

        // Label column
        for (size_t r = 0; r < rows.size(); ++r)
        {
            const TrackRow& row = rows[r];
            const TrackGroup& group = groups[static_cast<size_t>(row.group)];
            const float y = top.y + r * rowHeight;
            ImGui::SetCursorScreenPos(ImVec2(top.x, y));
            ImGui::PushID(static_cast<int>(r));
            ImGui::InvisibleButton("##label", ImVec2(column, rowHeight));
            const bool hovered = ImGui::IsItemHovered();
            const bool nodeSelected = row.channel < 0 && group.name == w.selectedNode;
            if (hovered || nodeSelected)
                drawList->AddRectFilled(ImVec2(top.x, y), ImVec2(top.x + column, y + rowHeight),
                                        ImGui::GetColorU32(nodeSelected ? ImGuiCol_Header : ImGuiCol_HeaderHovered));
            const float textY = y + (rowHeight - ImGui::GetFontSize()) * 0.5f;
            const float indent = std::min(group.depth * 10.f, labelWidth * 0.5f);
            if (row.channel < 0)
            {
                const bool expanded = w.expanded.count(group.name) != 0u;
                const bool hasChannels = !group.channels.empty();
                const float keyButtonX = top.x + column - 12.f;
                const bool overKeyButton = hovered && io.MousePos.x >= keyButtonX - 8.f;
                DrawDiamond(*drawList, ImVec2(keyButtonX, y + rowHeight * 0.5f), 5.f, overKeyButton ? selectedKeyColor : mutedColor);
                if (ImGui::IsItemClicked() && overKeyButton)
                {
                    w.selectedNode = group.name;
                    KeyCurrentPose(ctx, group.name, -1);
                }
                else if (ImGui::IsItemClicked())
                {
                    if (hasChannels && io.MousePos.x < top.x + indent + 16.f)
                    {
                        if (expanded) w.expanded.erase(group.name);
                        else          w.expanded.insert(group.name);
                    }
                    else
                    {
                        w.selectedNode = group.name;
                    }
                }
                const float cy = y + rowHeight * 0.5f;
                const float ax = top.x + indent;
                if (hasChannels && expanded)
                    drawList->AddTriangleFilled(ImVec2(ax + 5.f, cy - 3.f), ImVec2(ax + 13.f, cy - 3.f),
                                                ImVec2(ax + 9.f, cy + 3.f), textColor);
                else if (hasChannels)
                    drawList->AddTriangleFilled(ImVec2(ax + 6.f, cy - 4.f), ImVec2(ax + 6.f, cy + 4.f),
                                                ImVec2(ax + 12.f, cy), textColor);
                drawList->PushClipRect(ImVec2(top.x, y), ImVec2(top.x + column - 22.f, y + rowHeight), true);
                drawList->AddText(ImVec2(ax + 18.f, textY), hasChannels ? textColor : mutedColor, group.name.c_str());
                drawList->PopClipRect();
            }
            else
            {
                const AnimationChannel& shown = w.clip.channels[static_cast<size_t>(row.channel)];
                const float keyButtonX = top.x + indent + 20.f;
                const bool overKeyButton = hovered && io.MousePos.x < keyButtonX + 8.f;
                DrawDiamond(*drawList, ImVec2(keyButtonX, y + rowHeight * 0.5f), 4.5f, overKeyButton ? selectedKeyColor : mutedColor);
                if (ImGui::IsItemClicked() && overKeyButton)
                    KeyCurrentPose(ctx, group.name, static_cast<int>(shown.property));
                drawList->AddText(ImVec2(top.x + indent + 30.f, textY), mutedColor, PropertyLabel(shown.property));
                // The value at the playhead, next to the property name.
                const math::Vec3 value = DisplayValue(shown, std::min(w.time, w.clip.duration));
                static const char* const kAxisLetters[3] = {"X", "Y", "Z"};
                const ImU32 axisColors[3] = {LinearColor(235, 60, 70), LinearColor(95, 205, 60), LinearColor(55, 115, 240)};
                const float components[3] = {value.x, value.y, value.z};
                constexpr float kColumn = 62.f;
                for (int axis = 0; axis < 3; ++axis)
                {
                    char number[32];
                    std::snprintf(number, sizeof(number), "%.2f", components[axis]);
                    const float columnX = top.x + column - 4.f - (3 - axis) * kColumn;
                    drawList->AddText(ImVec2(columnX, textY), axisColors[axis], kAxisLetters[axis]);
                    drawList->AddText(ImVec2(columnX + 12.f, textY), textColor, number);
                }
                // Right-click copies the value and pastes it as a key at the playhead.
                const int channelIndex = row.channel;
                value_ui::Menu("##row_value", [value] { value_ui::Copy("float3", &value, sizeof(value)); },
                               value_ui::Compatible("float3", sizeof(math::Vec3)),
                               [channelIndex] {
                                   WindowState& state = W();
                                   if (state.modelMode)
                                       return false;
                                   math::Vec3 pasted;
                                   std::memcpy(&pasted, value_ui::Clipboard().bytes.data(), sizeof(pasted));
                                   BeginGesture();
                                   AnimationChannel& target = state.clip.channels[static_cast<size_t>(channelIndex)];
                                   anim::SetKey(target, state.time, pasted,
                                                anim::IsRotationChannel(target) ? math::Quat::FromEulerDeg(pasted.x, pasted.y, pasted.z) : math::Quat{});
                                   state.clip.duration = std::max(state.clip.duration, state.time);
                                   state.poseDirty = true;
                                   return true;
                               });
            }
            ImGui::PopID();
        }

        // Key area
        ImGui::SetCursorScreenPos(ImVec2(timelineX, top.y));
        ImGui::InvisibleButton("##anim_keys", ImVec2(areaWidth, totalHeight),
                               ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
        const bool hovered = ImGui::IsItemHovered();
        const bool activated = ImGui::IsItemActivated() && ImGui::IsMouseDown(ImGuiMouseButton_Left);
        const ImVec2 areaMin(timelineX, top.y);
        const ImVec2 areaMax(timelineX + areaWidth, top.y + totalHeight);
        drawList->PushClipRect(areaMin, areaMax, true);

        // Clip end
        const float endX = xOf(w.clip.duration);
        drawList->AddRectFilled(ImVec2(std::max(endX, areaMin.x), areaMin.y), areaMax, IM_COL32(0, 0, 0, 50));

        struct Hit { int row = -1; int key = -1; float time = 0.f; };
        Hit hit;
        for (size_t r = 0; r < rows.size(); ++r)
        {
            const TrackRow& row = rows[r];
            const float y = top.y + r * rowHeight;
            const float cy = y + rowHeight * 0.5f;
            drawList->AddLine(ImVec2(areaMin.x, y + rowHeight), ImVec2(areaMax.x, y + rowHeight), lineColor);

            std::vector<int> channels;
            if (row.channel >= 0) channels.push_back(row.channel);
            else channels = groups[static_cast<size_t>(row.group)].channels;

            for (const int channel : channels)
            {
                const AnimationChannel& data = w.clip.channels[static_cast<size_t>(channel)];
                for (size_t k = 0; k < data.times.size(); ++k)
                {
                    const float x = xOf(data.times[k]);
                    const bool selected = IsSelected(channel, static_cast<int>(k));
                    if (data.interpolation == AnimationInterpolation::Step)
                        drawList->AddRectFilled(ImVec2(x - 4.f, cy - 4.f), ImVec2(x + 4.f, cy + 4.f),
                                                selected ? selectedKeyColor : keyColor);
                    else
                        DrawDiamond(*drawList, ImVec2(x, cy), selected ? 6.f : 5.f, selected ? selectedKeyColor : keyColor);
                    if (hovered && std::fabs(io.MousePos.x - x) <= 6.f &&
                        io.MousePos.y >= y && io.MousePos.y < y + rowHeight)
                        hit = {static_cast<int>(r), static_cast<int>(k), data.times[k]};
                }
            }
        }

        if (hit.row >= 0 && !w.dragging)
        {
            const TrackRow& hitRow = rows[static_cast<size_t>(hit.row)];
            if (hitRow.channel >= 0)
            {
                const AnimationChannel& hitChannel = w.clip.channels[static_cast<size_t>(hitRow.channel)];
                const math::Vec3 value = DisplayValue(hitChannel, hit.time);
                ImGui::SetTooltip("%s  |  %s\n%s  (%.2f s)\n%.3f  %.3f  %.3f", hitChannel.targetName.c_str(),
                                  PropertyLabel(hitChannel.property), FormatTime(hit.time).c_str(), hit.time,
                                  value.x, value.y, value.z);
            }
            else
            {
                ImGui::SetTooltip("%s\n%s  (%.2f s)", groups[static_cast<size_t>(hitRow.group)].name.c_str(),
                                  FormatTime(hit.time).c_str(), hit.time);
            }
        }

        const bool nearPlayhead = hovered && std::fabs(io.MousePos.x - xOf(w.time)) <= 6.f;
        if (nearPlayhead && hit.row < 0)
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        if (w.scrubbing)
        {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
                SetTime(SnapTime(timeOf(io.MousePos.x)));
            else
                w.scrubbing = false;
        }
        if (activated && hit.row < 0 && nearPlayhead)
        {
            w.playing = false;
            w.scrubbing = true;
        }
        else if (activated)
        {
            if (hit.row >= 0)
            {
                SelectKeyAt(groups, rows[static_cast<size_t>(hit.row)], hit.key, hit.time, io.KeyCtrl);
                w.selectedNode = groups[static_cast<size_t>(rows[static_cast<size_t>(hit.row)].group)].name;
                w.expanded.insert(w.selectedNode);
                w.dragging = !w.selection.empty() && !w.modelMode;
                w.dragMouseX = io.MousePos.x;
                w.dragStartClip = w.clip;
                w.dragStartKeys = w.selection;
            }
            else if (!io.KeyCtrl)
            {
                w.selection.clear();
            }
        }
        if (w.dragging)
        {
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
            {
                w.dragging = false;
            }
            else if (std::fabs(io.MousePos.x - w.dragMouseX) > 2.f)
            {
                BeginGesture();
                const float delta = SnapTime((io.MousePos.x - w.dragMouseX) / w.pixelsPerSecond);
                w.clip = w.dragStartClip;
                w.selection = anim::MoveKeys(w.clip, w.dragStartKeys, delta);
                w.clip.duration = std::max(w.clip.duration, anim::LastKeyTime(w.clip));
                w.poseDirty = true;
            }
        }

        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && !w.modelMode)
        {
            const int rowIndex = static_cast<int>((io.MousePos.y - top.y) / rowHeight);
            w.popupTime = SnapTime(std::max(0.f, timeOf(io.MousePos.x)));
            w.popupChannelGroup = -1;
            w.popupChannel = -1;
            if (rowIndex >= 0 && rowIndex < static_cast<int>(rows.size()))
            {
                w.popupChannelGroup = rows[static_cast<size_t>(rowIndex)].group;
                w.popupChannel = rows[static_cast<size_t>(rowIndex)].channel;
            }
            if (hit.row >= 0)
            {
                const TrackRow& row = rows[static_cast<size_t>(hit.row)];
                const bool selected = row.channel >= 0 && IsSelected(row.channel, hit.key);
                if (!selected)
                    SelectKeyAt(groups, row, hit.key, hit.time, false);
            }
            ImGui::OpenPopup("##anim_key_menu");
        }

        // Zoom around the mouse, pan with the middle button or Shift + wheel.
        if (hovered && io.KeyCtrl && io.MouseWheel != 0.f)
        {
            const float pivot = timeOf(io.MousePos.x);
            w.pixelsPerSecond = std::clamp(w.pixelsPerSecond * std::pow(1.15f, io.MouseWheel), 30.f, 2000.f);
            w.viewStart = std::max(0.f, pivot - (io.MousePos.x - timelineX - kTimelineMargin) / w.pixelsPerSecond);
        }
        if (hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.f))
            w.viewStart = std::max(0.f, w.viewStart - io.MouseDelta.x / w.pixelsPerSecond);
        if (hovered && (io.MouseWheelH != 0.f || (io.KeyShift && io.MouseWheel != 0.f)))
            w.viewStart = std::max(0.f, w.viewStart - (io.MouseWheel + io.MouseWheelH) * 40.f / w.pixelsPerSecond);

        const float playheadX = xOf(w.time);
        drawList->AddLine(ImVec2(playheadX, areaMin.y), ImVec2(playheadX, areaMax.y), playheadColor, 1.5f);
        drawList->PopClipRect();

        if (ImGui::BeginPopup("##anim_key_menu"))
        {
            if (w.popupChannelGroup >= 0 &&
                ImGui::MenuItem(TranslateLabel("animation.add_key", "Add Keyframe Here").c_str()))
                AddKeysAt(w.popupChannelGroup, w.popupChannel, w.popupTime, groups);
            const bool hasSelection = !w.selection.empty();
            ImGui::BeginDisabled(!hasSelection);
            if (ImGui::MenuItem(TranslateLabel("animation.interp_linear", "Linear").c_str()))
            {
                BeginGesture();
                for (const anim::KeyRef& ref : w.selection)
                    w.clip.channels[static_cast<size_t>(ref.channel)].interpolation = AnimationInterpolation::Linear;
                w.poseDirty = true;
            }
            if (ImGui::MenuItem(TranslateLabel("animation.interp_step", "Step").c_str()))
            {
                BeginGesture();
                for (const anim::KeyRef& ref : w.selection)
                    w.clip.channels[static_cast<size_t>(ref.channel)].interpolation = AnimationInterpolation::Step;
                w.poseDirty = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem(TranslateLabel("animation.delete_keys", "Delete Keyframes").c_str(), "Del"))
                DeleteSelectedKeys();
            ImGui::EndDisabled();
            ImGui::EndPopup();
        }
    }
    ImGui::EndChild();

    // Horizontal scrollbar for the time range.
    {
        const float totalSeconds = std::max({w.clip.duration * 1.25f, w.time + 1.f, w.viewStart + visibleSeconds});
        ImGui::SetCursorScreenPos(ImVec2(timelineX, ImGui::GetCursorScreenPos().y + 2.f));
        ImGui::InvisibleButton("##anim_time_scroll", ImVec2(timelineWidth, kScrollbarHeight));
        const ImVec2 trackMin = ImGui::GetItemRectMin();
        const ImVec2 trackMax = ImGui::GetItemRectMax();
        const float thumbWidth = std::max(24.f, timelineWidth * std::min(1.f, visibleSeconds / totalSeconds));
        const float travel = std::max(1.f, timelineWidth - thumbWidth);
        const float maxStart = std::max(0.f, totalSeconds - visibleSeconds);
        if (ImGui::IsItemActive() && maxStart > 0.f)
            w.viewStart = std::clamp((io.MousePos.x - trackMin.x - thumbWidth * 0.5f) / travel, 0.f, 1.f) * maxStart;
        const float thumbX = trackMin.x + (maxStart > 0.f ? std::clamp(w.viewStart / maxStart, 0.f, 1.f) * travel : 0.f);
        ImDrawList* scrollList = ImGui::GetWindowDrawList();
        scrollList->AddRectFilled(trackMin, trackMax, ImGui::GetColorU32(ImGuiCol_ScrollbarBg), 4.f);
        scrollList->AddRectFilled(ImVec2(thumbX, trackMin.y + 1.f), ImVec2(thumbX + thumbWidth, trackMax.y - 1.f),
                                  ImGui::GetColorU32(ImGui::IsItemActive() ? ImGuiCol_ScrollbarGrabActive
                                                     : ImGui::IsItemHovered() ? ImGuiCol_ScrollbarGrabHovered : ImGuiCol_ScrollbarGrab), 4.f);
    }

    // The playhead continues through the ruler.
    ImDrawList* windowList = ImGui::GetWindowDrawList();
    windowList->PushClipRect(rulerMin, rulerMax, true);
    const float px = xOf(w.time);
    windowList->AddTriangleFilled(ImVec2(px - 5.f, rulerMax.y - 9.f), ImVec2(px + 5.f, rulerMax.y - 9.f),
                                  ImVec2(px, rulerMax.y), playheadColor);
    windowList->PopClipRect();
}

// ── 3D preview ───────────────────────────────────────────────────────────────

struct PreviewProjection
{
    math::Mat4 viewProjection;
    ImVec2 min;
    float width = 1.f;
    float height = 1.f;

    bool Project(const math::Vec3& point, ImVec2& out) const
    {
        const math::Vec4 clip = viewProjection * math::Vec4{point, 1.f};
        if (clip.w <= 0.0001f)
            return false;
        out = ImVec2(min.x + (clip.x / clip.w * 0.5f + 0.5f) * width,
                     min.y + (0.5f - clip.y / clip.w * 0.5f) * height);
        return true;
    }
};

void FrameTarget(EditorFrameContext& ctx, const TargetNodes& nodes)
{
    WindowState& w = W();
    math::Vec3 low{FLT_MAX, FLT_MAX, FLT_MAX};
    math::Vec3 high{-FLT_MAX, -FLT_MAX, -FLT_MAX};
    for (const EntityID id : nodes.all)
    {
        const auto* world = ctx.world.Get<WorldTransformComponent>(id);
        if (!world)
            continue;
        math::Vec3 boxLow = world->position;
        math::Vec3 boxHigh = world->position;
        // Meshes are often far from their pivot: frame what is really visible.
        if (const auto* bounds = ctx.world.Get<BoundsComponent>(id); bounds && ctx.world.Has<MeshComponent>(id) && !nodes.skeleton)
        {
            boxLow = bounds->centerWorld - bounds->extentsWorld;
            boxHigh = bounds->centerWorld + bounds->extentsWorld;
        }
        low = {std::min(low.x, boxLow.x), std::min(low.y, boxLow.y), std::min(low.z, boxLow.z)};
        high = {std::max(high.x, boxHigh.x), std::max(high.y, boxHigh.y), std::max(high.z, boxHigh.z)};
    }
    if (low.x > high.x)
        return;
    w.center = (low + high) * 0.5f;
    w.radius = std::max((high - low).Length() * 0.5f, 0.25f);
    // An emitter has no size of its own, but its particles need room in the view.
    for (const EntityID id : nodes.all)
        if (ctx.world.Has<engine::ParticleEmitterComponent>(id))
            w.radius = std::max(w.radius, 2.f);
    w.groundY = low.y;
    w.distance = w.radius * 3.4f;
    w.framed = true;
}

bool IsEditableNode(const TargetNodes& nodes, const std::string& name)
{
    return !nodes.skeleton || BoneIndex(*nodes.skeleton, name) >= 0;
}

float DistanceToSegment(const ImVec2& p, const ImVec2& a, const ImVec2& b)
{
    const float dx = b.x - a.x, dy = b.y - a.y;
    const float lengthSq = dx * dx + dy * dy;
    const float t = lengthSq > 0.f ? std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / lengthSq, 0.f, 1.f) : 0.f;
    const float ex = p.x - (a.x + t * dx), ey = p.y - (a.y + t * dy);
    return std::sqrt(ex * ex + ey * ey);
}

void ParentTransform(EditorFrameContext& ctx, EntityID id, math::Quat& rotation, math::Vec3& scale)
{
    rotation = math::Quat::Identity();
    scale = math::Vec3::One();
    if (const auto* parent = ctx.world.Get<ParentComponent>(id))
        if (const auto* world = ctx.world.Get<WorldTransformComponent>(parent->parent))
        {
            rotation = world->rotation;
            scale = world->scale;
        }
}

void DrawViewGizmo(ImDrawList& drawList, const ImVec2& center, const math::Vec3& forward, bool hovered, bool clicked,
                   bool orthographic, const char* viewName,
                   const std::function<void(const char*)>& onAxis, const std::function<void()>& onCenter)
{
    struct AxisHandle { math::Vec3 direction; const char* letter; const char* view; ImU32 color; bool positive; };
    const AxisHandle handles[6] = {
        {{ 1.f, 0.f, 0.f}, "X", "animation.view_right",  LinearColor(235, 60, 70),  true},
        {{-1.f, 0.f, 0.f}, "",  "animation.view_left",   LinearColor(235, 60, 70),  false},
        {{ 0.f, 1.f, 0.f}, "Y", "animation.view_top",    LinearColor(95, 205, 60),  true},
        {{ 0.f,-1.f, 0.f}, "",  "animation.view_bottom", LinearColor(95, 205, 60),  false},
        {{ 0.f, 0.f, 1.f}, "Z", "animation.view_front",  LinearColor(55, 115, 240), true},
        {{ 0.f, 0.f,-1.f}, "",  "animation.view_back",   LinearColor(55, 115, 240), false},
    };
    constexpr float kRadius = 38.f;
    const ImGuiIO& io = ImGui::GetIO();
    const math::Vec3 right = math::Vec3::Cross(forward, math::Vec3::Up()).Normalized();
    const math::Vec3 up = math::Vec3::Cross(right, forward).Normalized();
    const auto dot = [](const math::Vec3& a, const math::Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; };
    const auto tipOf = [&](const AxisHandle& h) {
        return ImVec2(center.x + dot(h.direction, right) * kRadius, center.y - dot(h.direction, up) * kRadius);
    };

    // Far handles first, so near ones cover them.
    std::array<int, 6> order{0, 1, 2, 3, 4, 5};
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        return dot(handles[a].direction, forward) > dot(handles[b].direction, forward);
    });

    drawList.AddCircleFilled(center, kRadius + 12.f, IM_COL32(0, 0, 0, 70), 40);
    const float mouseDistance = std::hypot(io.MousePos.x - center.x, io.MousePos.y - center.y);
    const bool areaHovered = hovered && mouseDistance < kRadius + 12.f;
    const bool centerHovered = areaHovered && mouseDistance < 9.f;
    int hoveredHandle = -1;
    for (const int index : order) // the last match is the nearest handle
    {
        const ImVec2 tip = tipOf(handles[index]);
        if (areaHovered && !centerHovered &&
            std::hypot(io.MousePos.x - tip.x, io.MousePos.y - tip.y) < (handles[index].positive ? 11.f : 9.f))
            hoveredHandle = index;
    }
    for (const int index : order)
    {
        const AxisHandle& handle = handles[index];
        const ImVec2 tip = tipOf(handle);
        const bool isHovered = hoveredHandle == index;
        if (handle.positive)
        {
            drawList.AddLine(center, tip, handle.color, 2.5f);
            drawList.AddCircleFilled(tip, isHovered ? 12.f : 10.f, handle.color, 24);
            const ImVec2 letterSize = ImGui::CalcTextSize(handle.letter);
            drawList.AddText(ImVec2(tip.x - letterSize.x * 0.5f, tip.y - letterSize.y * 0.5f), IM_COL32(255, 255, 255, 255), handle.letter);
        }
        else
        {
            drawList.AddCircleFilled(tip, isHovered ? 9.f : 7.f, LinearColor(40, 44, 52), 24);
            drawList.AddCircle(tip, isHovered ? 9.f : 7.f, handle.color, 24, 1.5f);
        }
    }
    drawList.AddCircleFilled(center, centerHovered ? 8.f : 6.f,
                             orthographic ? LinearColor(235, 240, 245) : LinearColor(120, 128, 140), 20);

    (void)viewName;
    const std::string label = Translate(orthographic ? "animation.view_ortho" : "animation.view_persp");
    const ImVec2 labelSize = ImGui::CalcTextSize(label.c_str());
    drawList.AddText(ImVec2(center.x - labelSize.x * 0.5f, center.y + kRadius + 14.f), IM_COL32(235, 240, 245, 255), label.c_str());

    if (centerHovered || hoveredHandle >= 0)
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    if (clicked && centerHovered)
        onCenter();
    else if (clicked && hoveredHandle >= 0)
        onAxis(handles[hoveredHandle].view);
}

void SetAxisView(const char* key)
{
    WindowState& w = W();
    const std::string view(key);
    w.orthographic = true;
    w.viewName = key;
    w.pitchDeg = view == "animation.view_top" ? 89.9f : (view == "animation.view_bottom" ? -89.9f : 0.f);
    w.yawDeg = view == "animation.view_back" ? 180.f
             : view == "animation.view_right" ? 90.f
             : view == "animation.view_left" ? -90.f : 0.f;
}

// Same controls as the particle bar of the viewport, for the emitters of the copy.
void DrawPreviewParticleBar(EditorFrameContext& ctx, const TargetNodes& nodes, const ImVec2& position)
{
    std::vector<EntityID> emitters;
    for (const EntityID id : nodes.all)
        if (ctx.world.Has<ParticleEmitterComponent>(id))
            emitters.push_back(id);
    if (emitters.empty())
        return;

    size_t active = 0u, looping = 0u;
    uint32_t particles = 0u;
    for (const EntityID id : emitters)
    {
        const auto* emitter = ctx.world.Get<ParticleEmitterComponent>(id);
        active += emitter->playing ? 1u : 0u;
        looping += emitter->looping ? 1u : 0u;
        if (ctx.animationParticleCount)
            particles += ctx.animationParticleCount(id);
    }

    ImGui::SetCursorScreenPos(position);
    ImVec4 background = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
    background.w = 0.88f;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, background);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.f, 6.f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.f);
    const bool open = ImGui::BeginChild("##preview_particles", ImVec2(0.f, 0.f),
        ImGuiChildFlags_AutoResizeX | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_Borders,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    if (open)
    {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(ICON_LC_SPARKLES);
        ImGui::SameLine();
        const std::string restart = std::string(ICON_LC_REFRESH_CW "  ") + Translate("common.restart") + "##preview_pe_restart";
        if (ImGui::Button(restart.c_str()) && ctx.restartAnimationParticles)
            for (const EntityID id : emitters)
                ctx.restartAnimationParticles(id);

        const auto toggle = [&](const char* key, const char* fallback, size_t on, bool ParticleEmitterComponent::* member) {
            ImGui::SameLine();
            bool value = on == emitters.size();
            if (ImGui::Checkbox(TranslateLabel(key, fallback).c_str(), &value))
                for (const EntityID id : emitters)
                    ctx.world.Get<ParticleEmitterComponent>(id)->*member = value;
        };
        toggle("field.enabled", "Enabled##preview_pe_on", active, &ParticleEmitterComponent::playing);
        toggle("field.loop", "Loop##preview_pe_loop", looping, &ParticleEmitterComponent::looping);

        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%u", particles);
    }
    ImGui::EndChild();
}

void DrawPreviewPane(EditorFrameContext& ctx, const TargetNodes& nodes)
{
    WindowState& w = W();
    const ImGuiIO& io = ImGui::GetIO();

    const ImVec2 available = ImGui::GetContentRegionAvail();
    const float width = std::max(64.f, available.x);
    const float size = std::max(64.f, available.y); // height of the view
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const ImVec2 max(min.x + width, min.y + size);
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##anim_preview", ImVec2(width, size),
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    const bool activated = ImGui::IsItemActivated() && ImGui::IsMouseDown(ImGuiMouseButton_Left);

    // World transforms and bounds of a new copy exist only after the next transform update.
    if (!w.framed && w.copyAge >= 3)
        FrameTarget(ctx, nodes);

    if (hovered)
    {
        ImGui::SetKeyOwner(ImGuiKey_MouseWheelY, ImGui::GetItemID());
        // Ctrl + wheel moves the view straight up or down, the wheel alone zooms.
        if (io.MouseWheel != 0.f && io.KeyCtrl)
            w.center.y += io.MouseWheel * w.radius * 0.15f;
        else if (io.MouseWheel != 0.f)
            w.distance = std::clamp(w.distance * std::pow(0.9f, io.MouseWheel), w.radius * 0.5f, w.radius * 30.f);
    }
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Right, 0.f) && !io.KeyCtrl)
    {
        w.yawDeg -= io.MouseDelta.x * 0.4f;
        w.pitchDeg = std::clamp(w.pitchDeg + io.MouseDelta.y * 0.4f, -89.9f, 89.9f);
        w.viewName = nullptr;
    }
    w.previewHovered = hovered;
    w.previewVisible = true;
    if (hovered)
        w.ownsGizmos = true;
    w.previewMin = min;
    w.previewSize = ImVec2(width, size);

    const float yaw = w.yawDeg * math::DEG_TO_RAD;
    const float pitch = w.pitchDeg * math::DEG_TO_RAD;
    const math::Vec3 toEye{std::cos(pitch) * std::sin(yaw), std::sin(pitch), std::cos(pitch) * std::cos(yaw)};
    // Middle drag, or Ctrl + moving the mouse: up/down/left/right as seen in this view.
    if ((ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.f)) ||
        (hovered && io.KeyCtrl && !ImGui::IsMouseDown(ImGuiMouseButton_Left)))
    {
        const math::Vec3 right = math::Vec3::Cross(math::Vec3::Up(), toEye).Normalized();
        const math::Vec3 up = math::Vec3::Cross(toEye, right).Normalized();
        const float unitsPerPixel = 2.f * w.distance * std::tan(kAnimationPreviewFovDeg * 0.5f * math::DEG_TO_RAD) / size;
        w.center = w.center - right * (io.MouseDelta.x * unitsPerPixel) + up * (io.MouseDelta.y * unitsPerPixel);
    }

    // Right mouse button + W/A/S/D/Q/E flies the camera, as in the editor viewport.
    if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Right))
    {
        const math::Vec3 flyRight = math::Vec3::Cross(math::Vec3::Up(), toEye).Normalized();
        const float speed = w.radius * (io.KeyShift ? 4.5f : 1.5f) * io.DeltaTime;
        if (ImGui::IsKeyDown(ImGuiKey_W)) w.center = w.center - toEye * speed;
        if (ImGui::IsKeyDown(ImGuiKey_S)) w.center = w.center + toEye * speed;
        if (ImGui::IsKeyDown(ImGuiKey_A)) w.center = w.center - flyRight * speed;
        if (ImGui::IsKeyDown(ImGuiKey_D)) w.center = w.center + flyRight * speed;
        if (ImGui::IsKeyDown(ImGuiKey_E)) w.center.y += speed;
        if (ImGui::IsKeyDown(ImGuiKey_Q)) w.center.y -= speed;
    }

    // Orthographic is a very long lens: same framing, parallel lines, and every
    // tool that works in perspective (gizmos, picking) keeps working.
    constexpr float kOrthoFovDeg = 1.f;
    const float fovDeg = w.orthographic ? kOrthoFovDeg : kAnimationPreviewFovDeg;
    const float eyeDistance = w.orthographic
        ? w.distance * std::tan(kAnimationPreviewFovDeg * 0.5f * math::DEG_TO_RAD) / std::tan(kOrthoFovDeg * 0.5f * math::DEG_TO_RAD)
        : w.distance;
    const float nearPlane = w.orthographic ? std::max(0.1f, eyeDistance - w.radius * 40.f) : kAnimationPreviewNear;
    const math::Vec3 eye = w.center + toEye * eyeDistance;
    ctx.state.animationPreviewFovDeg = fovDeg;
    ctx.state.animationPreviewNear = nearPlane;
    ctx.state.animationPreviewEye = eye;
    ctx.state.animationPreviewTarget = w.center;
    ctx.state.animationPreviewFar = eyeDistance + std::max(w.radius * 40.f, 1000.f);
    ctx.state.animationPreviewGridCamera = w.center + toEye * w.distance;
    ctx.state.animationPreviewAspect = width / size;

    PreviewProjection projection;
    projection.viewProjection =
        math::Mat4::PerspectiveFovRH(fovDeg * math::DEG_TO_RAD, ctx.state.animationPreviewAspect,
                                     nearPlane, ctx.state.animationPreviewFar) *
        math::Mat4::LookAtRH(eye, w.center, math::Vec3::Up());
    projection.min = min;
    projection.width = width;
    projection.height = size;

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(min, max, IM_COL32(26, 28, 33, 255));
    if (void* texture = ctx.animationPreviewTexture ? ctx.animationPreviewTexture() : nullptr)
    {
        const ImVec2 uv0 = ctx.animationPreviewFlipY ? ImVec2(0.f, 1.f) : ImVec2(0.f, 0.f);
        const ImVec2 uv1 = ctx.animationPreviewFlipY ? ImVec2(1.f, 0.f) : ImVec2(1.f, 1.f);
        drawList->AddImage(reinterpret_cast<ImTextureID>(texture), min, max, uv0, uv1);
    }
    drawList->AddRect(min, max, ImGui::GetColorU32(theme::kBorder));
    drawList->PushClipRect(min, max, true);

    // Bones / objects
    const ImU32 lineColor = LinearColor(120, 200, 255, 0.6f);
    const ImU32 dotColor = LinearColor(190, 225, 255, 0.9f);
    const ImU32 selectedColor = ImGui::GetColorU32(theme::kAccentHover);
    std::string pickedNode;
    float pickedDistance = 9.f;
    for (const auto& [name, id] : nodes.byName)
    {
        if (!IsEditableNode(nodes, name))
            continue;
        const auto* world = ctx.world.Get<WorldTransformComponent>(id);
        ImVec2 point;
        if (!world || !projection.Project(world->position, point))
            continue;
        if (const auto* parent = ctx.world.Get<ParentComponent>(id))
            if (const auto* parentName = ctx.world.Get<NameComponent>(parent->parent);
                parentName && IsEditableNode(nodes, parentName->name) && nodes.byName.count(parentName->name))
                if (const auto* parentWorld = ctx.world.Get<WorldTransformComponent>(parent->parent))
                {
                    ImVec2 parentPoint;
                    if (projection.Project(parentWorld->position, parentPoint))
                        drawList->AddLine(parentPoint, point, lineColor, 1.5f);
                }
        const bool selected = name == w.selectedNode;
        drawList->AddCircleFilled(point, selected ? 5.f : 3.5f, selected ? selectedColor : dotColor);
        const float distance = std::sqrt((io.MousePos.x - point.x) * (io.MousePos.x - point.x) +
                                         (io.MousePos.y - point.y) * (io.MousePos.y - point.y));
        if (hovered && distance < pickedDistance)
        {
            pickedDistance = distance;
            pickedNode = name;
        }
    }

    // The gizmos themselves are the editor's own (RunInAnimationPreview).
    const EditorState& gizmoState = ctx.state;
    const bool handleBusy = gizmoState.gizmoHoveredAxis >= 0 || gizmoState.gizmoDragAxis >= 0 ||
        gizmoState.rotGizmoHoveredAxis >= 0 || gizmoState.rotGizmoDragAxis >= 0 ||
        gizmoState.sclGizmoHoveredAxis >= 0 || gizmoState.sclGizmoDragAxis >= 0;
    if (activated && !handleBusy && !pickedNode.empty())
        w.selectedNode = pickedNode;

    // Which way is which, and what is being edited.
    {
        const math::Vec3 forward = (w.center - eye).Normalized();
        DrawViewGizmo(*drawList, ImVec2(max.x - 62.f, min.y + 62.f), forward, hovered, activated, w.orthographic, w.viewName,
                      [](const char* view) { SetAxisView(view); },
                      [&w] { w.orthographic = !w.orthographic; w.viewName = nullptr; });
        if (!w.selectedNode.empty())
            drawList->AddText(ImVec2(min.x + ImGui::CalcTextSize(TranslateLabel("animation.frame_object", "Frame Object").c_str(), nullptr, true).x + 34.f, min.y + 8.f),
                              IM_COL32(235, 240, 245, 255), w.selectedNode.c_str());
    }
    drawList->PopClipRect();

    const ImVec2 restoreCursor = ImGui::GetCursorScreenPos();
    DrawPreviewParticleBar(ctx, nodes, ImVec2(min.x + 10.f, max.y - ImGui::GetFrameHeight() - 22.f));
    const std::string frameLabel = TranslateLabel("animation.frame_object", "Frame Object");
    ImGui::SetCursorScreenPos(ImVec2(min.x + 6.f, min.y + 6.f));
    if (ImGui::SmallButton(frameLabel.c_str()))
        w.framed = false;
    ImGui::SetCursorScreenPos(restoreCursor);
    ImGui::Dummy(ImVec2(0.f, 0.f)); // ImGui requires an item after moving the cursor back
}

} // namespace

bool AnimationFileExists(EditorFrameContext& ctx, const std::string& relativePath);

// A skinned mesh sits beside its bones: edit from the rig's top player.
EntityID AnimatorTarget(EditorFrameContext& ctx, EntityID entity)
{
    EntityID target = entity;
    if (ctx.world.Has<SkinComponent>(entity))
    {
        EntityID current = entity;
        for (uint32_t depth = 0u; current.IsValid() && depth < 1024u; ++depth)
        {
            if (ctx.world.Has<AnimationPlayerComponent>(current))
                target = current;
            const auto* parent = ctx.world.Get<ParentComponent>(current);
            current = parent ? parent->parent : NULL_ENTITY;
        }
        // The mesh may sit beside its bones: use the animator that owns those bones.
        if (target == entity)
            ctx.world.View<AnimationPlayerComponent>([&](EntityID id, AnimationPlayerComponent&) {
                if (id != entity && !ctx.world.Has<SkinComponent>(id) && CollectNodes(ctx, id).skinEntity == entity)
                    target = id;
            });
    }
    return target;
}

// Model whose clips an animator edits: its clip reference, else the rig's model
// when that has animations; empty for .anim and objects without model clips.
std::string AnimatorModel(EditorFrameContext& ctx, EntityID target, const std::string& clipAssetPath)
{
    const std::string base = clipAssetPath.substr(0, clipAssetPath.rfind('#'));
    if (anim::IsModelFilePath(base))
        return base;
    if (!clipAssetPath.empty() || !ctx.assetPipeline)
        return {};
    const std::string model = RigModelPath(ctx, CollectNodes(ctx, target));
    if (!model.empty() && !GetModelClips(ctx.assetPipeline->GetAssetRoot(), model).settings.clips.empty())
        return model;
    return {};
}

// The open window shows the animator of whatever is selected.
void FollowSelection(EditorFrameContext& ctx)
{
    const EntityID selected = ctx.state.selectedEntity;
    if (selected == W().followSelected)
        return;
    W().followSelected = selected;
    if (ctx.playActive || !selected.IsValid() || !ctx.world.IsAlive(selected))
        return;
    const auto* player = ctx.world.Get<AnimationPlayerComponent>(selected);
    if (!player)
        return;
    WindowState& w = W();
    const EntityID target = AnimatorTarget(ctx, selected);
    if (w.session && ResolveTarget(ctx) && w.target == target)
        return;
    const std::string path = player->clipAssetPath.substr(0, player->clipAssetPath.rfind('#'));
    if (const std::string model = AnimatorModel(ctx, target, player->clipAssetPath); !model.empty())
        RunAfterUnsavedCheck(ctx, [&ctx, model, target, clip = player->clipAssetPath] { OpenModelClips(ctx, model, target, clip); });
    else if (AnimationFileExists(ctx, path))
        RunAfterUnsavedCheck(ctx, [&ctx, path, target] { OpenAnimation(ctx, path, target); });
    else
        RunAfterUnsavedCheck(ctx, [&ctx] { EndSession(ctx); }); // nothing editable yet: the window offers "New"
}

// The animator of the selection that uses this controller: the selected object's
// own or the rig's top animator. Outside Play only.
static EntityID FindControllerPreviewCandidate(EditorFrameContext& ctx, const std::string& controllerPath)
{
    const EntityID selected = ctx.state.selectedEntity;
    if (controllerPath.empty() || ctx.playActive || !selected.IsValid() || !ctx.world.IsAlive(selected) ||
        ctx.world.Has<EditorRuntimeGizmoTag>(selected))
        return NULL_ENTITY;
    for (const EntityID id : {selected, AnimatorTarget(ctx, selected)})
        if (const auto* player = ctx.world.Get<AnimationPlayerComponent>(id); player && player->controllerAssetPath == controllerPath)
            return id;
    return NULL_ENTITY;
}

static void BeginControllerPreview(EditorFrameContext& ctx, EntityID target, const std::string& controllerPath)
{
    AnimationClip placeholder; // the session needs a clip; controller mode never shows or saves it
    placeholder.name = "controller";
    BeginSession(ctx, target, controllerPath, {placeholder}, 0);
    WindowState& w = W();
    w.controllerMode = true;
    w.controllerPath = controllerPath;
    w.controllerAsked.clear();
    w.recording = false;
    w.playing = false;
}

// While the graph window shows the controller of the selected animator, the
// window plays that controller; otherwise a running preview ends. True while
// the preview owns the window (FollowSelection is skipped).
static bool SyncControllerPreview(EditorFrameContext& ctx)
{
    WindowState& w = W();
    static const std::string none;
    const std::string& path = ctx.state.controllerWindowOpen ? ControllerGraphPath() : none;
    if (w.controllerSuppressed && ctx.state.selectedEntity != w.suppressedSelection)
        w.controllerSuppressed = false;
    if (path.empty())
        w.controllerRequested = false;
    const EntityID candidate = (w.controllerSuppressed || !w.controllerRequested) ? NULL_ENTITY
                                                                                  : FindControllerPreviewCandidate(ctx, path);
    if (candidate.IsValid())
    {
        const bool running = w.session && w.controllerMode && w.controllerPath == path && ResolveTarget(ctx) && w.target == candidate;
        // Only a declined "unsaved changes" question blocks the start (it is not asked
        // again every frame); BeginControllerPreview clears the mark, so a preview that
        // ended for any other reason starts again.
        const std::string key = path + "#" + std::to_string(candidate.value);
        if (!running && w.controllerAsked != key && !w.pendingAction)
        {
            w.controllerAsked = key;
            RunAfterUnsavedCheck(ctx, [&ctx, candidate, path] { BeginControllerPreview(ctx, candidate, path); });
        }
        return true;
    }
    w.controllerAsked.clear();
    // The preview keeps running while nothing else with an animator is selected
    // (a clip clicked in the asset browser, an empty spot in the hierarchy).
    const EntityID selected = ctx.state.selectedEntity;
    const bool otherAnimator = selected.IsValid() && ctx.world.IsAlive(selected) &&
                               !ctx.world.Has<EditorRuntimeGizmoTag>(selected) &&
                               (ctx.world.Has<AnimationPlayerComponent>(selected) ||
                                ctx.world.Has<AnimationPlayerComponent>(AnimatorTarget(ctx, selected)));
    if (w.session && w.controllerMode && !path.empty() && w.controllerPath == path && !ctx.playActive &&
        !otherAnimator && ResolveTarget(ctx))
        return true;
    if (w.session && w.controllerMode)
    {
        EndSession(ctx);
        w.followSelected = NULL_ENTITY; // FollowSelection picks up the selection again
    }
    return false;
}

// An explicit open of clips keeps the controller preview away (see controllerSuppressed).
static void SuppressControllerPreview(EditorFrameContext& ctx)
{
    WindowState& w = W();
    w.controllerSuppressed = true;
    w.suppressedSelection = ctx.state.selectedEntity;
}

void OpenAnimationEditor(EditorFrameContext& ctx, EntityID entity)
{
    if (!ctx.assetPipeline || !entity.IsValid() || !ctx.world.IsAlive(entity))
        return;
    SuppressControllerPreview(ctx);
    ctx.state.animationWindowOpen = true;
    ImGui::SetWindowFocus(TranslateLabel("animation.title", "Animation##editor").c_str());

    const EntityID target = AnimatorTarget(ctx, entity);

    const auto* player = ctx.world.Get<AnimationPlayerComponent>(entity);
    const std::string clipAssetPath = player ? player->clipAssetPath : std::string{};
    const std::string path = clipAssetPath.substr(0, clipAssetPath.rfind('#'));
    if (const std::string model = AnimatorModel(ctx, target, clipAssetPath); !model.empty())
    {
        WindowState& w = W();
        if (w.session && w.modelMode && w.clipPath == model && ResolveTarget(ctx) && w.target == target)
            return;
        RunAfterUnsavedCheck(ctx, [&ctx, model, target, clipAssetPath] { OpenModelClips(ctx, model, target, clipAssetPath); });
    }
    else if (AnimationFileExists(ctx, path))
    {
        WindowState& w = W();
        if (w.session && w.clipPath == path && ResolveTarget(ctx) && w.target == target)
            return;
        RunAfterUnsavedCheck(ctx, [&ctx, path, target] { OpenAnimation(ctx, path, target); });
    }
    else
    {
        RunAfterUnsavedCheck(ctx, [&ctx, target] { CreateAnimation(ctx, target); });
    }
}

bool AnimationFileExists(EditorFrameContext& ctx, const std::string& relativePath)
{
    if (!ctx.assetPipeline || PathFromUtf8(relativePath).extension() != ".anim")
        return false;
    std::error_code ec;
    return std::filesystem::is_regular_file(AbsoluteClipPath(ctx, relativePath), ec);
}

void OpenAnimationFile(EditorFrameContext& ctx, const std::string& relativePath)
{
    if (!ctx.assetPipeline)
        return;
    SuppressControllerPreview(ctx);
    // Prefer the object already using this animation, otherwise the selection.
    EntityID target = NULL_ENTITY;
    ctx.world.View<AnimationPlayerComponent>([&](EntityID id, AnimationPlayerComponent& player) {
        if (player.clipAssetPath.substr(0, player.clipAssetPath.rfind('#')) == relativePath &&
            !ctx.world.Has<SkinComponent>(id))
            target = id;
    });
    // Then the selected object's animator.
    const EntityID selected = ctx.state.selectedEntity;
    if (!target.IsValid() && selected.IsValid() && ctx.world.IsAlive(selected) &&
        ctx.world.Has<AnimationPlayerComponent>(selected))
        target = AnimatorTarget(ctx, selected);
    // Then an animator whose hierarchy has the objects the clip animates.
    if (!target.IsValid())
    {
        std::ifstream in(AbsoluteClipPath(ctx, relativePath), std::ios::binary);
        const std::string source((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::vector<AnimationClip> clips;
        if (anim::ParseAnimationSet(source, clips))
            for (const AnimationClip& clip : clips)
                if (!clip.channels.empty() && !target.IsValid())
                    ctx.world.View<AnimationPlayerComponent>([&](EntityID id, AnimationPlayerComponent&) {
                        if (!target.IsValid() && !ctx.world.Has<SkinComponent>(id) &&
                            !ctx.world.Has<EditorRuntimeGizmoTag>(id) &&
                            CollectNodes(ctx, id).byName.count(clip.channels.front().targetName))
                            target = id;
                    });
    }
    // Last, whatever is selected.
    if (!target.IsValid())
        target = selected;
    if (!target.IsValid() || !ctx.world.IsAlive(target))
    {
        ctx.lastFileMessage = Translate("animation.select_object");
        return;
    }
    ctx.state.animationWindowOpen = true;
    // A docked window may sit behind another tab.
    ImGui::SetWindowFocus(TranslateLabel("animation.title", "Animation##editor").c_str());
    RunAfterUnsavedCheck(ctx, [&ctx, relativePath, target] { OpenAnimation(ctx, relativePath, target); });
}

void OpenModelClip(EditorFrameContext& ctx, const std::string& clipAssetPath)
{
    if (!ctx.assetPipeline)
        return;
    SuppressControllerPreview(ctx);
    const std::string model = clipAssetPath.substr(0, clipAssetPath.rfind('#'));
    // The object playing a clip of this model, otherwise the selected animator,
    // otherwise an animator whose rig comes from this model.
    EntityID target = NULL_ENTITY;
    ctx.world.View<AnimationPlayerComponent>([&](EntityID id, AnimationPlayerComponent& player) {
        if (!target.IsValid() && !ctx.world.Has<EditorRuntimeGizmoTag>(id) &&
            player.clipAssetPath.substr(0, player.clipAssetPath.rfind('#')) == model)
            target = AnimatorTarget(ctx, id);
    });
    const EntityID selected = ctx.state.selectedEntity;
    if (!target.IsValid() && selected.IsValid() && ctx.world.IsAlive(selected) &&
        ctx.world.Has<AnimationPlayerComponent>(selected))
        target = AnimatorTarget(ctx, selected);
    if (!target.IsValid())
        ctx.world.View<AnimationPlayerComponent>([&](EntityID id, AnimationPlayerComponent&) {
            if (!target.IsValid() && !ctx.world.Has<EditorRuntimeGizmoTag>(id) && !ctx.world.Has<SkinComponent>(id) &&
                RigModelPath(ctx, CollectNodes(ctx, id)) == model)
                target = id;
        });
    if (!target.IsValid() || !ctx.world.IsAlive(target))
    {
        ctx.lastFileMessage = Translate("animation.select_object");
        return;
    }
    ctx.state.animationWindowOpen = true;
    ImGui::SetWindowFocus(TranslateLabel("animation.title", "Animation##editor").c_str());
    RunAfterUnsavedCheck(ctx, [&ctx, model, target, clipAssetPath] { OpenModelClips(ctx, model, target, clipAssetPath); });
}

std::vector<std::string> AnimationClipNames(EditorFrameContext& ctx, const std::string& relativePath)
{
    std::vector<std::string> names;
    if (!ctx.assetPipeline)
        return names;
    static std::unordered_map<std::string, std::pair<std::filesystem::file_time_type, std::vector<std::string>>> cache;
    const std::filesystem::path path = AbsoluteClipPath(ctx, relativePath);
    std::error_code ec;
    const auto stamp = std::filesystem::last_write_time(path, ec);
    if (ec)
        return names;
    if (const auto it = cache.find(relativePath); it != cache.end() && it->second.first == stamp)
        return it->second.second;
    std::ifstream in(path, std::ios::binary);
    const std::string source((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<AnimationClip> clips;
    if (anim::ParseAnimationSet(source, clips))
        for (const AnimationClip& clip : clips)
            names.push_back(clip.name);
    cache[relativePath] = {stamp, names};
    return names;
}

bool SaveOpenAnimation(EditorFrameContext& ctx)
{
    return SaveAnimation(ctx);
}

bool SaveOpenAnimationIfChanged(EditorFrameContext& ctx)
{
    const WindowState& w = W();
    return w.session && w.dirty && SaveAnimation(ctx);
}

EntityID GetAnimationPreviewRoot()
{
    return W().copyRoot;
}

AnimationPlayerComponent* ControllerLivePlayer(EditorFrameContext& ctx, const std::string& controllerPath)
{
    const auto bound = [&](AnimationPlayerComponent* player) {
        return player && player->controllerAssetPath == controllerPath && player->controllerState.bound &&
               player->controllerState.controller ? player : nullptr;
    };
    if (controllerPath.empty())
        return nullptr;
    if (!ctx.playActive)
        return bound(PreviewPlayer(ctx));
    // Play: the selected object's animator in the running game.
    const EntityID selected = ctx.state.selectedEntity;
    if (!selected.IsValid() || !ctx.world.IsAlive(selected))
        return nullptr;
    for (const EntityID id : {selected, AnimatorTarget(ctx, selected)})
        if (AnimationPlayerComponent* player = bound(ctx.world.Get<AnimationPlayerComponent>(id)))
            return player;
    return nullptr;
}

EntityID ControllerPreviewCandidate(EditorFrameContext& ctx, const std::string& controllerPath)
{
    return FindControllerPreviewCandidate(ctx, controllerPath);
}

bool ControllerPreviewRunning(const std::string& controllerPath)
{
    const WindowState& w = W();
    return !controllerPath.empty() && w.session && w.controllerMode && w.controllerPath == controllerPath;
}

void RestartControllerPreview(EditorFrameContext& ctx)
{
    WindowState& w = W();
    w.controllerPaused = false;
    if (AnimationPlayerComponent* player = PreviewPlayer(ctx); player && player->controllerState.controller)
        ::engine::addons::animation::ResetAnimationController(*player->controllerState.controller, player->controllerState.instance);
}

bool ControllerPreviewPaused()
{
    return W().controllerPaused;
}

bool ControllerPreviewLastEvent(std::string& name, float& age)
{
    const WindowState& w = W();
    if (!w.controllerMode || w.previewEventName.empty())
        return false;
    name = w.previewEventName;
    age = static_cast<float>(ImGui::GetTime() - w.previewEventTime);
    return true;
}

int AddAnimationEvent(float time)
{
    return AddClipEvent(time);
}

const std::vector<assets::AnimationEvent>* AnimationWindowEvents()
{
    const WindowState& w = W();
    return w.session && !w.controllerMode ? &w.clip.events : nullptr;
}

void SelectAnimationEvent(int index)
{
    W().selectedEvent = index;
}

int SelectedAnimationEvent()
{
    return EventSelected() ? W().selectedEvent : -1;
}

bool DeleteSelectedAnimationEvent()
{
    return DeleteSelectedEvent();
}

bool AnimationEventScreenPosition(int index, float& x, float& y)
{
    const auto& markers = W().eventMarkers;
    const size_t slot = static_cast<size_t>(index + 1);
    if (index < -1 || slot >= markers.size())
        return false;
    x = markers[slot].first;
    y = markers[slot].second;
    return true;
}

void SetControllerPreviewPaused(bool paused)
{
    W().controllerPaused = paused;
}

bool CreateAnimationClip(EditorFrameContext& ctx, EntityID entity)
{
    if (ctx.assetPipeline)
        SuppressControllerPreview(ctx);
    return ctx.assetPipeline && CreateAnimation(ctx, entity);
}

void StopControllerPreview(EditorFrameContext& ctx)
{
    WindowState& w = W();
    w.controllerRequested = false;
    w.controllerPaused = false;
    if (w.session && w.controllerMode)
    {
        EndSession(ctx);
        w.followSelected = NULL_ENTITY; // the window shows the object's clips again, with gizmos
    }
}

void StartControllerPreview(EditorFrameContext& ctx)
{
    WindowState& w = W();
    w.controllerRequested = true;
    w.controllerSuppressed = false;
    w.controllerAsked.clear();
    ctx.state.animationWindowOpen = true;
    // The graph keeps the focus: the animation window opens without taking it.
    w.keepControllerFocus = 2;
}

namespace {

void ViewAngles(const char* key, float& yawDeg, float& pitchDeg)
{
    const std::string view(key);
    pitchDeg = view == "animation.view_top" ? -89.9f : (view == "animation.view_bottom" ? 89.9f : 0.f);
    yawDeg = view == "animation.view_back" ? 180.f
           : view == "animation.view_right" ? 90.f
           : view == "animation.view_left" ? -90.f : 0.f;
}

math::Vec3 ForwardOf(const EditorCameraState& camera)
{
    return math::Quat::FromEulerDeg(camera.pitchDeg, camera.yawDeg, 0.f).Rotate({0.f, 0.f, -1.f}).Normalized();
}

constexpr float kOrthoLensDeg = 1.f;

void SetViewportOrthographic(EditorFrameContext& ctx, bool enable)
{
    EditorState& state = ctx.state;
    EditorCameraState& camera = state.editorCamera;
    if (state.viewportOrthographic == enable)
        return;
    const math::Vec3 forward = ForwardOf(camera);
    if (enable)
    {
        float pivotDistance = 10.f;
        if (const auto* world = ctx.world.Get<WorldTransformComponent>(state.selectedEntity))
            pivotDistance = std::max((world->position - camera.position).Length(), 1.f);
        state.viewportPerspectiveFov = camera.fovDeg;
        state.viewportPerspectiveNear = camera.nearPlane;
        state.viewportPerspectiveFar = camera.farPlane;
        const float longDistance = pivotDistance * std::tan(camera.fovDeg * 0.5f * math::DEG_TO_RAD) /
                                   std::tan(kOrthoLensDeg * 0.5f * math::DEG_TO_RAD);
        camera.position = camera.position + forward * (pivotDistance - longDistance);
        state.viewportOrthoDistance = longDistance;
        camera.fovDeg = kOrthoLensDeg;
        camera.nearPlane = std::max(0.1f, longDistance * 0.2f);
        camera.farPlane = longDistance * 3.f + 2000.f;
    }
    else
    {
        const float pivotDistance = state.viewportOrthoDistance * std::tan(kOrthoLensDeg * 0.5f * math::DEG_TO_RAD) /
                                    std::tan(state.viewportPerspectiveFov * 0.5f * math::DEG_TO_RAD);
        camera.position = camera.position + forward * (state.viewportOrthoDistance - pivotDistance);
        camera.fovDeg = state.viewportPerspectiveFov;
        camera.nearPlane = state.viewportPerspectiveNear;
        camera.farPlane = state.viewportPerspectiveFar;
        state.viewportViewName = nullptr;
    }
    state.viewportOrthographic = enable;
}

} // namespace

void DrawViewportViewGizmo(EditorFrameContext& ctx, const ImVec2& origin, float width)
{
    EditorState& state = ctx.state;
    state.gizmoScaleFactor = std::tan(state.editorCamera.fovDeg * 0.5f * math::DEG_TO_RAD) /
                             std::tan((state.viewportOrthographic ? state.viewportPerspectiveFov : state.editorCamera.fovDeg) * 0.5f * math::DEG_TO_RAD);

    // Orthographic views have no useful depth movement: Ctrl + mouse moves within the view.
    const ImGuiIO& io = ImGui::GetIO();
    if (state.viewportOrthographic && ctx.sceneViewHovered && io.KeyCtrl && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
    {
        EditorCameraState& camera = state.editorCamera;
        const math::Vec3 forward = ForwardOf(camera);
        const math::Vec3 right = math::Vec3::Cross(forward, math::Vec3::Up()).Normalized();
        const math::Vec3 up = math::Vec3::Cross(right, forward).Normalized();
        const float unitsPerPixel = 2.f * state.viewportOrthoDistance * std::tan(camera.fovDeg * 0.5f * math::DEG_TO_RAD) /
                                    std::max(ctx.sceneViewHeight, 1.f);
        camera.position = camera.position - right * (io.MouseDelta.x * unitsPerPixel) + up * (io.MouseDelta.y * unitsPerPixel);
    }

    const ImVec2 center(origin.x + width - 62.f, origin.y + 62.f);
    ImGui::SetCursorScreenPos(ImVec2(center.x - 52.f, center.y - 52.f));
    ImGui::InvisibleButton("##viewport_view_gizmo", ImVec2(104.f, 124.f));
    const bool hovered = ImGui::IsItemHovered();
    if (hovered)
        ctx.sceneViewHovered = false; // clicks here must not pick objects behind the widget

    DrawViewGizmo(*ImGui::GetWindowDrawList(), center, ForwardOf(state.editorCamera), hovered,
                  ImGui::IsItemClicked(ImGuiMouseButton_Left), state.viewportOrthographic, state.viewportViewName,
                  [&ctx, &state](const char* view) {
                      SetViewportOrthographic(ctx, true);
                      EditorCameraState& camera = state.editorCamera;
                      const math::Vec3 pivot = camera.position + ForwardOf(camera) * state.viewportOrthoDistance;
                      ViewAngles(view, camera.yawDeg, camera.pitchDeg);
                      camera.position = pivot - ForwardOf(camera) * state.viewportOrthoDistance;
                      state.viewportViewName = view;
                  },
                  [&ctx, &state] {
                      SetViewportOrthographic(ctx, !state.viewportOrthographic);
                      state.viewportViewName = nullptr;
                  });
}

bool AnimationPreviewOwnsGizmos()
{
    const WindowState& w = W();
    // The controller poses the copy every frame: nothing to move by hand there.
    return w.session && w.previewVisible && w.ownsGizmos && !w.controllerMode;
}

void DrawAnimationPreviewGizmos(EditorFrameContext& ctx)
{
    RunInAnimationPreview(ctx, [&ctx] {
        detail::DrawTransformGizmo(ctx);
        detail::DrawRotationGizmo(ctx);
        detail::DrawScaleGizmo(ctx);
    });
}

void RunInAnimationPreview(EditorFrameContext& ctx, const std::function<void()>& gizmoWork)
{
    WindowState& w = W();
    EditorState& state = ctx.state;
    const EditorCameraState savedCamera = state.editorCamera;
    const EntityID savedSelection = state.selectedEntity;
    const auto savedMultiSelection = state.multiSelection;
    const float savedScale = state.gizmoScaleFactor;
    const float savedX = ctx.renderViewX, savedY = ctx.renderViewY;
    const float savedWidth = ctx.renderViewWidth, savedHeight = ctx.renderViewHeight;
    const bool savedHovered = ctx.sceneViewHovered;

    state.editorCamera.position = state.animationPreviewEye;
    state.editorCamera.yawDeg = w.yawDeg;
    state.editorCamera.pitchDeg = -w.pitchDeg;
    state.editorCamera.fovDeg = state.animationPreviewFovDeg;
    state.editorCamera.nearPlane = state.animationPreviewNear;
    state.editorCamera.farPlane = state.animationPreviewFar;
    // The gizmo size follows the camera distance; keep it constant on screen for any lens.
    state.gizmoScaleFactor = std::tan(state.animationPreviewFovDeg * 0.5f * math::DEG_TO_RAD) /
                             std::tan(30.f * math::DEG_TO_RAD);
    ctx.renderViewX = w.previewMin.x;
    ctx.renderViewY = w.previewMin.y;
    ctx.renderViewWidth = w.previewSize.x;
    ctx.renderViewHeight = w.previewSize.y;
    const bool dragging = state.gizmoDragAxis >= 0 || state.rotGizmoDragAxis >= 0 || state.sclGizmoDragAxis >= 0;
    ctx.sceneViewHovered = w.previewHovered || dragging;

    const TargetNodes nodes = w.copyRoot.IsValid() && ctx.world.IsAlive(w.copyRoot) ? CollectNodes(ctx, w.copyRoot) : TargetNodes{};
    const auto node = nodes.byName.find(w.selectedNode);
    state.selectedEntity = node != nodes.byName.end() ? node->second : NULL_ENTITY;
    state.multiSelection.clear();

    gizmoWork();

    state.editorCamera = savedCamera;
    state.selectedEntity = savedSelection;
    state.multiSelection = savedMultiSelection;
    state.gizmoScaleFactor = savedScale;
    ctx.renderViewX = savedX;
    ctx.renderViewY = savedY;
    ctx.renderViewWidth = savedWidth;
    ctx.renderViewHeight = savedHeight;
    ctx.sceneViewHovered = savedHovered;
}

void DrawAnimationPreviewGrid(EditorFrameContext& ctx, engine::addons::debug_draw::DebugDrawRenderer& debugDraw)
{
    detail::DrawGridLines(ctx, debugDraw, ctx.state.animationPreviewGridCamera, W().groundY);
}

void KeyAnimationSelection(EditorFrameContext& ctx)
{
    KeyCurrentPose(ctx, W().selectedNode, -1);
}

bool DrawAnimationKeyCard(EditorFrameContext& ctx)
{
    WindowState& w = W();
    if (ctx.state.animationWindowOpen && !ctx.playActive && EventSelected())
    {
        value_ui::InlineLabelScope rows;
        card::Card eventCard;
        if (card::Begin(eventCard, "##animation_event_card", "Event", ICON_LC_ACTIVITY, IM_COL32(232, 163, 58, 255)))
            DrawEventDetails();
        card::End(eventCard);
        return true;
    }
    if (!ctx.state.animationWindowOpen || !w.session || ctx.playActive || w.selection.empty() || w.modelMode)
        return false;
    // Same label column as the inspector's component rows.
    value_ui::InlineLabelScope rows;
    card::Card keyCard;
    if (card::Begin(keyCard, "##animation_key_card", "Keyframe", ICON_LC_PLAY, IM_COL32(120, 215, 195, 255)))
        DrawKeyDetails();
    card::End(keyCard);
    return true;
}

void SetAnimationPreviewTime(float seconds)
{
    SetTime(seconds);
}

// Transport, clip settings and keys as their own window, docked beside Assets
// and Console, so the 3D preview keeps the height of the animation window.
// It opens and closes with the animation window.
static void DrawTimelineWindow(EditorFrameContext& ctx)
{
    WindowState& w = W();
    const std::string title = TranslateLabel("animation.timeline", "Timeline##editor");
    if (!w.timelineShown)
    {
        // First use: the bottom dock of Assets and Console; the tab comes to the front.
        const auto assetsLabel = TranslateLabel("panel.assets", "Assets##editor");
        if (const auto* settings = ImGui::FindWindowSettingsByID(ImHashStr(assetsLabel.c_str()));
            settings && settings->DockId && ImGui::DockBuilderGetNode(settings->DockId))
            ImGui::SetNextWindowDockID(settings->DockId, ImGuiCond_FirstUseEver);
        if (w.keepControllerFocus == 0) // opened from the graph: the graph keeps the focus
            ImGui::SetNextWindowFocus();
    }
    w.timelineShown = true;
    ImGui::SetNextWindowSize(ImVec2(1040.f, 260.f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin(title.c_str(), nullptr, w.keepControllerFocus > 0 ? ImGuiWindowFlags_NoFocusOnAppearing : ImGuiWindowFlags_None))
    {
        if (ctx.playActive)
            ImGui::TextDisabled("%s", Translate("animation.play_active"));
        else if (!w.session)
            ImGui::TextWrapped("%s", Translate("animation.empty_hint"));
        else if (w.controllerMode)
        {
            // Controller preview: no keys to show; it is steered from the graph window.
        }
        else
        {
            DrawTimeline(ctx, AnimationColumnWidth(ImGui::GetContentRegionAvail().x));
            if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::IsAnyItemActive())
            {
                if (ImGui::IsKeyPressed(ImGuiKey_Delete))
                    DeleteSelectedKeys();
                if (ImGui::IsKeyPressed(ImGuiKey_K, false) && !w.selectedNode.empty() && !ImGui::GetIO().WantTextInput)
                    KeyCurrentPose(ctx, w.selectedNode, -1);
            }
        }
    }
    ImGui::End();
}

void DrawAnimationWindow(EditorFrameContext& ctx)
{
    WindowState& w = W();
    if (!ctx.state.animationWindowOpen)
    {
        if (w.session && w.dirty)
        {
            // Closing waits for the answer about unsaved changes.
            ctx.state.animationWindowOpen = true;
            RunAfterUnsavedCheck(ctx, [&ctx] {
                EndSession(ctx);
                ctx.state.animationWindowOpen = false;
            });
        }
        else
        {
            if (w.session)
                EndSession(ctx);
            ctx.state.animationPreviewActive = false;
            w.timelineShown = false;
            w.controllerRequested = false;
            return;
        }
    }
    if (!ctx.assetPipeline)
        return;

    if (!SyncControllerPreview(ctx))
        FollowSelection(ctx);
    TickSession(ctx);
    w.previewVisible = false;
    // The gizmos go back to the viewport as soon as it is used.
    if (ctx.sceneViewHovered)
        w.ownsGizmos = false;
    else if (!ctx.sceneViewVisible && w.session)
        w.ownsGizmos = true;

    ImGui::SetNextWindowSize(ImVec2(1040.f, 680.f), ImGuiCond_FirstUseEver);
    const ImGuiWindowFlags focusFlags = w.keepControllerFocus > 0 ? ImGuiWindowFlags_NoFocusOnAppearing : ImGuiWindowFlags_None;
    if (ImGui::Begin(TranslateLabel("animation.title", "Animation##editor").c_str(), &ctx.state.animationWindowOpen, focusFlags))
    {
        if (ctx.playActive)
        {
            ImGui::TextDisabled("%s", Translate("animation.play_active"));
        }
        else
        {
            if (!w.session)
            {
                if (ImGui::BeginChild("##anim_panel_empty", ImVec2(std::min(340.f, ImGui::GetContentRegionAvail().x), 0.f)))
                {
                    const EntityID selected = ctx.state.selectedEntity;
                    const bool canCreate = selected.IsValid() && ctx.world.IsAlive(selected) &&
                                           ctx.world.Has<AnimationPlayerComponent>(selected);
                    if (canCreate)
                    {
                        if (ImGui::Button(TranslateLabel("animation.new_animation", "New Animation").c_str(), ImVec2(-FLT_MIN, 0.f)))
                            CreateAnimation(ctx, AnimatorTarget(ctx, selected));
                    }
                    else
                    {
                        ImGui::TextWrapped("%s", Translate("animation.empty_hint"));
                    }
                }
                ImGui::EndChild();
            }
            else
            {
                // Save on top, the preview below over the whole window. Transport,
                // clip settings and keys live in the Timeline window at the bottom;
                // the selected keyframe's values are shown in the inspector.
                if (w.controllerMode)
                {
                    // Controller preview: nothing to save here, the graph window owns the file.
                    ImGui::TextColored(theme::kAccentHover, "%s", Translate("controller.preview_running"));
                    ImGui::SameLine();
                    value_ui::TextDisabled("   %s", w.controllerPath.c_str());
                    ImGui::Separator();
                }
                else
                    DrawAnimationActions(ctx);
                if (ImGui::BeginChild("##anim_preview_pane", ImVec2(0.f, 0.f)))
                    DrawPreviewPane(ctx, CollectNodes(ctx, w.copyRoot));
                ImGui::EndChild();
                if (!w.controllerMode && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::IsAnyItemActive())
                {
                    if (ImGui::IsKeyPressed(ImGuiKey_Delete))
                        DeleteSelectedKeys();
                    if (ImGui::IsKeyPressed(ImGuiKey_K, false) && !w.selectedNode.empty() && !ImGui::GetIO().WantTextInput)
                        KeyCurrentPose(ctx, w.selectedNode, -1);
                }
            }
        }

        if (w.askUnsaved)
        {
            w.askUnsaved = false;
            ImGui::OpenPopup("##anim_unsaved");
        }
        if (ImGui::BeginPopupModal("##anim_unsaved", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar))
        {
            ImGui::TextUnformatted(Translate("animation.unsaved"));
            const auto finish = [&](bool run) {
                std::function<void()> action = std::move(w.pendingAction);
                w.pendingAction = nullptr;
                ImGui::CloseCurrentPopup();
                if (run && action)
                    action();
            };
            if (ImGui::Button(TranslateLabel("common.save", "Save").c_str()))
            {
                if (SaveAnimation(ctx))
                    finish(true);
            }
            ImGui::SameLine();
            if (ImGui::Button(TranslateLabel("animation.discard", "Discard").c_str()))
            {
                w.dirty = false;
                finish(true);
            }
            ImGui::SameLine();
            if (ImGui::Button(TranslateLabel("common.cancel", "Cancel").c_str()))
                finish(false);
            ImGui::EndPopup();
        }
    }
    ImGui::End();
    DrawTimelineWindow(ctx);
    if (w.keepControllerFocus > 0)
    {
        --w.keepControllerFocus;
        ImGui::SetWindowFocus(TranslateLabel("controller.title", "Controller##editor").c_str());
    }
    // A hidden tab renders nothing and leaves the gizmos to the viewport.
    if (!w.previewVisible)
        ctx.state.animationPreviewActive = false;
}

} // namespace engine::renderer::addons::editor

#else

namespace engine::renderer::addons::editor {
void DrawAnimationWindow(EditorFrameContext&) {}
void OpenAnimationEditor(EditorFrameContext&, EntityID) {}
bool AnimationFileExists(EditorFrameContext&, const std::string&) { return false; }
void DrawViewportViewGizmo(EditorFrameContext&, const ImVec2&, float) {}
bool AnimationPreviewOwnsGizmos() { return false; }
void DrawAnimationPreviewGizmos(EditorFrameContext&) {}
void OpenAnimationFile(EditorFrameContext&, const std::string&) {}
void OpenModelClip(EditorFrameContext&, const std::string&) {}
std::vector<std::string> AnimationClipNames(EditorFrameContext&, const std::string&) { return {}; }
bool SaveOpenAnimation(EditorFrameContext&) { return false; }
bool SaveOpenAnimationIfChanged(EditorFrameContext&) { return false; }
void RunInAnimationPreview(EditorFrameContext&, const std::function<void()>& work) { work(); }
void DrawAnimationPreviewGrid(EditorFrameContext&, engine::addons::debug_draw::DebugDrawRenderer&) {}
EntityID GetAnimationPreviewRoot() { return NULL_ENTITY; }
AnimationPlayerComponent* ControllerLivePlayer(EditorFrameContext&, const std::string&) { return nullptr; }
EntityID ControllerPreviewCandidate(EditorFrameContext&, const std::string&) { return NULL_ENTITY; }
bool ControllerPreviewRunning(const std::string&) { return false; }
void RestartControllerPreview(EditorFrameContext&) {}
bool ControllerPreviewPaused() { return false; }
void SetControllerPreviewPaused(bool) {}
bool ControllerPreviewLastEvent(std::string&, float&) { return false; }
int AddAnimationEvent(float) { return -1; }
const std::vector<assets::AnimationEvent>* AnimationWindowEvents() { return nullptr; }
void SelectAnimationEvent(int) {}
int SelectedAnimationEvent() { return -1; }
bool DeleteSelectedAnimationEvent() { return false; }
bool AnimationEventScreenPosition(int, float&, float&) { return false; }
void StartControllerPreview(EditorFrameContext&) {}
void StopControllerPreview(EditorFrameContext&) {}
bool CreateAnimationClip(EditorFrameContext&, EntityID) { return false; }
void SetAnimationPreviewTime(float) {}
bool DrawAnimationKeyCard(EditorFrameContext&) { return false; }
void KeyAnimationSelection(EditorFrameContext&) {}
} // namespace engine::renderer::addons::editor

#endif
