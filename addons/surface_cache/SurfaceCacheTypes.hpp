#pragma once
// =============================================================================
// KROM Engine - addons/surface_cache/SurfaceCacheTypes.hpp
//
// Surface cache GI (doc/PLAN_GI.md): the scene BVH on the GPU, a compute trace
// kernel (shaders/surface_cache_trace.hlsl), a debug view and a measuring mode
// (package 2); the radiance cache in world space (package 3,
// shaders/surface_cache_update.hlsl); its resolve into a half-resolution
// texture the PBR shaders take as indirect diffuse light (package 4,
// shaders/surface_cache_apply.hlsl, shaders/pbr_gi.hlsl).
//
// Off by default. While off nothing is built, uploaded or added to the frame
// graph; switching it off releases every GPU object again.
// =============================================================================
#include "addons/mesh_renderer/MeshSceneBvh.hpp"
#include "renderer/FrameRingBuffer.hpp"
#include "renderer/IDevice.hpp"
#include "renderer/RenderFrameConstants.hpp"
#include "renderer/RenderPipelineTypes.hpp"
#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine::ecs { class World; }

namespace engine::renderer::addons::surface_cache {

enum class SurfaceCacheDebugView : uint32_t
{
    None = 0,
    Normal,        // world normal of the primary hit
    Distance,      // hit distance
    InstanceColor, // one colour per instance
    NodeHeatmap,   // BVH nodes visited by the primary ray
    CacheCells,    // one colour per cache cell
    CacheIrradiance, // irradiance stored in the cell (/ pi, tonemapped)
    CacheSamples,  // sample weight of the cell (blue: new, red: full history)
    // Package 4: drawn by the PBR shaders (pbr_gi.hlsl), not by the overlay.
    ShadedIrradiance, // GI irradiance at the pixel (/ pi)
    ShadedIndirect,   // indirect diffuse light as shaded (with AO and GTAO)
    ShadedCoverage,   // green: cache, red: far field, blue: GI not applied
    ShadedSpecularScale, // factor on the environment reflection (white 1, black 0)
    Count,
};

struct SurfaceCacheSettings
{
    bool                  enabled   = false;
    SurfaceCacheDebugView debugView = SurfaceCacheDebugView::Normal;
    float                 overlay   = 1.0f;   // 1 = debug view only, 0 = scene only
    float                 distanceRange = 50.0f;   // metres shown white in the distance view

    // Radiance cache (package 3).
    uint32_t rayBudget      = 128000u;  // cache rays per frame
    float    rayLength      = 0.0f;     // m, 0 = unbounded; beyond it local probes, the sky only if nothing lies behind
    float    cellSize       = 0.5f;     // m, cell size near the camera
    float    levelDistance  = 8.0f;     // m from the camera with the smallest cells; doubles per level
    uint32_t maxSamples     = 256u;     // history length (sample weight cap)
    uint32_t maxRaysPerCell = 32u;      // per visible cell and frame (cells outside the view: a quarter)
    bool     useCascades    = true;     // sun visibility from the CSM where it covers the hit (else rays)
    uint32_t registerStep   = 4u;       // one registration ray per 4 x 4 pixels
    uint32_t maxAge         = 300u;     // frames without registration until a cell is freed

