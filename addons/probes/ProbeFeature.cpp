#include "addons/probes/ProbeFeature.hpp"

#include "addons/forward/ForwardSceneBindings.hpp"
#include "addons/probes/ProbeComponents.hpp"
#include "addons/probes/ProbeData.hpp"
#include "core/Debug.hpp"
#include "core/PathUtf8.hpp"
#include "ecs/Components.hpp"
#include "renderer/ECSExtractor.hpp"
#include "renderer/IDevice.hpp"
#include "renderer/RenderExtractionContext.hpp"
#include "renderer/RenderFrameTypes.hpp"
#include "renderer/ShaderBindingModel.hpp"

#include <algorithm>

namespace engine::addons::probes {

using renderer::TexSlots;

// =============================================================================
// ProbeRuntime
// =============================================================================

void ProbeRuntime::SetAssetRoot(std::string assetRootUtf8)
{
    std::lock_guard lock(m_mutex);
    m_assetRoot = std::move(assetRootUtf8);
}

std::string ProbeRuntime::GetAssetRoot() const
{
    std::lock_guard lock(m_mutex);
    return m_assetRoot;
}

std::string ProbeRuntime::ResolvePath(const std::string& relativeUtf8) const
{
    std::lock_guard lock(m_mutex);
    if (m_assetRoot.empty())
        return relativeUtf8;
    return PathToUtf8(PathFromUtf8(m_assetRoot) / PathFromUtf8(relativeUtf8));
}

void ProbeRuntime::SetDevice(renderer::IDevice* device) noexcept
{
    std::lock_guard lock(m_mutex);
    m_device = device;
}

renderer::IDevice* ProbeRuntime::GetDevice() const noexcept
{
    std::lock_guard lock(m_mutex);
    return m_device;
}

void ProbeRuntime::SetEnabled(bool enabled) noexcept
{
    std::lock_guard lock(m_mutex);
    m_enabled = enabled;
}

bool ProbeRuntime::IsEnabled() const noexcept
{
    std::lock_guard lock(m_mutex);
    return m_enabled;
}

void ProbeRuntime::SetSuppressedViews(std::vector<const renderer::RenderView*> views)
{
    std::lock_guard lock(m_mutex);
    m_suppressedViews = std::move(views);
}

bool ProbeRuntime::IsViewSuppressed(const renderer::RenderView* view) const
{
    std::lock_guard lock(m_mutex);
    return view && std::find(m_suppressedViews.begin(), m_suppressedViews.end(), view) != m_suppressedViews.end();
}

const ProbeGpuData* ProbeRuntime::FindLoaded(const std::string& relativeUtf8) const
{
    std::lock_guard lock(m_mutex);
    const auto it = m_entries.find(relativeUtf8);
    if (it == m_entries.end() || it->second.failed)
        return nullptr;
    return &it->second.gpu;
}

namespace {

void DestroyGpu(renderer::IDevice& device, ProbeGpuData& gpu)
{
    if (gpu.irradiance.IsValid())
        device.DestroyTexture(gpu.irradiance);
    if (gpu.prefiltered.IsValid())
        device.DestroyTexture(gpu.prefiltered);
    gpu = {};
}

TextureHandle CreateCube(renderer::IDevice& device, uint32_t size, uint32_t mips, const char* name)
{
    renderer::TextureDesc desc{};
    desc.width = size;
    desc.height = size;
    desc.arraySize = 6u;
    desc.mipLevels = mips;
    desc.dimension = renderer::TextureDimension::Cubemap;
    desc.format = renderer::Format::RGBA16_FLOAT;
    desc.usage = renderer::ResourceUsage::ShaderResource | renderer::ResourceUsage::CopyDest;
    desc.initialState = renderer::ResourceState::ShaderRead;
    desc.debugName = name;
    return device.CreateTexture(desc);
}

// data: mip-major, 6 faces per mip, RGBA16F.
void UploadCube(renderer::IDevice& device, TextureHandle tex, const std::vector<uint8_t>& data,
                uint32_t baseSize, uint32_t mips)
{
    size_t offset = 0u;
    uint32_t size = baseSize;
    for (uint32_t mip = 0u; mip < mips; ++mip)
    {
        const size_t faceBytes = static_cast<size_t>(size) * size * 4u * sizeof(uint16_t);
        for (uint32_t face = 0u; face < 6u; ++face)
        {
            device.UploadTextureData(tex, data.data() + offset, faceBytes, mip, face);
            offset += faceBytes;
        }
        size = std::max(size / 2u, 1u);
    }
}

} // namespace

const ProbeGpuData* ProbeRuntime::EnsureLoaded(renderer::IDevice& device, const std::string& relativeUtf8)
{
    std::string path;
    {
        std::lock_guard lock(m_mutex);
        const auto it = m_entries.find(relativeUtf8);
        if (it != m_entries.end())
            return it->second.failed ? nullptr : &it->second.gpu;
        path = m_assetRoot.empty()
            ? relativeUtf8
            : PathToUtf8(PathFromUtf8(m_assetRoot) / PathFromUtf8(relativeUtf8));
    }

    Entry entry{};
    ProbeBakedData data{};
    std::string error;
    if (!ReadProbeFile(path, data, &error))
    {
        Debug::LogWarning("ProbeFeature.cpp: probe '%s' not loaded: %s", relativeUtf8.c_str(), error.c_str());
        entry.failed = true;
    }
    else if (!device.SupportsTextureFormat(renderer::Format::RGBA16_FLOAT,
                 renderer::ResourceUsage::ShaderResource | renderer::ResourceUsage::CopyDest))
    {
        Debug::LogWarning("ProbeFeature.cpp: RGBA16F cube maps not supported, probes disabled");
        entry.failed = true;
    }
    else
    {
        entry.gpu.irradiance = CreateCube(device, data.irradianceSize, 1u, "Probe_Irradiance");
        entry.gpu.prefiltered = CreateCube(device, data.prefilterBaseSize, data.prefilterMipCount, "Probe_Prefilter");
        entry.gpu.prefilterMipCount = data.prefilterMipCount;
        if (!entry.gpu.irradiance.IsValid() || !entry.gpu.prefiltered.IsValid())
        {
            Debug::LogError("ProbeFeature.cpp: failed to create probe textures for '%s'", relativeUtf8.c_str());
            DestroyGpu(device, entry.gpu);
            entry.failed = true;
        }
        else
        {
            UploadCube(device, entry.gpu.irradiance, data.irradianceData, data.irradianceSize, 1u);
            UploadCube(device, entry.gpu.prefiltered, data.prefilterData, data.prefilterBaseSize,
                       data.prefilterMipCount);
            Debug::Log("ProbeFeature.cpp: loaded probe '%s' (%u px, %u mips)",
                       relativeUtf8.c_str(), data.prefilterBaseSize, data.prefilterMipCount);
        }
    }

    std::lock_guard lock(m_mutex);
    auto [it, inserted] = m_entries.emplace(relativeUtf8, std::move(entry));
    if (!inserted)
        return it->second.failed ? nullptr : &it->second.gpu;
    return it->second.failed ? nullptr : &it->second.gpu;
}

void ProbeRuntime::InvalidateFile(renderer::IDevice& device, const std::string& relativeUtf8)
{
    std::lock_guard lock(m_mutex);
    const auto it = m_entries.find(relativeUtf8);
    if (it == m_entries.end())
        return;
    DestroyGpu(device, it->second.gpu);
    m_entries.erase(it);
}

void ProbeRuntime::ReleaseAll(renderer::IDevice& device)
{
    std::lock_guard lock(m_mutex);
    for (auto& [path, entry] : m_entries)
        DestroyGpu(device, entry.gpu);
    m_entries.clear();
}

// =============================================================================
// Feature
// =============================================================================
namespace {

// What the extraction step selected for this view.
struct ProbeFrameData
{
    bool         hasProbe = false;
    bool         gpuReady = false;
    std::string  bakedDataPath;
    math::Vec3   center{ 0.f, 0.f, 0.f };
    math::Vec3   extents{ 0.f, 0.f, 0.f };
    float        intensity = 1.f;
    uint32_t     prefilterMipCount = 0u;
};

class ProbeExtractionStep final : public renderer::ISceneExtractionStep
{
public:
    explicit ProbeExtractionStep(std::shared_ptr<ProbeRuntime> runtime)
        : m_runtime(std::move(runtime))
    {}

