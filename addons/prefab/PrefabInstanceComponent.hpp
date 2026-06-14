#pragma once
// =============================================================================
// KROM Engine - addons/prefab/PrefabInstanceComponent.hpp
// Markiert eine Entity als Live-Prefab-Instanz.
// ResolvePrefabBindings() liest den Quell-Pfad und synchronisiert
// Mesh/Material/Bounds, waehrend Transform (optional) erhalten bleibt.
// =============================================================================
#include "ecs/ComponentMeta.hpp"
#include <string>

namespace engine {

struct PrefabInstanceComponent
{
    // Relativer Pfad zum .prefab-Asset, z.B. "Prefabs/Barrel.prefab".
    // Wird relativ zu Assets/ im Projektordner aufgeloest.
    std::string prefabAssetPath;

    // true  → Position/Rotation/Scale dieser Instanz bleibt erhalten wenn
    //         das Prefab neu aufgeloest wird (Instanz-spezifische Platzierung).
    // false → Transform wird ebenfalls vom Prefab ueberschrieben.
    bool overrideTransform = true;
};

inline void RegisterPrefabInstanceComponent(ecs::ComponentMetaRegistry& registry)
{
    ecs::RegisterComponent<PrefabInstanceComponent>(registry, "PrefabInstanceComponent");
}

} // namespace engine
