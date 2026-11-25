#include "SurfaceCacheFeature.hpp"
#include "SurfaceCachePass.hpp"

#include "assets/AssetRegistry.hpp"
#include "core/Debug.hpp"
#include "renderer/IDevice.hpp"
#include "renderer/RenderExtractionContext.hpp"
#include "renderer/RenderFrameTypes.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace engine::renderer::addons::surface_cache {

namespace {

constexpr uint32_t kWorldIdleTicks = 300u;   // a world not traced this long loses its GPU data
constexpr const char* kFrameDataName = "surface_cache.frame";

void DestroyWorldGpu(IDevice& device, SurfaceCacheWorldState& state)
{
    for (BufferHandle* b : { &state.gpu.tlasNodes, &state.gpu.blasNodes, &state.gpu.blasTriangles, &state.gpu.instances })
        if (b->IsValid())
        {
            device.DestroyBuffer(*b);
            *b = {};
        }
    state.gpu = {};
}

bool AnythingAllocated(const SurfaceCacheGpuResources& res) noexcept
{
    return res.programsReady || res.programsFailed || !res.worlds.empty() || res.primary.IsValid();
}

// Uploads the scene BVH when its versions changed (BLAS pool: new or removed
// meshes; TLAS and instances: anything moved, came or went).
void UploadScene(IDevice& device, SurfaceCacheWorldState& state)
{
    const collision::SceneBvh& scene = state.bvh.Scene();
    SurfaceCacheSceneGpu& g = state.gpu;
    if (scene.BlasPoolVersion() != g.uploadedBlasVersion || !g.blasNodes.IsValid() || !g.blasTriangles.IsValid())
    {
        const auto& nodes = scene.BlasNodes();
        const auto& tris = scene.BlasTriangles();
        const bool ok =
            EnsureStructuredBuffer(device, g.blasNodes, g.blasNodeCapacity, nodes.size(), sizeof(collision::BvhNode), false, "SurfaceCache_BlasNodes") &&
            EnsureStructuredBuffer(device, g.blasTriangles, g.blasTriangleCapacity, tris.size(), sizeof(collision::BvhTriangle), false, "SurfaceCache_BlasTriangles");
        if (ok)
        {
            if (!nodes.empty())
                device.UploadBufferData(g.blasNodes, nodes.data(), nodes.size() * sizeof(collision::BvhNode));
            if (!tris.empty())
                device.UploadBufferData(g.blasTriangles, tris.data(), tris.size() * sizeof(collision::BvhTriangle));
            g.uploadedBlasVersion = scene.BlasPoolVersion();
            ++g.blasUploads;
        }
    }
    if (scene.Version() != g.uploadedVersion || !g.tlasNodes.IsValid() || !g.instances.IsValid())
    {
        const auto& nodes = scene.TlasNodes();
        const auto& instances = scene.Instances();
        const bool ok =
            EnsureStructuredBuffer(device, g.tlasNodes, g.tlasCapacity, nodes.size(), sizeof(collision::BvhNode), false, "SurfaceCache_TlasNodes") &&
            EnsureStructuredBuffer(device, g.instances, g.instanceCapacity, instances.size(), sizeof(collision::GpuBvhInstance), false, "SurfaceCache_Instances");
        if (ok)
        {
            if (!nodes.empty())
                device.UploadBufferData(g.tlasNodes, nodes.data(), nodes.size() * sizeof(collision::BvhNode));
            if (!instances.empty())
                device.UploadBufferData(g.instances, instances.data(), instances.size() * sizeof(collision::GpuBvhInstance));
            g.uploadedVersion = scene.Version();
            g.tlasNodeCount = static_cast<uint32_t>(nodes.size());
            g.instanceCount = scene.InstanceCount();
            g.triangleCount = scene.InstancedTriangleCount();
            ++g.uploads;
        }
        else
        {
            g.tlasNodeCount = 0u;   // trace nothing rather than stale data
        }
    }
}

class SurfaceCacheExtractionStep final : public ISceneExtractionStep
{
public:
    explicit SurfaceCacheExtractionStep(std::shared_ptr<SurfaceCacheGpuResources> resources)
        : m_resources(std::move(resources)) {}

    std::string_view GetName() const noexcept override { return "surface_cache.extract"; }

