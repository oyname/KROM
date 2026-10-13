#pragma once

#include "renderer/MaterialDomain.hpp"
#include "renderer/MaterialShaderRequest.hpp"
#include "renderer/MaterialSystem.hpp"
#include "renderer/runtime/MaterialRuntimeDesc.hpp"
#include "renderer/runtime/ShaderMaterialSource.hpp"
#include <unordered_map>

namespace engine::renderer {

struct MaterialRuntimeBridge
{
private:
    [[nodiscard]] static std::unordered_map<MaterialHandle, MaterialRuntimeDesc>& RuntimeDescs()
    {
        static std::unordered_map<MaterialHandle, MaterialRuntimeDesc> storage;
        return storage;
    }

    [[nodiscard]] static ShaderVariantFlag BuildFlags(const CompiledMaterialDesc& compiled,
                                                      MaterialFeatureFlags instanceOverrides) noexcept
    {
        ShaderVariantFlag flags = static_cast<ShaderVariantFlag>(compiled.features | instanceOverrides);
        if (compiled.renderState.renderPolicy.alphaTest)
            flags = flags | ShaderVariantFlag::AlphaTest;
        if (compiled.renderState.renderPolicy.cull.doubleSided)
            flags = flags | ShaderVariantFlag::DoubleSided;
        return flags;
    }

    [[nodiscard]] static MaterialRuntimeDesc BuildRuntimeDesc(const CompiledMaterialDesc& compiled,
                                                              const MaterialRuntimeDesc& source) noexcept
    {
        MaterialRuntimeDesc runtime = source;
        const MaterialRenderPolicy& policy = compiled.renderState.renderPolicy;

        switch (policy.blend.mode)
        {
        case MaterialBlendMode::Opaque:
            runtime.blend.blendEnable = false;
            break;
        case MaterialBlendMode::AlphaBlend:
            runtime.blend.blendEnable = true;
            runtime.blend.srcBlend = BlendFactor::SrcAlpha;
            runtime.blend.dstBlend = BlendFactor::InvSrcAlpha;
            runtime.blend.srcBlendAlpha = BlendFactor::One;
            runtime.blend.dstBlendAlpha = BlendFactor::InvSrcAlpha;
            break;
        case MaterialBlendMode::Additive:
            runtime.blend.blendEnable = true;
            runtime.blend.srcBlend = BlendFactor::One;
            runtime.blend.dstBlend = BlendFactor::One;
            runtime.blend.srcBlendAlpha = BlendFactor::One;
            runtime.blend.dstBlendAlpha = BlendFactor::One;
            break;
        }

        runtime.depthStencil.depthEnable = policy.depth.test;
        runtime.depthStencil.depthWrite = policy.depth.write;
        switch (policy.depth.compare)
        {
        case MaterialDepthCompare::Less: runtime.depthStencil.depthFunc = DepthFunc::Less; break;
        case MaterialDepthCompare::LessEqual: runtime.depthStencil.depthFunc = DepthFunc::LessEqual; break;
        case MaterialDepthCompare::Equal: runtime.depthStencil.depthFunc = DepthFunc::Equal; break;
        case MaterialDepthCompare::Always: runtime.depthStencil.depthFunc = DepthFunc::Always; break;
        }

        if (policy.cull.doubleSided)
        {
            runtime.rasterizer.cullMode = CullMode::None;
        }
        else
        {
            switch (policy.cull.mode)
            {
            case MaterialCullMode::Back:  runtime.rasterizer.cullMode = CullMode::Back; break;
            case MaterialCullMode::Front: runtime.rasterizer.cullMode = CullMode::Front; break;
            case MaterialCullMode::None:  runtime.rasterizer.cullMode = CullMode::None; break;
            }
        }

        return runtime;
    }

