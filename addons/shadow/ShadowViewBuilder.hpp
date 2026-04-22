#pragma once
#include "addons/shadow/ShadowTypes.hpp"
#include "core/Math.hpp"
#include "ecs/Components.hpp"

namespace engine::addons::shadow {

class ShadowViewBuilder
{
public:
    static void BuildViews(const LightComponent& light,
                           const WorldTransformComponent& worldTransform,
                           const collision::AABB& casterBoundsWorld,
                           ShadowRequest& outRequest);

    static void BuildDirectional(const LightComponent& light,
                                 const WorldTransformComponent& worldTransform,
                                 const collision::AABB& casterBoundsWorld,
                                 ShadowRequest& outRequest);

    static void BuildSpot(const LightComponent& light,
                          const WorldTransformComponent& worldTransform,
                          ShadowRequest& outRequest);

    static void BuildPoint(const LightComponent& light,
                           const WorldTransformComponent& worldTransform,
                           ShadowRequest& outRequest);
};

} // namespace engine::addons::shadow
