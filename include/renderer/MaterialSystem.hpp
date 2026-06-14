#pragma once

#include "renderer/CompiledMaterialDesc.hpp"
#include "renderer/MaterialInstance.hpp"
#include <string>
#include <unordered_map>
#include <vector>

namespace engine::renderer {

class MaterialSystem
{
public:
    MaterialSystem() = default;
    ~MaterialSystem() = default;

    [[nodiscard]] MaterialHandle RegisterMaterial(MaterialDesc desc);
    [[nodiscard]] MaterialHandle RegisterMaterialDefinition(MaterialDefinition definition);
    [[nodiscard]] MaterialHandle CreateInstance(MaterialHandle base,
                                                std::string instanceName = "");

    [[nodiscard]] const MaterialDefinition* GetDefinition(MaterialHandle h) const noexcept;
    [[nodiscard]] const CompiledMaterialDesc* GetCompiledDesc(MaterialHandle h) const noexcept;
    [[nodiscard]] const MaterialDesc*     GetDesc(MaterialHandle h) const noexcept;
    [[nodiscard]] const MaterialParameterLayout* GetParameterLayout(MaterialHandle h) const noexcept;
    [[nodiscard]] MaterialHandle          FindMaterial(const std::string& name) const noexcept;
    [[nodiscard]] MaterialInstance*       GetInstance(MaterialHandle h) noexcept;
    [[nodiscard]] const MaterialInstance* GetInstance(MaterialHandle h) const noexcept;

    void SetFloat(MaterialHandle h, const std::string& name, float v);
    void SetInt(MaterialHandle h, const std::string& name, int32_t v);
    void SetBool(MaterialHandle h, const std::string& name, bool v);
    void SetVec2(MaterialHandle h, const std::string& name, const math::Vec2& v);
    void SetVec3(MaterialHandle h, const std::string& name, const math::Vec3& v);
    void SetVec4(MaterialHandle h, const std::string& name, const math::Vec4& v);
    void SetTexture(MaterialHandle h, const std::string& name, TextureHandle tex);
    void MarkDirty(MaterialHandle h);

    // Gibt den Slot frei und inkrementiert die Generation (existierende Handles werden invalid).
    void DestroyMaterial(MaterialHandle h);

    const std::vector<uint8_t>& GetCBData(MaterialHandle h);
    const CbLayout&             GetCBLayout(MaterialHandle h);

    size_t DescCount() const noexcept { return m_descs.size(); }
    size_t InstanceCount() const noexcept { return m_instances.size(); }
    [[nodiscard]] uint64_t GetRevision(MaterialHandle h) const noexcept;

private:
    struct InstanceDerivedData
    {
        MaterialParameterLayout layout{};
        CbLayout cbLayout{};
        std::vector<uint8_t> cbData;
        bool cbDirty = true;
        bool layoutDirty = true;
    };

    struct DescSlot
    {
        MaterialDefinition definition;
        CompiledMaterialDesc compiled;
        bool                isInstance = false;
        MaterialHandle      baseHandle;
    };

    std::vector<DescSlot> m_descs;
    std::vector<MaterialInstance> m_instances;
    std::vector<InstanceDerivedData> m_instanceDerived;
    std::unordered_map<std::string, MaterialHandle> m_nameLookup;
    std::vector<uint32_t> m_generations;
    std::vector<uint32_t> m_freeSlots;
    uint32_t AllocSlot();
    [[nodiscard]] bool ValidHandle(MaterialHandle h) const noexcept;

    void NormalizeDesc(MaterialDesc& desc) const noexcept;
    void InitializeInstanceFromDesc(MaterialInstance& inst, const MaterialDesc& desc) const noexcept;
    [[nodiscard]] MaterialParameterLayout BuildLayoutFromDesc(const MaterialDesc& desc) const noexcept;
    void SyncBlobFromParams(MaterialInstance& inst, InstanceDerivedData& derived) const;
    template<typename Fn>
    bool MutateParameter(MaterialHandle h, const std::string& name, MaterialParam::Type expectedType, Fn&& fn);
};

} // namespace engine::renderer