    [[nodiscard]] static PipelineKey BuildPipelineKey(const MaterialRuntimeDesc& runtime,
                                                      uint64_t parameterLayoutHash) noexcept
    {
        const bool isShadowPass = runtime.renderPass == StandardRenderPasses::Shadow();
        const ShaderHandle pipelineVS = (isShadowPass && runtime.shadowShader.IsValid()) ? runtime.shadowShader : runtime.vertexShader;

        PipelineDesc pd;
        if (pipelineVS.IsValid())
            pd.shaderStages.push_back({ pipelineVS, ShaderStageMask::Vertex });
        if (!isShadowPass && runtime.fragmentShader.IsValid())
            pd.shaderStages.push_back({ runtime.fragmentShader, ShaderStageMask::Fragment });
        pd.rasterizer = runtime.rasterizer;
        pd.depthStencil = runtime.depthStencil;
        pd.blendStates[0] = runtime.blend;
        pd.topology = runtime.topology;
        pd.vertexLayout = runtime.vertexLayout;
        pd.colorFormat = isShadowPass ? Format::Unknown : runtime.colorFormat;
        pd.depthFormat = runtime.depthFormat;
        pd.shaderContractHash = parameterLayoutHash;
        pd.pipelineLayoutHash = parameterLayoutHash ^ (static_cast<uint64_t>(runtime.renderPass.value) << 32u);
        return PipelineKey::From(pd, runtime.renderPass);
    }

public:
    [[nodiscard]] static MaterialHandle RegisterMaterial(MaterialSystem& materials,
                                                         MaterialDesc desc,
                                                         MaterialRuntimeDesc runtime)
    {
        MaterialHandle handle = materials.RegisterMaterial(std::move(desc));
        if (handle.IsValid())
            SetRuntimeDesc(materials, handle, runtime);
        return handle;
    }

    [[nodiscard]] static MaterialHandle RegisterMaterialDefinition(MaterialSystem& materials,
                                                                   MaterialDefinition definition,
                                                                   MaterialRuntimeDesc runtime)
    {
        MaterialHandle handle = materials.RegisterMaterialDefinition(std::move(definition));
        if (handle.IsValid())
            SetRuntimeDesc(materials, handle, runtime);
        return handle;
    }

    static void SetRuntimeDesc(MaterialSystem& materials,
                               MaterialHandle material,
                               MaterialRuntimeDesc runtime) noexcept
    {
        const CompiledMaterialDesc* compiled = materials.GetCompiledDesc(material);
        if (!compiled)
            return;

        // Frühe Domain-Validation: Warnung beim Registrieren, nicht erst beim ersten Frame.
#if defined(KROM_DEBUG) || defined(_DEBUG) || !defined(NDEBUG)
        if (const MaterialDesc* desc = materials.GetDesc(material))
            ValidatePolicyForDomain(compiled->renderState.renderPolicy, desc->domain, desc->name.c_str());
#endif

        RuntimeDescs()[material] = BuildRuntimeDesc(*compiled, runtime);
        if (MaterialInstance* inst = materials.GetInstance(material))
        {
            inst->dirty = true;
            ++inst->revision;
        }
    }

    [[nodiscard]] static const MaterialRuntimeDesc* GetRuntimeDesc(const MaterialSystem& materials,
                                                                   MaterialHandle material) noexcept
    {
        auto& storage = RuntimeDescs();
        if (auto it = storage.find(material); it != storage.end())
            return &it->second;

        const MaterialInstance* inst = materials.GetInstance(material);
        if (!inst || inst->materialDefinition == material)
            return nullptr;

        if (auto it = storage.find(inst->materialDefinition); it != storage.end())
            return &it->second;
        return nullptr;
    }

    [[nodiscard]] static PipelineKey BuildPipelineKey(const MaterialSystem& materials,
                                                      MaterialHandle material) noexcept
    {
        const MaterialRuntimeDesc* runtime = GetRuntimeDesc(materials, material);
        const MaterialParameterLayout* layout = materials.GetParameterLayout(material);
        if (!runtime || !layout)
            return PipelineKey{};
        return BuildPipelineKey(*runtime, layout->layoutHash);
    }

    [[nodiscard]] static ShaderVariantFlag BuildShaderVariantFlags(const MaterialSystem& materials,
                                                                   MaterialHandle material) noexcept
    {
        const CompiledMaterialDesc* compiled = materials.GetCompiledDesc(material);
        const MaterialInstance* inst = materials.GetInstance(material);
        if (!compiled || !inst)
            return ShaderVariantFlag::None;
        return BuildFlags(*compiled, inst->featureOverrides);
    }

    [[nodiscard]] static std::vector<MaterialShaderRequest> BuildShaderRequests(const MaterialSystem& materials,
                                                                                MaterialHandle material)
    {
        const CompiledMaterialDesc* compiled = materials.GetCompiledDesc(material);
        const MaterialInstance* inst = materials.GetInstance(material);
        if (!compiled || !inst)
            return {};
        return BuildMaterialShaderRequests(*compiled, inst->featureOverrides);
    }
};

class IMaterialRuntimeBridge
{
public:
    virtual ~IMaterialRuntimeBridge() = default;

