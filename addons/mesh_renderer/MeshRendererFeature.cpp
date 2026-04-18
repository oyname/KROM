#include "addons/mesh_renderer/MeshRendererFeature.hpp"

#include "addons/mesh_renderer/MeshRendererExtraction.hpp"

namespace engine::addons::mesh_renderer {
namespace {

class MeshRendererExtractionStep final : public renderer::ISceneExtractionStep
{
public:
    std::string_view GetName() const noexcept override { return "mesh_renderer.extract"; }

    void Extract(const ecs::World& world, renderer::RenderWorld& renderWorld) const override
    {
        ExtractRenderables(world, renderWorld);
    }
};

class MeshRendererFeature final : public renderer::IEngineFeature
{
public:
    MeshRendererFeature()
        : m_extractionStep(std::make_shared<MeshRendererExtractionStep>())
    {
    }

    std::string_view GetName() const noexcept override { return "krom-mesh-renderer"; }
    renderer::FeatureID GetID() const noexcept override
    {
        return renderer::FeatureID::FromString("krom-mesh-renderer");
    }

    void Register(renderer::FeatureRegistrationContext& context) override
    {
        context.RegisterSceneExtractionStep(m_extractionStep);
    }

    bool Initialize(const renderer::FeatureInitializationContext& context) override
    {
        (void)context;
        return true;
    }

    void Shutdown(const renderer::FeatureShutdownContext& context) override
    {
        (void)context;
    }

private:
    renderer::SceneExtractionStepPtr m_extractionStep;
};

} // namespace

std::unique_ptr<renderer::IEngineFeature> CreateMeshRendererFeature()
{
    return std::make_unique<MeshRendererFeature>();
}

} // namespace engine::addons::mesh_renderer
