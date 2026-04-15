#pragma once
#include "assets/AssetRegistry.hpp"
#include "renderer/IDevice.hpp"
#include "renderer/ShaderCompiler.hpp"
#include "renderer/ShaderVariantCache.hpp"
#include "renderer/MaterialSystem.hpp"
#include "renderer/Environment.hpp"
#include "renderer/PipelineCache.hpp"
#include "renderer/ShaderBindingModel.hpp"
#include "renderer/ShaderContract.hpp"
#include <thread>
#include <unordered_map>
#include <vector>

namespace engine::renderer {

struct ShaderValidationIssue
{
    enum class Severity : uint8_t { Warning, Error };
    Severity severity = Severity::Warning;
    std::string message;
};

struct ShaderAssetStatus
{
    ShaderHandle assetHandle;
    ShaderHandle gpuHandle;
    ShaderStageMask stage = ShaderStageMask::None;
    assets::ShaderTargetProfile target = assets::ShaderTargetProfile::Generic;
    ShaderPipelineContract contract{};
    uint64_t compiledHash = 0ull;
    bool loaded = false;
    bool fromBytecode = false;
    bool fromCompiledArtifact = false;
};

struct ResolvedMaterialBinding
{
    enum class Kind : uint8_t { ConstantBuffer, Texture, Sampler };
    Kind kind = Kind::ConstantBuffer;
    std::string name;
    uint32_t slot = 0u;
    ShaderStageMask stages = ShaderStageMask::None;
    TextureHandle texture;
    uint32_t samplerIndex = 0u;
};

struct MaterialGpuState
{
    MaterialHandle material;
    PipelineHandle pipeline;
    ShaderHandle vertexShader;
    ShaderHandle fragmentShader;
    BufferHandle perMaterialCB;
    uint32_t perMaterialCBSize = 0u;
    uint64_t contentHash = 0u;
    uint64_t materialRevision = 0ull;
    uint64_t environmentRevision = 0ull;
    bool valid = false;
    std::vector<ResolvedMaterialBinding> bindings;
    std::vector<ShaderValidationIssue> issues;
};

class ShaderRuntime
{
public:
    ShaderRuntime() = default;
    ~ShaderRuntime() = default;

    bool Initialize(IDevice& device);
    void Shutdown();

    void SetAssetRegistry(assets::AssetRegistry* registry) noexcept { m_assets = registry; }
    [[nodiscard]] assets::AssetRegistry* GetAssetRegistry() const noexcept { return m_assets; }

    [[nodiscard]] bool CollectShaderRequests(const MaterialSystem& materials,
                                             std::vector<ShaderHandle>& outRequests) const;
    [[nodiscard]] bool CollectMaterialRequests(const MaterialSystem& materials,
                                               std::vector<MaterialHandle>& outRequests) const;

    [[nodiscard]] ShaderHandle PrepareShaderAsset(ShaderHandle shaderAssetHandle);
    [[nodiscard]] bool CommitShaderRequests(const std::vector<ShaderHandle>& requests);
    [[nodiscard]] bool PrepareAllShaderAssets();

    [[nodiscard]] bool PrepareMaterial(const MaterialSystem& materials, MaterialHandle material);
    [[nodiscard]] bool CommitMaterialRequests(const MaterialSystem& materials,
                                              const std::vector<MaterialHandle>& requests);
    [[nodiscard]] bool PrepareAllMaterials(const MaterialSystem& materials);

    [[nodiscard]] const MaterialGpuState* GetMaterialState(MaterialHandle material) const noexcept;
    [[nodiscard]] const ShaderAssetStatus* GetShaderStatus(ShaderHandle shaderAssetHandle) const noexcept;

    [[nodiscard]] ShaderHandle GetOrCreateVariant(ShaderHandle shaderAssetHandle, ShaderPassType pass, ShaderVariantFlag flags);
    [[nodiscard]] const ShaderVariantCache& GetVariantCache() const noexcept { return m_variantCache; }

    // Legacy-Overload: bindet ganze Buffer (kein Range-Offset).
    [[nodiscard]] bool BindMaterial(ICommandList& cmd,
                                    const MaterialSystem& materials,
                                    MaterialHandle material,
                                    BufferHandle perFrameCB,
                                    BufferHandle perObjectCB,
                                    BufferHandle perPassCB = BufferHandle::Invalid(),
                                    RenderPassTag passOverride = RenderPassTag::Opaque);

