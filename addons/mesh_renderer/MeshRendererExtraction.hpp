#pragma once

#include "ecs/World.hpp"
#include "renderer/RenderExtractionContext.hpp"

namespace engine::jobs { class JobSystem; }

namespace engine::addons::mesh_renderer {

void ExtractRenderables(const ecs::World& world, renderer::RenderExtractionView extraction);
void ExtractRenderables(const renderer::SceneExtractionContext& context);
void ExtractRenderables(const ecs::World& world,
                        renderer::RenderExtractionView extraction,
                        jobs::JobSystem* jobSystem);

} // namespace engine::addons::mesh_renderer
