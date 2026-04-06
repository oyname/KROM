#pragma once

#include "ecs/World.hpp"
#include "events/EventBus.hpp"
#include "renderer/FeatureID.hpp"
#include "renderer/IDevice.hpp"
#include "renderer/MaterialSystem.hpp"
#include "renderer/SceneSnapshot.hpp"
#include "renderer/ShaderRuntime.hpp"
#include "rendergraph/FramePipeline.hpp"
#include "rendergraph/RenderGraph.hpp"
#include <limits>
#include <memory>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace engine::renderer {

struct RenderQueue;

class ISceneExtractionStep
{
public:
    virtual ~ISceneExtractionStep() = default;
    virtual std::string_view GetName() const noexcept = 0;
    virtual void Extract(const ecs::World& world, SceneSnapshot& snapshot) const = 0;
};

using SceneExtractionStepPtr = std::shared_ptr<const ISceneExtractionStep>;

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
    const rendergraph::FramePipelineCallbacks& externalCallbacks;
};

struct RenderPipelineBuildResult
{
    rendergraph::FramePipelineResources resources{};
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
    void RegisterRenderPipeline(const IEngineFeature& owner, RenderPipelinePtr pipeline, bool makeDefault) noexcept;

    [[nodiscard]] const std::vector<const ISceneExtractionStep*>& GetSceneExtractionSteps() const noexcept;
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

    [[nodiscard]] bool TopologicallySorted(std::vector<IEngineFeature*>& outSorted) const;
    [[nodiscard]] bool IsKnownFeature(const IEngineFeature& feature) const noexcept;
    [[nodiscard]] std::string_view GetRegisteredFeatureName(FeatureID id) const noexcept;
    void RefreshSceneExtractionStepViews() const noexcept;
    void RefreshRenderPipelineViews() const noexcept;

    std::vector<std::unique_ptr<IEngineFeature>> m_features;
    std::unordered_map<FeatureID, IEngineFeature*> m_featuresById;
    std::unordered_map<FeatureID, std::string_view> m_featureNamesById;

    mutable std::vector<RegisteredExtractionStep> m_registeredSceneExtractionSteps;
    mutable std::vector<const ISceneExtractionStep*> m_sceneExtractionSteps;
    mutable std::vector<RegisteredRenderPipeline> m_registeredRenderPipelines;
    mutable std::vector<const IRenderPipeline*> m_renderPipelines;
    mutable const IRenderPipeline* m_activeRenderPipeline = nullptr;
    mutable size_t m_activeRenderPipelineIndex = kInvalidRegistrationIndex;
    std::vector<IEngineFeature*> m_initOrder;
    bool m_initialized = false;
};

} // namespace engine::renderer
