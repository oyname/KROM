#pragma once
#include "renderer/IDevice.hpp"
#include "renderer/RenderFramePassInterfaces.hpp"
#include "renderer/RendererTypes.hpp"
#include <memory>
#include <string_view>

namespace engine::renderer::addons::gtao {

struct GtaoSettings
{
    bool  enabled   = true;
    float radius    = 0.10f;  // world-space metres (visible range: 0.01 – 0.5 m)
    float intensity = 0.65f;  // AO strength multiplier
    float bias      = 0.05f;  // horizon angle bias vs self-occlusion
};

struct GtaoGpuResources
{
    ShaderHandle   vsFullscreen;
    ShaderHandle   psLinearizeDepth;
    ShaderHandle   psGtaoMain;
    ShaderHandle   psBlur;
    ShaderHandle   psComposite;
    PipelineHandle pipelineLinearize;
    PipelineHandle pipelineGtaoMain;
    PipelineHandle pipelineBlur;
    PipelineHandle pipelineComposite;
    GtaoSettings   settings;
    bool           initialized = false;

    // Persistent AO history target (Option B). The blur pass renders the
    // denoised AO here; the next frame's opaque pass samples it to attenuate
    // only the ambient term. Created/resized lazily on the first blur execution.
    RenderTargetHandle aoHistoryRT;
    TextureHandle      aoHistoryTex;
    uint32_t           aoWidth  = 0u;
    uint32_t           aoHeight = 0u;
    // False until the blur pass has written the history at least once at the
    // current size. The opaque pass must not sample it before then (contents are
    // undefined right after creation/resize), which would flash garbage AO.
    bool               aoHistoryValid = false;
};

bool InitializeGtaoResources(IDevice& device, GtaoGpuResources& res);
void DestroyGtaoResources(IDevice& device, GtaoGpuResources& res);

// Creates/resizes the persistent AO history render target to (w, h). Called once
// per frame (before pass building) from the GTAO frame-constants contributor,
// which is the earliest point with both a device and the viewport size.
void EnsureGtaoHistoryTarget(IDevice& device, GtaoGpuResources& res, uint32_t w, uint32_t h);

class GtaoPass final : public IFramePass
{
public:
    explicit GtaoPass(uint32_t id, std::shared_ptr<GtaoGpuResources> resources);
    ~GtaoPass() override = default;

    uint32_t  GetContributorId()  const noexcept override { return m_id; }
    PassPhase GetPhase()          const noexcept override { return PassPhase::AfterOpaque; }
    bool      DeclaresResource(std::string_view name) const noexcept override;
    void      BuildPass(const PassBuildContext& ctx) const override;

    void      SetSettings(const GtaoSettings& s);
    bool      IsEnabled() const noexcept;

private:
    uint32_t m_id;
    std::shared_ptr<GtaoGpuResources> m_resources;
};

} // namespace engine::renderer::addons::gtao
