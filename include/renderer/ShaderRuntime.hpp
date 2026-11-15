#pragma once
#include "renderer/ShaderCompiler.hpp"
#include "renderer/ShaderVariantCache.hpp"
#include "renderer/runtime/ShaderMaterialSource.hpp"
#include "renderer/Environment.hpp"
#include "renderer/PipelineCache.hpp"
#include "renderer/RenderPassRegistry.hpp"
#include "renderer/ShaderBindingModel.hpp"
#include "renderer/ShaderContract.hpp"
#include "renderer/GpuResourceRuntime.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace engine::jobs { class JobSystem; }

namespace engine::renderer {

struct PerObjectConstants;

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
    // Fehlgeschlagen: nicht jeden Frame neu kompilieren, erst wenn sich die
    // Quelle (sourceHash) oder das Ziel aendert.
    bool failed = false;
    uint64_t sourceHash = 0ull;
};

// Progress of the background shader compiles (see SetAsyncShaderCompileJobs).
// A batch starts with the first queued compile and ends when none is left.
struct ShaderCompileProgress
{
    uint32_t completed = 0u;   // finished in the current batch
    uint32_t total = 0u;       // queued, running and finished in the current batch
    uint32_t running = 0u;     // currently on a worker
    std::string shaderName;    // oldest running compile, e.g. "pbr_forward_plus.ps"
    std::string defines;       // its variant defines, space separated
    [[nodiscard]] bool Active() const noexcept { return total > completed; }
};

struct ResolvedMaterialBinding
{
    enum class Kind : uint8_t { ConstantBuffer, Texture, Sampler, Buffer };
    Kind kind = Kind::ConstantBuffer;
    std::string name;
    uint32_t slot = 0u;
    ShaderStageMask stages = ShaderStageMask::None;
    TextureHandle texture;
    BufferHandle buffer;
    uint32_t samplerIndex = 0u;
};

enum class SceneDepthMode : uint8_t { Material, Prepass, ReadOnly };

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
    // Per-Pass-Pipeline-Cache: erspart BuildPipelineDescForPass + heap-alloc pro Draw.
    // Key: pass id, sample count, extra attachment count and scene depth mode.
    std::unordered_map<uint32_t, PipelineHandle> resolvedPipelines;
    // A shader of this material is still compiling in the background: the
    // material is not drawn and is prepared again once a compile has finished.
    bool waitingForShaders = false;
    uint64_t shaderCompileRevision = 0ull;
};

class ShaderRuntime
{
public:
    ShaderRuntime() = default;
    ~ShaderRuntime() = default;

    bool Initialize(IDevice& device);
    void Shutdown();
    void SetGpuResourceRuntime(GpuResourceRuntime* runtime) noexcept { m_gpuRuntime = runtime; }

    void SetAssetRegistry(assets::AssetRegistry* registry) noexcept { m_assets = registry; }
    [[nodiscard]] assets::AssetRegistry* GetAssetRegistry() const noexcept { return m_assets; }

    void SetPassRegistry(const RenderPassRegistry* registry) noexcept { m_passRegistry = registry; }
    [[nodiscard]] const RenderPassRegistry* GetPassRegistry() const noexcept { return m_passRegistry; }

    [[nodiscard]] IDevice* GetDevice() const noexcept { return m_device; }

    [[nodiscard]] bool CollectShaderRequests(const IShaderMaterialSource& materials,
                                             std::vector<ShaderHandle>& outRequests) const;
    [[nodiscard]] bool CollectMaterialRequests(const IShaderMaterialSource& materials,
                                               std::vector<MaterialHandle>& outRequests) const;

    [[nodiscard]] ShaderHandle PrepareShaderAsset(ShaderHandle shaderAssetHandle);
    [[nodiscard]] bool CommitShaderRequests(const std::vector<ShaderHandle>& requests);
    [[nodiscard]] bool PrepareAllShaderAssets();
    [[nodiscard]] bool ParallelCompileAllShaderAssets(jobs::JobSystem& jobSystem);

