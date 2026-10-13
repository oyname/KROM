#pragma once
#include "renderer/RenderFeatureInterfaces.hpp"
#include "addons/gtao/GtaoPass.hpp"
#include <memory>

namespace engine::renderer::addons::gtao {

// outResources receives the shared GPU resources so callers can update settings at runtime.
[[nodiscard]] std::unique_ptr<IEngineFeature> CreateGtaoFeature(
    std::shared_ptr<GtaoGpuResources>* outResources = nullptr);

} // namespace engine::renderer::addons::gtao
