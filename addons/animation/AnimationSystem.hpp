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
#include "addons/animation/AnimationController.hpp"
#include "assets/AssetRegistry.hpp"
#include "ecs/World.hpp"

#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>

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

    // Where controllers (*.animctrl) are read from, and how a clip that is not
    // registered yet gets imported (e.g. mesh_renderer::ImportModelAnimationAssets).
    void SetAssetSources(const std::filesystem::path& assetRoot,
                         std::function<bool(const std::string& clipAssetPath)> importClip);
    [[nodiscard]] AnimationControllerLibrary& Controllers() noexcept { return m_controllers; }
    // Loads and binds the player's controller now (scripts before the first
    // Update). False without controller or if it cannot be loaded.
    bool EnsureControllerBound(ecs::World& world, EntityID entity, AnimationPlayerComponent& player);

private:
    const assets::AssetRegistry& m_registry;
    AnimationControllerLibrary   m_controllers;
    std::function<bool(const std::string&)> m_importClip;
    std::unordered_map<std::string, bool> m_failedImports;

    AnimationClipInfo ResolveClip(const std::string& clipAssetPath);
    void UpdateController(ecs::World& world, EntityID entity, AnimationPlayerComponent& player, float dt);

    // Baut nodeBindings-Cache für alle Channels eines Clips.
    // Scannt die World nach NameComponent == channel.targetName.
    static void RebuildBindings(
        const ecs::World&               world,
        EntityID                        owner,
        const assets::AnimationClip&    clip,
        AnimationPlayerComponent&       player);
};

} // namespace engine::addons::animation
