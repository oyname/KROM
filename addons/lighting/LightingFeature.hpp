#pragma once

#include "renderer/RenderFeatureInterfaces.hpp"
#include <memory>

namespace engine::addons::lighting {

std::unique_ptr<renderer::IEngineFeature> CreateLightingFeature();

} // namespace engine::addons::lighting
