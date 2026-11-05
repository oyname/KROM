#pragma once
// =============================================================================
// KROM Engine - addons/particles/ParticleView.hpp
// Draws the particles of one ParticleSystem from one camera.
//
// Bundles the per-frame steps an app would otherwise repeat: collect the
// billboards, set the camera (with the device's clip-space adjustment),
// build the quads, create missing textures outside the render pass and
// register the particle pass. Each camera that shows particles needs its own
// ParticleView, because the renderer holds the geometry of one view.
// =============================================================================

#include "addons/particles/ParticleRenderer.hpp"
#include "addons/particles/ParticleSystem.hpp"
#include "renderer/RenderFrameTypes.hpp"

#include <memory>
#include <vector>

namespace engine::assets {
class AssetPipeline;
class AssetRegistry;
}

namespace engine::particles {

class ParticleView
{
public:
    void SetTextureLoader(ParticleTextureLoader loader);

    // Loads particle textures relative to the asset root through the asset
    // pipeline. The pipeline is looked up per load, so it may be created later.
    void UseAssetTextures(const std::unique_ptr<assets::AssetPipeline>& pipeline,
                          const assets::AssetRegistry& registry);

    // Collects the billboards of system for view and registers the particle
    // pass in callbacks. Without particles nothing is registered (returns
    // false). Without a device the textures are not created this frame.
    bool Submit(const ParticleSystem& system, const renderer::RenderView& view,
                renderer::IDevice* device, renderer::FramePipelineCallbacks& callbacks);

    void OnDeviceShutdown() noexcept { m_renderer.OnDeviceShutdown(); }

    [[nodiscard]] ParticleRenderer& Renderer() noexcept { return m_renderer; }

private:
    ParticleRenderer m_renderer;
    std::vector<ParticleBillboard> m_alpha;
    std::vector<ParticleBillboard> m_additive;
};

} // namespace engine::particles