    void Extract(const SceneExtractionContext& ctx) const override
    {
        auto& frame = ctx.GetOrCreateFrameData<SurfaceCacheFrameData>(kFrameDataName);
        frame = SurfaceCacheFrameData{};
        SurfaceCacheGpuResources* res = m_resources.get();
        if (!res || !res->device)
            return;
        IDevice& device = *res->device;
        if (!res->settings.enabled || !res->supported)
        {
            // Off: give everything back, so an unused feature costs nothing.
            if (AnythingAllocated(*res))
            {
                ReleaseSurfaceCacheGpu(device, *res);
                res->programsFailed = false;
                Debug::Log("SurfaceCacheFeature.cpp: surface cache off, GPU data released");
            }
            return;
        }
        // Main scene and game views only (they allow the depth prepass);
        // previews, captures and probe bakes have their own worlds.
        if (!ctx.view || !ctx.view->enableDepthPrepass || !ctx.assetRegistry)
            return;
        if (!EnsureSurfaceCachePrograms(device, *res))
            return;

        std::shared_ptr<SurfaceCacheWorldState>& slot = res->worlds[&ctx.world];
        if (!slot)
            slot = std::make_shared<SurfaceCacheWorldState>();
        slot->idleExtractions = 0u;
        slot->bvh.SetJobSystem(ctx.jobSystem);
        slot->bvh.Update(ctx.world, *ctx.assetRegistry);
        UploadScene(device, *slot);
        frame.world = slot;

        res->lastInstances = slot->gpu.instanceCount;
        res->lastTriangles = slot->gpu.triangleCount;
        res->lastPendingBlas = slot->bvh.PendingBlasBuilds();
        res->lastVersion = slot->bvh.Scene().Version();
        res->lastUploads = slot->gpu.uploads;
    }

private:
    std::shared_ptr<SurfaceCacheGpuResources> m_resources;
};

class SurfaceCacheFeature final : public IEngineFeature
{
public:
    SurfaceCacheFeature()
        : m_resources(std::make_shared<SurfaceCacheGpuResources>())
        , m_extraction(std::make_shared<SurfaceCacheExtractionStep>(m_resources))
        , m_compute(std::make_shared<SurfaceCacheComputePass>(m_resources))
        , m_overlay(std::make_shared<SurfaceCacheOverlayPass>(m_resources))
    {}

    std::string_view GetName() const noexcept override { return "krom-surface-cache"; }
    FeatureID GetID() const noexcept override { return FeatureID::FromString("krom-surface-cache"); }

    void Register(FeatureRegistrationContext& context) override
    {
        context.RegisterSceneExtractionStep(m_extraction);
        context.RegisterPassContributor(m_compute);
        context.RegisterPassContributor(m_overlay);
    }

    bool Initialize(const FeatureInitializationContext& context) override
    {
        m_resources->device = &context.device;
        const ComputeRuntimeDesc compute = context.device.GetComputeRuntime();
        m_resources->supported = compute.computePipelinesSupported && compute.computeDispatchSupported;
        if (!m_resources->supported)
            Debug::LogWarning("SurfaceCacheFeature.cpp: backend without compute, surface cache not available");
        return true;
    }

    void Shutdown(const FeatureShutdownContext&) override
    {
        if (m_resources && m_resources->device)
            ReleaseSurfaceCacheGpu(*m_resources->device, *m_resources);
        if (m_resources)
            m_resources->device = nullptr;
    }

