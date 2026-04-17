#pragma once

#include "ecs/World.hpp"
#include "renderer/RenderWorld.hpp"

namespace engine::addons::mesh_renderer {

void ExtractRenderables(const ecs::World& world, renderer::RenderWorld& renderWorld);

} // namespace engine::addons::mesh_renderer