    std::string_view GetName() const noexcept override { return "probes.extract"; }

    void Extract(const renderer::SceneExtractionContext& ctx) const override
    {
        auto& frame = ctx.extractionView.GetOrCreateFrameData<ProbeFrameData>("probes.frame_data");
        frame = ProbeFrameData{};
        auto& bindings = ctx.extractionView.GetOrCreateFrameData<renderer::addons::forward::ForwardSceneBindings>(
            renderer::addons::forward::ForwardSceneBindings::kFrameDataName);

        TextureHandle prefiltered = TextureHandle::Invalid();
        TextureHandle irradiance = TextureHandle::Invalid();

        if (m_runtime && m_runtime->IsEnabled() && !m_runtime->IsViewSuppressed(ctx.view))
        {
            // Step 1: a single probe, the one with the lowest entity id that has
            // baked data. Several probes and blending come later.
            EntityID chosen = NULL_ENTITY;
            ctx.world.View<ReflectionProbeComponent, WorldTransformComponent>(
                [&](EntityID entity, const ReflectionProbeComponent& probe, const WorldTransformComponent& xf)
                {
                    if (probe.bakedDataPath.empty() || !renderer::ECSExtractor::IsEntityActive(ctx.world, entity))
                        return;
                    if (chosen.IsValid() && entity.value > chosen.value)
                        return;
                    chosen = entity;
                    frame.hasProbe = true;
                    frame.bakedDataPath = probe.bakedDataPath;
                    frame.center = xf.position;
                    frame.extents = probe.extents;
                    frame.intensity = probe.intensity;
                });

            if (frame.hasProbe)
            {
                renderer::IDevice* device = m_runtime->GetDevice();
                const ProbeGpuData* gpu = device
                    ? m_runtime->EnsureLoaded(*device, frame.bakedDataPath)
                    : m_runtime->FindLoaded(frame.bakedDataPath);
                if (gpu)
                {
                    frame.gpuReady = true;
                    frame.prefilterMipCount = gpu->prefilterMipCount;
                    prefiltered = gpu->prefiltered;
                    irradiance = gpu->irradiance;
                }
            }
        }

        // Publish the slots every frame; invalid handles clear them.
        bindings.SetTexture(TexSlots::FeatureCube0, prefiltered);
        bindings.SetTexture(TexSlots::FeatureCube1, irradiance);
    }

private:
    std::shared_ptr<ProbeRuntime> m_runtime;
};

class ProbeFrameConstantsContributor final : public renderer::IFrameConstantsContributor
{
public:
    explicit ProbeFrameConstantsContributor(std::shared_ptr<ProbeRuntime> runtime)
        : m_runtime(std::move(runtime))
    {}

