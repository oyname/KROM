#include "addons/particles/ParticleView.hpp"

#include "assets/AssetPipeline.hpp"
#include "assets/AssetRegistry.hpp"

#include <utility>

namespace engine::particles {

void ParticleView::SetTextureLoader(ParticleTextureLoader loader)
{
    m_renderer.SetTextureLoader(std::move(loader));
}

void ParticleView::UseAssetTextures(const std::unique_ptr<assets::AssetPipeline>& pipeline,
                                    const assets::AssetRegistry& registry)
{
    m_renderer.SetTextureLoader([&pipeline, &registry](const std::string& path, ParticleImage& image) {
        return pipeline && LoadParticleImage(*pipeline, registry, path, image);
    });
}

bool ParticleView::Submit(const ParticleSystem& system, const renderer::RenderView& view,
                          renderer::IDevice* device, renderer::FramePipelineCallbacks& callbacks)
{
    system.CollectBillboards(view.cameraPosition, m_alpha, m_additive);
    if (m_alpha.empty() && m_additive.empty())
        return false;

    const math::Mat4 clipAdjustment = device ? device->GetClipSpaceAdjustment() : math::Mat4::Identity();
    m_renderer.SetCamera(clipAdjustment * view.projection * view.view, view.view);
    m_renderer.Submit(m_alpha, m_additive);
    // Create textures outside the render pass.
    if (device)
        m_renderer.Prepare(*device);
    AddParticleCallback(callbacks, m_renderer);
    return true;
}

} // namespace engine::particles
