#pragma once
// =============================================================================
// KROM Engine - addons/prefab/PrefabInstanceComponent.hpp
// Markiert eine Entity als Live-Prefab-Instanz.
// Source recipes are merged into existing entities, preserving local overrides
// and script ownership. Scene GUIDs keep bindings valid after loading/undo.
// =============================================================================
#include "ecs/ComponentMeta.hpp"
#include <string>
#include <vector>

namespace engine {

struct PrefabInstanceComponent
{
    // Relativer Pfad zum .prefab-Asset, z.B. "Prefabs/Barrel.prefab".
    // Wird relativ zu Assets/ im Projektordner aufgeloest.
    std::string prefabAssetPath;

    // true: keep the instance's position/rotation (scene placement). Scale is
    // inherited unless explicitly overridden locally, like other properties.
    // false: inherit the entire root transform, including position/rotation.
    bool overrideTransform = true;

    // Last inherited recipe and stable record -> scene GUID bindings. GUIDs are
    // serialized instead of EntityIDs so scene loading/undo can remap safely.
    std::string baselinePrefabJson;
    struct Binding
    {
        std::string recordId;
        std::string entityGuid;
        std::vector<std::string> overriddenProperties;
    };
    std::vector<Binding> bindings;
};

// Identity within the source recipe (independent of a scene entity's GUID).
struct PrefabSourceComponent { std::string recordId; };

inline void RegisterPrefabInstanceComponent(ecs::ComponentMetaRegistry& registry)
{
    ecs::RegisterComponent<PrefabInstanceComponent>(registry, "PrefabInstanceComponent");
    ecs::RegisterComponent<PrefabSourceComponent>(registry, "PrefabSourceComponent");
}

} // namespace engine
