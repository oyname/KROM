// =============================================================================
// KROM Engine - addons/animation/AnimationSystem.cpp
// =============================================================================
#include "AnimationSystem.hpp"
#include "animation/AnimationEvaluator.hpp"
#include "animation/AnimationSampler.hpp"
#include "animation/SkeletonPoseEvaluator.hpp"
#include "assets/AssetRegistry.hpp"
#include "ecs/Components.hpp"
#include "ecs/World.hpp"

#include <algorithm>
#include <cmath>

namespace engine::addons::animation {

// =============================================================================
// RebuildBindings — einmaliger NameComponent-Scan pro Clip-Wechsel
// =============================================================================

static bool IsSelfOrDescendant(const ecs::World& world, EntityID entity, EntityID ancestor)
{
    uint32_t depth = 0u;
    while (entity.IsValid() && depth++ < 1024u)
    {
        if (entity == ancestor)
            return true;
        const auto* parent = world.Get<ParentComponent>(entity);
        entity = parent ? parent->parent : NULL_ENTITY;
    }
    return false;
}

void AnimationSystem::RebuildBindings(
    const ecs::World&            world,
    EntityID                     owner,
    const assets::AnimationClip& clip,
    AnimationPlayerComponent&    player)
{
    // Größe des Binding-Vektors = max(targetIndex) + 1
    int32_t maxIndex = -1;
    for (const auto& ch : clip.channels)
        maxIndex = std::max(maxIndex, ch.targetIndex);

    if (maxIndex < 0)
    {
        player.nodeBindings.clear();
        player.bindingsDirty = false;
        return;
    }

    player.nodeBindings.assign(static_cast<size_t>(maxIndex + 1), NULL_ENTITY);

    // Für jeden Channel mit gültigem targetIndex: Entity per NameComponent suchen.
    // Channels mit demselben targetIndex teilen sich denselben Slot → einmaliger Scan.
    for (const auto& ch : clip.channels)
    {
        if (ch.targetIndex < 0 || ch.targetName.empty()) continue;
        EntityID& slot = player.nodeBindings[static_cast<size_t>(ch.targetIndex)];
        if (slot.IsValid()) continue;  // bereits gefunden

        world.View<NameComponent>(
            [&](EntityID id, const NameComponent& name)
            {
                // Only the player's own hierarchy: never move another object of the same name.
                if (name.name == ch.targetName && IsSelfOrDescendant(world, id, owner))
                    slot = id;
            });

        // A clip for one object is reusable: on another object it animates that object.
        const bool singleTarget = std::all_of(clip.channels.begin(), clip.channels.end(),
            [&](const assets::AnimationChannel& other) { return other.targetName == ch.targetName; });
        if (!slot.IsValid() && singleTarget)
            slot = owner;
    }

    player.bindingsDirty = false;
}

// =============================================================================
// Node-Animation — schreibt TransformComponent der Ziel-Entities
// =============================================================================

static void UpdateNodeAnimation(
    ecs::World&                  world,
    AnimationPlayerComponent&    player,
    const assets::AnimationClip& clip,
    float                        sampleTime)
{
    for (const auto& ch : clip.channels)
    {
        if (ch.targetIndex < 0) continue;
        const size_t idx = static_cast<size_t>(ch.targetIndex);
        if (idx >= player.nodeBindings.size()) continue;

        const EntityID target = player.nodeBindings[idx];
        if (!target.IsValid()) continue;

        auto* tf = world.Get<TransformComponent>(target);
        if (!tf) continue;

        // EvaluateChannel schreibt nur die eigene Eigenschaft; die anderen
        // bleiben unverändert (korrekte Akkumulation mehrerer Channels).
        math::Vec3 t = tf->localPosition;
        math::Quat r = tf->localRotation;
        math::Vec3 s = tf->localScale;

        if (engine::animation::EvaluateChannel(ch, sampleTime, t, r, s))
        {
            tf->localPosition = t;
            tf->localRotation = r;
            tf->localScale    = s;
            tf->dirty         = true;
            ++tf->localVersion;
        }
    }
}

// =============================================================================
// Skelett-Animation — baut skin.bonePalette via AnimationSampler + Evaluator
// =============================================================================

static void UpdateSkeletalAnimation(
    SkinComponent&               skin,
    const assets::AnimationClip& clip,
    const assets::SkeletonAsset& skeleton,
    float                        sampleTime)
{
    const std::vector<engine::animation::BonePose> localPoses =
        engine::animation::AnimationSampler::SampleForSkeleton(
            clip, sampleTime, skeleton.bones.size());

    skin.bonePalette =
        engine::animation::SkeletonPoseEvaluator::Evaluate(skeleton, localPoses);
}

// =============================================================================
// AnimationSystem::Update
// =============================================================================

// =============================================================================
// Controller mode
// =============================================================================

namespace {

using engine::animation::BonePose;

BonePose MixPose(const BonePose& from, const BonePose& to, float t) noexcept
{
    BonePose r;
    r.position = math::Vec3::Lerp(from.position, to.position, t);
    r.rotation = math::Quat::Slerp(from.rotation, to.rotation, t);
    r.scale    = math::Vec3::Lerp(from.scale, to.scale, t);
    return r;
}

// Writes the channels of a clip into the pose slots; slots without a channel keep
// what is in `pose` (the rest pose). `slots` maps channel -> slot (node mode);
// empty means the channel's targetIndex is the slot (bone index).
void SampleClipInto(const assets::AnimationClip& clip, float time, const std::vector<int32_t>* slots,
                    std::vector<BonePose>& pose) noexcept
{
    for (size_t c = 0; c < clip.channels.size(); ++c)
    {
        const assets::AnimationChannel& ch = clip.channels[c];
        const int32_t slot = slots ? (c < slots->size() ? (*slots)[c] : -1) : ch.targetIndex;
        if (slot < 0 || static_cast<size_t>(slot) >= pose.size())
            continue;
        BonePose& p = pose[static_cast<size_t>(slot)];
        (void)engine::animation::EvaluateChannel(ch, time, p.position, p.rotation, p.scale);
    }
}

// The skin driven by a player: its own, else the first one below it.
EntityID FindDrivenSkin(const ecs::World& world, EntityID owner, const assets::AssetRegistry& registry)
{
    if (const auto* skin = world.Get<SkinComponent>(owner); skin && registry.skeletons.Get(skin->skeleton))
        return owner;
    EntityID found = NULL_ENTITY;
    world.View<SkinComponent>([&](EntityID id, const SkinComponent& skin) {
        if (!found.IsValid() && registry.skeletons.Get(skin.skeleton) && IsSelfOrDescendant(world, id, owner))
            found = id;
    });
    return found;
}

// A player without controller below one with controller would fight over the
// same skin (imported rigs carry a player on the root and on the mesh).
bool IsBelowController(const ecs::World& world, EntityID entity)
{
    const auto* parent = world.Get<ParentComponent>(entity);
    EntityID current = parent ? parent->parent : NULL_ENTITY;
    for (uint32_t depth = 0u; current.IsValid() && depth < 1024u; ++depth)
    {
        if (const auto* owner = world.Get<AnimationPlayerComponent>(current); owner && owner->HasController())
            return true;
        const auto* next = world.Get<ParentComponent>(current);
        current = next ? next->parent : NULL_ENTITY;
    }
    return false;
}

} // namespace

void AnimationSystem::SetAssetSources(const std::filesystem::path& assetRoot,
                                      std::function<bool(const std::string&)> importClip)
{
    m_controllers.SetAssetRoot(assetRoot);
    m_importClip = std::move(importClip);
    m_failedImports.clear();
    m_controllers.SetClipResolver([this](const std::string& path) { return ResolveClip(path); });
}

AnimationClipInfo AnimationSystem::ResolveClip(const std::string& clipAssetPath)
{
    AnimationClipInfo info;
    AnimationHandle handle = m_registry.FindAnimationByPath(clipAssetPath);
    if (!handle.IsValid() && m_importClip && !m_failedImports.contains(clipAssetPath))
    {
        m_importClip(clipAssetPath);
        handle = m_registry.FindAnimationByPath(clipAssetPath);
        if (!handle.IsValid())
            m_failedImports[clipAssetPath] = true;
    }
    if (const auto* clip = m_registry.animations.Get(handle))
    {
        info.handle = handle;
        info.duration = clip->duration;
        info.looping = clip->looping;
        info.found = true;
    }
    return info;
}

bool AnimationSystem::EnsureControllerBound(ecs::World& world, EntityID entity, AnimationPlayerComponent& player)
{
    AnimationControllerRuntime& rt = player.controllerState;
    if (!player.HasController())
        return false;
    const auto controller = m_controllers.Get(player.controllerAssetPath);
    if (!controller)
        return false;

    // (Re)bind: sizes every buffer once; the frame below allocates nothing.
    if (!rt.bound || rt.controller != controller || rt.boundPath != player.controllerAssetPath)
    {
        const bool sameAsset = rt.bound && rt.boundPath == player.controllerAssetPath;
        const std::vector<float> keptParameters = rt.instance.parameters;
        rt.controller = controller;
        rt.boundPath = player.controllerAssetPath;
        ResetAnimationController(*controller, rt.instance);
        // An edited controller keeps the values scripts set, as far as they fit.
        if (sameAsset)
            for (size_t i = 0; i < std::min(keptParameters.size(), rt.instance.parameters.size()); ++i)
                rt.instance.parameters[i] = keptParameters[i];

        rt.skinEntity = FindDrivenSkin(world, entity, m_registry);
        rt.nodes.clear();
        rt.nodeNames.clear();
        rt.stateSlots.clear();
        size_t slotCount = 0;
        if (rt.skinEntity.IsValid())
        {
            const auto* skeleton = m_registry.skeletons.Get(world.Get<SkinComponent>(rt.skinEntity)->skeleton);
            slotCount = skeleton ? skeleton->bones.size() : 0u;
            rt.rest.assign(slotCount, BonePose{});
        }
        else
        {
            // Node mode: one slot per animated object name in any state's clip.
            rt.stateSlots.resize(controller->states.size());
            for (size_t s = 0; s < controller->states.size(); ++s)
            {
                const auto* clip = m_registry.animations.Get(controller->states[s].clip);
                if (!clip)
                    continue;
                const bool singleTarget = !clip->channels.empty() &&
                    std::all_of(clip->channels.begin(), clip->channels.end(), [&](const assets::AnimationChannel& ch) {
                        return ch.targetName == clip->channels.front().targetName;
                    });
                for (const assets::AnimationChannel& ch : clip->channels)
                {
                    // A clip made for one object animates whatever object plays it.
                    const std::string name = singleTarget ? std::string{} : ch.targetName;
                    auto it = std::find(rt.nodeNames.begin(), rt.nodeNames.end(), name);
                    if (it == rt.nodeNames.end())
                    {
                        rt.nodeNames.push_back(name);
                        it = rt.nodeNames.end() - 1;
                    }
                    rt.stateSlots[s].push_back(static_cast<int32_t>(it - rt.nodeNames.begin()));
                }
            }
            rt.nodes.assign(rt.nodeNames.size(), NULL_ENTITY);
            for (size_t i = 0; i < rt.nodeNames.size(); ++i)
            {
                if (rt.nodeNames[i].empty())
                {
                    rt.nodes[i] = entity;
                    continue;
                }
                world.View<NameComponent>([&](EntityID id, const NameComponent& name) {
                    if (!rt.nodes[i].IsValid() && name.name == rt.nodeNames[i] && IsSelfOrDescendant(world, id, entity))
                        rt.nodes[i] = id;
                });
            }
            slotCount = rt.nodes.size();
            rt.rest.assign(slotCount, BonePose{});
            for (size_t i = 0; i < slotCount; ++i)
                if (const auto* tf = rt.nodes[i].IsValid() ? world.Get<TransformComponent>(rt.nodes[i]) : nullptr)
                    rt.rest[i] = {tf->localPosition, tf->localRotation, tf->localScale};
        }
        rt.current.assign(slotCount, BonePose{});
        rt.previous.assign(slotCount, BonePose{});
        rt.frozen.assign(slotCount, BonePose{});
        rt.output = rt.rest;
        rt.bound = true;
    }
    return true;
}

void AnimationSystem::UpdateController(ecs::World& world, EntityID entity, AnimationPlayerComponent& player, float dt)
{
    if (!EnsureControllerBound(world, entity, player))
        return;
    AnimationControllerRuntime& rt = player.controllerState;
    const CompiledAnimationController& c = *rt.controller;
    AnimationControllerInstance& in = rt.instance;
    const bool wasBlending = in.Blending();
    const bool fired = StepAnimationController(c, in, dt * player.speed);
    // An interrupted cross-fade fades out from the pose shown last frame.
    if (fired && in.previousFrozen && wasBlending)
        rt.frozen = rt.output;
    // The state machine runs even when there is nothing to pose (scripts read it).
    if (rt.output.empty())
        return;

    const auto sampleState = [&](int32_t state, float time, std::vector<BonePose>& pose) {
        pose = rt.rest; // same size: copies, no allocation
        if (state < 0)
            return;
        const auto* clip = m_registry.animations.Get(c.states[static_cast<size_t>(state)].clip);
        if (!clip)
            return;
        const std::vector<int32_t>* slots = rt.skinEntity.IsValid() ? nullptr : &rt.stateSlots[static_cast<size_t>(state)];
        SampleClipInto(*clip, StateSampleTime(c, state, time), slots, pose);
    };
    sampleState(in.current, in.currentTime, rt.current);
    if (in.Blending())
    {
        const std::vector<BonePose>* from = &rt.frozen;
        if (!in.previousFrozen)
        {
            sampleState(in.previous, in.previousTime, rt.previous);
            from = &rt.previous;
        }
        const float weight = in.CurrentWeight();
        for (size_t i = 0; i < rt.output.size(); ++i)
            rt.output[i] = MixPose((*from)[i], rt.current[i], weight);
    }
    else
    {
        rt.output = rt.current;
    }

    if (rt.skinEntity.IsValid())
    {
        auto* skin = world.Get<SkinComponent>(rt.skinEntity);
        const auto* skeleton = skin ? m_registry.skeletons.Get(skin->skeleton) : nullptr;
        if (skeleton)
            engine::animation::SkeletonPoseEvaluator::EvaluateInto(*skeleton, rt.output, rt.worldScratch, skin->bonePalette);
        return;
    }
    for (size_t i = 0; i < rt.nodes.size(); ++i)
        if (auto* tf = rt.nodes[i].IsValid() ? world.Get<TransformComponent>(rt.nodes[i]) : nullptr)
        {
            tf->localPosition = rt.output[i].position;
            tf->localRotation = rt.output[i].rotation;
            tf->localScale    = rt.output[i].scale;
            tf->dirty         = true;
            ++tf->localVersion;
        }
}

void AnimationSystem::Update(ecs::World& world, float dt)
{
    bool anyController = false;
    world.View<AnimationPlayerComponent>([&](EntityID entity, AnimationPlayerComponent& player) {
        if (!player.HasController())
            return;
        anyController = true;
        if (player.playing)
            UpdateController(world, entity, player, dt);
    });

    world.View<AnimationPlayerComponent>(
        [&](EntityID entity, AnimationPlayerComponent& player)
        {
            if (player.HasController() || !player.playing || !player.clip.IsValid())
                return;
            if (anyController && IsBelowController(world, entity))
                return;

            const assets::AnimationClip* clip =
                m_registry.animations.Get(player.clip);
            if (!clip)
                return;

            // --- Zeit vorrücken ---
            player.currentTime += dt * player.speed;

            if (player.looping && clip->duration > 0.f)
            {
                player.currentTime = std::fmod(player.currentTime, clip->duration);
                if (player.currentTime < 0.f)
                    player.currentTime += clip->duration;
            }
            else if (player.currentTime > clip->duration)
            {
                player.currentTime = clip->duration;
                player.playing     = false;  // am Ende stoppen (non-looping vorwärts)
            }
            else if (player.currentTime < 0.f)
            {
                player.currentTime = 0.f;
                player.playing     = false;  // am Anfang stoppen (non-looping rückwärts)
            }

            const float sampleTime = player.currentTime;

            // --- Binding-Cache aufbauen wenn nötig ---
            if (player.bindingsDirty)
                RebuildBindings(world, entity, *clip, player);

            // --- Skelett-Pfad: AnimationPlayerComponent + SkinComponent ---
            if (auto* skin = world.Get<SkinComponent>(entity))
            {
                const assets::SkeletonAsset* skeleton =
                    m_registry.skeletons.Get(skin->skeleton);
                if (skeleton)
                {
                    UpdateSkeletalAnimation(*skin, *clip, *skeleton, sampleTime);
                    return;
                }
            }

            // --- Node-Pfad: reine TransformComponent-Schreibweise ---
            UpdateNodeAnimation(world, player, *clip, sampleTime);
        });
}

} // namespace engine::addons::animation
