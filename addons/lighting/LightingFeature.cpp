#include "addons/lighting/LightingFeature.hpp"

#include "addons/lighting/LightingExtraction.hpp"
#include "addons/lighting/LightingFrameData.hpp"

namespace engine::addons::lighting {
namespace {

class LightingExtractionStep final : public renderer::ISceneExtractionStep
{
public:
    std::string_view GetName() const noexcept override { return "lighting.extract"; }

    void Extract(const renderer::SceneExtractionContext& ctx) const override
    {
        if (ctx.snapshot)
            ExtractLights(ctx.world, *ctx.snapshot);
        else if (ctx.renderWorld)
            ExtractLights(ctx.world, *ctx.renderWorld);
    }
};

class LightingFeature final : public renderer::IEngineFeature
{
public:
    LightingFeature()
        : m_extractionStep(std::make_shared<LightingExtractionStep>())
        , m_frameContributor(CreateLightingFrameConstantsContributor())
    {
    }

    std::string_view GetName() const noexcept override { return "krom-lighting"; }
    renderer::FeatureID GetID() const noexcept override
    {
        return renderer::FeatureID::FromString("krom-lighting");
    }

    void Register(renderer::FeatureRegistrationContext& context) override
    {
        context.RegisterSceneExtractionStep(m_extractionStep);
        context.RegisterFrameConstantsContributor(m_frameContributor);
    }

    bool Initialize(const renderer::FeatureInitializationContext& context) override
    {
        (void)context;
        return true;
    }

    void Shutdown(const renderer::FeatureShutdownContext& context) override
    {
        (void)context;
        m_frameContributor.reset();
        m_extractionStep.reset();
    }

private:
    renderer::SceneExtractionStepPtr m_extractionStep;
    renderer::FrameConstantsContributorPtr m_frameContributor;
};

} // namespace

std::unique_ptr<renderer::IEngineFeature> CreateLightingFeature()
{
    return std::make_unique<LightingFeature>();
}

} // namespace engine::addons::lighting
