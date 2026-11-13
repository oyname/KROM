#pragma once
// =============================================================================
// KROM Engine - addons/probes/ProbeBake.hpp
//
// Baking a reflection probe: six 90-degree views from the probe position go
// through the normal render path (offscreen requests). A replacement tonemap
// step writes the linear HDR colour RGBE-encoded into an RGBA8 target, which
// is read back, decoded and handed to IBLBaker::BakeFromCubeFaces. The result
// is written as .kprobe and reloaded by the probe feature.
//
// Two ways to use it:
//  - ProbeCapture: the host adds GetRequests() to one of its own frames and
//    calls Finish() after that frame (the editor does this).
//  - BakeProbeBlocking: renders one frame itself (programs without editor).
// =============================================================================
#include "addons/probes/ProbeFeature.hpp"
#include "core/Math.hpp"
#include "core/Types.hpp"
#include "renderer/RenderFrameTypes.hpp"
#include "renderer/RenderPipelineTypes.hpp"
#include "renderer/RenderSystem.hpp"
#include <array>
#include <cstdint>
#include <span>
#include <string>

namespace engine::jobs { class JobSystem; }
namespace engine::ecs { class World; }
namespace engine::platform { class IPlatformTiming; }
namespace engine::renderer { class MaterialSystem; }

namespace engine::addons::probes {

struct ProbeBakeRequest
{
    math::Vec3  position{ 0.f, 0.f, 0.f };
    math::Vec3  extents{ 5.f, 3.f, 5.f };
    uint32_t    resolution = 128u;    // prefilter mip 0; the capture uses twice this
    std::string relativePath;         // target file, relative to the asset root
    float       nearPlane = 0.05f;
    float       farPlane = 1000.f;
    // false (first pass): the capture is lit by the global IBL only.
    // true (second pass): the probes baked so far light the capture, so the
    // room's own indirect light replaces the sky light on its walls.
    bool        lightWithProbes = false;
    uint64_t    inputHash = 0u;           // stored in the file; see ComputeProbeInputHash
};

// "Baked/<sceneName>/<probeId as 16 hex digits>.kprobe"
[[nodiscard]] std::string MakeProbeDataPath(const std::string& sceneNameUtf8, uint64_t probeId);
// Random non-zero id for a new probe.
[[nodiscard]] uint64_t GenerateProbeId();

// Hash of what a probe's bake depends on: its own transform, box and
// resolution, and every mesh (asset path, material paths, transform) and
// light in the world. Independent of entity order and ids, floats rounded to
// a millimetre, so it survives saving and loading. Compared with the hash
// stored in the baked file to show "outdated". Not covered: the sky /
// environment and edits inside a material file.
[[nodiscard]] uint64_t ComputeProbeInputHash(const ecs::World& world, EntityID probeEntity);

class ProbeCapture
{
public:
    ProbeCapture() = default;
    ProbeCapture(const ProbeCapture&) = delete;
    ProbeCapture& operator=(const ProbeCapture&) = delete;
    ~ProbeCapture();

    // baseView supplies background, layer masks and ambient; matrices and
    // post effects are replaced. Creates targets and the encode pipeline.
    [[nodiscard]] bool Begin(renderer::IDevice& device, ProbeRuntime& runtime,
                             const renderer::RenderView& baseView, const ProbeBakeRequest& request,
                             std::string* outError = nullptr);

    // Valid between Begin and Finish/Cancel; six entries.
    [[nodiscard]] std::span<const renderer::OffscreenRenderRequest> GetRequests() const noexcept;

    // After the frame that rendered GetRequests(): waits for the GPU, reads
    // back, convolves, writes the file and reloads it in the runtime.
    [[nodiscard]] bool Finish(renderer::IDevice& device, ProbeRuntime& runtime,
                              jobs::JobSystem* jobSystem, std::string* outError = nullptr);

    void Cancel(renderer::IDevice& device, ProbeRuntime& runtime);

    [[nodiscard]] bool IsActive() const noexcept { return m_active; }

private:
    void Release(renderer::IDevice& device, ProbeRuntime& runtime);

    bool m_active = false;
    ProbeBakeRequest m_request{};
    uint32_t m_captureSize = 0u;
    std::array<renderer::RenderView, 6> m_views{};
    std::array<RenderTargetHandle, 6> m_targets{};
    std::array<renderer::OffscreenRenderRequest, 6> m_requests{};
    renderer::FramePipelineCallbacks m_callbacks;
    ShaderHandle m_vs = ShaderHandle::Invalid();
    ShaderHandle m_ps = ShaderHandle::Invalid();
    PipelineHandle m_pipeline = PipelineHandle::Invalid();
};

// Renders one frame with the six capture views (the main view is drawn too
// and presented), then finishes the bake. For programs without the editor.
[[nodiscard]] bool BakeProbeBlocking(renderer::RenderSystem& renderSystem,
                                     const ecs::World& world,
                                     const renderer::MaterialSystem& materials,
                                     const platform::IPlatformTiming& timing,
                                     const renderer::RenderView& baseView,
                                     ProbeRuntime& runtime,
                                     const ProbeBakeRequest& request,
                                     std::string* outError = nullptr);

} // namespace engine::addons::probes
