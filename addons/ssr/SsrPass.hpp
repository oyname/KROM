#pragma once
// =============================================================================
// KROM Engine - addons/ssr/SsrPass.hpp
//
// Screen-space reflections for the forward renderer.
//
// The opaque pass writes the SSR inputs as extra colour targets (MRT,
// FrameRecipe::sceneOutputs; the PBR shaders' KROM_SSR_OUTPUT variant, see
// shaders/pbr_ssr_output.hlsl):
//   SsrNormalRoughness  octahedral shading normal, roughness, valid flag
//   SsrSpecularWeight   factor the shader applied to the prefiltered radiance
//   SsrSpecularLight    specular probe/IBL light it added to the colour
// After the opaque stage (with MSAA after resolving those inputs):
//   SsrTrace      marches the reflected view ray against the scene depth and
//                 reads the scene colour at the hit -> SsrReflection (rgb, a =
//                 confidence)
//   SsrComposite  filters the hit by roughness and adds
//                 confidence * (hit * weight - specular light) to the scene
//                 colour buffer the overlays and tonemap use, so the hit
//                 replaces the probe/IBL reflection; misses keep it.
// Not on OpenGL (no MRT path there).
// =============================================================================
#include "renderer/IDevice.hpp"
#include "renderer/RenderFrameConstants.hpp"
#include "renderer/RenderFramePassInterfaces.hpp"
#include "renderer/RendererTypes.hpp"
#include <memory>
#include <string_view>

namespace engine::renderer::addons::ssr {

struct SsrSettings
{
    bool  enabled      = true;
    float intensity    = 1.0f;   // 0 = probe/IBL reflection only
    float maxRoughness = 0.6f;   // rougher surfaces keep the probe/IBL reflection
    float maxDistance  = 30.0f;  // world units a ray travels at most
    float thickness    = 0.3f;   // depth range (world units) behind a surface that counts as a hit
};

// SSR's block in FrameConstants::featureParams (slot 7); the SSR shaders read it
// as featureParams[7].
struct SsrFrameParams
{
    float maxDistance  = 0.f;
    float thickness    = 0.f;
    float maxRoughness = 0.f;
    float intensity    = 0.f;
};
static_assert(sizeof(SsrFrameParams) == 16u, "SsrFrameParams must fill exactly one slot");
inline constexpr FrameParameterBlockDesc kSsrFrameParameterBlock{"krom-ssr", 7u, 1u};

// The post-opaque scene colour buffer: a copy of HDRSceneColor that overlays
// (outline, editor gizmos) draw onto and tonemap reads. GTAO creates it; when
// GTAO is off, SSR creates it.
inline constexpr std::string_view kSceneCompositeResource = "GTAOComposite";

struct SsrGpuResources
{
    ShaderHandle   vsFullscreen;
    ShaderHandle   psResolve;
    ShaderHandle   psTrace;
    ShaderHandle   psComposite;
    ShaderHandle   psCopy;
    PipelineHandle pipelineResolve;    // MRT: 3 colour targets
    PipelineHandle pipelineTrace;
    PipelineHandle pipelineComposite;  // additive into the scene composite
    PipelineHandle pipelineCopy;       // HDRSceneColor -> scene composite
    SsrSettings    settings;
    bool           initialized = false;
};

bool InitializeSsrResources(IDevice& device, SsrGpuResources& res);
void DestroySsrResources(IDevice& device, SsrGpuResources& res);

class SsrPass final : public IFramePass
{
public:
    SsrPass(uint32_t id, std::shared_ptr<SsrGpuResources> resources);

    uint32_t  GetContributorId() const noexcept override { return m_id; }
    PassPhase GetPhase() const noexcept override { return PassPhase::BeforeTransparent; }
    bool      DeclaresResource(std::string_view name) const noexcept override;
    uint64_t  GetStructureRevision() const noexcept override;
    void      BuildPass(const PassBuildContext& ctx) const override;

private:
    [[nodiscard]] bool Active() const noexcept;

    uint32_t m_id;
    std::shared_ptr<SsrGpuResources> m_resources;
};

} // namespace engine::renderer::addons::ssr
