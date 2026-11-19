#pragma once
// =============================================================================
// KROM Engine - addons/script/ScriptColorGrading.hpp
// Conversion between the script view of the colour grading
// (script::ColorGrading) and the forward add-on's settings. Header only; used
// by the hosts (editor, krom.h runtime) that implement IRenderSettingsService.
// =============================================================================
#include "addons/forward/ForwardColorGrading.hpp"
#include "addons/script/IRenderSettingsService.hpp"

namespace engine::script {

[[nodiscard]] inline ColorGrading ToScriptColorGrading(const renderer::addons::forward::ColorGradingSettings& in) noexcept
{
    ColorGrading out{};
    out.agxLook = static_cast<uint8_t>(in.look);
    out.temperature = in.temperature;
    out.tint = in.tint;
    for (int i = 0; i < 3; ++i)
    {
        out.colorFilter[i] = in.colorFilter[i];
        out.lift[i] = in.lift.color[i];
        out.gamma[i] = in.gamma.color[i];
        out.gain[i] = in.gain.color[i];
    }
    out.hueShift = in.hueShift;
    out.contrast = in.contrast;
    out.saturation = in.saturation;
    out.lift[3] = in.lift.value;
    out.gamma[3] = in.gamma.value;
    out.gain[3] = in.gain.value;
    return out;
}

[[nodiscard]] inline renderer::addons::forward::ColorGradingSettings FromScriptColorGrading(const ColorGrading& in) noexcept
{
    renderer::addons::forward::ColorGradingSettings out{};
    out.look = static_cast<renderer::addons::forward::AgxLook>(in.agxLook);
    out.temperature = in.temperature;
    out.tint = in.tint;
    for (int i = 0; i < 3; ++i)
    {
        out.colorFilter[i] = in.colorFilter[i];
        out.lift.color[i] = in.lift[i];
        out.gamma.color[i] = in.gamma[i];
        out.gain.color[i] = in.gain[i];
    }
    out.hueShift = in.hueShift;
    out.contrast = in.contrast;
    out.saturation = in.saturation;
    out.lift.value = in.lift[3];
    out.gamma.value = in.gamma[3];
    out.gain.value = in.gain[3];
    out.Clamp();   // also maps unknown look ids to none
    return out;
}

} // namespace engine::script