    // Shading (package 4).
    bool     apply          = true;     // the PBR shaders take the cache as indirect diffuse light
    float    strength       = 1.0f;     // 0 = probe/IBL only, 1 = GI, > 1 exaggerates the difference
    float    depthTolerance = 0.1f;     // upsampling: relative view-depth difference still taken
};

// The debug views the PBR shaders draw themselves (giDebugView, pbr_gi.hlsl).
[[nodiscard]] constexpr bool IsShadedDebugView(SurfaceCacheDebugView view) noexcept
{
    return static_cast<uint32_t>(view) >= static_cast<uint32_t>(SurfaceCacheDebugView::ShadedIrradiance) &&
           view != SurfaceCacheDebugView::Count;
}

// FrameConstants::featureParams[8] (shaders/pbr_gi.hlsl).
struct SurfaceCacheFrameParams
{
    uint32_t flags = 0u;           // kSurfaceCacheShadeFlag*
    float    strength = 1.0f;
    uint32_t debugView = 0u;       // 0 none, 1 irradiance, 2 indirect diffuse, 3 coverage, 4 specular scale
    float    depthTolerance = 0.1f;
};
static_assert(sizeof(SurfaceCacheFrameParams) == 16u, "SurfaceCacheFrameParams layout");
inline constexpr uint32_t kSurfaceCacheShadeFlagEnabled = 1u << 0u;
inline constexpr FrameParameterBlockDesc kSurfaceCacheFrameParameterBlock{ "krom-surface-cache", 8u, 1u };

// GPU layout of the dispatch constants of the cache kernels and the overlay
// (cbuffer SurfaceCacheParams, shaders/surface_cache_common.hlsl).
struct SurfaceCacheConstants
{
    uint32_t capacityMask = 0u;
    uint32_t frame = 0u;
    uint32_t rayBudget = 0u;
    uint32_t tlasNodeCount = 0u;
    float    cellSize = 0.5f;
    float    levelDistance = 8.0f;
    float    rayLength = 0.0f;
    uint32_t maxRaysPerCell = 0u;
    uint32_t viewportWidth = 0u;
    uint32_t viewportHeight = 0u;
    uint32_t registerStep = 4u;
    uint32_t dirtyBoxCount = 0u;
    uint32_t maxAge = 0u;
    uint32_t flags = 0u;          // kSurfaceCacheFlag*
    uint32_t maxSamples = 0u;
    uint32_t materialCount = 0u;
    uint32_t materialIndex = 0u;
    uint32_t view = 0u;           // overlay
    float    overlayAlpha = 1.0f;
    float    viewScale = 50.0f;   // overlay: distance range or irradiance exposure
    float    albedoFactor[4] = { 1.0f, 1.0f, 1.0f, 0.0f };    // w: albedo texture bound
    float    emissionFactor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };  // w: emission texture bound
};
static_assert(sizeof(SurfaceCacheConstants) == 112u, "SurfaceCacheConstants layout");

inline constexpr uint32_t kSurfaceCacheFlagIbl       = 1u << 0u;
inline constexpr uint32_t kSurfaceCacheFlagShadowMap = 1u << 1u;
inline constexpr uint32_t kSurfaceCacheFlagProbes    = 1u << 2u;

// Counters of the cache kernels (SC_COUNTER_*, shaders/surface_cache_update.hlsl).
struct SurfaceCacheCounters
{
    uint32_t active = 0u;     // cells registered this frame
    uint32_t created = 0u;    // new cells
    uint32_t full = 0u;       // registrations that found no free slot
    uint32_t rays = 0u;
    uint32_t occupied = 0u;   // slots in use
    uint32_t evicted = 0u;
    float    change = 0.0f;   // mean relative change of the resolved cells (0..1)
    uint32_t resolved = 0u;
    float    meanRadiance = 0.0f;   // mean luminance of E / pi of the resolved cells
    uint32_t sunFromCascades = 0u;  // sun visibility lookups served by the CSM
    uint32_t sunRays = 0u;          // sun visibility from occlusion rays
    float    sunLit = 0.0f;         // mean sun visibility of those lookups (0..1)
    uint32_t fromHits = 0u;         // cells activated because cache rays hit them (outside the view)
};

// Ray kinds of the measuring mode.
enum class SurfaceCacheRayKind : uint32_t
{
    Primary = 0,   // camera rays, closest hit (also feeds the others)
    Diffuse = 1,   // cosine-distributed ray from the primary hit, closest hit
    Occlusion = 2, // same rays, any hit
    Validate = 3,  // a 16 x 16 grid of camera rays, written out for the CPU check
};

// One timed dispatch of the measuring mode.
struct SurfaceCacheBenchPass
{
    std::string         name;         // render-graph pass name (GPU profiler)
    SurfaceCacheRayKind kind = SurfaceCacheRayKind::Primary;
    uint32_t            width = 0u;
    uint32_t            height = 0u;
    float               tMax = 0.0f;  // 0 = unbounded
};

// The scene BVH of one world on the GPU.
struct SurfaceCacheSceneGpu
{
    BufferHandle tlasNodes;
    BufferHandle blasNodes;
    BufferHandle blasTriangles;
    BufferHandle instances;
    uint64_t tlasCapacity = 0u;        // elements
    uint64_t blasNodeCapacity = 0u;
    uint64_t blasTriangleCapacity = 0u;
    uint64_t instanceCapacity = 0u;
    uint64_t uploadedVersion = UINT64_MAX;
    uint64_t uploadedBlasVersion = UINT64_MAX;
    uint32_t tlasNodeCount = 0u;
    uint32_t instanceCount = 0u;
    uint64_t triangleCount = 0u;       // over all instances
    uint32_t uploads = 0u;             // TLAS/instance uploads so far (diagnostics)
    uint32_t blasUploads = 0u;
};

