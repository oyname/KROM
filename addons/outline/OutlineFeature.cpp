#include "OutlineFeature.hpp"
#include "OutlinePass.hpp"
#include "renderer/IDevice.hpp"
#include "core/Debug.hpp"
#include <memory>
#include <utility>

namespace engine::renderer::addons::outline {

namespace {

class OutlineFeature final : public IEngineFeature
{
public:
    OutlineFeature(std::function<EntityID()> getSelected,
                   std::function<bool(EntityID)> shouldOutlineEntity)
        : m_resources(std::make_shared<OutlineGpuResources>())
        , m_pass(std::make_shared<OutlinePass>(0xF7A1u,
                                               m_resources,
                                               std::move(getSelected),
                                               std::move(shouldOutlineEntity)))
    {}

    std::string_view GetName()  const noexcept override { return "krom-outline"; }
    FeatureID        GetID()    const noexcept override { return FeatureID::FromString("krom-outline"); }

    void Register(FeatureRegistrationContext& context) override
    {
        context.RegisterPassContributor(m_pass);
    }

    bool Initialize(const FeatureInitializationContext& context) override
    {
        m_device = &context.device;
        return InitializeOutlineResources(context.device, *m_resources);
    }

    void Shutdown(const FeatureShutdownContext& /*context*/) override
    {
        if (m_device && m_resources && m_resources->initialized)
            DestroyOutlineResources(*m_device, *m_resources);
        m_pass.reset();
        m_resources.reset();
        m_device = nullptr;
    }

private:
    IDevice*                             m_device = nullptr;
    std::shared_ptr<OutlineGpuResources> m_resources;
    std::shared_ptr<OutlinePass>         m_pass;
};

} // namespace

std::unique_ptr<IEngineFeature> CreateOutlineFeature(std::function<EntityID()> getSelected,
                                                     std::function<bool(EntityID)> shouldOutlineEntity)
{
    return std::make_unique<OutlineFeature>(std::move(getSelected), std::move(shouldOutlineEntity));
}

} // namespace engine::renderer::addons::outline
