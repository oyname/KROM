#include "GtaoFeature.hpp"
#include "GtaoPass.hpp"
#include "renderer/RenderFeatureInterfaces.hpp"
#include "renderer/RenderExtractionContext.hpp"
#include "renderer/RenderFrameConstants.hpp"
#include "renderer/RenderFrameTypes.hpp"
#include "renderer/IDevice.hpp"
#include "assets/AssetRegistry.hpp"
#include "core/Debug.hpp"
#include <memory>

namespace engine::renderer::addons::gtao {

namespace {

// Writes GTAO params into its claimed FrameConstants::featureParams slot
// (kGtaoFrameParameterBlock); shaders read it from b0 as featureParams[0].
class GtaoFrameConstantsContributor final : public renderer::IFrameConstantsContributor
{
public:
    explicit GtaoFrameConstantsContributor(std::shared_ptr<GtaoGpuResources> resources)
        : m_resources(std::move(resources))
    {}

    std::string_view GetName() const noexcept override { return "krom-gtao-constants"; }

    void Contribute(const renderer::FrameConstantsContributionContext& ctx,
                    renderer::FrameConstants& fc) const override
    {
        if (!m_resources) return;
        const GtaoSettings& s = m_resources->settings;
        GtaoFrameParams params{};
        params.radius    = s.radius;
        params.bias      = s.bias;
        params.intensity = s.intensity;

        // Earliest point per frame with both a device and the viewport size:
        // (re)create the persistent AO history target so GtaoPass::BuildPass can
        // import it as a cross-frame history buffer (Option B). Must run before
        // gtaoEnabled is decided: a resize invalidates the history.
        const bool viewAllowsGtao = ctx.view.enableAmbientOcclusion;
        if (s.enabled && viewAllowsGtao && m_resources->initialized && ctx.device)
            EnsureGtaoHistoryTarget(*ctx.device, *m_resources, ctx.viewportWidth, ctx.viewportHeight);

        // Gate the shader-side sampling on the same condition GtaoPass uses to
        // publish the AO texture (aoHistoryValid). Otherwise the lit shader would
        // sample an unbound/stale t8 on the first frame and right after a resize.
        const bool sampleHistory = s.enabled && viewAllowsGtao && m_resources->aoHistoryValid;
        params.enabled = sampleHistory ? kGtaoFlagEnabled : 0u;

        // Reprojection: hand the shaders the view-projection the history was
        // rendered with, then remember this frame's for the next one. Without a
        // valid history matrix the shaders fall back to the same screen position.
        if (sampleHistory && s.reprojectHistory && m_resources->historyViewProjValid)
        {
            params.enabled |= kGtaoFlagReproject;
            renderer::WriteFrameParameterBlock(fc, kGtaoReprojectionFrameParameterBlock,
                                               m_resources->historyViewProj);
        }
        if (s.enabled && viewAllowsGtao)
        {
            m_resources->pendingViewProj = MakeGtaoReprojectionParams(fc.viewProjMatrix);
            m_resources->pendingViewProjValid = true;
        }
        renderer::WriteFrameParameterBlock(fc, kGtaoFrameParameterBlock, params);
    }

private:
    std::shared_ptr<GtaoGpuResources> m_resources;
};

class GtaoFeature final : public IEngineFeature
{
public:
    GtaoFeature()
        : m_resources(std::make_shared<GtaoGpuResources>())
        , m_pass(std::make_shared<GtaoPass>(0xA07Au, m_resources))
        , m_contributor(std::make_shared<GtaoFrameConstantsContributor>(m_resources))
    {}

    std::string_view GetName() const noexcept override { return "krom-gtao"; }
    FeatureID GetID() const noexcept override { return FeatureID::FromString("krom-gtao"); }

    void Register(FeatureRegistrationContext& context) override
    {
        context.RegisterPassContributor(m_pass);
        if (context.RegisterFrameParameterBlock(kGtaoFrameParameterBlock) &&
            context.RegisterFrameParameterBlock(kGtaoReprojectionFrameParameterBlock))
            context.RegisterFrameConstantsContributor(m_contributor);
    }

    bool Initialize(const FeatureInitializationContext& context) override
    {
        if (context.device.GetShaderTargetProfile() ==
            assets::ShaderTargetProfile::OpenGL_GLSL450)
        {
            Debug::LogWarning("GtaoFeature: GTAO not supported on OpenGL, disabled");
            return true;  // Not a fatal error; pass stays as no-op
        }
        m_device = &context.device;
        return InitializeGtaoResources(context.device, *m_resources);
    }

    void Shutdown(const FeatureShutdownContext& /*context*/) override
    {
        if (m_device && m_resources && m_resources->initialized)
            DestroyGtaoResources(*m_device, *m_resources);
        m_pass.reset();
        m_resources.reset();
        m_device = nullptr;
    }

    std::shared_ptr<GtaoGpuResources> GetResources() const noexcept { return m_resources; }

private:
    IDevice*                          m_device = nullptr;
    std::shared_ptr<GtaoGpuResources> m_resources;
    std::shared_ptr<GtaoPass>         m_pass;
    std::shared_ptr<GtaoFrameConstantsContributor> m_contributor;
};

} // namespace

std::unique_ptr<IEngineFeature> CreateGtaoFeature(std::shared_ptr<GtaoGpuResources>* outResources)
{
    auto feature = std::make_unique<GtaoFeature>();
    if (outResources)
        *outResources = feature->GetResources();
    return feature;
}

} // namespace engine::renderer::addons::gtao