    [[nodiscard]] std::shared_ptr<SurfaceCacheGpuResources> GetResources() const noexcept { return m_resources; }

private:
    std::shared_ptr<SurfaceCacheGpuResources>  m_resources;
    std::shared_ptr<SurfaceCacheExtractionStep> m_extraction;
    std::shared_ptr<SurfaceCacheComputePass>    m_compute;
    std::shared_ptr<SurfaceCacheOverlayPass>    m_overlay;
};

const char* KindName(SurfaceCacheRayKind kind) noexcept
{
    switch (kind)
    {
    case SurfaceCacheRayKind::Primary:   return "primary";
    case SurfaceCacheRayKind::Diffuse:   return "diffuse";
    case SurfaceCacheRayKind::Occlusion: return "occlusion";
    default:                             return "validate";
    }
}

// stats: the counters of SurfaceCacheGpuResources::stats. Secondary rays start
// only where the primary ray of the same resolution hit, so their ray count is
// the average number of hits of that primary pass.
void FinishBench(SurfaceCacheGpuResources& res, const uint32_t* stats)
{
    SurfaceCacheBench& bench = res.bench;
    bench.results.clear();
    const auto average = [&](size_t slot) -> double
    {
        const uint32_t dispatches = stats[3u + 2u * slot];
        return dispatches > 0u ? static_cast<double>(stats[2u + 2u * slot]) / dispatches : 0.0;
    };
    for (size_t i = 0u; i < bench.passes.size(); ++i)
    {
        const SurfaceCacheBenchPass& p = bench.passes[i];
        const double ms = bench.samples[i] > 0u ? bench.sumMs[i] / bench.samples[i] : 0.0;
        double rays = static_cast<double>(p.width) * p.height;
        if (p.kind != SurfaceCacheRayKind::Primary)
            for (size_t k = 0u; k < bench.passes.size(); ++k)
                if (bench.passes[k].kind == SurfaceCacheRayKind::Primary &&
                    bench.passes[k].width == p.width && bench.passes[k].height == p.height)
                    rays = average(k);
        const double hits = average(i);
        char tMax[16];
        if (p.tMax > 0.0f)
            std::snprintf(tMax, sizeof(tMax), "%.0fm", p.tMax);
        else
            std::snprintf(tMax, sizeof(tMax), "inf");
        char line[640];
        std::snprintf(line, sizeof(line),
            "SurfaceCacheFeature.cpp: GI bench backend=%s scene=%s res=%ux%u kind=%s tMax=%s rays/frame=%.0f "
            "hit=%.1f%% ms=%.3f rays/ms=%.0f instances=%u triangles=%llu maxStack=%u overflows=%u samples=%u",
            bench.backend.c_str(), bench.label.c_str(), p.width, p.height, KindName(p.kind), tMax, rays,
            rays > 0.0 ? 100.0 * hits / rays : 0.0, ms, ms > 0.0 ? rays / ms : 0.0, bench.instances,
            static_cast<unsigned long long>(bench.triangles), stats[0], stats[1], bench.samples[i]);
        bench.results.emplace_back(line);
        Debug::Log("%s", line);
    }
    bench.state = SurfaceCacheBench::State::Idle;
}

void FinishValidation(SurfaceCacheGpuResources& res)
{
    SurfaceCacheValidation& v = res.validation;
    v.state = SurfaceCacheValidation::State::Idle;
    const std::shared_ptr<SurfaceCacheWorldState> world = v.world.lock();
    if (!world || world->bvh.Scene().GeometryVersion() != v.version)
    {
        v.result = "SurfaceCacheFeature.cpp: GPU/CPU check skipped, the scene changed meanwhile - run it again";
        Debug::LogWarning("%s", v.result.c_str());
        return;
    }
    const void* mapped = res.device->MapBuffer(res.readback[1]);
    if (!mapped)
    {
        v.result = "SurfaceCacheFeature.cpp: GPU/CPU check failed, readback not mappable";
        Debug::LogError("%s", v.result.c_str());
        return;
    }
    std::vector<uint32_t> words(SurfaceCacheValidation::kRays * SurfaceCacheValidation::kWordsPerRay);
    std::memcpy(words.data(), mapped, words.size() * sizeof(uint32_t));
    res.device->UnmapBuffer(res.readback[1]);

    const auto asFloat = [](uint32_t u) { float f; std::memcpy(&f, &u, sizeof(f)); return f; };
    uint32_t hits = 0u, mismatches = 0u, gpuOverflows = 0u;
    float maxDiff = 0.0f;
    for (uint32_t i = 0u; i < SurfaceCacheValidation::kRays; ++i)
    {
        const uint32_t* w = words.data() + i * SurfaceCacheValidation::kWordsPerRay;
        const math::Vec3 origin{ asFloat(w[0]), asFloat(w[1]), asFloat(w[2]) };
        const math::Vec3 dir{ asFloat(w[3]), asFloat(w[4]), asFloat(w[5]) };
        const bool gpuHit = w[6] != 0xFFFFFFFFu;
        gpuOverflows += w[11];
        collision::SceneRayHit cpu{};
        const bool cpuHit = world->bvh.Scene().TraceClosest(origin, dir, 0.0f, 1e30f, cpu);
        hits += gpuHit ? 1u : 0u;
        if (gpuHit != cpuHit)
        {
            ++mismatches;
            continue;
        }
        if (!gpuHit)
            continue;
        const float gpuT = asFloat(w[6]);
        const float diff = std::fabs(gpuT - cpu.distance);
        maxDiff = std::max(maxDiff, diff);
        const bool sameSurface = w[9] == cpu.entity || diff < 1e-4f;
        if (diff > 1e-3f * std::max(1.0f, cpu.distance) || !sameSurface)
            ++mismatches;
    }
    char line[256];
    std::snprintf(line, sizeof(line),
        "SurfaceCacheFeature.cpp: GPU/CPU check: %u rays, %u hits, %u mismatches, max |dt| %.6f, GPU stack overflows %u",
        SurfaceCacheValidation::kRays, hits, mismatches, maxDiff, gpuOverflows);
    v.result = line;
    if (mismatches == 0u)
        Debug::Log("%s", line);
    else
        Debug::LogWarning("%s", line);
}

} // namespace

std::unique_ptr<IEngineFeature> CreateSurfaceCacheFeature(std::shared_ptr<SurfaceCacheGpuResources>* outResources)
{
    auto feature = std::make_unique<SurfaceCacheFeature>();
    if (outResources)
        *outResources = feature->GetResources();
    return feature;
}

