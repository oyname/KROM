#pragma once
// =============================================================================
// KROM Engine - addons/probes/ProbeFeature.hpp
//
// Local reflection probes. Up to kMaxActiveProbes probes per view; each pixel
// blends at most two of them (priority order) and fades into the global IBL
// near the box walls. Data for the shaders:
//   featureParams[1] (kProbeFrameParameterBlock): probe count, flags, mip count
//   t18 TexSlots::FeatureCubeArray: prefiltered radiance of all probes
//   t19 TexSlots::FeatureTexture:   one row per probe (box, blend, SH irradiance)
// The same block carries the specular occlusion switch and the debug view, so
// both work without any probe in the scene.
// =============================================================================
#include "core/Math.hpp"
#include "renderer/RenderFeatureInterfaces.hpp"
#include "renderer/RenderFrameConstants.hpp"
#include "renderer/RendererTypes.hpp"
#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine::renderer { struct RenderView; class IDevice; }

namespace engine::addons::probes {

inline constexpr uint32_t kMaxActiveProbes = 32u;
inline constexpr uint32_t kProbeDataTexelsPerProbe = 16u;

// Layout of one probe row in the data texture (RGBA32F texels); must match
// assets/pbr_local_probe*.
//   0..2  world -> box rows (xyz axis, w translation)
//   3     xyz half extents, w blend distance
//   4     xyz capture point in box space, w intensity
//   5     x box projection (0/1), y cube index in the array
//   6..14 SH L2 irradiance, rgb per coefficient
inline constexpr uint32_t kProbeDataShTexel = 6u;

enum class ProbeDebugView : uint32_t
{
    None = 0,
    ProbeIndex = 1,          // colour of the first probe used, black = global IBL
    BlendWeight = 2,         // share of the local probes (white) vs. global IBL (black)
    SpecularOcclusion = 3,   // factor applied to the specular IBL
};

inline constexpr uint32_t kProbeFlagSpecularOcclusion = 1u;
inline constexpr uint32_t kProbeFlagDebugShift = 8u;

// featureParams[1]; must match the #defines in the PBR shaders.
struct ProbeFrameParams
{
    uint32_t probeCount = 0u;
    uint32_t flags = 0u;
    float    prefilterMipCount = 0.f;
    float    _pad = 0.f;
};
static_assert(sizeof(ProbeFrameParams) == 16u, "ProbeFrameParams must fill one slot");
inline constexpr renderer::FrameParameterBlockDesc kProbeFrameParameterBlock{ "krom-probes", 1u, 1u };

struct ProbeSettings
{
    bool           enabled = true;               // off = global IBL everywhere
    bool           specularOcclusion = false;    // specular IBL from AO, roughness and NoV (off = former look)
    ProbeDebugView debugView = ProbeDebugView::None;
    // Bake passes: 1 = capture lit by the global IBL only; 2 = a second capture
    // lit by the probes of the first, so a room's own light replaces the sky
    // light on its walls.
    uint32_t       bakePasses = 2u;
};

// Shared between the feature, the bake functions and the host application.
class ProbeRuntime
{
public:
    // Folder that ReflectionProbeComponent::bakedDataPath is relative to
    // (the project's Assets folder), UTF-8.
    void SetAssetRoot(std::string assetRootUtf8);
    [[nodiscard]] std::string GetAssetRoot() const;
    [[nodiscard]] std::string ResolvePath(const std::string& relativeUtf8) const;

    void SetSettings(const ProbeSettings& settings) noexcept;
    [[nodiscard]] ProbeSettings GetSettings() const noexcept;
    // Shortcuts for SetSettings.
    void SetEnabled(bool enabled) noexcept;
    [[nodiscard]] bool IsEnabled() const noexcept;

    // Forgets a baked file so it is read again. Call only while the GPU is
    // idle (e.g. right after a bake read back); rebuilds the cube array.
    void InvalidateFile(renderer::IDevice& device, const std::string& relativeUtf8);

    // Input hash stored in a baked file (header read once, then cached);
    // false if the file is missing or outdated in format.
    [[nodiscard]] bool GetBakedInputHash(const std::string& relativeUtf8, uint64_t& outHash);

    // Views that must not use probes (the faces of a running capture).
    void SetSuppressedViews(std::vector<const renderer::RenderView*> views);
    [[nodiscard]] bool IsViewSuppressed(const renderer::RenderView* view) const;

    // --- used by the feature -------------------------------------------------
    // Device for loading baked files and building the GPU data. That happens in
    // the scene extraction step, which runs on the render thread (frame
    // constants are prepared in a worker job, where OpenGL has no context).
    void SetDevice(renderer::IDevice* device) noexcept;
    [[nodiscard]] renderer::IDevice* GetDevice() const noexcept;

    struct ActiveProbe
    {
        std::string path;
        math::Vec3  boxCenter{ 0.f, 0.f, 0.f };
        math::Vec3  captureInBox{ 0.f, 0.f, 0.f };
        math::Quat  rotation = math::Quat::Identity();
        math::Vec3  extents{ 1.f, 1.f, 1.f };
        float       blendDistance = 1.f;
        float       intensity = 1.f;
        bool        boxProjection = true;
    };
    struct GpuState
    {
        uint32_t      probeCount = 0u;
        uint32_t      prefilterMipCount = 0u;
        TextureHandle cubeArray = TextureHandle::Invalid();
        TextureHandle data = TextureHandle::Invalid();
    };
    // probes in priority order. Loads files, (re)builds the cube array and
    // uploads the data rows when they change.
    GpuState Prepare(renderer::IDevice& device, const std::vector<ActiveProbe>& probes);
    void ReleaseAll(renderer::IDevice& device);

private:
    struct FileEntry
    {
        bool     failed = false;
        uint32_t prefilterBaseSize = 0u;
        uint32_t prefilterMipCount = 0u;
        std::vector<uint8_t> prefilterData;
        std::array<float, 27> sh{};   // 9 coefficients x rgb
    };

    const FileEntry* LoadFileLocked(const std::string& relativeUtf8);
    void DestroyGpuLocked(renderer::IDevice& device);

    mutable std::mutex m_mutex;
    std::string m_assetRoot;
    renderer::IDevice* m_device = nullptr;
    ProbeSettings m_settings{};
    std::unordered_map<std::string, FileEntry> m_files;
    std::unordered_map<std::string, uint64_t> m_bakedHashes;   // 0 = no valid file
    std::vector<const renderer::RenderView*> m_suppressedViews;

    std::vector<std::string> m_arrayPaths;      // cube i of m_cubeArray
    TextureHandle m_cubeArray = TextureHandle::Invalid();
    uint32_t m_arraySize = 0u;
    uint32_t m_arrayMips = 0u;
    TextureHandle m_dataTexture = TextureHandle::Invalid();
    std::vector<float> m_uploadedRows;
};

[[nodiscard]] std::unique_ptr<renderer::IEngineFeature> CreateProbeFeature(
    std::shared_ptr<ProbeRuntime>* outRuntime = nullptr);

} // namespace engine::addons::probes
