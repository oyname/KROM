#pragma once
// =============================================================================
// KROM Engine - renderer/ECSExtractor.hpp
//
// Statische Hilfsklasse an der ECS/Renderer-Grenze.
// Beinhaltet nur noch generische Hilfen, die von Feature-AddOns geteilt werden.
// =============================================================================
#include "ecs/World.hpp"
#include "renderer/RenderWorld.hpp"

namespace engine::renderer {

class ECSExtractor
{
public:
    // Gemeinsamer ECS-Aktivitätsfilter für alle Extraction-Steps.
    [[nodiscard]] static bool IsEntityActive(const ecs::World& world, EntityID entity);

    ECSExtractor() = delete;
};

} // namespace engine::renderer
