#pragma once
#include "renderer/MaterialFeatureEval.hpp"
#include "renderer/MaterialTypes.hpp"
#include <string>
#include <unordered_map>
#include <vector>

namespace engine::renderer {

class MaterialSystem
{
public:
    MaterialSystem()  = default;
    ~MaterialSystem() = default;

    [[nodiscard]] MaterialHandle RegisterMaterial(MaterialDesc desc);
    [[nodiscard]] MaterialHandle CreateInstance(MaterialHandle base,
                                                std::string instanceName = "");

    [[nodiscard]] const MaterialDesc*     GetDesc(MaterialHandle h) const noexcept;
    [[nodiscard]] MaterialHandle          FindMaterial(const std::string& name) const noexcept;
    [[nodiscard]] MaterialInstance*       GetInstance(MaterialHandle h) noexcept;
    [[nodiscard]] const MaterialInstance* GetInstance(MaterialHandle h) const noexcept;

    [[nodiscard]] PipelineKey BuildPipelineKey(MaterialHandle h) const noexcept {
        if (!ValidHandle(h)) return PipelineKey{};
        const MaterialDesc& desc = m_descs[h.Index()].desc;
        const MaterialInstance& inst = m_instances[h.Index()];
        return MaterialFeatureEval::BuildPipelineKey(desc, inst);
    }

    void SetFloat  (MaterialHandle h, const std::string& name, float v);
    void SetVec4   (MaterialHandle h, const std::string& name, const math::Vec4& v);
    void SetTexture(MaterialHandle h, const std::string& name, TextureHandle tex);
    void MarkDirty (MaterialHandle h);

    void SetSemanticFloat(MaterialHandle h, MaterialSemantic semantic, float v);
    void SetSemanticVec4(MaterialHandle h, MaterialSemantic semantic, const math::Vec4& v);
    void SetSemanticTexture(MaterialHandle h, MaterialSemantic semantic, TextureHandle tex, uint32_t samplerIdx = 0u);
    void ClearSemanticTexture(MaterialHandle h, MaterialSemantic semantic);

    [[nodiscard]] MaterialFeatureFlag GetFeatureFlags(MaterialHandle h) const noexcept;
    [[nodiscard]] ShaderVariantFlag BuildShaderVariantFlags(MaterialHandle h) const noexcept {
        if (!ValidHandle(h)) return ShaderVariantFlag::None;
        const MaterialDesc& desc = m_descs[h.Index()].desc;
        const MaterialInstance& inst = m_instances[h.Index()];
        return MaterialFeatureEval::BuildShaderVariantFlags(desc, inst);
    }
    [[nodiscard]] bool HasExplicitSemanticValue(MaterialHandle h, MaterialSemantic semantic) const noexcept;
    [[nodiscard]] bool HasExplicitSemanticTexture(MaterialHandle h, MaterialSemantic semantic) const noexcept;
    [[nodiscard]] TextureHandle GetSemanticTexture(MaterialHandle h, MaterialSemantic semantic) const noexcept;
    [[nodiscard]] MaterialSemanticValue GetSemanticValue(MaterialHandle h, MaterialSemantic semantic) const noexcept;
    [[nodiscard]] MaterialSemanticValue ResolveSemanticValue(MaterialHandle h, MaterialSemantic semantic) const noexcept;

    const std::vector<uint8_t>& GetCBData  (MaterialHandle h);
    const CbLayout&             GetCBLayout(MaterialHandle h);

    size_t DescCount()     const noexcept { return m_descs.size(); }
    size_t InstanceCount() const noexcept { return m_instances.size(); }

    [[nodiscard]] uint64_t GetRevision(MaterialHandle h) const noexcept;

    static const char* SemanticName(MaterialSemantic semantic) noexcept;

private:
    struct DescSlot
    {
        MaterialDesc      desc;
        std::string       name;
        bool              isInstance = false;
        MaterialHandle    baseHandle;
    };

    std::vector<DescSlot>         m_descs;
    std::vector<MaterialInstance> m_instances;
    std::unordered_map<std::string, MaterialHandle> m_nameLookup;
    std::vector<uint32_t>         m_generations;
    std::vector<uint32_t>         m_freeSlots;

    uint32_t AllocSlot();
    [[nodiscard]] bool ValidHandle(MaterialHandle h) const noexcept;

    void NormalizeDesc(MaterialDesc& desc) const noexcept {
        MaterialFeatureEval::NormalizeDesc(desc);
    }
    void InitializeInstanceFromDesc(MaterialInstance& inst, const MaterialDesc& desc) const noexcept;
    [[nodiscard]] std::vector<MaterialParam> BuildCanonicalParams(const MaterialDesc& desc,
                                                                  const MaterialInstance& inst) const;
};

} // namespace engine::renderer
