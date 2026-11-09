#pragma once

#include "assets/MaterialGraph.hpp"
#include "renderer/MaterialSystem.hpp"
#include "renderer/ShaderBindingModel.hpp"

namespace engine::renderer {

inline constexpr std::array<const char*, assets::kMaterialGraphMaxNodes> kGraphInstructionParams{
    "graphNode0", "graphNode1", "graphNode2", "graphNode3",
    "graphNode4", "graphNode5", "graphNode6", "graphNode7",
    "graphNode8", "graphNode9", "graphNode10", "graphNode11",
    "graphNode12", "graphNode13", "graphNode14", "graphNode15"
};
inline constexpr std::array<const char*, assets::kMaterialGraphMaxTextures> kGraphTextureParams{
    "graphTexture0", "graphTexture1", "graphTexture2", "graphTexture3"
};
// Two entries per texture: scale.xy + offset.xy, then scroll.xy.
inline constexpr std::array<const char*, assets::kMaterialGraphMaxTextures * 2> kGraphUVParams{
    "graphUV0", "graphUV1", "graphUV2", "graphUV3",
    "graphUV4", "graphUV5", "graphUV6", "graphUV7"
};

// Appends the graph parameters. They must follow the last field of the
// material's PerMaterial block so that shaders can declare them behind it.
inline void AddMaterialGraphParameters(MaterialDesc& desc, bool openGL)
{
    desc.features = desc.features | MaterialFeatureFlags::MaterialGraph;

    const auto addVec4 = [&desc](const char* name, const math::Vec4& value) {
        MaterialParam param{};
        param.name = name;
        param.type = MaterialParam::Type::Vec4;
        param.value.f[0] = value.x;
        param.value.f[1] = value.y;
        param.value.f[2] = value.z;
        param.value.f[3] = value.w;
        desc.parameters.push_back(param);
    };
    addVec4("graphOutputs", {-1.f, -1.f, 0.f, 0.f});
    for (const char* name : kGraphInstructionParams)
        addVec4(name, {});
    for (size_t i = 0; i < kGraphUVParams.size(); ++i)
        addVec4(kGraphUVParams[i], i % 2 == 0 ? math::Vec4{1.f, 1.f, 0.f, 0.f} : math::Vec4{});
    for (const char* name : kGraphTextureParams)
    {
        MaterialParam param{};
        param.name = name;
        param.type = MaterialParam::Type::Texture;
        desc.parameters.push_back(param);
    }

    desc.textureBindingOverrides[kGraphTextureParams[0]] = 13u;
    // GLSL uses unit 9 for raw shadow depth. HLSL uses t14 for Forward+ shadow data.
    desc.textureBindingOverrides[kGraphTextureParams[1]] = openGL ? 14u : 9u;
    desc.textureBindingOverrides[kGraphTextureParams[2]] = TexSlots::GraphTexture2;
    desc.textureBindingOverrides[kGraphTextureParams[3]] = TexSlots::GraphTexture3;
}

inline void SetMaterialGraph(MaterialSystem& materials, MaterialHandle material,
                             const assets::CompiledMaterialGraph& graph,
                             const std::array<TextureHandle, assets::kMaterialGraphMaxTextures>& textures)
{
    if (!material.IsValid() || !graph.Active())
        return;
    materials.SetVec4(material, "graphOutputs", graph.outputs);
    for (size_t i = 0; i < static_cast<size_t>(graph.outputs.z); ++i)
        materials.SetVec4(material, kGraphInstructionParams[i], graph.instructions[i]);
    for (const auto& parameter : graph.parameters)
        materials.SetParameterAlias(material, parameter.name, kGraphInstructionParams[parameter.instruction]);
    for (size_t i = 0; i < textures.size(); ++i)
    {
        materials.SetVec4(material, kGraphUVParams[i * 2], graph.textureUV[i]);
        materials.SetVec4(material, kGraphUVParams[i * 2 + 1],
                          {graph.textureScroll[i].x, graph.textureScroll[i].y, 0.f, 0.f});
        materials.SetTexture(material, kGraphTextureParams[i], textures[i]);
    }
}

} // namespace engine::renderer