    std::string_view GetName() const noexcept override { return "krom-probes-constants"; }

    void Contribute(const renderer::FrameConstantsContributionContext& ctx,
                    renderer::FrameConstants& fc) const override
    {
        ProbeFrameParams params{};
        const ProbeFrameData* frame = ctx.frameData.GetFrameData<ProbeFrameData>();
        if (frame && frame->hasProbe && frame->gpuReady)
        {
            {
                params.enabled = 1u;
                params.boxMin[0] = frame->center.x - frame->extents.x;
                params.boxMin[1] = frame->center.y - frame->extents.y;
                params.boxMin[2] = frame->center.z - frame->extents.z;
                params.boxMax[0] = frame->center.x + frame->extents.x;
                params.boxMax[1] = frame->center.y + frame->extents.y;
                params.boxMax[2] = frame->center.z + frame->extents.z;
                params.intensity = frame->intensity;
                params.capturePosition[0] = frame->center.x;
                params.capturePosition[1] = frame->center.y;
                params.capturePosition[2] = frame->center.z;
                params.prefilterMipCount = static_cast<float>(frame->prefilterMipCount);
            }
        }
        renderer::WriteFrameParameterBlock(fc, kProbeFrameParameterBlock, params);
    }

private:
    std::shared_ptr<ProbeRuntime> m_runtime;
};

class ProbeFeature final : public renderer::IEngineFeature
{
public:
    ProbeFeature()
        : m_runtime(std::make_shared<ProbeRuntime>())
        , m_extraction(std::make_shared<ProbeExtractionStep>(m_runtime))
        , m_contributor(std::make_shared<ProbeFrameConstantsContributor>(m_runtime))
    {}

    std::string_view GetName() const noexcept override { return "krom-probes"; }
    renderer::FeatureID GetID() const noexcept override { return renderer::FeatureID::FromString("krom-probes"); }

    void Register(renderer::FeatureRegistrationContext& context) override
    {
        context.RegisterSceneExtractionStep(m_extraction);
        if (context.RegisterFrameParameterBlock(kProbeFrameParameterBlock))
            context.RegisterFrameConstantsContributor(m_contributor);
    }

    bool Initialize(const renderer::FeatureInitializationContext& context) override
    {
        m_device = &context.device;
        m_runtime->SetDevice(m_device);
        return true;
    }

    void Shutdown(const renderer::FeatureShutdownContext&) override
    {
        if (m_device && m_runtime)
        {
            m_runtime->ReleaseAll(*m_device);
            m_runtime->SetDevice(nullptr);
        }
        m_device = nullptr;
    }

    [[nodiscard]] std::shared_ptr<ProbeRuntime> GetRuntime() const noexcept { return m_runtime; }

private:
    renderer::IDevice* m_device = nullptr;
    std::shared_ptr<ProbeRuntime> m_runtime;
    std::shared_ptr<ProbeExtractionStep> m_extraction;
    std::shared_ptr<ProbeFrameConstantsContributor> m_contributor;
};

} // namespace

std::unique_ptr<renderer::IEngineFeature> CreateProbeFeature(std::shared_ptr<ProbeRuntime>* outRuntime)
{
    auto feature = std::make_unique<ProbeFeature>();
    if (outRuntime)
        *outRuntime = feature->GetRuntime();
    return feature;
}

} // namespace engine::addons::probes
