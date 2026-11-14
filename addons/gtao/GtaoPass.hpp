#pragma once
#include "renderer/IDevice.hpp"
#include "renderer/RenderFrameConstants.hpp"
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
    // The lit shaders read the AO of the previous frame. On: they look it up
    // where the pixel's world position was on screen in that frame, so the AO
    // stays on the geometry while the camera moves. Off: same screen position
    // (the former behaviour; AO trails by one frame of camera motion).
    bool  reprojectHistory = false;
};

// GTAO's per-frame parameter block in FrameConstants::featureParams.
// Shaders read it as featureParams[0] (pbr_lit.ps.hlsl, pbr_forward_plus.ps.hlsl,
// the GTAO pass shaders in GtaoPass.cpp); keep the slot in sync with them.
struct GtaoFrameParams
{
    float    radius    = 0.f;
    float    bias      = 0.f;
    float    intensity = 0.f;
    uint32_t enabled   = 0u;   // kGtaoFlag* bits
};
static_assert(sizeof(GtaoFrameParams) == 16u, "GtaoFrameParams must fill exactly one slot");
inline constexpr FrameParameterBlockDesc kGtaoFrameParameterBlock{"krom-gtao", 0u, 1u};
inline constexpr uint32_t kGtaoFlagEnabled   = 1u << 0;   // sample the AO history
inline constexpr uint32_t kGtaoFlagReproject = 1u << 1;   // look it up through kGtaoReprojectionFrameParameterBlock

// Rows x, y and w of the view-projection the AO history was rendered with
// (featureParams[3..5]): dot(row, float4(positionWS, 1)) gives that frame's
// clip-space x, y and w.
struct GtaoReprojectionFrameParams
{
    float rowX[4] = { 0.f, 0.f, 0.f, 0.f };
    float rowY[4] = { 0.f, 0.f, 0.f, 0.f };
    float rowW[4] = { 0.f, 0.f, 0.f, 0.f };
};
static_assert(sizeof(GtaoReprojectionFrameParams) == 48u, "GtaoReprojectionFrameParams must fill three slots");
inline constexpr FrameParameterBlockDesc kGtaoReprojectionFrameParameterBlock{"krom-gtao-reprojection", 3u, 3u};

// Takes rows x, y, w out of a view-projection as stored in FrameConstants
// (16 floats, element [column * 4 + row]).
[[nodiscard]] inline GtaoReprojectionFrameParams MakeGtaoReprojectionParams(const float viewProj[16]) noexcept
{
    GtaoReprojectionFrameParams p{};
    for (int column = 0; column < 4; ++column)
    {
        p.rowX[column] = viewProj[column * 4 + 0];
        p.rowY[column] = viewProj[column * 4 + 1];
        p.rowW[column] = viewProj[column * 4 + 3];
    }
    return p;
}

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

    // View-projection of the frame that wrote the AO history (taken over by the
    // blur pass from pendingViewProj, which the frame-constants contributor
    // fills for the frame being prepared).
    GtaoReprojectionFrameParams historyViewProj{};
    GtaoReprojectionFrameParams pendingViewProj{};
    bool                        historyViewProjValid = false;
    bool                        pendingViewProjValid = false;
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
