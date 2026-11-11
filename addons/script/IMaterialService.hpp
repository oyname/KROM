#pragma once
// =============================================================================
// KROM Engine - addons/script/IMaterialService.hpp
// Per-object material values for scripts.
// =============================================================================
#include "core/Math.hpp"
#include "core/Types.hpp"
#include <string_view>

namespace engine::script {

class IMaterialService
{
public:
    virtual ~IMaterialService() = default;

    // Sets the named Parameter node for this entity only. False if the entity has
    // no material with a Parameter node of that name.
    virtual bool SetParameter(EntityID entity, std::string_view name, const math::Vec3& value) = 0;

    virtual bool SetUV(EntityID, const math::Vec2&, const math::Vec2&) { return false; }
};

} // namespace engine::script
