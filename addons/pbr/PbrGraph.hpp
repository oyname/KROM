#pragma once

#include "assets/MaterialGraph.hpp"
#include "renderer/MaterialSystem.hpp"

namespace engine::renderer::pbr {

inline constexpr std::array<const char*, assets::kMaterialGraphMaxNodes> kGraphInstructionParams{
    "graphNode0", "graphNode1", "graphNode2", "graphNode3",
    "graphNode4", "graphNode5", "graphNode6", "graphNode7"
};
inline constexpr std::array<const char*, assets::kMaterialGraphMaxTextures> kGraphTextureParams{
    "graphTexture0", "graphTexture1"
};

inline void SetPbrGraph(MaterialSystem& materials, MaterialHandle material,
                        const assets::CompiledMaterialGraph& graph,
                        const std::array<TextureHandle, assets::kMaterialGraphMaxTextures>& textures)
{
    if (!material.IsValid() || !graph.Active())
        return;
    materials.SetVec4(material, "graphOutputs", graph.outputs);
    for (size_t i = 0; i < static_cast<size_t>(graph.outputs.z); ++i)
        materials.SetVec4(material, kGraphInstructionParams[i], graph.instructions[i]);
    for (size_t i = 0; i < textures.size(); ++i)
        materials.SetTexture(material, kGraphTextureParams[i], textures[i]);
}

} // namespace engine::renderer::pbr
