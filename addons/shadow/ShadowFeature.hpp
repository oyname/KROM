#pragma once
#include "renderer/RenderFeatureInterfaces.hpp"
#include <memory>

namespace engine::addons::shadow {

[[nodiscard]] std::unique_ptr<renderer::IEngineFeature> CreateShadowFeature();

} // namespace engine::addons::shadow
