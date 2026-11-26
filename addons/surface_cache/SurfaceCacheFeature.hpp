#pragma once
// =============================================================================
// KROM Engine - addons/surface_cache/SurfaceCacheFeature.hpp
//
// Surface cache GI add-on, package 2 (doc/PLAN_GI.md): scene BVH on the GPU,
// trace kernel, debug view, measuring mode, GPU-against-CPU check.
//
// The host keeps the shared SurfaceCacheGpuResources (like the GTAO and contact
// shadow add-ons), sets its settings and calls TickSurfaceCache() once per frame
// after rendering. Off by default; while off it costs nothing.
// =============================================================================
#include "SurfaceCacheTypes.hpp"
#include "renderer/GpuPassProfiler.hpp"
#include "renderer/RenderFeatureInterfaces.hpp"
#include <memory>
#include <string>

namespace engine::renderer::addons::surface_cache {

std::unique_ptr<IEngineFeature> CreateSurfaceCacheFeature(std::shared_ptr<SurfaceCacheGpuResources>* outResources);

// Starts a measuring run (SurfaceCacheBench): warm-up, then kFrames frames
// averaged, then one log line per ray kind. backend and label go into the lines.
void StartSurfaceCacheBench(SurfaceCacheGpuResources& res, std::string backend, std::string label);
// Traces a 16 x 16 grid of camera rays on the GPU and compares them with the
// CPU BVH (SceneBvh::TraceClosest); the result is logged and kept in
// res.validation.result.
void RequestSurfaceCacheValidation(SurfaceCacheGpuResources& res);
// True while a measuring run needs the GPU pass timings.
[[nodiscard]] bool SurfaceCacheWantsGpuTimings(const SurfaceCacheGpuResources& res) noexcept;
// Once per frame after rendering. report: RenderSystem::GetGpuPassTimings().
void TickSurfaceCache(SurfaceCacheGpuResources& res, const GpuPassTimingReport& report);

// Readable state for the editor.
[[nodiscard]] bool SurfaceCacheBenchRunning(const SurfaceCacheGpuResources& res) noexcept;

} // namespace engine::renderer::addons::surface_cache
