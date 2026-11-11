#pragma once
// =============================================================================
// KROM Engine - addons/animation/AnimationScriptService.hpp
// IAnimationService on top of the AnimationSystem: binds a controller on first
// use, so scripts can set parameters already in OnStart.
// =============================================================================
#include "addons/animation/AnimationSystem.hpp"
#include "addons/script/IAnimationService.hpp"

namespace engine::addons::animation {

class AnimationScriptService final : public script::IAnimationService
{
public:
    AnimationScriptService(AnimationSystem& system, ecs::World& world) noexcept
        : m_system(system), m_world(world)
    {}

    int32_t FindParameter(EntityID entity, std::string_view name) override;
    bool    GetParameterKind(EntityID entity, int32_t index, script::AnimationParameterKind& out) override;
    bool    SetParameter(EntityID entity, int32_t index, float value) override;
    bool    GetParameter(EntityID entity, int32_t index, float& out) override;
    std::string_view GetCurrentState(EntityID entity) override;
    float   GetNormalizedTime(EntityID entity) override;
    bool    IsInTransition(EntityID entity) override;
    bool    CrossFade(EntityID entity, std::string_view state, float blendSeconds) override;

private:
    // The animator with a bound controller, or nullptr (reported once per entity).
    AnimationPlayerComponent* Bound(EntityID entity);

    AnimationSystem& m_system;
    ecs::World&      m_world;
};

} // namespace engine::addons::animation