    [[nodiscard]] bool PrepareMaterial(const IShaderMaterialSource& materials, MaterialHandle material);
    [[nodiscard]] bool CommitMaterialRequests(const IShaderMaterialSource& materials,
                                              const std::vector<MaterialHandle>& requests);
    [[nodiscard]] bool PrepareAllMaterials(const IShaderMaterialSource& materials);

    [[nodiscard]] const MaterialGpuState* GetMaterialState(MaterialHandle material) const noexcept;
    [[nodiscard]] const ShaderAssetStatus* GetShaderStatus(ShaderHandle shaderAssetHandle) const noexcept;

    // outPending (optional) is set when the variant is compiling in the background.
    [[nodiscard]] ShaderHandle GetOrCreateVariant(ShaderHandle shaderAssetHandle, ShaderPassType pass, ShaderVariantFlag flags,
                                                  bool* outPending = nullptr);

    // Background compilation of shaders that are not in the on-disk cache.
    // With a pool (at least two workers), a cache miss in PrepareShaderAsset or
    // GetOrCreateVariant queues the CPU-side compile (fxc/DXC/GLSL preprocessing,
    // no device calls) as a background job and returns Invalid; materials that
    // need it are skipped until it is done. GPU shader objects are still created
    // on the render thread, in PumpAsyncShaderCompiles. Without a pool (default)
    // every compile runs synchronously as before. The pool belongs to the host.
    void SetAsyncShaderCompileJobs(jobs::JobSystem* jobSystem) noexcept { m_asyncCompileJobs = jobSystem; }
    // Takes finished background compiles over and starts queued ones. Render thread;
    // called by CommitShaderRequests once per frame.
    void PumpAsyncShaderCompiles();
    [[nodiscard]] bool HasPendingShaderCompiles() const noexcept { return !m_asyncCompiles.empty(); }
    [[nodiscard]] bool IsShaderCompilePending(ShaderHandle shaderAssetHandle) const noexcept;
    [[nodiscard]] ShaderCompileProgress GetShaderCompileProgress() const;
    [[nodiscard]] const ShaderVariantCache& GetVariantCache() const noexcept { return m_variantCache; }

    [[nodiscard]] bool BindMaterial(ICommandList& cmd,
                                    const IShaderMaterialSource& materials,
                                    MaterialHandle material,
                                    BufferHandle perFrameCB,
                                    BufferHandle perObjectCB,
                                    BufferHandle perPassCB = BufferHandle::Invalid(),
                                    RenderPassID passOverride = StandardRenderPasses::Opaque());

    [[nodiscard]] bool BindMaterialWithRange(ICommandList& cmd,
                                             const IShaderMaterialSource& materials,
                                             MaterialHandle material,
                                             BufferHandle   perFrameCB,
                                             BufferBinding  perFrameBinding,
                                             BufferBinding  perObjectBinding,
                                             BufferBinding  perPassBinding,
                                             const PerObjectConstants* perObjectConstants = nullptr,
                                             RenderPassID   passOverride = StandardRenderPasses::Opaque(),
                                             SceneDepthMode depthMode = SceneDepthMode::Material);

    // Aktualisiert nur die per-Object-Bindung (Push Constants auf Vulkan, CB-Range sonst).
    // Setzt keine Pipeline, kein Material, keine Texturen — nur für consecutive Draws
    // mit identischem Material aufrufen, nachdem BindMaterialWithRange einmalig aufgerufen wurde.
    void UpdatePerObjectBinding(ICommandList& cmd,
                                BufferBinding perObjectBinding,
                                const PerObjectConstants* perObjectConstants);

    [[nodiscard]] bool ValidateMaterial(const IShaderMaterialSource& materials,
                                        MaterialHandle material,
                                        std::vector<ShaderValidationIssue>& outIssues) const;

