#pragma once

#include "renderer/RenderFeatureInterfaces.hpp"
#include "ecs/World.hpp"
#include <functional>
#include <memory>

namespace engine::renderer::addons::outline {

// Erzeugt das Screen-Space-Outline-Feature.
// getSelected: Lambda das pro Frame die selektierte EntityID liefert.
// Gibt nullptr zurueck, wenn keine Entity selektiert ist (kein Outline gerendert).
// Auf OpenGL deaktiviert (HLSL-only Shader).
std::unique_ptr<IEngineFeature> CreateOutlineFeature(
    std::function<EntityID()> getSelected,
    std::function<bool(EntityID)> shouldOutlineEntity);

} // namespace engine::renderer::addons::outline
