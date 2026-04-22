#include "renderer/ShaderRuntime.hpp"
#include <algorithm>

namespace engine::renderer {

ShaderStageMask ShaderRuntime::ToStageMask(assets::ShaderStage stage) noexcept
{
    switch (stage)
    {
    case assets::ShaderStage::Vertex: return ShaderStageMask::Vertex;
    case assets::ShaderStage::Fragment: return ShaderStageMask::Fragment;
    case assets::ShaderStage::Compute: return ShaderStageMask::Compute;
    case assets::ShaderStage::Geometry: return ShaderStageMask::Geometry;
    case assets::ShaderStage::Hull: return ShaderStageMask::Hull;
    case assets::ShaderStage::Domain: return ShaderStageMask::Domain;
    default: return ShaderStageMask::None;
    }
}

uint64_t ShaderRuntime::HashBytes(const void* data, size_t size) noexcept
{
    const auto* bytes = static_cast<const uint8_t*>(data);
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < size; ++i)
    {
        h ^= static_cast<uint64_t>(bytes[i]);
        h *= 1099511628211ull;
    }
    return h;
}

const assets::CompiledShaderArtifact* ShaderRuntime::FindCompiledArtifact(const assets::ShaderAsset& shaderAsset) const noexcept
{
    if (!m_device)
        return nullptr;

    const auto target = ShaderCompiler::ResolveTargetProfile(*m_device);
    auto it = std::find_if(shaderAsset.compiledArtifacts.begin(), shaderAsset.compiledArtifacts.end(), [&](const assets::CompiledShaderArtifact& artifact) {
        return artifact.target == target && artifact.stage == shaderAsset.stage && artifact.IsValid();
    });
    if (it != shaderAsset.compiledArtifacts.end())
        return &(*it);

    it = std::find_if(shaderAsset.compiledArtifacts.begin(), shaderAsset.compiledArtifacts.end(), [&](const assets::CompiledShaderArtifact& artifact) {
        return artifact.target == assets::ShaderTargetProfile::Generic && artifact.stage == shaderAsset.stage && artifact.IsValid();
    });
    return it != shaderAsset.compiledArtifacts.end() ? &(*it) : nullptr;
}

uint64_t ShaderRuntime::HashMaterialState(const std::vector<uint8_t>& cbData,
                                          const std::vector<ResolvedMaterialBinding>& bindings) noexcept
{
    uint64_t h = cbData.empty() ? 0ull : HashBytes(cbData.data(), cbData.size());
    for (const auto& binding : bindings)
    {
        h ^= HashBytes(binding.name.data(), binding.name.size());
        h ^= static_cast<uint64_t>(binding.slot) << 7u;
        h ^= static_cast<uint64_t>(binding.texture.value) << 13u;
        h ^= static_cast<uint64_t>(binding.buffer.value) << 19u;
        h ^= static_cast<uint64_t>(binding.samplerIndex) << 23u;
        h ^= static_cast<uint64_t>(binding.kind == ResolvedMaterialBinding::Kind::Texture ? 0xAAu
                           : binding.kind == ResolvedMaterialBinding::Kind::Sampler ? 0xBBu
                           : binding.kind == ResolvedMaterialBinding::Kind::Buffer ? 0xCCu : 0xDDu);
        h *= 1099511628211ull;
    }
    return h;
}

void ShaderRuntime::CreateFallbackTextures()
{
    if (!m_device)
        return;

    auto create1x1 = [&](const char* name, const std::array<uint8_t, 4>& rgba) -> TextureHandle
    {
        TextureDesc td{};
        td.width = 1u;
        td.height = 1u;
        td.format = Format::RGBA8_UNORM;
        td.usage = ResourceUsage::ShaderResource | ResourceUsage::CopyDest;
        td.initialState = ResourceState::ShaderRead;
        td.debugName = name;
        TextureHandle tex = m_device->CreateTexture(td);
        if (tex.IsValid())
            m_device->UploadTextureData(tex, rgba.data(), rgba.size(), 0u, 0u);
        return tex;
    };

    m_fallbackTextures.white = create1x1("Fallback_White", { 255u, 255u, 255u, 255u });
    m_fallbackTextures.black = create1x1("Fallback_Black", { 0u, 0u, 0u, 255u });
    m_fallbackTextures.gray = create1x1("Fallback_Gray", { 128u, 128u, 128u, 255u });
    m_fallbackTextures.ormNeutral = create1x1("Fallback_ORMNeutral", { 255u, 128u, 0u, 255u });
    m_fallbackTextures.neutralNormal = create1x1("Fallback_NeutralNormal", { 128u, 128u, 255u, 255u });
    m_fallbackTextures.iblIrradiance = create1x1("Fallback_IBLIrradianceBlack", { 0u, 0u, 0u, 255u });
    m_fallbackTextures.iblPrefiltered = create1x1("Fallback_IBLPrefilteredBlack", { 0u, 0u, 0u, 255u });
    m_fallbackTextures.brdfLut = create1x1("Fallback_BRDFLutBlack", { 0u, 0u, 0u, 255u });
}

void ShaderRuntime::CreateDefaultSamplers()
{
    if (!m_device)
        return;

    SamplerDesc linearWrap;
    linearWrap.addressU = linearWrap.addressV = linearWrap.addressW = WrapMode::Repeat;
    linearWrap.minFilter = linearWrap.magFilter = linearWrap.mipFilter = FilterMode::Linear;
    m_samplers.linearWrap = m_device->CreateSampler(linearWrap);

    SamplerDesc linearClamp = linearWrap;
    linearClamp.addressU = linearClamp.addressV = linearClamp.addressW = WrapMode::Clamp;
    m_samplers.linearClamp = m_device->CreateSampler(linearClamp);

    SamplerDesc pointClamp = linearClamp;
    pointClamp.minFilter = pointClamp.magFilter = pointClamp.mipFilter = FilterMode::Nearest;
    m_samplers.pointClamp = m_device->CreateSampler(pointClamp);

    SamplerDesc shadow = pointClamp;
    shadow.addressU = shadow.addressV = shadow.addressW = WrapMode::Clamp;
    shadow.borderColor[0] = 1.0f;
    shadow.borderColor[1] = 1.0f;
    shadow.borderColor[2] = 1.0f;
    shadow.borderColor[3] = 1.0f;
    shadow.compareFunc = CompareFunc::LessEqual;
    m_samplers.shadowPCF = m_device->CreateSampler(shadow);
    Debug::Log("ShadowSamplerDesc(engine): handle=%u min=%d mag=%d mip=%d addr=(%d,%d,%d) cmp=%d border=(%.1f %.1f %.1f %.1f)",
        m_samplers.shadowPCF,
        static_cast<int>(shadow.minFilter),
        static_cast<int>(shadow.magFilter),
        static_cast<int>(shadow.mipFilter),
        static_cast<int>(shadow.addressU),
        static_cast<int>(shadow.addressV),
        static_cast<int>(shadow.addressW),
        static_cast<int>(shadow.compareFunc),
        shadow.borderColor[0],
        shadow.borderColor[1],
        shadow.borderColor[2],
        shadow.borderColor[3]);
}

} // namespace engine::renderer
