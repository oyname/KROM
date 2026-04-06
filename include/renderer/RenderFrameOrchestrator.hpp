#pragma once

#include "events/EventBus.hpp"
#include "jobs/JobSystem.hpp"
#include "renderer/FeatureRegistry.hpp"
#include "renderer/FrameExecutionStage.hpp"
#include "renderer/FrameExtractionStage.hpp"
#include "renderer/FrameGraphStage.hpp"
#include "renderer/FramePreparationStage.hpp"

namespace engine::renderer {

struct RenderFrameOrchestratorContext
{
    const ecs::World& world;
    const MaterialSystem& materials;
    const RenderView& view;
    const platform::IPlatformTiming& timing;
    const rendergraph::FramePipelineCallbacks& callbacks;
    bool isOpenGLBackend = false;
    uint32_t backbufferIndex = 0u;
    uint32_t viewportWidth = 0u;
    uint32_t viewportHeight = 0u;
    IDevice& device;
    ISwapchain& swapchain;
    ICommandList& commandList;
    IFence* frameFence = nullptr;
    GpuResourceRuntime& gpuRuntime;
    ShaderRuntime& shaderRuntime;
    RenderWorld& renderWorld;
    FeatureRegistry& featureRegistry;
    jobs::JobSystem& jobSystem;
    events::EventBus* eventBus = nullptr;
    RenderStats& stats;
    MaterialHandle defaultTonemapMaterial;
    const MaterialSystem* tonemapMaterialSystem = nullptr;
    uint64_t& nextFenceValue;
};

struct RenderFrameExecutionState
{
    FrameStageStatus extractionStatus{};
    FrameStageStatus prepareFrameStatus{};
    FrameStageStatus collectShadersStatus{};
    FrameStageStatus collectMaterialsStatus{};
    FrameStageStatus buildQueuesStatus{};
    FrameStageStatus collectUploadsStatus{};
    FrameStageStatus commitShadersStatus{};
    FrameStageStatus commitMaterialsStatus{};
    FrameStageStatus commitUploadsStatus{};
    FrameStageStatus buildGraphStatus{};
    FrameStageStatus executeStatus{};
    FrameExtractionStageResult extraction{};
    FramePreparationStageResult preparation{};
    FrameGraphStageResult graph{};
    FrameExecutionStageResult execution{};

    [[nodiscard]] bool Succeeded() const noexcept
    {
        return extractionStatus.succeeded
            && prepareFrameStatus.succeeded
            && collectShadersStatus.succeeded
            && collectMaterialsStatus.succeeded
            && buildQueuesStatus.succeeded
            && collectUploadsStatus.succeeded
            && commitShadersStatus.succeeded
            && commitMaterialsStatus.succeeded
            && commitUploadsStatus.succeeded
            && buildGraphStatus.succeeded
            && executeStatus.succeeded;
    }

    [[nodiscard]] const FrameStageStatus* FirstFailure() const noexcept
    {
        const FrameStageStatus* ordered[] = {
            &extractionStatus,
            &prepareFrameStatus,
            &collectShadersStatus,
            &collectMaterialsStatus,
            &buildQueuesStatus,
            &collectUploadsStatus,
            &commitShadersStatus,
            &commitMaterialsStatus,
            &commitUploadsStatus,
            &buildGraphStatus,
            &executeStatus,
        };

        for (const FrameStageStatus* status : ordered)
        {
            if (!status->succeeded)
                return status;
        }
        return nullptr;
    }
};

class RenderFrameOrchestrator
{
public:
    [[nodiscard]] bool Execute(const RenderFrameOrchestratorContext& context,
                               RenderFrameExecutionState& state) const;
};

} // namespace engine::renderer