// The radiance cache of one world (package 3): an open-addressing hash grid in
// persistent buffers, held by the add-on (the render graph has no buffer
// import, doc/PLAN_GI.md finding 5).
struct SurfaceCacheGrid
{
    static constexpr uint32_t kCapacity = 1u << 18u;   // slots (power of two)
    static constexpr uint32_t kMaxDirtyBoxes = 64u;
    static constexpr uint32_t kCounterWords = 32u;
    static constexpr uint32_t kReadbackInterval = 16u;   // frames between counter readbacks
    static constexpr uint32_t kReadbackFrames = 4u;

    BufferHandle keys;        // uint per slot
    BufferHandle cellA;       // uint4 per slot: irradiance, weight
    BufferHandle cellB;       // uint4 per slot: position, normal
    BufferHandle cellFrame;   // uint per slot
    BufferHandle accum;       // 4 uint per slot
    BufferHandle activeList;  // uint per slot
    BufferHandle pending;     // uint per slot: cells hit by cache rays, active next frame
    BufferHandle counters;    // kCounterWords uint
    BufferHandle readback;    // CPU copy of the counters
    FrameRingBuffer dirtyBoxes;   // 2 float4 per box
    uint32_t frame = 0u;      // 1-based, advanced per update
    bool     ready = false;

    // Changed regions (SceneBvh change list) waiting for the next update.
    std::vector<float> pendingBoxes;   // 8 floats per box: min xyz, pad, max xyz, pad

    uint32_t readbackWait = 0u;        // frames until the copied counters can be read
    uint32_t readbackFrameOfCopy = 0u;
    bool     readbackPending = false;
    SurfaceCacheCounters lastCounters;
    uint32_t lastCountersFrame = 0u;
};

struct SurfaceCacheWorldState
{
    mesh_renderer::MeshSceneBvh bvh;
    SurfaceCacheSceneGpu        gpu;
    SurfaceCacheGrid            grid;
    uint32_t                    idleExtractions = 0u;
};

// Mean albedo and emission per material (doc/PLAN_GI.md, finding 7), filled on
// the GPU from the smallest mips. The uploaded instance table carries the index
// into this table instead of the material handle.
struct SurfaceCacheMaterialTable
{
    struct Entry
    {
        uint32_t materialId = 0u;      // MaterialHandle value (MaterialSystem)
        uint64_t revision = UINT64_MAX;
        bool     dirty = true;
    };
    static constexpr uint32_t kUpdatesPerFrame = 64u;

    BufferHandle buffer;               // 2 float4 per entry
    uint64_t     capacity = 0u;        // float4 elements
    std::vector<Entry> entries;
    std::unordered_map<uint32_t, uint32_t> indexById;

    uint32_t IndexOf(uint32_t materialId)
    {
        const auto it = indexById.find(materialId);
        if (it != indexById.end())
            return it->second;
        const uint32_t index = static_cast<uint32_t>(entries.size());
        entries.push_back(Entry{ materialId });
        indexById.emplace(materialId, index);
        return index;
    }
};

// What the extraction step hands to the passes of this view.
struct SurfaceCacheFrameData
{
    std::shared_ptr<SurfaceCacheWorldState> world;   // null: nothing to trace in this view
};

// Measuring mode: averages the GPU time of the bench passes over kFrames frames.
struct SurfaceCacheBench
{
    enum class State : uint8_t { Idle, Warmup, Measuring, Readback };
    static constexpr uint32_t kWarmupFrames = 30u;
    static constexpr uint32_t kFrames = 200u;
    static constexpr uint32_t kReadbackFrames = 4u;

    State    state = State::Idle;
    uint32_t frame = 0u;
    std::vector<SurfaceCacheBenchPass> passes;
    std::vector<double>  sumMs;     // per pass
    std::vector<uint32_t> samples;  // per pass
    std::string backend;
    std::string label;              // e.g. scene name, written into the log line
    uint32_t instances = 0u;
    uint64_t triangles = 0u;
    std::vector<std::string> results;   // finished log lines of the last run

