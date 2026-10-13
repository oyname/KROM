#pragma once
// =============================================================================
// KROM Engine - addons/animation/AnimationComponents.hpp
// Runtime animation components owned by the animation addon.
// =============================================================================
#include "core/Math.hpp"
#include "core/Types.hpp"
#include "ecs/ComponentMeta.hpp"

#include <vector>

namespace engine {

struct SkinComponent
{
    SkeletonHandle skeleton;
    std::vector<math::Mat4> bonePalette;
    BufferHandle gpuBuffer;
};

struct AnimationPlayerComponent
{
    AnimationHandle clip;
    float currentTime = 0.f;
    float speed = 1.f;
    bool playing = false;
    bool looping = false;

    std::vector<EntityID> nodeBindings;
    bool bindingsDirty = true;

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
};

inline void RegisterAnimationComponents(ecs::ComponentMetaRegistry& registry)
{
    using namespace ecs;
    RegisterComponent<SkinComponent>(registry, "SkinComponent");
    RegisterComponent<AnimationPlayerComponent>(registry, "AnimationPlayerComponent");
}

} // namespace engine
