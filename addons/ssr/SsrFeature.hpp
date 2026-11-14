#pragma once
#include "renderer/RenderFeatureInterfaces.hpp"
#include "addons/ssr/SsrPass.hpp"
#include <memory>

namespace engine::renderer::addons::ssr {

// outResources receives the shared GPU resources so callers can change the
// settings at runtime (SsrGpuResources::settings).
[[nodiscard]] std::unique_ptr<IEngineFeature> CreateSsrFeature(
    std::shared_ptr<SsrGpuResources>* outResources = nullptr);

} // namespace engine::renderer::addons::ssr