    [[nodiscard]] virtual const MaterialRuntimeDesc* GetRuntimeDesc(MaterialHandle material) const noexcept = 0;
    [[nodiscard]] virtual PipelineKey BuildPipelineKey(MaterialHandle material) const noexcept = 0;
    [[nodiscard]] virtual ShaderVariantFlag BuildShaderVariantFlags(MaterialHandle material) const noexcept = 0;
    [[nodiscard]] virtual std::vector<MaterialShaderRequest> BuildShaderRequests(MaterialHandle material) const = 0;
};

class MaterialSystemRuntimeBridge final : public IMaterialRuntimeBridge
{
public:
    explicit MaterialSystemRuntimeBridge(const MaterialSystem& materials) noexcept
        : m_materials(&materials)
    {
    }

    [[nodiscard]] const MaterialRuntimeDesc* GetRuntimeDesc(MaterialHandle material) const noexcept override
    {
        return m_materials ? MaterialRuntimeBridge::GetRuntimeDesc(*m_materials, material) : nullptr;
    }

    [[nodiscard]] PipelineKey BuildPipelineKey(MaterialHandle material) const noexcept override
    {
        return m_materials ? MaterialRuntimeBridge::BuildPipelineKey(*m_materials, material) : PipelineKey{};
    }

    [[nodiscard]] ShaderVariantFlag BuildShaderVariantFlags(MaterialHandle material) const noexcept override
    {
        return m_materials ? MaterialRuntimeBridge::BuildShaderVariantFlags(*m_materials, material) : ShaderVariantFlag::None;
    }

    [[nodiscard]] std::vector<MaterialShaderRequest> BuildShaderRequests(MaterialHandle material) const override
    {
        return m_materials ? MaterialRuntimeBridge::BuildShaderRequests(*m_materials, material) : std::vector<MaterialShaderRequest>{};
    }

private:
    const MaterialSystem* m_materials = nullptr;
};

class MaterialSystemShaderMaterialSource final : public IShaderMaterialSource
{
public:
    explicit MaterialSystemShaderMaterialSource(const MaterialSystem& materials) noexcept
        : m_materials(&materials)
    {
    }

    [[nodiscard]] size_t DescCount() const noexcept override
    {
        return m_materials ? m_materials->DescCount() : 0u;
    }

    [[nodiscard]] const MaterialDesc* GetDesc(MaterialHandle material) const noexcept override
    {
        return m_materials ? m_materials->GetDesc(material) : nullptr;
    }

    [[nodiscard]] const MaterialInstance* GetInstance(MaterialHandle material) const noexcept override
    {
        return m_materials ? m_materials->GetInstance(material) : nullptr;
    }

    [[nodiscard]] const MaterialParameterLayout* GetParameterLayout(MaterialHandle material) const noexcept override
    {
        return m_materials ? m_materials->GetParameterLayout(material) : nullptr;
    }

    [[nodiscard]] const MaterialRuntimeDesc* GetRuntimeDesc(MaterialHandle material) const noexcept override
    {
        return m_materials ? MaterialRuntimeBridge::GetRuntimeDesc(*m_materials, material) : nullptr;
    }

    [[nodiscard]] PipelineKey BuildPipelineKey(MaterialHandle material) const noexcept override
    {
        return m_materials ? MaterialRuntimeBridge::BuildPipelineKey(*m_materials, material) : PipelineKey{};
    }

    [[nodiscard]] ShaderVariantFlag BuildShaderVariantFlags(MaterialHandle material) const noexcept override
    {
        return m_materials ? MaterialRuntimeBridge::BuildShaderVariantFlags(*m_materials, material) : ShaderVariantFlag::None;
    }

    [[nodiscard]] uint64_t GetRevision(MaterialHandle material) const noexcept override
    {
        return m_materials ? m_materials->GetRevision(material) : 0ull;
    }

    [[nodiscard]] const std::vector<uint8_t>& GetCBData(MaterialHandle material) const override
    {
        static const std::vector<uint8_t> kEmpty{};
        return m_materials ? const_cast<MaterialSystem*>(m_materials)->GetCBData(material) : kEmpty;
    }

    [[nodiscard]] const CbLayout& GetCBLayout(MaterialHandle material) const override
    {
        static const CbLayout kEmpty{};
        return m_materials ? const_cast<MaterialSystem*>(m_materials)->GetCBLayout(material) : kEmpty;
    }

private:
    const MaterialSystem* m_materials = nullptr;
};

} // namespace engine::renderer
