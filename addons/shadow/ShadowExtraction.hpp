#pragma once
#include "ecs/World.hpp"
#include "renderer/RenderWorld.hpp"

namespace engine::addons::shadow {

// Baut einen gemeinsamen Shadow-Plan fuer alle relevanten Schattenlichter.
// Der aktuelle Renderpfad verwendet daraus einen ausgewaehlten Request, die
// CPU-Datenhaltung bleibt aber bereits lichttyp-uebergreifend.
void ExtractShadow(const ecs::World& world, renderer::RenderWorld& renderWorld);

} // namespace engine::addons::shadow
