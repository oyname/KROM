#pragma once

#include "renderer/FeatureRegistry.hpp"
#include <memory>

namespace engine::addons::mesh_renderer {

std::unique_ptr<renderer::IEngineFeature> CreateMeshRendererFeature();

} // namespace engine::addons::mesh_renderer
