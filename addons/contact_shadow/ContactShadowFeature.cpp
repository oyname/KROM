#include "ContactShadowFeature.hpp"
#include "ContactShadowPass.hpp"
#include "addons/lighting/LightingFrameData.hpp"
#include "renderer/RenderExtractionContext.hpp"
#include "renderer/RenderFrameConstants.hpp"
#include "renderer/RenderFrameTypes.hpp"
#include "renderer/IDevice.hpp"
#include "assets/AssetRegistry.hpp"
#include "core/Debug.hpp"
#include <algorithm>
#include <cmath>
#include <memory>

namespace engine::renderer::addons::contact_shadow {

int SelectContactShadowLight(const std::vector<engine::addons::lighting::ExtractedLight>& lights) noexcept
{
    for (size_t i = 0u; i < lights.size(); ++i)
    {
        const auto& light = lights[i];
        if (light.type == engine::addons::lighting::ExtractedLightType::Directional &&
            light.castShadows && light.contactShadows)
            return static_cast<int>(i);
    }
    return -1;
}

namespace {

class ContactShadowFrameConstantsContributor final : public renderer::IFrameConstantsContributor
{
public:
    explicit ContactShadowFrameConstantsContributor(std::shared_ptr<ContactShadowGpuResources> resources)
        : m_resources(std::move(resources))
    {}

    std::string_view GetName() const noexcept override { return "krom-contact-shadow-constants"; }

    void Contribute(const renderer::FrameConstantsContributionContext& ctx,
                    renderer::FrameConstants& fc) const override
    {
        ContactShadowFrameParams params{};
        const bool active = m_resources && m_resources->initialized && m_resources->settings.enabled &&
                            ctx.view.enableDepthPrepass;
        const auto* lighting = active ? ctx.GetFrameData<engine::addons::lighting::LightingFrameData>() : nullptr;
        const int lightIndex = lighting ? SelectContactShadowLight(lighting->lights) : -1;
        if (lightIndex >= 0)
        {
            const ContactShadowSettings& s = m_resources->settings;
            // lights[].positionWS of a directional light holds its direction; the
            // shaders light along -direction.
            const math::Vec3 dir = lighting->lights[static_cast<size_t>(lightIndex)].directionWorld;
            const float len = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
            if (len > 1e-6f)
            {
                params.lightDirWS[0] = -dir.x / len;
                params.lightDirWS[1] = -dir.y / len;
                params.lightDirWS[2] = -dir.z / len;
                params.lightIndexPlusOne = static_cast<uint32_t>(lightIndex) + 1u;
                params.length    = std::clamp(s.length, kContactShadowLengthMin, kContactShadowLengthMax);
                params.thickness = std::clamp(s.thickness, kContactShadowThicknessMin, kContactShadowThicknessMax);
                params.intensity = std::clamp(s.intensity, 0.0f, 1.0f);
                params.steps     = std::clamp(s.steps, kContactShadowStepsMin, kContactShadowStepsMax);
            }
        }
        renderer::WriteFrameParameterBlock(fc, kContactShadowFrameParameterBlock, params);
    }

private:
    std::shared_ptr<ContactShadowGpuResources> m_resources;
};

class ContactShadowFeature final : public IEngineFeature
{
public:
    ContactShadowFeature()
        : m_resources(std::make_shared<ContactShadowGpuResources>())
        , m_pass(std::make_shared<ContactShadowPass>(0xC057u, m_resources))
        , m_contributor(std::make_shared<ContactShadowFrameConstantsContributor>(m_resources))
    {}

    std::string_view GetName() const noexcept override { return "krom-contact-shadow"; }
    FeatureID GetID() const noexcept override { return FeatureID::FromString("krom-contact-shadow"); }

    void Register(FeatureRegistrationContext& context) override
    {
        context.RegisterPassContributor(m_pass);
        if (context.RegisterFrameParameterBlock(kContactShadowFrameParameterBlock))
            context.RegisterFrameConstantsContributor(m_contributor);
    }

    bool Initialize(const FeatureInitializationContext& context) override
    {
        if (context.device.GetShaderTargetProfile() == assets::ShaderTargetProfile::OpenGL_GLSL450)
        {
            Debug::LogWarning("ContactShadowFeature: contact shadows not supported on OpenGL, disabled");
            return true; // not fatal; the pass stays a no-op
        }
        m_device = &context.device;
        return InitializeContactShadowResources(context.device, *m_resources);
    }

    void Shutdown(const FeatureShutdownContext& /*context*/) override
    {
        if (m_device && m_resources && m_resources->initialized)
            DestroyContactShadowResources(*m_device, *m_resources);
        m_pass.reset();
        m_resources.reset();
        m_device = nullptr;
    }

    std::shared_ptr<ContactShadowGpuResources> GetResources() const noexcept { return m_resources; }

private:
    IDevice*                                   m_device = nullptr;
    std::shared_ptr<ContactShadowGpuResources> m_resources;
    std::shared_ptr<ContactShadowPass>         m_pass;
    std::shared_ptr<ContactShadowFrameConstantsContributor> m_contributor;
};

} // namespace

std::unique_ptr<IEngineFeature> CreateContactShadowFeature(std::shared_ptr<ContactShadowGpuResources>* outResources)
{
    auto feature = std::make_unique<ContactShadowFeature>();
    if (outResources)
        *outResources = feature->GetResources();
    return feature;
}

} // namespace engine::renderer::addons::contact_shadow
