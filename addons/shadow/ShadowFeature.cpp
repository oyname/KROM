#include "addons/shadow/ShadowFeature.hpp"

#include "addons/lighting/LightingFrameData.hpp"
#include "addons/shadow/ShadowExtraction.hpp"
#include "addons/shadow/ShadowFrameData.hpp"
#include "renderer/FeatureID.hpp"
#include "renderer/RenderWorld.hpp"
#include <cstring>

namespace {

void FillMatrixRowMajor(const engine::math::Mat4& m, float out[16]) noexcept
{
    std::memcpy(out, m.Data(), sizeof(float) * 16u);
}

struct ShadowHardwareDepthBias
{
    float constantFactor = 0.0f;
    float slopeFactor = 0.0f;
};

[[nodiscard]] constexpr ShadowHardwareDepthBias GetShadowHardwareDepthBias() noexcept
{
    // Gemeinsamer Shadow-Vertrag:
    // - die fachliche Bias-Wahrheit lebt in ShadowSettings::bias / normalBias
    //   und wird im gemeinsamen HLSL-Pfad ausgewertet
    // - die Hardware-Rasterizer-Bias-Semantik ist zwischen DX11/Vulkan/OpenGL
    //   nicht identisch und war hier zuletzt backend-spezifisch verbogen
    // - fuer konsistentes Cross-Backend-Verhalten schalten wir den globalen
    //   Hardware-Bias deshalb standardmaessig ab
    //
    // Spaeter kann hier wieder ein kanonischer Engine-Vertrag eingefuehrt
    // werden, aber erst dann mit expliziter Uebersetzung pro Backend.
    return {};
}

} // namespace

namespace engine::addons::shadow {
namespace {

class ShadowExtractionStep final : public renderer::ISceneExtractionStep
{
public:
    std::string_view GetName() const noexcept override { return "shadow.extract"; }

    void Extract(const ecs::World& world, renderer::RenderWorld& renderWorld) const override
    {
        ExtractShadow(world, renderWorld);
    }
};

class ShadowFrameConstantsContributor final : public renderer::IFrameConstantsContributor
{
public:
    std::string_view GetName() const noexcept override { return "shadow.frame_constants"; }

    void Contribute(const renderer::FrameConstantsContributionContext& context,
                    renderer::FrameConstants& fc) const override
    {
        const ShadowFrameData* shadow = context.renderWorld.GetFeatureData<ShadowFrameData>();
        const ShadowRequest* request = shadow ? shadow->GetSelectedRequest() : nullptr;
        const ShadowView* selectedView = shadow ? shadow->GetSelectedView() : nullptr;
        if (!request || !selectedView)
        {
            fc.shadowCascadeCount = 0u;
            fc.shadowBias         = 0.f;
            fc.shadowNormalBias   = 0.f;
            fc.shadowStrength     = 1.f;
            fc.shadowTexelSize    = 0.f;
            std::memset(fc.featurePayload + lighting::kShadowVPOffset,
                        0, lighting::kShadowVPBytes);
            return;
        }

        fc.shadowCascadeCount = 1u;
        fc.shadowBias         = request->settings.bias;
        fc.shadowNormalBias   = request->settings.normalBias;
        fc.shadowStrength     = request->settings.strength;
        fc.shadowTexelSize    = 1.f / static_cast<float>(std::max(1u, request->settings.resolution));

        const math::Mat4 adjustedShadowVP = context.shadowClipSpaceAdjustment * selectedView->viewProj;
        FillMatrixRowMajor(adjustedShadowVP,
                           reinterpret_cast<float*>(fc.featurePayload + lighting::kShadowVPOffset));
    }
};

class ShadowFeature final : public renderer::IEngineFeature
{
public:
    ShadowFeature()
        : m_extractionStep(std::make_shared<ShadowExtractionStep>())
        , m_frameContributor(std::make_shared<ShadowFrameConstantsContributor>())
    {
    }

    std::string_view GetName() const noexcept override { return "krom-shadow"; }

    renderer::FeatureID GetID() const noexcept override
    {
        return renderer::FeatureID::FromString("krom-shadow");
    }

    std::vector<renderer::FeatureID> GetDependencies() const noexcept override
    {
        return { renderer::FeatureID::FromString("krom-lighting") };
    }

    void Register(renderer::FeatureRegistrationContext& context) override
    {
        context.RegisterSceneExtractionStep(m_extractionStep);
        context.RegisterFrameConstantsContributor(m_frameContributor);
    }

    bool Initialize(const renderer::FeatureInitializationContext& context) override
    {
        const ShadowHardwareDepthBias bias = GetShadowHardwareDepthBias();
        context.shaderRuntime.SetShadowDepthBias(bias.constantFactor, bias.slopeFactor);
        return true;
    }

    void Shutdown(const renderer::FeatureShutdownContext& context) override
    {
        (void)context;
    }

private:
    renderer::SceneExtractionStepPtr       m_extractionStep;
    renderer::FrameConstantsContributorPtr m_frameContributor;
};

} // namespace

std::unique_ptr<renderer::IEngineFeature> CreateShadowFeature()
{
    return std::make_unique<ShadowFeature>();
}

} // namespace engine::addons::shadow
