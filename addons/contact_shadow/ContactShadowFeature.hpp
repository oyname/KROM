#pragma once
#include "renderer/RenderFeatureInterfaces.hpp"
#include "addons/contact_shadow/ContactShadowPass.hpp"
#include "addons/lighting/LightingFrameData.hpp"
#include <memory>
#include <vector>

namespace engine::renderer::addons::contact_shadow {

// outResources receives the shared GPU resources so callers can update settings at runtime.
[[nodiscard]] std::unique_ptr<IEngineFeature> CreateContactShadowFeature(
    std::shared_ptr<ContactShadowGpuResources>* outResources = nullptr);

// The light that gets contact shadows: the first directional light that casts shadows
// and has ShadowSettings::contactShadows on. The index is the one of lights[] in the
// shaders (LightingFrameData::lights order). -1 when there is none.
[[nodiscard]] int SelectContactShadowLight(const std::vector<engine::addons::lighting::ExtractedLight>& lights) noexcept;

} // namespace engine::renderer::addons::contact_shadow
