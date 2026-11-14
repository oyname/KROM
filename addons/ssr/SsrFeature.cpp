#include "SsrFeature.hpp"
#include "SsrPass.hpp"
#include "renderer/RenderFeatureInterfaces.hpp"
#include "renderer/RenderExtractionContext.hpp"
#include "renderer/RenderFrameTypes.hpp"
#include "renderer/RenderFrameConstants.hpp"
#include "renderer/IDevice.hpp"
#include "core/Debug.hpp"
#include <algorithm>
#include <memory>

namespace engine::renderer::addons::ssr {

namespace {

class SsrFrameConstantsContributor final : public renderer::IFrameConstantsContributor
{
public:
    explicit SsrFrameConstantsContributor(std::shared_ptr<SsrGpuResources> resources)
        : m_resources(std::move(resources))
    {}

    std::string_view GetName() const noexcept override { return "krom-ssr-constants"; }

    void Contribute(const renderer::FrameConstantsContributionContext& /*ctx*/,
                    renderer::FrameConstants& fc) const override
    {
        if (!m_resources)
            return;
        const SsrSettings& s = m_resources->settings;
        SsrFrameParams params{};
        params.maxDistance  = std::max(s.maxDistance, 0.1f);
        params.thickness    = std::max(s.thickness, 0.001f);
        params.maxRoughness = std::clamp(s.maxRoughness, 0.01f, 1.0f);
        params.intensity    = std::clamp(s.intensity, 0.0f, 1.0f);
        renderer::WriteFrameParameterBlock(fc, kSsrFrameParameterBlock, params);
    }

private:
    std::shared_ptr<SsrGpuResources> m_resources;
};

class SsrFeature final : public IEngineFeature
{
public:
    SsrFeature()
        : m_resources(std::make_shared<SsrGpuResources>())
        , m_pass(std::make_shared<SsrPass>(0x55A0u, m_resources))
        , m_contributor(std::make_shared<SsrFrameConstantsContributor>(m_resources))
    {}

    std::string_view GetName() const noexcept override { return "krom-ssr"; }
    FeatureID GetID() const noexcept override { return FeatureID::FromString("krom-ssr"); }

    void Register(FeatureRegistrationContext& context) override
    {
        context.RegisterPassContributor(m_pass);
        if (context.RegisterFrameParameterBlock(kSsrFrameParameterBlock))
            context.RegisterFrameConstantsContributor(m_contributor);
    }

    bool Initialize(const FeatureInitializationContext& context) override
    {
        // The opaque pass hands SSR its inputs as extra colour targets.
        if (!context.device.SupportsFeature("mrt"))
        {
            Debug::LogWarning("SsrFeature: backend has no multiple render targets, SSR disabled");
            return true;   // not fatal; the pass stays inactive
        }
        m_device = &context.device;
        return InitializeSsrResources(context.device, *m_resources);
    }

    void Shutdown(const FeatureShutdownContext& /*context*/) override
    {
        if (m_device && m_resources && m_resources->initialized)
            DestroySsrResources(*m_device, *m_resources);
        m_pass.reset();
        m_resources.reset();
        m_device = nullptr;
    }

    std::shared_ptr<SsrGpuResources> GetResources() const noexcept { return m_resources; }

private:
    IDevice*                         m_device = nullptr;
    std::shared_ptr<SsrGpuResources> m_resources;
    std::shared_ptr<SsrPass>         m_pass;
    std::shared_ptr<SsrFrameConstantsContributor> m_contributor;
};

} // namespace

std::unique_ptr<IEngineFeature> CreateSsrFeature(std::shared_ptr<SsrGpuResources>* outResources)
{
    auto feature = std::make_unique<SsrFeature>();
    if (outResources)
        *outResources = feature->GetResources();
    return feature;
}

} // namespace engine::renderer::addons::ssr
