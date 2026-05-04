#pragma once

#include "renderer/MaterialSystem.hpp"
#include "renderer/ShaderBindingModel.hpp"
#include "PbrSlot.hpp"
#include <string>
#include <unordered_map>
#include <vector>

namespace engine::renderer::pbr {

class PbrInstanceBuilder;

// ---------------------------------------------------------------------------
// PbrMasterMaterial
//
// Defines the PBR shader template and manages permutation caching.
// One master covers all slot configurations — permutations are registered
// on demand when Build() is called on a PbrInstanceBuilder.
//
// This is the Unreal "Master Material" equivalent.
// ---------------------------------------------------------------------------
class PbrMasterMaterial
{
public:
    struct Config
    {
        ShaderHandle     vs;
        ShaderHandle     fs;
        ShaderHandle     shadow;
        VertexLayout     vertexLayout;
        RenderPassID     renderPass    = StandardRenderPasses::Opaque();
        Format           colorFormat   = Format::RGBA16_FLOAT;
        Format           depthFormat   = Format::D24_UNORM_S8_UINT;
        MaterialCullMode cullMode      = MaterialCullMode::None;
        WindingOrder     frontFace     = WindingOrder::CCW;
        bool             castShadows   = true;
        bool             receiveShadows = true;
    };

    [[nodiscard]] static PbrMasterMaterial Create(MaterialSystem& materials, Config config);

    // Create a new instance builder — the instance is finalized by calling Build()
    [[nodiscard]] PbrInstanceBuilder CreateInstance(std::string name) noexcept;

    // Editor introspection — returns the slot schema
    [[nodiscard]] const std::vector<PbrSlotDesc>& GetSlotDescs() const noexcept { return m_slotDescs; }

    // Runtime slot access for editor (set a slot value on an existing instance)
    void SetSlotValue(MaterialHandle instance,
                      const std::string& slotId,
                      const PbrSlotValue& value) noexcept;

    [[nodiscard]] bool IsValid() const noexcept { return m_materials != nullptr; }

    // Called by PbrInstanceBuilder::Build() — not for direct use
    [[nodiscard]] MaterialHandle GetOrRegisterPermutation(uint64_t flagBits);
    [[nodiscard]] MaterialSystem* GetMaterialSystem() noexcept { return m_materials; }

private:
    MaterialSystem*                          m_materials = nullptr;
    Config                                   m_config{};
    std::vector<PbrSlotDesc>                 m_slotDescs;
    std::unordered_map<uint64_t, MaterialHandle> m_permCache;

    [[nodiscard]] static std::vector<PbrSlotDesc> BuildSlotDescs();
};

} // namespace engine::renderer::pbr
