#pragma once
#include "renderer/IDevice.hpp"
#include "renderer/RenderFrameConstants.hpp"
#include "renderer/RenderFramePassInterfaces.hpp"
#include "renderer/RendererTypes.hpp"
#include <memory>
#include <string_view>

namespace engine::renderer::addons::contact_shadow {

// Screen-space contact shadows for the main directional light (doc/PLAN_KONTAKTSCHATTEN.md).
struct ContactShadowSettings
{
    bool     enabled   = true;
    float    length    = 0.30f;  // ray length towards the light, world units
    float    thickness = 0.05f;  // assumed depth of occluders, world units
    float    intensity = 1.0f;   // 0 = no contact shadow
    uint32_t steps     = 12u;    // ray steps (4..32)
};

inline constexpr float    kContactShadowLengthMin    = 0.02f;
inline constexpr float    kContactShadowLengthMax    = 2.0f;
inline constexpr float    kContactShadowThicknessMin = 0.005f;
inline constexpr float    kContactShadowThicknessMax = 0.5f;
inline constexpr uint32_t kContactShadowStepsMin     = 4u;
inline constexpr uint32_t kContactShadowStepsMax     = 32u;

// Parameter block in FrameConstants::featureParams, slots 3 and 4. Read by the trace
// shader (ContactShadowPass.cpp) and the PBR shaders (pbr_lit.ps.hlsl,
// pbr_forward_plus.ps.hlsl); keep the slots in sync with them.
struct ContactShadowFrameParams
{
    float    lightDirWS[3] = {0.f, 0.f, 0.f}; // direction to the main light, world space
    uint32_t lightIndexPlusOne = 0u;          // index in lights[] + 1; 0 = no contact shadow
    float    length    = 0.f;
    float    thickness = 0.f;
    float    intensity = 0.f;
    uint32_t steps     = 0u;
};
static_assert(sizeof(ContactShadowFrameParams) == 32u, "ContactShadowFrameParams must fill two slots");
inline constexpr FrameParameterBlockDesc kContactShadowFrameParameterBlock{"krom-contact-shadow", 3u, 2u};

struct ContactShadowGpuResources
{
    ShaderHandle          vsFullscreen;
    ShaderHandle          psTrace;
    ShaderHandle          psTraceMS;
    ShaderHandle          psBlur;
    PipelineHandle        pipelineTrace;
    PipelineHandle        pipelineTraceMS;
    PipelineHandle        pipelineBlur;
    ContactShadowSettings settings;
    bool                  initialized = false;
};

bool InitializeContactShadowResources(IDevice& device, ContactShadowGpuResources& res);
void DestroyContactShadowResources(IDevice& device, ContactShadowGpuResources& res);

class ContactShadowPass final : public IFramePass
{
public:
    ContactShadowPass(uint32_t id, std::shared_ptr<ContactShadowGpuResources> resources);
    ~ContactShadowPass() override = default;

    uint32_t  GetContributorId()  const noexcept override { return m_id; }
    PassPhase GetPhase()          const noexcept override { return PassPhase::AfterDepthPrepass; }
    bool      DeclaresResource(std::string_view name) const noexcept override;
    uint64_t  GetStructureRevision() const noexcept override;
    void      BuildPass(const PassBuildContext& ctx) const override;

private:
    bool Active() const noexcept;

    uint32_t m_id;
    std::shared_ptr<ContactShadowGpuResources> m_resources;
};

} // namespace engine::renderer::addons::contact_shadow