    void SetEnvironmentState(const EnvironmentRuntimeState& state) noexcept;
    // Sample count of the render target the next material draws go to. Material
    // pipelines are created per sample count. The pipeline that owns a
    // multisampled target sets it around those draws and restores 1 afterwards.
    // The shadow pass always uses 1.
    [[nodiscard]] TextureHandle GetWhiteFallbackTexture() const noexcept { return m_fallbackTextures.white; }
    void SetTargetSampleCount(uint32_t sampleCount) noexcept { m_targetSampleCount = sampleCount > 1u ? sampleCount : 1u; }
    [[nodiscard]] uint32_t GetTargetSampleCount() const noexcept { return m_targetSampleCount; }
    // Extra colour targets (MRT) of the pass the next material draws go to.
    // Material pipelines are created per layout: fragment shaders that support
    // KROM_SSR_OUTPUT get that variant and write the targets, all others leave
    // them untouched (write mask 0). The pass that binds them sets the formats
    // around its draws and clears them afterwards (count 0).
    void SetTargetExtraColorFormats(const Format* formats, uint32_t count) noexcept
    {
        m_targetExtraColorCount = std::min(count, kMaxExtraColorTargets);
        m_targetExtraColorFormats = {};
        for (uint32_t i = 0u; i < m_targetExtraColorCount; ++i)
            m_targetExtraColorFormats[i] = formats[i];
    }
    [[nodiscard]] uint32_t GetTargetExtraColorCount() const noexcept { return m_targetExtraColorCount; }
    [[nodiscard]] const EnvironmentRuntimeState& GetEnvironmentState() const noexcept { return m_environment; }
    [[nodiscard]] bool HasIBL() const noexcept { return m_environment.active; }

    // Optionaler Hardware-Tiefenbias fuer den Shadow-Pass.
    // Die kanonische Shadow-Bias-Semantik der Engine lebt im gemeinsamen
    // Shaderpfad (FrameConstants / HLSL). Hardware-Bias sollte nur gesetzt
    // werden, wenn dafuer eine explizite, backend-uebersetzte Engine-Semantik
    // existiert.
    void SetShadowDepthBias(float constantFactor, float slopeFactor) noexcept
    {
        m_shadowDepthBias      = constantFactor;
        m_shadowSlopeBias      = slopeFactor;
    }

    [[nodiscard]] size_t PreparedShaderCount() const noexcept { return m_shaderAssets.size(); }
    [[nodiscard]] size_t PreparedMaterialCount() const noexcept { return m_materialStates.size(); }
    [[nodiscard]] bool IsRenderThread() const noexcept;
    // True when the fragment shader asset declares the KROM_SSR_OUTPUT outputs.
    [[nodiscard]] bool SupportsSsrOutput(ShaderHandle fragmentShaderAsset) const;

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
    GpuResourceRuntime* m_gpuRuntime = nullptr;
    assets::AssetRegistry* m_assets = nullptr;
    const RenderPassRegistry* m_passRegistry = nullptr;
    float    m_shadowDepthBias = 0.f;
    float    m_shadowSlopeBias = 0.f;
    PipelineCache m_pipelineCache;
    RuntimeSamplerSet m_samplers{};
    RuntimeFallbackTextures m_fallbackTextures{};
    EnvironmentRuntimeState m_environment{};
    uint32_t m_targetSampleCount = 1u;
    std::array<Format, kMaxExtraColorTargets> m_targetExtraColorFormats{};
    uint32_t m_targetExtraColorCount = 0u;
    // Fragment shader assets whose source handles KROM_SSR_OUTPUT.
    mutable std::unordered_map<ShaderHandle, bool> m_ssrOutputSupport;
    uint64_t m_environmentRevision = 1ull;
    std::unordered_map<ShaderHandle, ShaderAssetStatus> m_shaderAssets;
    ShaderVariantCache m_variantCache;
    std::unordered_map<MaterialHandle, MaterialGpuState> m_materialStates;
    // Uebersprungene Materialien: Revision der letzten Warnung (nur einmal loggen).
    std::unordered_map<MaterialHandle, uint64_t> m_skippedMaterialWarnings;
    std::thread::id m_renderThreadId{};

