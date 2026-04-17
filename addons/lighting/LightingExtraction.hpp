#pragma once

#include "ecs/World.hpp"
#include "addons/lighting/LightingFrameData.hpp"

namespace engine::addons::lighting {

void ExtractLights(const ecs::World& world, renderer::RenderWorld& renderWorld);

} // namespace engine::addons::lighting
