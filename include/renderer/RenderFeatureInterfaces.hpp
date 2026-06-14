#pragma once

#include "renderer/FeatureID.hpp"
#include "renderer/RenderPipelineFeatureTypes.hpp"
#include <memory>
#include <string_view>
#include <vector>

namespace engine::events { class EventBus; }

namespace engine::renderer {

class FeatureRegistry;
class IDevice;
class ShaderRuntime;

class IExtractionFeature;
class IFrameConstantsContributor;
class IFramePass;

using ISceneExtractionStep = IExtractionFeature;
using IPassContributor = IFramePass;

using SceneExtractionStepPtr = std::shared_ptr<const ISceneExtractionStep>;
using FrameConstantsContributorPtr = std::shared_ptr<const IFrameConstantsContributor>;
using PassContributorPtr = std::shared_ptr<IPassContributor>;

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

class IRenderFeature
{
public:
    struct RuntimeRegistrationOwnerToken final {};

    virtual ~IRenderFeature() = default;
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

using IEngineFeature = IRenderFeature;

struct FeatureRegistrationContext
{
    FeatureRegistrationContext(FeatureRegistry& ownerRegistry,
                               const IEngineFeature& ownerFeature) noexcept
        : registry(ownerRegistry), owner(ownerFeature) {}

    bool RegisterSceneExtractionStep(SceneExtractionStepPtr step) const noexcept;
    bool RegisterFrameConstantsContributor(FrameConstantsContributorPtr contributor) const noexcept;
    bool RegisterRenderPipeline(RenderPipelinePtr pipeline, bool makeDefault = false) const noexcept;
    bool RegisterPassContributor(PassContributorPtr contributor) const noexcept;

    [[nodiscard]] bool HadError() const noexcept { return m_hadError; }

    FeatureRegistry& registry;
    const IEngineFeature& owner;

private:
    mutable bool m_hadError = false;
};

} // namespace engine::renderer