    // Background compiles. The job owns its copy of the asset and its result,
    // so a worker never touches the registry or this runtime.
    struct AsyncCompileJob
    {
        assets::ShaderAsset asset;
        assets::ShaderTargetProfile target = assets::ShaderTargetProfile::Generic;
        ShaderVariantFlag flags = ShaderVariantFlag::None;
        assets::CompiledShaderArtifact artifact;
        std::string error;
        bool ok = false;
        std::atomic<bool> done{ false };
    };
    struct AsyncCompileEntry
    {
        ShaderVariantKey key{};     // normalized
        bool baseAsset = false;     // PrepareShaderAsset artifact, not a variant
        uint64_t sourceHash = 0ull; // base only: discard the result if the source changed
        bool dispatched = false;
        std::string shaderName;
        std::string defines;
        std::shared_ptr<AsyncCompileJob> job;
    };
    jobs::JobSystem* m_asyncCompileJobs = nullptr;
    std::vector<AsyncCompileEntry> m_asyncCompiles;   // FIFO: queued and running
    std::unordered_set<ShaderVariantKey> m_failedAsyncVariants;
    uint64_t m_asyncCompileRevision = 0ull;
    uint32_t m_asyncBatchCompleted = 0u;
    uint32_t m_asyncBatchTotal = 0u;

    [[nodiscard]] bool AsyncShaderCompilesEnabled() const noexcept;
    [[nodiscard]] const AsyncCompileEntry* FindAsyncCompile(const ShaderVariantKey& key, bool baseAsset) const noexcept;
    void QueueAsyncCompile(const assets::ShaderAsset& asset, assets::ShaderTargetProfile target,
                           const ShaderVariantKey& key, bool baseAsset, uint64_t sourceHash);
    void DispatchQueuedAsyncCompiles();
    void ApplyAsyncCompile(AsyncCompileEntry& entry);
    [[nodiscard]] static uint64_t ShaderSourceHash(const assets::ShaderAsset& asset) noexcept;

    [[nodiscard]] static ShaderStageMask ToStageMask(assets::ShaderStage stage) noexcept;
    [[nodiscard]] static uint64_t HashBytes(const void* data, size_t size) noexcept;
    [[nodiscard]] const assets::CompiledShaderArtifact* FindCompiledArtifact(const assets::ShaderAsset& shaderAsset) const noexcept;
    [[nodiscard]] static uint64_t HashMaterialState(const std::vector<uint8_t>& cbData,
                                                    const std::vector<ResolvedMaterialBinding>& bindings) noexcept;
    [[nodiscard]] std::vector<ResolvedMaterialBinding> ResolveBindings(const IShaderMaterialSource& materials,
                                                                       MaterialHandle material) const;
    [[nodiscard]] PipelineDesc BuildPipelineDesc(const IShaderMaterialSource& materials,
                                                 MaterialHandle material,
                                                 ShaderHandle gpuVS,
                                                 ShaderHandle gpuPS) const;
    [[nodiscard]] PipelineDesc BuildPipelineDescForPass(const IShaderMaterialSource& materials,
                                                        MaterialHandle material,
                                                        ShaderHandle gpuVS,
                                                        ShaderHandle gpuPS,
                                                        RenderPassID pass) const;
    [[nodiscard]] PipelineHandle ResolvePipelineForPass(const IShaderMaterialSource& materials,
                                                        MaterialHandle material,
                                                        MaterialGpuState& state,
                                                        RenderPassID pass,
                                                        SceneDepthMode depthMode = SceneDepthMode::Material);
    void CreateDefaultSamplers();
    void CreateFallbackTextures();
    [[nodiscard]] bool NeedsMaterialRebuild(const IShaderMaterialSource& materials,
                                            MaterialHandle material,
                                            const MaterialGpuState& state) const noexcept;
    void RetireMaterialState(MaterialGpuState& state);
    void DestroyMaterialState(MaterialGpuState& state);
    [[nodiscard]] bool RequireRenderThread(const char* opName) const noexcept;
};

} // namespace engine::renderer
