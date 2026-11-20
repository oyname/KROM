#pragma once
// =============================================================================
// KROM Engine - addons/postprocess/PostProcessVolumeComponent.hpp
//
// Post-process volume: overrides scene exposure and colour grading values and
// the intensity of bloom, GTAO, SSR and contact shadows while the camera is
// inside its box (or everywhere when global). The box works like the one of the
// reflection probe: it turns with the entity, the entity's scale is ignored and
// blendDistance fades the volume in from the walls (0 at the wall, 1 at that
// depth). Each value only acts when its override flag is set.
//
// Volumes never change the stored scene values; the host blends them into a
// copy every frame (PostProcessBlend.hpp). Not overridable on purpose: exposure
// mode, tonemap operator, MSAA, post-AA and effects on/off (they rebuild the
// frame graph or pipelines).
// =============================================================================
#include "addons/forward/ForwardColorGrading.hpp"
#include "core/Math.hpp"
#include "ecs/ComponentMeta.hpp"
#include <cstdint>

namespace engine {

// Override flags of PostProcessVolumeComponent::overrides. Saved by name
// (PostProcessOverrideName); keep the list and the names in sync.
enum class PostProcessOverride : uint32_t
{
    ExposureEv             = 1u << 0,   // fixed exposure, or correction in automatic mode
    AutoExposureMinEv      = 1u << 1,
    AutoExposureMaxEv      = 1u << 2,
    GradingLook            = 1u << 3,   // no in-between values: switches above 50 %
    GradingTemperature     = 1u << 4,
    GradingTint            = 1u << 5,
    GradingColorFilter     = 1u << 6,
    GradingHueShift        = 1u << 7,
    GradingContrast        = 1u << 8,
    GradingSaturation      = 1u << 9,
    GradingLift            = 1u << 10,  // tint and value together
    GradingGamma           = 1u << 11,
    GradingGain            = 1u << 12,
    BloomIntensity         = 1u << 13,
    GtaoIntensity          = 1u << 14,
    SsrIntensity           = 1u << 15,
    ContactShadowIntensity = 1u << 16,
};
inline constexpr uint32_t kPostProcessOverrideCount = 17u;

[[nodiscard]] constexpr uint32_t PostProcessOverrideBit(PostProcessOverride o) noexcept
{
    return static_cast<uint32_t>(o);
}

struct PostProcessVolumeComponent
{
    // Head
    bool       global = false;             // box ignored, acts everywhere
    float      weight = 1.f;               // 0..1, scripts may change it (e.g. damage effect)
    int32_t    priority = 0;               // higher priority is applied later (wins)
    // Box (local axes of the entity, scale ignored)
    math::Vec3 extents{ 5.f, 3.f, 5.f };   // half size, metres
    math::Vec3 boxOffset{ 0.f, 0.f, 0.f }; // box center relative to the entity
    float      blendDistance = 1.f;        // metres inward from the walls; 0 = hard edge

    uint32_t   overrides = 0u;             // PostProcessOverride bits

    // Exposure (same scale as TonemapSettings)
    float exposureEv = 0.f;
    float autoExposureMinEv = -4.f;
    float autoExposureMaxEv = 4.f;
    // Colour grading (same fields as the scene card)
    renderer::addons::forward::ColorGradingSettings grading{};
    // Effect intensities (defaults = project defaults)
    float bloomIntensity = 0.12f;
    float gtaoIntensity = 0.65f;
    float ssrIntensity = 1.f;
    float contactShadowIntensity = 1.f;

    [[nodiscard]] bool Overrides(PostProcessOverride o) const noexcept
    {
        return (overrides & PostProcessOverrideBit(o)) != 0u;
    }
    void SetOverride(PostProcessOverride o, bool on) noexcept
    {
        if (on) overrides |= PostProcessOverrideBit(o);
        else    overrides &= ~PostProcessOverrideBit(o);
    }

    bool operator==(const PostProcessVolumeComponent&) const = default;
};

[[nodiscard]] inline const char* PostProcessOverrideName(PostProcessOverride o) noexcept
{
    switch (o)
    {
    case PostProcessOverride::ExposureEv:             return "exposureEv";
    case PostProcessOverride::AutoExposureMinEv:      return "autoExposureMinEv";
    case PostProcessOverride::AutoExposureMaxEv:      return "autoExposureMaxEv";
    case PostProcessOverride::GradingLook:            return "look";
    case PostProcessOverride::GradingTemperature:     return "temperature";
    case PostProcessOverride::GradingTint:            return "tint";
    case PostProcessOverride::GradingColorFilter:     return "colorFilter";
    case PostProcessOverride::GradingHueShift:        return "hueShift";
    case PostProcessOverride::GradingContrast:        return "contrast";
    case PostProcessOverride::GradingSaturation:      return "saturation";
    case PostProcessOverride::GradingLift:            return "lift";
    case PostProcessOverride::GradingGamma:           return "gamma";
    case PostProcessOverride::GradingGain:            return "gain";
    case PostProcessOverride::BloomIntensity:         return "bloomIntensity";
    case PostProcessOverride::GtaoIntensity:          return "gtaoIntensity";
    case PostProcessOverride::SsrIntensity:           return "ssrIntensity";
    case PostProcessOverride::ContactShadowIntensity: return "contactShadowIntensity";
    }
    return "";
}

inline void RegisterPostProcessComponents(ecs::ComponentMetaRegistry& registry)
{
    ecs::RegisterComponent<PostProcessVolumeComponent>(registry, "PostProcessVolumeComponent");
}

} // namespace engine