    // Range-Binding-Overload: bindet per-Object und per-Pass als BufferBinding (mit Offset+Size).
    // Zu bevorzugen für Arena-basierten Per-Object-CB-Pfad.
    [[nodiscard]] bool BindMaterialWithRange(ICommandList& cmd,
                                             const MaterialSystem& materials,
                                             MaterialHandle material,
                                             BufferHandle   perFrameCB,
                                             BufferBinding  perObjectBinding,
                                             BufferBinding  perPassBinding = {},
                                             RenderPassTag  passOverride = RenderPassTag::Opaque);

    [[nodiscard]] bool ValidateMaterial(const MaterialSystem& materials,
                                        MaterialHandle material,
                                        std::vector<ShaderValidationIssue>& outIssues) const;

    void SetEnvironmentState(const EnvironmentRuntimeState& state) noexcept;
    [[nodiscard]] const EnvironmentRuntimeState& GetEnvironmentState() const noexcept { return m_environment; }
    [[nodiscard]] bool HasIBL() const noexcept { return m_environment.active; }

    [[nodiscard]] size_t PreparedShaderCount() const noexcept { return m_shaderAssets.size(); }
    [[nodiscard]] size_t PreparedMaterialCount() const noexcept { return m_materialStates.size(); }
    [[nodiscard]] bool IsRenderThread() const noexcept;

private:
    struct RuntimeSamplerSet
    {
        uint32_t linearWrap = 0u;
        uint32_t linearClamp = 0u;
        uint32_t pointClamp = 0u;
        uint32_t shadowPCF = 0u;
    };

    struct RuntimeFallbackTextures
    {
        TextureHandle white = TextureHandle::Invalid();
        TextureHandle black = TextureHandle::Invalid();
        TextureHandle gray = TextureHandle::Invalid();
        TextureHandle ormNeutral = TextureHandle::Invalid();
        TextureHandle neutralNormal = TextureHandle::Invalid();
        TextureHandle iblIrradiance = TextureHandle::Invalid();
        TextureHandle iblPrefiltered = TextureHandle::Invalid();
        TextureHandle brdfLut = TextureHandle::Invalid();
    };


    IDevice* m_device = nullptr;
    assets::AssetRegistry* m_assets = nullptr;
    PipelineCache m_pipelineCache;
    RuntimeSamplerSet m_samplers{};
    RuntimeFallbackTextures m_fallbackTextures{};
    EnvironmentRuntimeState m_environment{};
    uint64_t m_environmentRevision = 1ull;
    std::unordered_map<ShaderHandle, ShaderAssetStatus> m_shaderAssets;
    ShaderVariantCache m_variantCache;
    std::unordered_map<MaterialHandle, MaterialGpuState> m_materialStates;
    std::thread::id m_renderThreadId{};

    [[nodiscard]] static ShaderStageMask ToStageMask(assets::ShaderStage stage) noexcept;
    [[nodiscard]] static uint64_t HashBytes(const void* data, size_t size) noexcept;
    [[nodiscard]] const assets::CompiledShaderArtifact* FindCompiledArtifact(const assets::ShaderAsset& shaderAsset) const noexcept;
    [[nodiscard]] static uint64_t HashMaterialState(const std::vector<uint8_t>& cbData,
                                                   const std::vector<ResolvedMaterialBinding>& bindings) noexcept;
    [[nodiscard]] std::vector<ResolvedMaterialBinding> ResolveBindings(const MaterialSystem& materials,
                                                                       MaterialHandle material) const;
    [[nodiscard]] PipelineDesc BuildPipelineDesc(const MaterialSystem& materials,
                                                 MaterialHandle material,
                                                 ShaderHandle gpuVS,
                                                 ShaderHandle gpuPS) const;
    [[nodiscard]] PipelineDesc BuildPipelineDescForPass(const MaterialSystem& materials,
                                                        MaterialHandle material,
                                                        ShaderHandle gpuVS,
                                                        ShaderHandle gpuPS,
                                                        RenderPassTag pass) const;
    [[nodiscard]] PipelineHandle ResolvePipelineForPass(const MaterialSystem& materials,
                                                        MaterialHandle material,
                                                        const MaterialGpuState& state,
                                                        RenderPassTag pass);
    void CreateDefaultSamplers();
    void CreateFallbackTextures();
    [[nodiscard]] bool NeedsMaterialRebuild(const MaterialSystem& materials,
                                            MaterialHandle material,
                                            const MaterialGpuState& state) const noexcept;
    [[nodiscard]] TextureHandle ResolveFallbackTexture(MaterialSemantic semantic) const noexcept;
    void DestroyMaterialState(MaterialGpuState& state);
    [[nodiscard]] bool RequireRenderThread(const char* opName) const noexcept;
};

} // namespace engine::renderer
