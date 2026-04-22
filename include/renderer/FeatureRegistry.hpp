#pragma once

#include "ecs/World.hpp"
#include "events/EventBus.hpp"
#include "renderer/FeatureID.hpp"
#include "renderer/IDevice.hpp"
#include "renderer/MaterialSystem.hpp"
#include "renderer/RenderPipelineTypes.hpp"
#include "renderer/RenderWorld.hpp"
#include "renderer/ShaderRuntime.hpp"
#include "rendergraph/RenderGraph.hpp"
#include <limits>
#include <memory>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace engine::platform {
class IPlatformTiming;
}

namespace engine::renderer {

struct RenderQueue;
struct FrameConstants;
struct RenderView;

class ISceneExtractionStep
{
public:
    virtual ~ISceneExtractionStep() = default;
    virtual std::string_view GetName() const noexcept = 0;
    virtual void Extract(const ecs::World& world, RenderWorld& renderWorld) const = 0;
};

using SceneExtractionStepPtr = std::shared_ptr<const ISceneExtractionStep>;

struct FrameConstantsContributionContext
{
    math::Mat4 projectionClipSpaceAdjustment = math::Mat4::Identity();
    math::Mat4 shadowClipSpaceAdjustment = math::Mat4::Identity();
    uint32_t viewportWidth = 0u;
    uint32_t viewportHeight = 0u;
    const RenderView& view;
    const platform::IPlatformTiming& timing;
    const RenderWorld& renderWorld;

    FrameConstantsContributionContext(const math::Mat4& projectionAdjustment,
                                      const math::Mat4& shadowAdjustment,
                                      uint32_t viewportWidthIn,
                                      uint32_t viewportHeightIn,
                                      const RenderView& viewIn,
                                      const platform::IPlatformTiming& timingIn,
                                      const RenderWorld& renderWorldIn)
        : projectionClipSpaceAdjustment(projectionAdjustment)
        , shadowClipSpaceAdjustment(shadowAdjustment)
        , viewportWidth(viewportWidthIn)
        , viewportHeight(viewportHeightIn)
        , view(viewIn)
        , timing(timingIn)
        , renderWorld(renderWorldIn)
    {
    }
};

class IFrameConstantsContributor
{
public:
    virtual ~IFrameConstantsContributor() = default;
    virtual std::string_view GetName() const noexcept = 0;
    virtual void Contribute(const FrameConstantsContributionContext& context,
                            FrameConstants& frameConstants) const = 0;
};

using FrameConstantsContributorPtr = std::shared_ptr<const IFrameConstantsContributor>;

struct FrameGraphRuntimeBindings
{
    const RenderQueue*  renderQueue           = nullptr;
    ShaderRuntime*      shaderRuntime         = nullptr;
    const MaterialSystem* materials           = nullptr;
    BufferHandle        perFrameCB;
    BufferHandle        perObjectArena;
    uint32_t            perObjectStride       = 0u;
    MaterialHandle      defaultTonemapMaterial;
    const MaterialSystem* tonemapMaterialSystem = nullptr;
    events::EventBus*   eventBus              = nullptr;
    FramePipelineCallbacks externalCallbacks;
};

struct RenderPipelineBuildContext
{
    rendergraph::RenderGraph& renderGraph;
    uint32_t viewportWidth = 0u;
    uint32_t viewportHeight = 0u;
    RenderTargetHandle backbufferRT;
    TextureHandle backbufferTex;
    const RenderQueue& renderQueue;
    ShaderRuntime& shaderRuntime;
    const MaterialSystem& materials;
    BufferHandle perFrameCB;
    MaterialHandle defaultTonemapMaterial;
    const MaterialSystem* tonemapMaterialSystem = nullptr;
    events::EventBus* eventBus = nullptr;
    const FramePipelineCallbacks& externalCallbacks;
    std::shared_ptr<FrameGraphRuntimeBindings> runtimeBindings;
};

struct RenderPipelineBuildResult
{
    rendergraph::RGResourceID backbuffer = rendergraph::RG_INVALID_RESOURCE;
};

class IRenderPipeline
{
public:
    virtual ~IRenderPipeline() = default;
    virtual std::string_view GetName() const noexcept = 0;
    virtual bool Build(const RenderPipelineBuildContext& context,
                       RenderPipelineBuildResult& result) const = 0;
};

using RenderPipelinePtr = std::shared_ptr<const IRenderPipeline>;

struct FeatureRegistrationContext;
struct FeatureInitializationContext
{
    IDevice& device;
    ShaderRuntime& shaderRuntime;
    events::EventBus* eventBus = nullptr;
};

struct FeatureShutdownContext
{
    events::EventBus* eventBus = nullptr;
};

class IEngineFeature
{
public:
    struct RuntimeRegistrationOwnerToken final {};

