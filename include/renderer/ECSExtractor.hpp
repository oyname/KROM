#pragma once
// =============================================================================
// KROM Engine - renderer/ECSExtractor.hpp
//
// Kleine Kern-Hilfsklasse an der Renderer/ECS-Grenze.
// Verantwortlich für Snapshot-Lebenszyklus und gemeinsame ECS-Helfer.
// Feature-spezifische Extraktion läuft über registrierte ISceneExtractionSteps.
// =============================================================================
#include "ecs/World.hpp"
#include "renderer/SceneSnapshot.hpp"

namespace engine::renderer {

class ECSExtractor
{
public:
    // Bereitet einen Snapshot für einen neuen Frame vor.
    static void BeginSnapshot(SceneSnapshot& snapshot);

    // Gemeinsamer ECS-Filter für Extraction-Steps.
    [[nodiscard]] static bool IsEntityActive(const ecs::World& world, EntityID entity);

    // Kompatibilitäts-Fallback: extrahiert den bisherigen Standardpfad direkt.
    static void Extract(const ecs::World& world, SceneSnapshot& snapshot);

    ECSExtractor() = delete;
};

} // namespace engine::renderer