void StartSurfaceCacheBench(SurfaceCacheGpuResources& res, std::string backend, std::string label)
{
    SurfaceCacheBench& bench = res.bench;
    bench = SurfaceCacheBench{};
    bench.state = SurfaceCacheBench::State::Warmup;
    bench.passes = MakeBenchPasses();
    bench.sumMs.assign(bench.passes.size(), 0.0);
    bench.samples.assign(bench.passes.size(), 0u);
    bench.backend = std::move(backend);
    bench.label = std::move(label);
    res.statsReadbackPending = false;
    res.statsCopied = false;
    if (res.device && res.stats.IsValid())
    {
        const std::array<uint32_t, SurfaceCacheGpuResources::kStatsWords> zeros{};
        res.device->UploadBufferData(res.stats, zeros.data(), sizeof(zeros));
    }
    Debug::Log("SurfaceCacheFeature.cpp: GI bench started (%u passes, %u warm-up + %u frames)",
               static_cast<uint32_t>(bench.passes.size()), SurfaceCacheBench::kWarmupFrames, SurfaceCacheBench::kFrames);
}

void RequestSurfaceCacheValidation(SurfaceCacheGpuResources& res)
{
    if (res.validation.state == SurfaceCacheValidation::State::Idle)
    {
        res.validation.state = SurfaceCacheValidation::State::Requested;
        res.validation.result.clear();
    }
}

bool SurfaceCacheWantsGpuTimings(const SurfaceCacheGpuResources& res) noexcept
{
    return res.bench.state == SurfaceCacheBench::State::Warmup || res.bench.state == SurfaceCacheBench::State::Measuring;
}

bool SurfaceCacheBenchRunning(const SurfaceCacheGpuResources& res) noexcept
{
    return res.bench.state != SurfaceCacheBench::State::Idle;
}

void TickSurfaceCache(SurfaceCacheGpuResources& res, const GpuPassTimingReport& report)
{
    ++res.frameCounter;

    // Worlds no view traced for a while (closed preview, ended Play) give their data back.
    if (res.device)
        for (auto it = res.worlds.begin(); it != res.worlds.end();)
        {
            if (it->second && ++it->second->idleExtractions > kWorldIdleTicks)
            {
                DestroyWorldGpu(*res.device, *it->second);
                it = res.worlds.erase(it);
            }
            else
                ++it;
        }

    SurfaceCacheBench& bench = res.bench;
    if (!res.settings.enabled && bench.state != SurfaceCacheBench::State::Idle)
    {
        Debug::LogWarning("SurfaceCacheFeature.cpp: GI bench aborted, surface cache was switched off");
        bench.state = SurfaceCacheBench::State::Idle;
    }
    switch (bench.state)
    {
    case SurfaceCacheBench::State::Warmup:
        if (++bench.frame >= SurfaceCacheBench::kWarmupFrames)
        {
            bench.state = SurfaceCacheBench::State::Measuring;
            bench.frame = 0u;
        }
        break;
    case SurfaceCacheBench::State::Measuring:
        for (size_t i = 0u; i < bench.passes.size(); ++i)
            for (const GpuPassTiming& timing : report.passes)
                if (timing.name == bench.passes[i].name && timing.ms > 0.0f)
                {
                    bench.sumMs[i] += timing.ms;
                    ++bench.samples[i];
                    break;
                }
        bench.instances = res.lastInstances;
        bench.triangles = res.lastTriangles;
        if (++bench.frame >= SurfaceCacheBench::kFrames)
        {
            bench.state = SurfaceCacheBench::State::Readback;
            res.statsReadbackPending = true;
            res.statsCopied = false;
            if (!report.supported)
                Debug::LogWarning("SurfaceCacheFeature.cpp: backend has no GPU timestamps, bench times are 0");
        }
        break;
    case SurfaceCacheBench::State::Readback:
        if (res.statsCopied)
        {
            if (res.statsReadbackWait > 0u)
                --res.statsReadbackWait;
            if (res.statsReadbackWait == 0u)
            {
                std::array<uint32_t, SurfaceCacheGpuResources::kStatsWords> stats{};
                if (const void* mapped = res.device ? res.device->MapBuffer(res.readback[0]) : nullptr)
                {
                    std::memcpy(stats.data(), mapped, sizeof(stats));
                    res.device->UnmapBuffer(res.readback[0]);
                }
                res.lastMaxStack = stats[0];
                res.lastOverflows = stats[1];
                res.statsReadbackPending = false;
                res.statsCopied = false;
                FinishBench(res, stats.data());
            }
        }
        break;
    default:
        break;
    }

    SurfaceCacheValidation& v = res.validation;
    if (v.state == SurfaceCacheValidation::State::Dispatched)
    {
        if (v.waitFrames > 0u)
            --v.waitFrames;
        if (v.waitFrames == 0u && res.device)
            FinishValidation(res);
    }
}

} // namespace engine::renderer::addons::surface_cache
