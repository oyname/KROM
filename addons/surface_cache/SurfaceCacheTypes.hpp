#pragma once
// =============================================================================
// KROM Engine - addons/surface_cache/SurfaceCacheTypes.hpp
//
// Surface cache GI, package 2 (doc/PLAN_GI.md): the scene BVH on the GPU, a
// compute trace kernel (shaders/surface_cache_trace.hlsl), a debug view and a
// measuring mode. No cache and no GI yet.
//
// Off by default. While off nothing is built, uploaded or added to the frame
// graph; switching it off releases every GPU object again.
// =============================================================================
#include "addons/mesh_renderer/MeshSceneBvh.hpp"
#include "renderer/FrameRingBuffer.hpp"
#include "renderer/IDevice.hpp"
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
    Count,
};

struct SurfaceCacheSettings
{
    bool                  enabled   = false;
    SurfaceCacheDebugView debugView = SurfaceCacheDebugView::Normal;
    float                 overlay   = 1.0f;   // 1 = debug view only, 0 = scene only
    float                 distanceRange = 50.0f;   // metres shown white in the distance view
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

struct SurfaceCacheWorldState
{
    mesh_renderer::MeshSceneBvh bvh;
    SurfaceCacheSceneGpu        gpu;
    uint32_t                    idleExtractions = 0u;
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
