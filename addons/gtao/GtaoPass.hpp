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
};

// GTAO's per-frame parameter block in FrameConstants::featureParams.
// Shaders read it as featureParams[0] (pbr_lit.ps.hlsl, pbr_forward_plus.ps.hlsl,
// the GTAO pass shaders in GtaoPass.cpp); keep the slot in sync with them.
struct GtaoFrameParams
{
    float    radius    = 0.f;
    float    bias      = 0.f;
    float    intensity = 0.f;
    uint32_t enabled   = 0u;   // nonzero when AO is available
};
static_assert(sizeof(GtaoFrameParams) == 16u, "GtaoFrameParams must fill exactly one slot");
inline constexpr FrameParameterBlockDesc kGtaoFrameParameterBlock{"krom-gtao", 0u, 1u};

struct GtaoGpuResources
{
    ShaderHandle   vsFullscreen;
    ShaderHandle   psLinearizeDepth;
    ShaderHandle   psLinearizeDepthMS;
    ShaderHandle   psGtaoMain;
    ShaderHandle   psBlur;
    ShaderHandle   psComposite;
    PipelineHandle pipelineLinearize;
    PipelineHandle pipelineLinearizeMS;
    PipelineHandle pipelineGtaoMain;
    PipelineHandle pipelineBlur;
    PipelineHandle pipelineComposite;
    GtaoSettings   settings;
    bool           initialized = false;
};

bool InitializeGtaoResources(IDevice& device, GtaoGpuResources& res);
void DestroyGtaoResources(IDevice& device, GtaoGpuResources& res);

class GtaoPass final : public IFramePass
{
public:
    GtaoPass(uint32_t id, PassPhase phase, std::shared_ptr<GtaoGpuResources> resources);
    ~GtaoPass() override = default;

    uint32_t  GetContributorId()  const noexcept override { return m_id; }
    PassPhase GetPhase()          const noexcept override { return m_phase; }
    bool      DeclaresResource(std::string_view name) const noexcept override;
    uint64_t  GetStructureRevision() const noexcept override;
    void      BuildPass(const PassBuildContext& ctx) const override;

private:
    void BuildComposite(const PassBuildContext& ctx) const;
    bool Active() const noexcept;

    uint32_t m_id;
    PassPhase m_phase;
    std::shared_ptr<GtaoGpuResources> m_resources;
};

} // namespace engine::renderer::addons::gtao
