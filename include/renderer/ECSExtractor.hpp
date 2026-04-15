#pragma once
// =============================================================================
// KROM Engine - renderer/ECSExtractor.hpp
//
// Statische Hilfsklasse an der ECS/Renderer-Grenze.
// Befüllt RenderWorld direkt – kein SceneSnapshot-Zwischenpuffer.
// Feature-spezifische Extraktion läuft über registrierte ISceneExtractionSteps,
// die diese Methoden intern aufrufen.
// =============================================================================
#include "ecs/World.hpp"
#include "renderer/RenderWorld.hpp"

namespace engine::renderer {

class ECSExtractor
{
public:
    // Gemeinsamer ECS-Aktivitätsfilter für alle Extraction-Steps.
    [[nodiscard]] static bool IsEntityActive(const ecs::World& world, EntityID entity);

    // Standard-Extraktion: ECS → RenderWorld direkt.
    static void ExtractRenderables(const ecs::World& world, RenderWorld& renderWorld);
    static void ExtractLights    (const ecs::World& world, RenderWorld& renderWorld);

    ECSExtractor() = delete;
};

} // namespace engine::renderer
