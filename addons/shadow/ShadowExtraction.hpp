#pragma once
#include "ecs/World.hpp"
#include "renderer/RenderExtractionContext.hpp"

namespace engine::addons::shadow {

// Baut einen gemeinsamen Shadow-Plan fuer alle relevanten Schattenlichter.
// Der aktuelle Renderpfad verwendet daraus einen ausgewaehlten Request, die
// CPU-Datenhaltung bleibt aber bereits lichttyp-uebergreifend.
void ExtractShadow(const renderer::SceneExtractionContext& context);

} // namespace engine::addons::shadow
