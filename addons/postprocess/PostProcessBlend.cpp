#include "addons/postprocess/PostProcessBlend.hpp"

#include "ecs/Components.hpp"
#include "ecs/HierarchyState.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace engine::addons::postprocess {
namespace {

using renderer::addons::forward::ColorGradingWheel;

float Lerp(float a, float b, float t) noexcept { return a + (b - a) * t; }

void LerpColor(std::array<float, 3>& a, const std::array<float, 3>& b, float t) noexcept
{
    for (size_t i = 0; i < 3u; ++i)
        a[i] = Lerp(a[i], b[i], t);
}

void LerpWheel(ColorGradingWheel& a, const ColorGradingWheel& b, float t) noexcept
{
    LerpColor(a.color, b.color, t);
    a.value = Lerp(a.value, b.value, t);
}

} // namespace

float PostProcessVolumeFactor(const PostProcessVolumeComponent& volume,
                              const math::Vec3& entityPosition,
                              const math::Quat& entityRotation,
                              const math::Vec3& viewPosition) noexcept
{
    if (volume.global)
        return 1.f;
    const math::Quat rotation = entityRotation.Normalized();
    const math::Vec3 center = entityPosition + rotation.Rotate(volume.boxOffset);
    const math::Vec3 local = rotation.Conjugate().Rotate(viewPosition - center);
    // Distance to the nearest wall, negative outside.
    const float depth = std::min({ volume.extents.x - std::abs(local.x),
                                   volume.extents.y - std::abs(local.y),
                                   volume.extents.z - std::abs(local.z) });
    if (!(depth >= 0.f))
        return 0.f;
    if (!(volume.blendDistance > 0.f))
        return 1.f;
    return std::clamp(depth / volume.blendDistance, 0.f, 1.f);
}

void ApplyPostProcessVolume(PostProcessValues& values, const PostProcessVolumeComponent& v,
                            float amount) noexcept
{
    const float t = std::clamp(amount, 0.f, 1.f);
    if (!(t > 0.f))
        return;
    using O = PostProcessOverride;
    auto& tm = values.tonemap;
    auto& g = tm.grading;
    const auto& vg = v.grading;

    if (v.Overrides(O::ExposureEv))        tm.exposureEv = Lerp(tm.exposureEv, v.exposureEv, t);
    if (v.Overrides(O::AutoExposureMinEv)) tm.autoExposure.minEv = Lerp(tm.autoExposure.minEv, v.autoExposureMinEv, t);
    if (v.Overrides(O::AutoExposureMaxEv)) tm.autoExposure.maxEv = Lerp(tm.autoExposure.maxEv, v.autoExposureMaxEv, t);
    // No in-between looks: the volume's look wins above half.
    if (v.Overrides(O::GradingLook) && t > 0.5f) g.look = vg.look;
    if (v.Overrides(O::GradingTemperature)) g.temperature = Lerp(g.temperature, vg.temperature, t);
    if (v.Overrides(O::GradingTint))        g.tint = Lerp(g.tint, vg.tint, t);
    if (v.Overrides(O::GradingColorFilter)) LerpColor(g.colorFilter, vg.colorFilter, t);
    if (v.Overrides(O::GradingHueShift))    g.hueShift = Lerp(g.hueShift, vg.hueShift, t);
    if (v.Overrides(O::GradingContrast))    g.contrast = Lerp(g.contrast, vg.contrast, t);
    if (v.Overrides(O::GradingSaturation))  g.saturation = Lerp(g.saturation, vg.saturation, t);
    if (v.Overrides(O::GradingLift))        LerpWheel(g.lift, vg.lift, t);
    if (v.Overrides(O::GradingGamma))       LerpWheel(g.gamma, vg.gamma, t);
    if (v.Overrides(O::GradingGain))        LerpWheel(g.gain, vg.gain, t);
    if (v.Overrides(O::BloomIntensity))         values.bloomIntensity = Lerp(values.bloomIntensity, v.bloomIntensity, t);
    if (v.Overrides(O::BloomThreshold))         values.bloomThreshold = Lerp(values.bloomThreshold, v.bloomThreshold, t);
    if (v.Overrides(O::GtaoIntensity))          values.gtaoIntensity = Lerp(values.gtaoIntensity, v.gtaoIntensity, t);
    if (v.Overrides(O::SsrIntensity))           values.ssrIntensity = Lerp(values.ssrIntensity, v.ssrIntensity, t);
    if (v.Overrides(O::ContactShadowIntensity)) values.contactShadowIntensity =
        Lerp(values.contactShadowIntensity, v.contactShadowIntensity, t);
}

PostProcessValues BlendPostProcessVolumes(const ecs::World& world, const math::Vec3& viewPosition,
                                          const PostProcessValues& base)
{
    PostProcessValues result = base;
    struct Entry { const PostProcessVolumeComponent* volume; float amount; float boxVolume; uint32_t entity; };
    std::vector<Entry> entries;
    world.View<PostProcessVolumeComponent, WorldTransformComponent>(
        [&](EntityID entity, const PostProcessVolumeComponent& volume, const WorldTransformComponent& xf) {
            if (volume.overrides == 0u || !(volume.weight > 0.f) || !IsEntityActiveInHierarchy(world, entity))
                return;
            const float amount = std::clamp(volume.weight, 0.f, 1.f) *
                PostProcessVolumeFactor(volume, xf.position, xf.rotation, viewPosition);
            if (!(amount > 0.f))
                return;
            const float boxVolume = volume.global ? INFINITY
                                                  : volume.extents.x * volume.extents.y * volume.extents.z;
            entries.push_back({ &volume, amount, boxVolume, entity.value });
        });
    if (entries.empty())
        return result;
    // Applied in this order, so the last one wins: low priority first; on equal
    // priority the larger box (and global volumes) first.
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        if (a.volume->priority != b.volume->priority) return a.volume->priority < b.volume->priority;
        if (a.boxVolume != b.boxVolume) return a.boxVolume > b.boxVolume;
        return a.entity < b.entity;
    });
    for (const Entry& e : entries)
        ApplyPostProcessVolume(result, *e.volume, e.amount);

    auto& ae = result.tonemap.autoExposure;
    if (ae.maxEv < ae.minEv)
        ae.maxEv = ae.minEv;
    result.tonemap.grading.Clamp();
    return result;
}

} // namespace engine::addons::postprocess
