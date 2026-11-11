#pragma once
// =============================================================================
// KROM Engine - addons/animation/AnimationComponents.hpp
// Runtime animation components owned by the animation addon.
//
// AnimationPlayerComponent ("Animator" in the editor) has two modes:
//   controller   controllerAssetPath set: the *.animctrl state machine picks
//                the clips and cross-fades between them; scripts steer it
//                through parameters
//   single clip  clip / looping / speed, as before
// `playing` ("play on start") and `speed` apply to both.
// =============================================================================
#include "addons/animation/AnimationController.hpp"
#include "animation/AnimationSampler.hpp"
#include "core/Math.hpp"
#include "core/Types.hpp"
#include "ecs/ComponentMeta.hpp"

#include <memory>
#include <string>
#include <vector>

namespace engine {

struct SkinComponent
{
    SkeletonHandle skeleton;
    // "<model>#skeleton/<n>", relative to Assets: saved in scenes, resolved on load.
    std::string skeletonAssetPath;
    std::vector<math::Mat4> bonePalette;
    BufferHandle gpuBuffer;
};

// Runtime of the controller mode, rebuilt by the AnimationSystem whenever the
// controller (pointer) changes. Pose buffers are sized once when bound.
struct AnimationControllerRuntime
{
    std::shared_ptr<const addons::animation::CompiledAnimationController> controller;
    std::string                              boundPath;
    addons::animation::AnimationControllerInstance instance;

    // Pose slots: the bones of the driven skeleton, or for objects without one
    // the animated nodes (an object alone is a skeleton with one bone).
    EntityID                                 skinEntity = NULL_ENTITY;
    std::vector<EntityID>                    nodes;          // node mode: entity per slot
    std::vector<std::string>                 nodeNames;
    std::vector<std::vector<int32_t>>        stateSlots;     // node mode: per state, slot per channel
    std::vector<animation::BonePose>         rest;
    std::vector<animation::BonePose>         current;
    std::vector<animation::BonePose>         previous;
    std::vector<animation::BonePose>         frozen;
    std::vector<animation::BonePose>         output;
    std::vector<math::Mat4>                  worldScratch;
    bool                                     bound = false;

    void Reset() noexcept
    {
        controller.reset();
        boundPath.clear();
        bound = false;
    }
};

struct AnimationPlayerComponent
{
    // --- Controller mode -----------------------------------------------------
    // "*.animctrl", relative to Assets. Takes precedence over the single clip.
    std::string controllerAssetPath;
    AnimationControllerRuntime controllerState;

    // --- Single clip mode ----------------------------------------------------
    AnimationHandle clip;
    // "<model>#clip/<id>", "<file>.anim#<clip>" or "<model>#animation/<n>",
    // relative to Assets: saved in scenes, resolved on load.
    std::string clipAssetPath;
    float currentTime = 0.f;
    float speed = 1.f;         // also scales the controller
    bool playing = false;      // saved as "play on start"
    bool looping = false;

    std::vector<EntityID> nodeBindings;
    bool bindingsDirty = true;

    [[nodiscard]] bool HasController() const noexcept { return !controllerAssetPath.empty(); }

    void Play() noexcept { playing = true; }
    void Pause() noexcept { playing = false; }
    void Stop() noexcept { playing = false; currentTime = 0.f; bindingsDirty = true; }
    void Rewind() noexcept { currentTime = 0.f; }

    void SetClip(AnimationHandle nextClip) noexcept
    {
        if (clip != nextClip)
        {
            clip = nextClip;
            currentTime = 0.f;
            bindingsDirty = true;
        }
    }

    // Controller parameters by index (see CompiledAnimationController::FindParameter);
    // false before the controller is bound or for an index out of range.
    bool SetParameter(int32_t index, float value) noexcept
    {
        auto& values = controllerState.instance.parameters;
        if (index < 0 || static_cast<size_t>(index) >= values.size())
            return false;
        values[static_cast<size_t>(index)] = value;
        return true;
    }
    [[nodiscard]] float GetParameter(int32_t index) const noexcept
    {
        const auto& values = controllerState.instance.parameters;
        return index >= 0 && static_cast<size_t>(index) < values.size() ? values[static_cast<size_t>(index)] : 0.f;
    }
};

inline void RegisterAnimationComponents(ecs::ComponentMetaRegistry& registry)
{
    using namespace ecs;
    RegisterComponent<SkinComponent>(registry, "SkinComponent");
    RegisterComponent<AnimationPlayerComponent>(registry, "AnimationPlayerComponent");
}

} // namespace engine
