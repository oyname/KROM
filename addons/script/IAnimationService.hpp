#pragma once
// =============================================================================
// KROM Engine - addons/script/IAnimationService.hpp
// Animation controller access for scripts: parameters, current state, direct
// cross-fades. Works on the animator (AnimationPlayerComponent) of an entity
// that has a controller (*.animctrl). Scripts use the helpers in
// IScriptContext (SetAnimationFloat, SetAnimationTrigger, ...).
// =============================================================================
#include "core/Types.hpp"

#include <cstdint>
#include <string_view>

namespace engine::script {

enum class AnimationParameterKind : uint8_t { Float, Int, Bool, Trigger };

class IAnimationService
{
public:
    virtual ~IAnimationService() = default;

    // Index of a controller parameter, -1 if the entity has no animator with a
    // controller or the controller has no parameter of that name (logged once).
    virtual int32_t FindParameter(EntityID entity, std::string_view name) = 0;
    virtual bool    GetParameterKind(EntityID entity, int32_t index, AnimationParameterKind& out) = 0;
    // Bool and trigger values are 0 or 1.
    virtual bool    SetParameter(EntityID entity, int32_t index, float value) = 0;
    virtual bool    GetParameter(EntityID entity, int32_t index, float& out) = 0;

    // Name of the active state; empty without controller.
    virtual std::string_view GetCurrentState(EntityID entity) = 0;
    // Time in the active state, 1 = one pass through its clip (2.5 = middle of the third).
    virtual float GetNormalizedTime(EntityID entity) = 0;
    virtual bool  IsInTransition(EntityID entity) = 0;
    // Cross-fades into the named state now, without a transition. False if unknown.
    virtual bool  CrossFade(EntityID entity, std::string_view state, float blendSeconds) = 0;
};

} // namespace engine::script
