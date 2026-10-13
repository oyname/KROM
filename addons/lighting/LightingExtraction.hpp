#pragma once

#include "ecs/World.hpp"
#include "addons/lighting/LightingFrameData.hpp"
#include "renderer/RenderExtractionContext.hpp"

namespace engine::addons::lighting {

void ExtractLights(const ecs::World& world, renderer::RenderExtractionView extraction);
void ExtractLights(const renderer::SceneExtractionContext& context);

} // namespace engine::addons::lighting
