#pragma once
#include "assets/AssetRegistry.hpp"
#include "core/Debug.hpp"
#include "renderer/IDevice.hpp"
#include "renderer/ShaderCompiler.hpp"
#include <functional>
#include <unordered_map>

namespace engine::renderer {

class ShaderVariantCache
{
public:
    using UploadFn = std::function<ShaderHandle(const assets::ShaderAsset&, const assets::CompiledShaderArtifact&)>;

    void SetUploadFunction(UploadFn fn) { m_uploadFn = std::move(fn); }

    [[nodiscard]] ShaderHandle GetOrCreate(const assets::ShaderAsset& asset,
                                           assets::ShaderTargetProfile target,
                                           const ShaderVariantKey& key)
    {
        const ShaderVariantKey normalized = key.Normalized();
        const auto it = m_cache.find(normalized);
        if (it != m_cache.end())
            return it->second;

        assets::CompiledShaderArtifact artifact;
        std::string error;
        if (!ShaderCompiler::CompileVariant(asset, target, normalized.flags, artifact, &error))
        {
            Debug::LogError("ShaderVariantCache: CompileVariant failed for '%s': %s",
                            asset.debugName.c_str(), error.c_str());
            return ShaderHandle::Invalid();
        }

        const ShaderHandle handle = m_uploadFn ? m_uploadFn(asset, artifact) : ShaderHandle::Invalid();
        if (!handle.IsValid())
        {
            Debug::LogError("ShaderVariantCache: GPU upload failed for '%s'", asset.debugName.c_str());
            return ShaderHandle::Invalid();
        }

        m_cache.emplace(normalized, handle);
        ++m_totalVariants;
        return handle;
    }

    void Clear()
    {
        m_cache.clear();
        m_totalVariants = 0u;
    }

    [[nodiscard]] size_t CachedCount() const noexcept { return m_cache.size(); }
    [[nodiscard]] uint32_t TotalVariants() const noexcept { return m_totalVariants; }

private:
    std::unordered_map<ShaderVariantKey, ShaderHandle> m_cache;
    UploadFn m_uploadFn;
    uint32_t m_totalVariants = 0u;
};

} // namespace engine::renderer
