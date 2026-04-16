#include "renderer/ShaderRuntime.hpp"
#include "renderer/MaterialSystem.hpp"
#include <algorithm>
#include <cstring>

namespace engine::renderer {

    ShaderHandle ShaderRuntime::GetOrCreateVariant(ShaderHandle shaderAssetHandle,
        ShaderPassType pass,
        ShaderVariantFlag flags)
    {
        if (!m_device || !m_assets || !shaderAssetHandle.IsValid())
            return ShaderHandle::Invalid();

        auto* shaderAsset = m_assets->shaders.Get(shaderAssetHandle);
        if (!shaderAsset)
            return ShaderHandle::Invalid();

        const ShaderVariantKey key{ shaderAssetHandle, pass, flags };
        const assets::ShaderTargetProfile target = ShaderCompiler::ResolveTargetProfile(*m_device);
        return m_variantCache.GetOrCreate(*shaderAsset, target, key);
    }

    bool ShaderRuntime::CollectShaderRequests(const MaterialSystem& materials,
        std::vector<ShaderHandle>& outRequests) const
    {
        outRequests.clear();
        outRequests.reserve(materials.DescCount() * 2u);

        for (uint32_t i = 0; i < materials.DescCount(); ++i)
        {
            const MaterialHandle material = MaterialHandle::Make(i, 1u);
            const MaterialDesc* desc = materials.GetDesc(material);
            if (!desc)
                continue;
            if (desc->vertexShader.IsValid())
                outRequests.push_back(desc->vertexShader);
            if (desc->fragmentShader.IsValid())
                outRequests.push_back(desc->fragmentShader);
        }

        std::sort(outRequests.begin(), outRequests.end(), [](const ShaderHandle& a, const ShaderHandle& b) {
            return a.value < b.value;
            });
        outRequests.erase(std::unique(outRequests.begin(), outRequests.end(), [](const ShaderHandle& a, const ShaderHandle& b) {
            return a == b;
            }), outRequests.end());
        return true;
    }

    bool ShaderRuntime::CollectMaterialRequests(const MaterialSystem& materials,
        std::vector<MaterialHandle>& outRequests) const
    {
        outRequests.clear();
        outRequests.reserve(materials.DescCount());

        for (uint32_t i = 0; i < materials.DescCount(); ++i)
        {
            const MaterialHandle material = MaterialHandle::Make(i, 1u);
            if (materials.GetDesc(material))
                outRequests.push_back(material);
        }

        std::sort(outRequests.begin(), outRequests.end(), [](const MaterialHandle& a, const MaterialHandle& b) {
            return a.value < b.value;
            });
        outRequests.erase(std::unique(outRequests.begin(), outRequests.end(), [](const MaterialHandle& a, const MaterialHandle& b) {
            return a == b;
            }), outRequests.end());
        return true;
    }

