#pragma once
// =============================================================================
// KROM Engine - addons/probes/ProbeFeature.hpp
//
// Local reflection probes (step 1): one active probe. Pixels inside its box
// take diffuse and specular IBL from the probe's baked cubes instead of the
// global environment. Shaders read the probe from featureParams slots 1..3
// (kProbeFrameParameterBlock) and the cubes from TexSlots::FeatureCube0/1.
// =============================================================================
#include "renderer/RenderFeatureInterfaces.hpp"
#include "renderer/RenderFrameConstants.hpp"
#include "renderer/RendererTypes.hpp"
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine::renderer { struct RenderView; class IDevice; }

namespace engine::addons::probes {

// featureParams[1..3]; must match the #defines in the PBR shaders.
struct ProbeFrameParams
{
    float    boxMin[3]{};
    uint32_t enabled = 0u;
    float    boxMax[3]{};
    float    intensity = 0.f;
    float    capturePosition[3]{};
    float    prefilterMipCount = 0.f;
};
static_assert(sizeof(ProbeFrameParams) == 48u, "ProbeFrameParams must fill three slots");
inline constexpr renderer::FrameParameterBlockDesc kProbeFrameParameterBlock{ "krom-probes", 1u, 3u };

// GPU copy of one baked probe.
struct ProbeGpuData
{
    TextureHandle irradiance = TextureHandle::Invalid();
    TextureHandle prefiltered = TextureHandle::Invalid();
    uint32_t prefilterMipCount = 0u;
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

    // Global switch; off = global IBL everywhere.
    void SetEnabled(bool enabled) noexcept;
    [[nodiscard]] bool IsEnabled() const noexcept;

    // Destroys the cached GPU data of a file so the next frame loads it again.
    // Call only while the GPU is idle (e.g. right after a bake read back).
    void InvalidateFile(renderer::IDevice& device, const std::string& relativeUtf8);

    // Views that must not use probes (the faces of a running capture).
    void SetSuppressedViews(std::vector<const renderer::RenderView*> views);
    [[nodiscard]] bool IsViewSuppressed(const renderer::RenderView* view) const;

    // --- used by the feature -------------------------------------------------
    // Device for loading baked files. Loading happens in the scene extraction
    // step, which runs on the render thread (frame constants are prepared in a
    // worker job, where OpenGL has no context).
    void SetDevice(renderer::IDevice* device) noexcept;
    [[nodiscard]] renderer::IDevice* GetDevice() const noexcept;
    [[nodiscard]] const ProbeGpuData* FindLoaded(const std::string& relativeUtf8) const;
    // Loads the file if not yet tried; returns nullptr if missing or invalid.
    const ProbeGpuData* EnsureLoaded(renderer::IDevice& device, const std::string& relativeUtf8);
    void ReleaseAll(renderer::IDevice& device);

private:
    struct Entry
    {
        ProbeGpuData gpu{};
        bool         failed = false;
    };

    mutable std::mutex m_mutex;
    std::string m_assetRoot;
    renderer::IDevice* m_device = nullptr;
    bool m_enabled = true;
    std::unordered_map<std::string, Entry> m_entries;
    std::vector<const renderer::RenderView*> m_suppressedViews;
};

[[nodiscard]] std::unique_ptr<renderer::IEngineFeature> CreateProbeFeature(
    std::shared_ptr<ProbeRuntime>* outRuntime = nullptr);

} // namespace engine::addons::probes
