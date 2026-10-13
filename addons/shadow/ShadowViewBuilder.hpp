#pragma once
#include "addons/shadow/ShadowTypes.hpp"
#include "core/Math.hpp"
#include "ecs/Components.hpp"

#include <array>

namespace engine::addons::shadow {

class ShadowViewBuilder
{
public:
    static void BuildViews(const LightComponent& light,
                           const WorldTransformComponent& worldTransform,
                           const collision::AABB& casterBoundsWorld,
                           ShadowRequest& outRequest);

    // fitCornersWorld (optional): 8 Welt-Ecken, an die die Licht-Ortho in XY
    // direkt eingepasst wird (tightere CSM-Cascade als AABB-Einpassung).
    // nullptr → Einpassung an die Ecken von receiverBoundsWorld (Standardverhalten).
    static void BuildDirectional(const LightComponent& light,
                                 const WorldTransformComponent& worldTransform,
                                 const collision::AABB& casterBoundsWorld,
                                 const collision::AABB& receiverBoundsWorld,
                                 ShadowRequest& outRequest,
                                 const std::array<math::Vec3, 8>* fitCornersWorld = nullptr);

    static void BuildSpot(const LightComponent& light,
                          const WorldTransformComponent& worldTransform,
                          ShadowRequest& outRequest);

    static void BuildPoint(const LightComponent& light,
                           const WorldTransformComponent& worldTransform,
                           ShadowRequest& outRequest);
};

} // namespace engine::addons::shadow
