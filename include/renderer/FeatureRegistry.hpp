#pragma once

#include "renderer/RenderExtractionContext.hpp"
#include "renderer/RenderFeatureInterfaces.hpp"
#include "renderer/RenderFramePassInterfaces.hpp"
#include "renderer/RenderPipelineFeatureTypes.hpp"
#include <limits>
#include <memory>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace engine::renderer {

class FeatureRegistry
{
public:
    bool AddFeature(std::unique_ptr<IEngineFeature> feature);
    bool InitializeAll(const FeatureInitializationContext& context);
    void ShutdownAll(const FeatureShutdownContext& context) noexcept;

    bool RegisterSceneExtractionStep(const IEngineFeature& owner, SceneExtractionStepPtr step) noexcept;
    bool RegisterFrameConstantsContributor(const IEngineFeature& owner, FrameConstantsContributorPtr contributor) noexcept;
    bool RegisterRenderPipeline(const IEngineFeature& owner, RenderPipelinePtr pipeline, bool makeDefault) noexcept;
    bool RegisterPassContributor(const IEngineFeature& owner, PassContributorPtr contributor) noexcept;

    [[nodiscard]] const std::vector<const ISceneExtractionStep*>& GetSceneExtractionSteps() const noexcept;
    [[nodiscard]] const std::vector<const IFrameConstantsContributor*>& GetFrameConstantsContributors() const noexcept;
    [[nodiscard]] const IRenderPipeline* GetActiveRenderPipeline() const noexcept;
    [[nodiscard]] const std::vector<const IPassContributor*>& GetPassContributors() const noexcept;

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

    struct RegisteredPassContributor
    {
        PassContributorPtr contributor;
        std::weak_ptr<const IEngineFeature::RuntimeRegistrationOwnerToken> ownerToken;
        const IEngineFeature* owner = nullptr;
    };

    [[nodiscard]] bool TopologicallySorted(std::vector<IEngineFeature*>& outSorted) const;
    [[nodiscard]] bool IsKnownFeature(const IEngineFeature& feature) const noexcept;
    [[nodiscard]] std::string_view GetRegisteredFeatureName(FeatureID id) const noexcept;
    void RefreshSceneExtractionStepViews() const noexcept;
    void RefreshFrameConstantsContributorViews() const noexcept;
    void RefreshRenderPipelineViews() const noexcept;
    void RefreshPassContributorViews() const noexcept;

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
    mutable std::vector<RegisteredPassContributor> m_registeredPassContributors;
    mutable std::vector<const IPassContributor*> m_passContributors;
    mutable size_t m_activeRenderPipelineIndex = kInvalidRegistrationIndex;
    std::vector<IEngineFeature*> m_initOrder;
    bool m_initialized = false;
};

} // namespace engine::renderer