    ShaderHandle ShaderRuntime::PrepareShaderAsset(ShaderHandle shaderAssetHandle)
    {
        if (!RequireRenderThread("PrepareShaderAsset"))
            return ShaderHandle::Invalid();
        if (!m_device || !shaderAssetHandle.IsValid())
            return ShaderHandle::Invalid();
        if (!m_assets)
            return shaderAssetHandle;

        if (auto it = m_shaderAssets.find(shaderAssetHandle); it != m_shaderAssets.end() && it->second.gpuHandle.IsValid())
            return it->second.gpuHandle;

        const assets::ShaderAsset* shaderAsset = m_assets->shaders.Get(shaderAssetHandle);
        if (!shaderAsset)
            return shaderAssetHandle;

        const ShaderStageMask stageMask = ToStageMask(shaderAsset->stage);
        ShaderHandle gpuHandle = ShaderHandle::Invalid();
        bool fromBytecode = false;
        bool fromCompiledArtifact = false;
        uint64_t compiledHash = 0ull;
        const auto target = ShaderCompiler::ResolveTargetProfile(*m_device);

        auto* mutableShaderAsset = m_assets->shaders.Get(shaderAssetHandle);
        if (mutableShaderAsset)
        {
            const bool needsCompiledTarget = (target == assets::ShaderTargetProfile::Vulkan_SPIRV)
                || (target == assets::ShaderTargetProfile::DirectX11_SM5)
                || (target == assets::ShaderTargetProfile::DirectX12_SM6)
                || (target == assets::ShaderTargetProfile::OpenGL_GLSL450);

            if (needsCompiledTarget && !FindCompiledArtifact(*mutableShaderAsset))
            {
                assets::CompiledShaderArtifact compiledArtifact{};
                std::string compileError;
                if (!ShaderCompiler::CompileForTarget(*mutableShaderAsset, target, compiledArtifact, &compileError))
                {
                    Debug::LogError("ShaderRuntime.cpp: failed to compile shader '%s' for %s: %s",
                        mutableShaderAsset->debugName.c_str(),
                        ShaderCompiler::ToString(target),
                        compileError.c_str());
                }
                else
                {
                    mutableShaderAsset->compiledArtifacts.push_back(std::move(compiledArtifact));
                }
            }
        }

        shaderAsset = m_assets->shaders.Get(shaderAssetHandle);
        if (const auto* compiled = FindCompiledArtifact(*shaderAsset))
        {
            fromCompiledArtifact = true;
            compiledHash = compiled->sourceHash;
            if (!compiled->bytecode.empty())
            {
                gpuHandle = m_device->CreateShaderFromBytecode(compiled->bytecode.data(), compiled->bytecode.size(), stageMask, compiled->debugName.empty() ? shaderAsset->debugName : compiled->debugName);
                fromBytecode = true;
            }
            else
            {
                if (target == assets::ShaderTargetProfile::Vulkan_SPIRV)
                {
                    Debug::LogError("ShaderRuntime.cpp: Vulkan requires SPIR-V bytecode for '%s'",
                        (compiled->debugName.empty() ? shaderAsset->debugName : compiled->debugName).c_str());
                }
                else
                {
                    gpuHandle = m_device->CreateShaderFromSource(compiled->sourceText, stageMask, compiled->entryPoint, compiled->debugName.empty() ? shaderAsset->debugName : compiled->debugName);
                }
            }
        }
        else if (!shaderAsset->bytecode.empty())
        {
            gpuHandle = m_device->CreateShaderFromBytecode(shaderAsset->bytecode.data(), shaderAsset->bytecode.size(), stageMask, shaderAsset->debugName);
            fromBytecode = true;
            compiledHash = HashBytes(shaderAsset->bytecode.data(), shaderAsset->bytecode.size());
        }
        else
        {
            if (target == assets::ShaderTargetProfile::Vulkan_SPIRV)
            {
                Debug::LogError("ShaderRuntime.cpp: no SPIR-V bytecode available for shader '%s'", shaderAsset->debugName.c_str());
            }
            else
            {
                gpuHandle = m_device->CreateShaderFromSource(shaderAsset->sourceCode, stageMask, shaderAsset->entryPoint, shaderAsset->debugName);
                compiledHash = HashBytes(shaderAsset->sourceCode.data(), shaderAsset->sourceCode.size());
            }
        }

        ShaderAssetStatus status{};
        status.assetHandle = shaderAssetHandle;
        status.gpuHandle = gpuHandle;
        status.stage = stageMask;
        status.target = target;
        if (const auto* compiled = FindCompiledArtifact(*shaderAsset))
            status.contract = compiled->contract;
        status.compiledHash = compiledHash;
        status.loaded = gpuHandle.IsValid();
        status.fromBytecode = fromBytecode;
        status.fromCompiledArtifact = fromCompiledArtifact;
        m_shaderAssets[shaderAssetHandle] = status;
        return gpuHandle;
    }

    bool ShaderRuntime::CommitShaderRequests(const std::vector<ShaderHandle>& requests)
    {
        if (!RequireRenderThread("CommitShaderRequests"))
            return false;
        // Kein AssetRegistry → keine Asset-Kompilierung nötig.
        // Handles werden als direkte GPU-Handles behandelt.
        if (!m_assets)
            return true;

        bool ok = true;
        for (ShaderHandle handle : requests)
            ok = PrepareShaderAsset(handle).IsValid() && ok;
        return ok;
    }

    bool ShaderRuntime::PrepareAllShaderAssets()
    {
        if (!m_assets) return true;
        std::vector<ShaderHandle> requests;
        m_assets->shaders.ForEach([&](ShaderHandle handle, assets::ShaderAsset&) {
            requests.push_back(handle);
            });
        std::sort(requests.begin(), requests.end(), [](const ShaderHandle& a, const ShaderHandle& b) { return a.value < b.value; });
        requests.erase(std::unique(requests.begin(), requests.end(), [](const ShaderHandle& a, const ShaderHandle& b) { return a == b; }), requests.end());
        return CommitShaderRequests(requests);
    }
    const ShaderAssetStatus* ShaderRuntime::GetShaderStatus(ShaderHandle shaderAssetHandle) const noexcept
    {
        if (auto it = m_shaderAssets.find(shaderAssetHandle); it != m_shaderAssets.end())
            return &it->second;
        return nullptr;
    }

} // namespace engine::renderer
