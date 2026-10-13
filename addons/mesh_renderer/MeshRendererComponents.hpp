#pragma once

#include "core/Types.hpp"
#include "ecs/ComponentMeta.hpp"
#include "renderer/RenderLayers.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace engine {

struct MeshComponent
{
    MeshHandle  mesh;
    bool        visible        = true;
    bool        castShadows    = true;
    bool        receiveShadows = true;
    uint32_t    layerMask      = renderer::LAYER_DEFAULT;
    // Pfad zum Quell-Asset — persistiert ueber Sessions.
    // Format: "path/to/model.glb#mesh/<index>" (Prefab-Import)
    //      oder "path/to/mesh.kmesh" (direkter Mesh-Asset-Pfad).
    // Wird in SyncEditorSceneState genutzt um das Mesh beim Laden neu zu laden.
    std::string meshAssetPath;

    MeshComponent() = default;
    explicit MeshComponent(MeshHandle m) : mesh(m) {}
};

struct MaterialComponent
{
    struct SlotOverride
    {
        uint32_t       submeshIndex = 0u;
        MaterialHandle material;
        std::string    materialAssetPath;
    };

    // Legacy/entity-wide override. Bestehende Codepfade und alte Szenen nutzen
    // dieses Feld weiter; neue per-slot Zuweisungen liegen in slotOverrides.
    MaterialHandle material;
    uint32_t       submeshIndex = 0u;
    std::string    materialAssetPath;
    std::string    baseColorTexturePath;
    std::vector<SlotOverride> slotOverrides;

    MaterialComponent() = default;
    explicit MaterialComponent(MaterialHandle m) : material(m) {}

    [[nodiscard]] const SlotOverride* FindSlotOverride(uint32_t index) const noexcept
    {
        for (const SlotOverride& slot : slotOverrides)
            if (slot.submeshIndex == index)
                return &slot;
        return nullptr;
    }

    [[nodiscard]] SlotOverride* FindSlotOverride(uint32_t index) noexcept
    {
        for (SlotOverride& slot : slotOverrides)
            if (slot.submeshIndex == index)
                return &slot;
        return nullptr;
    }
};

inline void RegisterMeshRendererComponents(ecs::ComponentMetaRegistry& registry)
{
    using namespace ecs;
    RegisterComponent<MeshComponent>(registry, "MeshComponent");
    RegisterComponent<MaterialComponent>(registry, "MaterialComponent");
}

} // namespace engine