    virtual ~IEngineFeature() = default;
    virtual std::string_view GetName() const noexcept = 0;
    virtual FeatureID GetID() const noexcept = 0;
    virtual std::vector<FeatureID> GetDependencies() const noexcept { return {}; }
    virtual void Register(FeatureRegistrationContext& context) = 0;
    virtual bool Initialize(const FeatureInitializationContext& context) = 0;
    virtual void Shutdown(const FeatureShutdownContext& context) = 0;

    [[nodiscard]] std::weak_ptr<const RuntimeRegistrationOwnerToken> GetRuntimeRegistrationOwnerToken() const noexcept
    {
        return m_runtimeRegistrationOwnerToken;
    }

private:
    std::shared_ptr<RuntimeRegistrationOwnerToken> m_runtimeRegistrationOwnerToken =
        std::make_shared<RuntimeRegistrationOwnerToken>();
};

class FeatureRegistry;

struct FeatureRegistrationContext
{
    FeatureRegistrationContext(FeatureRegistry& ownerRegistry,
                               const IEngineFeature& ownerFeature) noexcept
        : registry(ownerRegistry), owner(ownerFeature) {}

    void RegisterSceneExtractionStep(SceneExtractionStepPtr step) const noexcept;
    void RegisterFrameConstantsContributor(FrameConstantsContributorPtr contributor) const noexcept;
    void RegisterRenderPipeline(RenderPipelinePtr pipeline, bool makeDefault = false) const noexcept;

    FeatureRegistry& registry;
    const IEngineFeature& owner;
};

class FeatureRegistry
{
public:
    bool AddFeature(std::unique_ptr<IEngineFeature> feature);
    bool InitializeAll(const FeatureInitializationContext& context);
    void ShutdownAll(const FeatureShutdownContext& context) noexcept;

    void RegisterSceneExtractionStep(const IEngineFeature& owner, SceneExtractionStepPtr step) noexcept;
    void RegisterFrameConstantsContributor(const IEngineFeature& owner, FrameConstantsContributorPtr contributor) noexcept;
    void RegisterRenderPipeline(const IEngineFeature& owner, RenderPipelinePtr pipeline, bool makeDefault) noexcept;

    [[nodiscard]] const std::vector<const ISceneExtractionStep*>& GetSceneExtractionSteps() const noexcept;
    [[nodiscard]] const std::vector<const IFrameConstantsContributor*>& GetFrameConstantsContributors() const noexcept;
    [[nodiscard]] const IRenderPipeline* GetActiveRenderPipeline() const noexcept;

    [[nodiscard]] const std::vector<std::unique_ptr<IEngineFeature>>& GetFeatures() const noexcept
    { return m_features; }

    void ClearRegistrations() noexcept;

private:
    static constexpr size_t kInvalidRegistrationIndex = std::numeric_limits<size_t>::max();

    struct RegisteredExtractionStep
    {
        SceneExtractionStepPtr step;
        std::weak_ptr<const IEngineFeature::RuntimeRegistrationOwnerToken> ownerToken;
        const IEngineFeature* owner = nullptr;
    };

    struct RegisteredRenderPipeline
    {
        RenderPipelinePtr pipeline;
        std::weak_ptr<const IEngineFeature::RuntimeRegistrationOwnerToken> ownerToken;
        const IEngineFeature* owner = nullptr;
    };

    struct RegisteredFrameConstantsContributor
    {
        FrameConstantsContributorPtr contributor;
        std::weak_ptr<const IEngineFeature::RuntimeRegistrationOwnerToken> ownerToken;
        const IEngineFeature* owner = nullptr;
    };

    [[nodiscard]] bool TopologicallySorted(std::vector<IEngineFeature*>& outSorted) const;
    [[nodiscard]] bool IsKnownFeature(const IEngineFeature& feature) const noexcept;
    [[nodiscard]] std::string_view GetRegisteredFeatureName(FeatureID id) const noexcept;
    void RefreshSceneExtractionStepViews() const noexcept;
    void RefreshFrameConstantsContributorViews() const noexcept;
    void RefreshRenderPipelineViews() const noexcept;

    std::vector<std::unique_ptr<IEngineFeature>> m_features;
    std::unordered_map<FeatureID, IEngineFeature*> m_featuresById;
    std::unordered_map<FeatureID, std::string_view> m_featureNamesById;

    mutable std::vector<RegisteredExtractionStep> m_registeredSceneExtractionSteps;
    mutable std::vector<const ISceneExtractionStep*> m_sceneExtractionSteps;
    mutable std::vector<RegisteredFrameConstantsContributor> m_registeredFrameConstantsContributors;
    mutable std::vector<const IFrameConstantsContributor*> m_frameConstantsContributors;
    mutable std::vector<RegisteredRenderPipeline> m_registeredRenderPipelines;
    mutable std::vector<const IRenderPipeline*> m_renderPipelines;
    mutable const IRenderPipeline* m_activeRenderPipeline = nullptr;
    mutable size_t m_activeRenderPipelineIndex = kInvalidRegistrationIndex;
    std::vector<IEngineFeature*> m_initOrder;
    bool m_initialized = false;
};

} // namespace engine::renderer
