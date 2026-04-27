#pragma once

#include "ecs/World.hpp"
#include "renderer/RenderWorld.hpp"

namespace engine::jobs { class JobSystem; }

namespace engine::addons::mesh_renderer {

void ExtractRenderables(const ecs::World& world, renderer::RenderSceneSnapshot& snapshot);
void ExtractRenderables(const ecs::World& world, renderer::RenderWorld& renderWorld);
void ExtractRenderables(const ecs::World& world,
                        renderer::RenderSceneSnapshot& snapshot,
                        jobs::JobSystem* jobSystem);
void ExtractRenderables(const ecs::World& world,
                        renderer::RenderWorld& renderWorld,
                        jobs::JobSystem* jobSystem);

} // namespace engine::addons::mesh_renderer
