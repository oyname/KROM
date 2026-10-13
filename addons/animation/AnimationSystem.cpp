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

void AnimationSystem::RebuildBindings(
    const ecs::World&            world,
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
                if (name.name == ch.targetName)
                    slot = id;
            });
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

void AnimationSystem::Update(ecs::World& world, float dt)
{
    world.View<AnimationPlayerComponent>(
        [&](EntityID entity, AnimationPlayerComponent& player)
        {
            if (!player.playing || !player.clip.IsValid())
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
                RebuildBindings(world, *clip, player);

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
