#pragma once
// =============================================================================
// KROM Engine - addons/postprocess/PostProcessBlend.hpp
//
// Blends post-process volumes over the base values of a frame:
//   base   = scene cards "Exposure"/"Color grading" + project settings
//   result = base, then each active volume in ascending priority order
//            (equal priority: larger box first, so the smaller box wins; then
//            entity id) lerped over it with amount = weight * blend factor.
// Values without in-between steps (AgX look) switch when the amount exceeds 0.5.
// The host hands the result to the add-ons every frame; stored scene values stay.
// =============================================================================
#include "addons/forward/ForwardTonemap.hpp"
#include "addons/postprocess/PostProcessVolumeComponent.hpp"
#include "core/Math.hpp"
#include "ecs/World.hpp"

namespace engine::addons::postprocess {

// Values a volume can change. tonemap also carries what volumes leave alone
// (operator, mode, speeds), so the result can be handed on as a whole.
struct PostProcessValues
{
    renderer::addons::forward::TonemapSettings tonemap{};
    float bloomIntensity = 0.12f;
    float bloomThreshold = 1.f;
    float gtaoIntensity = 0.65f;
    float ssrIntensity = 1.f;
    float contactShadowIntensity = 1.f;
};

// 0 outside the box, rising linearly from the walls to 1 at blendDistance depth.
// Global volumes give 1. Ignores weight.
[[nodiscard]] float PostProcessVolumeFactor(const PostProcessVolumeComponent& volume,
                                            const math::Vec3& entityPosition,
                                            const math::Quat& entityRotation,
                                            const math::Vec3& viewPosition) noexcept;

// Lerps the overridden fields of one volume over values (amount 0..1).
void ApplyPostProcessVolume(PostProcessValues& values, const PostProcessVolumeComponent& volume,
                            float amount) noexcept;

// All active volumes of the world (WorldTransformComponent, active in the
// hierarchy) at viewPosition over base.
[[nodiscard]] PostProcessValues BlendPostProcessVolumes(const ecs::World& world,
                                                        const math::Vec3& viewPosition,
                                                        const PostProcessValues& base);

} // namespace engine::addons::postprocess
