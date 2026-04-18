#pragma once

#include "renderer/MaterialFeatureEval.hpp"
#include "renderer/ShaderReflector.hpp"
#include <string>
#include <unordered_map>
#include <vector>

namespace engine::renderer {

class MaterialSystem
{
public:
    MaterialSystem() = default;
    ~MaterialSystem() = default;

    void SetShaderReflector(const IShaderReflector* reflector) noexcept { m_reflector = reflector; }

    [[nodiscard]] MaterialHandle RegisterMaterial(MaterialDesc desc);
    [[nodiscard]] MaterialHandle CreateInstance(MaterialHandle base,
                                                std::string instanceName = "");

    [[nodiscard]] const MaterialDesc*     GetDesc(MaterialHandle h) const noexcept;
    [[nodiscard]] MaterialHandle          FindMaterial(const std::string& name) const noexcept;
    [[nodiscard]] MaterialInstance*       GetInstance(MaterialHandle h) noexcept;
    [[nodiscard]] const MaterialInstance* GetInstance(MaterialHandle h) const noexcept;

    [[nodiscard]] PipelineKey BuildPipelineKey(MaterialHandle h) const noexcept;
    [[nodiscard]] ShaderVariantFlag BuildShaderVariantFlags(MaterialHandle h) const noexcept;

    void SetFloat(MaterialHandle h, const std::string& name, float v);
    void SetVec4(MaterialHandle h, const std::string& name, const math::Vec4& v);
    void SetTexture(MaterialHandle h, const std::string& name, TextureHandle tex);
    void MarkDirty(MaterialHandle h);

    const std::vector<uint8_t>& GetCBData(MaterialHandle h);
    const CbLayout&             GetCBLayout(MaterialHandle h);

    size_t DescCount() const noexcept { return m_descs.size(); }
    size_t InstanceCount() const noexcept { return m_instances.size(); }
    [[nodiscard]] uint64_t GetRevision(MaterialHandle h) const noexcept;

private:
    struct DescSlot
    {
        MaterialDesc   desc;
        std::string    name;
        bool           isInstance = false;
        MaterialHandle baseHandle;
    };

    std::vector<DescSlot> m_descs;
    std::vector<MaterialInstance> m_instances;
    std::unordered_map<std::string, MaterialHandle> m_nameLookup;
    std::vector<uint32_t> m_generations;
    std::vector<uint32_t> m_freeSlots;
    const IShaderReflector* m_reflector = nullptr;

    uint32_t AllocSlot();
    [[nodiscard]] bool ValidHandle(MaterialHandle h) const noexcept;

    void NormalizeDesc(MaterialDesc& desc) const noexcept;
    void InitializeInstanceFromDesc(MaterialInstance& inst, const MaterialDesc& desc) const noexcept;
    [[nodiscard]] ShaderParameterLayout BuildLayoutFromDesc(const MaterialDesc& desc) const noexcept;
    void SyncBlobFromParams(MaterialInstance& inst) const;
    template<typename Fn>
    bool MutateParameter(MaterialHandle h, const std::string& name, MaterialParam::Type expectedType, Fn&& fn);
};

} // namespace engine::renderer
