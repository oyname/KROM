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
#include <cmath>
#include <cstring>

namespace engine::addons::probes {

using renderer::TexSlots;

namespace {

float HalfToFloat(uint16_t h) noexcept
{
    const uint32_t exp = (h >> 10u) & 0x1Fu;
    const uint32_t mant = h & 0x3FFu;
    float v = 0.f;
    if (exp == 0u)
        v = std::ldexp(static_cast<float>(mant), -24);
    else if (exp == 31u)
        v = 0.f;   // inf/nan never come out of the baker; treat as black
    else
        v = std::ldexp(static_cast<float>(mant | 0x400u), static_cast<int>(exp) - 25);
    return (h & 0x8000u) ? -v : v;
}

// Same cube convention as IBLBaker (D3D: +X face has +Y up, -Z right).
math::Vec3 DirectionFromCubemapFace(uint32_t face, float u, float v) noexcept
{
    const float sx = 2.f * u - 1.f;
    const float sy = 2.f * v - 1.f;
    math::Vec3 d{};
    switch (face)
    {
    case 0: d = {  1.f, -sy, -sx }; break;
    case 1: d = { -1.f, -sy,  sx }; break;
    case 2: d = {  sx,  1.f,  sy }; break;
    case 3: d = {  sx, -1.f, -sy }; break;
    case 4: d = {  sx, -sy,  1.f }; break;
    default: d = { -sx, -sy, -1.f }; break;
    }
    return d.Normalized();
}

void ShBasis(const math::Vec3& n, float out[9]) noexcept
{
    out[0] = 0.282095f;
    out[1] = 0.488603f * n.y;
    out[2] = 0.488603f * n.z;
    out[3] = 0.488603f * n.x;
    out[4] = 1.092548f * n.x * n.y;
    out[5] = 1.092548f * n.y * n.z;
    out[6] = 0.315392f * (3.f * n.z * n.z - 1.f);
    out[7] = 1.092548f * n.x * n.z;
    out[8] = 0.546274f * (n.x * n.x - n.y * n.y);
}

// Projects the baked irradiance cube (RGBA16F, 6 faces) onto SH L2. The cube
// is already convolved, so evaluating the SH gives the irradiance directly.
std::array<float, 27> ProjectIrradianceSh(const std::vector<uint8_t>& cube, uint32_t size)
{
    std::array<float, 27> sh{};
    if (size == 0u || cube.size() < static_cast<size_t>(6u) * size * size * 8u)
        return sh;
    double acc[27]{};
    double weightSum = 0.0;
    const auto* texels = reinterpret_cast<const uint16_t*>(cube.data());
    for (uint32_t f = 0u; f < 6u; ++f)
        for (uint32_t y = 0u; y < size; ++y)
            for (uint32_t x = 0u; x < size; ++x)
            {
                const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(size);
                const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(size);
                const float su = 2.f * u - 1.f;
                const float sv = 2.f * v - 1.f;
                const float tmp = 1.f + su * su + sv * sv;
                const double dOmega = 4.0 / (static_cast<double>(size) * size * tmp * std::sqrt(tmp));
                float basis[9];
                ShBasis(DirectionFromCubemapFace(f, u, v), basis);
                const uint16_t* px = texels + ((static_cast<size_t>(f) * size + y) * size + x) * 4u;
                const float rgb[3] = { HalfToFloat(px[0]), HalfToFloat(px[1]), HalfToFloat(px[2]) };
                for (int k = 0; k < 9; ++k)
                    for (int c = 0; c < 3; ++c)
                        acc[k * 3 + c] += rgb[c] * basis[k] * dOmega;
                weightSum += dOmega;
            }
    // Normalise the summed solid angle to exactly 4 pi.
    const double norm = weightSum > 0.0 ? (4.0 * 3.14159265358979 / weightSum) : 0.0;
    for (int i = 0; i < 27; ++i)
        sh[static_cast<size_t>(i)] = static_cast<float>(acc[i] * norm);
    return sh;
}

} // namespace

// =============================================================================
// ProbeRuntime
// =============================================================================

void ProbeRuntime::SetAssetRoot(std::string assetRootUtf8)
{
    std::lock_guard lock(m_mutex);
    if (m_assetRoot != assetRootUtf8)
        m_files.clear();   // relative paths now point elsewhere; GPU data follows on Prepare
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

void ProbeRuntime::SetSettings(const ProbeSettings& settings) noexcept
{
    std::lock_guard lock(m_mutex);
    m_settings = settings;
}

ProbeSettings ProbeRuntime::GetSettings() const noexcept
{
    std::lock_guard lock(m_mutex);
    return m_settings;
}

void ProbeRuntime::SetEnabled(bool enabled) noexcept
{
    std::lock_guard lock(m_mutex);
    m_settings.enabled = enabled;
}

bool ProbeRuntime::IsEnabled() const noexcept
{
    std::lock_guard lock(m_mutex);
    return m_settings.enabled;
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

const ProbeRuntime::FileEntry* ProbeRuntime::LoadFileLocked(const std::string& relativeUtf8)
{
    if (const auto it = m_files.find(relativeUtf8); it != m_files.end())
        return it->second.failed ? nullptr : &it->second;

    const std::string path = m_assetRoot.empty()
        ? relativeUtf8
        : PathToUtf8(PathFromUtf8(m_assetRoot) / PathFromUtf8(relativeUtf8));
    FileEntry entry{};
    ProbeBakedData data{};
    std::string error;
    if (!ReadProbeFile(path, data, &error))
    {
        Debug::LogWarning("ProbeFeature.cpp: probe '%s' not loaded: %s", relativeUtf8.c_str(), error.c_str());
        entry.failed = true;
    }
    else
    {
        entry.prefilterBaseSize = data.prefilterBaseSize;
        entry.prefilterMipCount = data.prefilterMipCount;
        entry.prefilterData = std::move(data.prefilterData);
        entry.sh = ProjectIrradianceSh(data.irradianceData, data.irradianceSize);
    }
    auto [it, inserted] = m_files.emplace(relativeUtf8, std::move(entry));
    (void)inserted;
    return it->second.failed ? nullptr : &it->second;
}

void ProbeRuntime::DestroyGpuLocked(renderer::IDevice& device)
{
    if (m_cubeArray.IsValid())
        device.DestroyTexture(m_cubeArray);
    if (m_dataTexture.IsValid())
        device.DestroyTexture(m_dataTexture);
    m_cubeArray = TextureHandle::Invalid();
    m_dataTexture = TextureHandle::Invalid();
    m_arrayPaths.clear();
    m_arraySize = 0u;
    m_arrayMips = 0u;
    m_uploadedRows.clear();
}

void ProbeRuntime::InvalidateFile(renderer::IDevice& device, const std::string& relativeUtf8)
{
    std::lock_guard lock(m_mutex);
    m_files.erase(relativeUtf8);
    if (std::find(m_arrayPaths.begin(), m_arrayPaths.end(), relativeUtf8) != m_arrayPaths.end())
    {
        if (m_cubeArray.IsValid())
            device.DestroyTexture(m_cubeArray);
        m_cubeArray = TextureHandle::Invalid();
        m_arrayPaths.clear();
        m_uploadedRows.clear();
    }
}

void ProbeRuntime::ReleaseAll(renderer::IDevice& device)
{
    std::lock_guard lock(m_mutex);
    DestroyGpuLocked(device);
    m_files.clear();
}

ProbeRuntime::GpuState ProbeRuntime::Prepare(renderer::IDevice& device, const std::vector<ActiveProbe>& probes)
{
    std::lock_guard lock(m_mutex);
    GpuState state{};

    // Probes with loaded data; all cubes share one size (the first probe's).
    struct Usable { const ActiveProbe* probe; const FileEntry* file; };
    std::vector<Usable> usable;
    uint32_t size = 0u;
    uint32_t mips = 0u;
    for (const ActiveProbe& probe : probes)
    {
        if (usable.size() >= kMaxActiveProbes)
            break;
        const FileEntry* file = LoadFileLocked(probe.path);
        if (!file)
            continue;
        if (size == 0u)
        {
            size = file->prefilterBaseSize;
            mips = file->prefilterMipCount;
        }
        if (file->prefilterBaseSize != size || file->prefilterMipCount != mips)
        {
            static bool warned = false;
            if (!warned)
            {
                warned = true;
                Debug::LogWarning("ProbeFeature.cpp: probe '%s' has another resolution than the others, skipped; "
                                  "bake all probes of a scene with the same resolution", probe.path.c_str());
            }
            continue;
        }
        usable.push_back({ &probe, file });
    }
    if (usable.empty())
        return state;

    // Cube array: one cube per distinct file, rebuilt when the set changes.
    std::vector<std::string> paths;
    for (const Usable& u : usable)
        if (std::find(paths.begin(), paths.end(), u.probe->path) == paths.end())
            paths.push_back(u.probe->path);
    if (paths != m_arrayPaths || !m_cubeArray.IsValid())
    {
        // Rare (probe added, removed or baked): wait so the old array is free.
        device.WaitIdle();
        if (m_cubeArray.IsValid())
            device.DestroyTexture(m_cubeArray);
        renderer::TextureDesc desc{};
        desc.width = size;
        desc.height = size;
        desc.mipLevels = mips;
        desc.arraySize = static_cast<uint32_t>(paths.size());
        desc.dimension = renderer::TextureDimension::CubemapArray;
        desc.format = renderer::Format::RGBA16_FLOAT;
        desc.usage = renderer::ResourceUsage::ShaderResource | renderer::ResourceUsage::CopyDest;
        desc.initialState = renderer::ResourceState::ShaderRead;
        desc.debugName = "ProbeCubeArray";
        m_cubeArray = device.CreateTexture(desc);
        m_arrayPaths.clear();
        m_uploadedRows.clear();
        if (!m_cubeArray.IsValid())
        {
            Debug::LogError("ProbeFeature.cpp: failed to create the probe cube array (%zu cubes)", paths.size());
            return state;
        }
        for (uint32_t cube = 0u; cube < paths.size(); ++cube)
        {
            const FileEntry& file = m_files.at(paths[cube]);
            size_t offset = 0u;
            uint32_t mipSize = size;
            for (uint32_t mip = 0u; mip < mips; ++mip)
            {
                const size_t faceBytes = static_cast<size_t>(mipSize) * mipSize * 4u * sizeof(uint16_t);
                for (uint32_t face = 0u; face < 6u; ++face)
                {
                    device.UploadTextureData(m_cubeArray, file.prefilterData.data() + offset, faceBytes,
                                             mip, cube * 6u + face);
                    offset += faceBytes;
                }
                mipSize = std::max(mipSize / 2u, 1u);
            }
        }
        m_arrayPaths = paths;
        m_arraySize = static_cast<uint32_t>(paths.size());
        m_arrayMips = mips;
    }

    // Data rows.
    std::vector<float> rows(static_cast<size_t>(kMaxActiveProbes) * kProbeDataTexelsPerProbe * 4u, 0.f);
    for (uint32_t i = 0u; i < usable.size(); ++i)
    {
        const ActiveProbe& p = *usable[i].probe;
        float* row = rows.data() + static_cast<size_t>(i) * kProbeDataTexelsPerProbe * 4u;
        const math::Vec3 axes[3] = {
            p.rotation.Rotate({ 1.f, 0.f, 0.f }),
            p.rotation.Rotate({ 0.f, 1.f, 0.f }),
            p.rotation.Rotate({ 0.f, 0.f, 1.f }),
        };
        for (int a = 0; a < 3; ++a)
        {
            row[a * 4 + 0] = axes[a].x;
            row[a * 4 + 1] = axes[a].y;
            row[a * 4 + 2] = axes[a].z;
            row[a * 4 + 3] = -math::Vec3::Dot(axes[a], p.position);
        }
        row[12] = p.extents.x; row[13] = p.extents.y; row[14] = p.extents.z;
        row[15] = std::max(p.blendDistance, 1e-3f);
        row[16] = 0.f; row[17] = 0.f; row[18] = 0.f;   // capture point = box centre
        row[19] = p.intensity;
        row[20] = p.boxProjection ? 1.f : 0.f;
        const auto cube = std::find(m_arrayPaths.begin(), m_arrayPaths.end(), p.path) - m_arrayPaths.begin();
        row[21] = static_cast<float>(cube);
        const std::array<float, 27>& sh = usable[i].file->sh;
        for (uint32_t k = 0u; k < 9u; ++k)
        {
            float* t = row + (kProbeDataShTexel + k) * 4u;
            t[0] = sh[k * 3u + 0u];
            t[1] = sh[k * 3u + 1u];
            t[2] = sh[k * 3u + 2u];
        }
    }

    if (!m_dataTexture.IsValid())
    {
        renderer::TextureDesc desc{};
        desc.width = kProbeDataTexelsPerProbe;
        desc.height = kMaxActiveProbes;
        desc.format = renderer::Format::RGBA32_FLOAT;
        desc.usage = renderer::ResourceUsage::ShaderResource | renderer::ResourceUsage::CopyDest;
        desc.initialState = renderer::ResourceState::ShaderRead;
        desc.debugName = "ProbeData";
        m_dataTexture = device.CreateTexture(desc);
        m_uploadedRows.clear();
        if (!m_dataTexture.IsValid())
        {
            Debug::LogError("ProbeFeature.cpp: failed to create the probe data texture");
            return state;
        }
    }
    if (rows != m_uploadedRows)
    {
        // Changes only when probes move or change; wait so no frame in flight
        // still reads the old rows.
        if (!m_uploadedRows.empty())
            device.WaitIdle();
        device.UploadTextureData(m_dataTexture, rows.data(), rows.size() * sizeof(float), 0u, 0u);
        m_uploadedRows = std::move(rows);
    }

    state.probeCount = static_cast<uint32_t>(usable.size());
    state.prefilterMipCount = m_arrayMips;
    state.cubeArray = m_cubeArray;
    state.data = m_dataTexture;
    return state;
}

// =============================================================================
// Feature
// =============================================================================
namespace {

// What the extraction step selected for this view.
struct ProbeFrameData
{
    uint32_t probeCount = 0u;
    uint32_t prefilterMipCount = 0u;
    ProbeSettings settings{};
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

        TextureHandle cubeArray = TextureHandle::Invalid();
        TextureHandle data = TextureHandle::Invalid();
        if (m_runtime)
        {
            frame.settings = m_runtime->GetSettings();
            renderer::IDevice* device = m_runtime->GetDevice();
            if (device && frame.settings.enabled && !m_runtime->IsViewSuppressed(ctx.view))
            {
                struct Candidate { ProbeRuntime::ActiveProbe probe; int32_t priority; float volume; uint32_t entity; };
                std::vector<Candidate> candidates;
                ctx.world.View<ReflectionProbeComponent, WorldTransformComponent>(
                    [&](EntityID entity, const ReflectionProbeComponent& c, const WorldTransformComponent& xf)
                    {
                        if (c.bakedDataPath.empty() || !renderer::ECSExtractor::IsEntityActive(ctx.world, entity))
                            return;
                        Candidate cand{};
                        cand.probe.path = c.bakedDataPath;
                        cand.probe.position = xf.position;
                        cand.probe.rotation = xf.rotation.Normalized();
                        cand.probe.extents = c.extents;
                        cand.probe.blendDistance = c.blendDistance;
                        cand.probe.intensity = c.intensity;
                        cand.probe.boxProjection = c.boxProjection;
                        cand.priority = c.priority;
                        cand.volume = c.extents.x * c.extents.y * c.extents.z;
                        cand.entity = entity.value;
                        candidates.push_back(std::move(cand));
                    });
                // Higher priority first, then the smaller box, then entity id (stable).
                std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
                    if (a.priority != b.priority) return a.priority > b.priority;
                    if (a.volume != b.volume) return a.volume < b.volume;
                    return a.entity < b.entity;
                });
                std::vector<ProbeRuntime::ActiveProbe> ordered;
                ordered.reserve(candidates.size());
                for (Candidate& c : candidates)
                    ordered.push_back(std::move(c.probe));
                if (!ordered.empty())
                {
                    const ProbeRuntime::GpuState gpu = m_runtime->Prepare(*device, ordered);
                    frame.probeCount = gpu.probeCount;
                    frame.prefilterMipCount = gpu.prefilterMipCount;
                    if (gpu.probeCount > 0u)
                    {
                        cubeArray = gpu.cubeArray;
                        data = gpu.data;
                    }
                }
            }
        }

        // Publish the slots every frame; invalid handles clear them.
        bindings.SetTexture(TexSlots::FeatureCubeArray, cubeArray);
        bindings.SetTexture(TexSlots::FeatureTexture, data);
    }

private:
    std::shared_ptr<ProbeRuntime> m_runtime;
};

class ProbeFrameConstantsContributor final : public renderer::IFrameConstantsContributor
{
public:
    std::string_view GetName() const noexcept override { return "krom-probes-constants"; }

    void Contribute(const renderer::FrameConstantsContributionContext& ctx,
                    renderer::FrameConstants& fc) const override
    {
        ProbeFrameParams params{};
        if (const ProbeFrameData* frame = ctx.frameData.GetFrameData<ProbeFrameData>())
        {
            params.probeCount = frame->probeCount;
            params.prefilterMipCount = static_cast<float>(frame->prefilterMipCount);
            if (frame->settings.specularOcclusion)
                params.flags |= kProbeFlagSpecularOcclusion;
            params.flags |= (static_cast<uint32_t>(frame->settings.debugView) & 0xFFu) << kProbeFlagDebugShift;
        }
        renderer::WriteFrameParameterBlock(fc, kProbeFrameParameterBlock, params);
    }
};

class ProbeFeature final : public renderer::IEngineFeature
{
public:
    ProbeFeature()
        : m_runtime(std::make_shared<ProbeRuntime>())
        , m_extraction(std::make_shared<ProbeExtractionStep>(m_runtime))
        , m_contributor(std::make_shared<ProbeFrameConstantsContributor>())
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