    // Radiance cache passes (package 3), averaged like the bench passes.
    // Plus the resolve for shading (package 4).
    static constexpr size_t kCachePassCount = 4u;
    static constexpr const char* kCachePassNames[kCachePassCount] = {
        "SurfaceCacheRegister", "SurfaceCacheRayTrace", "SurfaceCacheResolve", "SurfaceCacheApply" };
    std::array<double, kCachePassCount>   cacheSumMs{};
    std::array<uint32_t, kCachePassCount> cacheSamples{};
    // Opaque pass (MainOpaquePass) with or without GI in the PBR shaders.
    double   opaqueSumMs = 0.0;
    uint32_t opaqueSamples = 0u;
    double   resultApplyMs = 0.0;    // of the last finished run
    double   resultOpaqueMs = 0.0;
    struct CacheSample { uint32_t frame; float change; uint32_t active; uint32_t occupied; };
    std::vector<CacheSample> cacheTimeline;   // counter readbacks during the run
};

// GPU-against-CPU check of a grid of camera rays.
struct SurfaceCacheValidation
{
    static constexpr uint32_t kGrid = 16u;
    static constexpr uint32_t kRays = kGrid * kGrid;
    static constexpr uint32_t kWordsPerRay = 12u;
    static constexpr uint32_t kReadbackFrames = 4u;

    enum class State : uint8_t { Idle, Requested, Dispatched };
    State    state = State::Idle;
    uint32_t waitFrames = 0u;
    std::weak_ptr<SurfaceCacheWorldState> world;
    uint64_t version = 0u;          // SceneBvh::GeometryVersion() the GPU traced
    std::string result;             // last result line
};

struct SurfaceCacheGpuResources
{
    IDevice*             device = nullptr;
    SurfaceCacheSettings settings;
    bool                 supported = false;   // compute available on this backend

    // Programs, created on first use while enabled.
    bool           programsReady = false;
    bool           programsFailed = false;
    ShaderHandle   csTrace;
    ShaderHandle   vsFullscreen;
    ShaderHandle   psOverlay;
    PipelineHandle traceCompute;
    // Resolve for shading (shaders/surface_cache_apply.hlsl): [0] depth, [1] MSAA depth.
    std::array<ShaderHandle, 2>   psApply{};
    std::array<PipelineHandle, 2> applyPipelines{};
    // Radiance cache kernels (shaders/surface_cache_update.hlsl).
    enum CacheKernel : uint32_t { kBegin = 0, kRegister, kTrace, kResolve, kAge, kMaterial, kPending, kCacheKernelCount };
    std::array<ShaderHandle, kCacheKernelCount>   cacheShaders{};
    std::array<PipelineHandle, kCacheKernelCount> cachePipelines{};
    SurfaceCacheMaterialTable materials;
    bool resetRequested = false;       // clear every cache on the next update
    std::unordered_map<uint32_t, PipelineHandle> overlayPipelines;   // by output Format

    // Per-pixel results and statistics.
    BufferHandle primary;          // uint4 per pixel: t bits, octahedral normal, instance, nodes | stack << 16
    uint64_t     primaryCapacity = 0u;
    BufferHandle benchOut;         // uint per ray (keeps the bench dispatches alive)
    uint64_t     benchCapacity = 0u;
    // Statistics (kStatsWords uint): [0] max stack, [1] overflows, then per dispatch slot
    // (bench pass index; debug trace and check at the end) hits and dispatch count.
    static constexpr uint32_t kStatsWords = 64u;
    static constexpr uint32_t kDebugTraceSlot = 29u;
    static constexpr uint32_t kValidateSlot = 30u;
    BufferHandle stats;
    BufferHandle validate;         // kRays * kWordsPerRay uint
    std::array<BufferHandle, 2> readback{};   // 0: stats, 1: validation
    std::vector<BufferHandle> constants;      // per-dispatch constant buffers, used round-robin
    uint32_t     constantIndex = 0u;

    std::unordered_map<const ecs::World*, std::shared_ptr<SurfaceCacheWorldState>> worlds;
    uint32_t frameCounter = 0u;

    SurfaceCacheBench      bench;
    SurfaceCacheValidation validation;
    bool                   statsReadbackPending = false;
    bool                   statsCopied = false;
    uint32_t               statsReadbackWait = 0u;
    uint32_t               lastMaxStack = 0u;
    uint32_t               lastOverflows = 0u;

    // Diagnostics for the editor (last extraction of a main view).
    uint32_t lastInstances = 0u;
    uint64_t lastTriangles = 0u;
    uint32_t lastPendingBlas = 0u;
    uint64_t lastVersion = 0u;
    uint32_t lastUploads = 0u;
};

} // namespace engine::renderer::addons::surface_cache
