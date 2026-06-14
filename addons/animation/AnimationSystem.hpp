#pragma once
// =============================================================================
// KROM Engine - addons/animation/AnimationSystem.hpp
// Animations-ECS-System — einmal pro Frame aufrufen, vor RenderFrame.
//
// Zwei unabhängige Pfade, abhängig von den Komponenten der Entity:
//
// Node-Animation (AnimationPlayerComponent allein):
//   • Verwaltet Wiedergabezeit (play/pause/looping/speed)
//   • Baut Binding-Cache (nodeBindings) per NameComponent-Scan — einmalig
//   • Sampelt jeden Channel mit AnimationEvaluator und schreibt das Ergebnis
//     direkt in TransformComponent der Ziel-Entity → setzt dirty = true
//   → TransformSystem propagiert World-Transforms im nächsten Update
//
// Skelett-Animation (AnimationPlayerComponent + SkinComponent):
//   • Sampelt Channels per Bone-Index mit AnimationSampler::SampleForSkeleton
//   • Evaluiert Skelett-Hierarchie mit SkeletonPoseEvaluator
//   • Schreibt skin.bonePalette — SkinningSystem lädt diese per Frame auf GPU
// =============================================================================
#include "addons/animation/AnimationComponents.hpp"
#include "assets/AssetRegistry.hpp"
#include "ecs/World.hpp"

namespace engine::addons::animation {

class AnimationSystem
{
public:
    explicit AnimationSystem(const assets::AssetRegistry& registry) noexcept
        : m_registry(registry)
    {}

    // dt in Sekunden. Muss außerhalb des ECS-ReadPhase aufgerufen werden
    // (schreibt TransformComponent und AnimationPlayerComponent).
    void Update(ecs::World& world, float dt);

private:
    const assets::AssetRegistry& m_registry;

    // Baut nodeBindings-Cache für alle Channels eines Clips.
    // Scannt die World nach NameComponent == channel.targetName.
    static void RebuildBindings(
        const ecs::World&               world,
        const assets::AnimationClip&    clip,
        AnimationPlayerComponent&       player);
};

} // namespace engine::addons::animation
