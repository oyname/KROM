#pragma once

#include "events/EventBus.hpp"
#include "jobs/JobSystem.hpp"
#include "jobs/TaskGraph.hpp"
#include "renderer/FeatureRegistry.hpp"
#include "renderer/internal/FrameExecutionStage.hpp"
#include "renderer/internal/FrameExtractionStage.hpp"
#include "renderer/internal/FrameGraphStage.hpp"
#include "renderer/internal/FrameConstantStage.hpp"
#include "renderer/internal/FrameShaderStage.hpp"
#include "renderer/internal/FrameUploadStage.hpp"
#include "renderer/RenderPassRegistry.hpp"

namespace engine::renderer {

struct RenderFrameOrchestratorContext
{
    const ecs::World& world;
    const MaterialSystem& materials;
    const RenderView& view;
    const platform::IPlatformTiming& timing;
    const FramePipelineCallbacks& callbacks;
    uint32_t backbufferIndex  = 0u;
    uint32_t viewportWidth    = 0u;
    uint32_t viewportHeight   = 0u;
    IDevice& device;
    ISwapchain& swapchain;
    ICommandList& graphicsCommandList;
    ICommandList* computeCommandList  = nullptr;
    ICommandList* transferCommandList = nullptr;
    IFence* frameFence                = nullptr;
    GpuResourceRuntime& gpuRuntime;
    ShaderRuntime& shaderRuntime;
    RenderWorld& renderWorld;
    const RenderPassRegistry& renderPassRegistry;
    FeatureRegistry& featureRegistry;
    jobs::JobSystem& jobSystem;
    events::EventBus* eventBus        = nullptr;
    RenderStats& stats;
    MaterialHandle defaultTonemapMaterial;
    const MaterialSystem* tonemapMaterialSystem = nullptr;
    uint64_t& nextFenceValue;
    bool presentVsync = true;
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
    FrameConstantsResult frameConstants{};
    FrameShaderResult shaderPrep{};
    FrameUploadResult upload{};
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
                               RenderFrameExecutionState& state);

private:
    void EnsurePreparationTaskGraphBuilt();

    jobs::TaskGraph m_preparationTaskGraph;
    bool m_preparationTaskGraphBuilt    = false;
    jobs::TaskHandle m_extractTask      = jobs::INVALID_TASK;
    jobs::TaskHandle m_prepareFrameTask = jobs::INVALID_TASK;
    jobs::TaskHandle m_collectShadersTask   = jobs::INVALID_TASK;
    jobs::TaskHandle m_collectMaterialsTask = jobs::INVALID_TASK;
    jobs::TaskHandle m_buildQueuesTask      = jobs::INVALID_TASK;
    jobs::TaskHandle m_collectUploadsTask   = jobs::INVALID_TASK;
    FrameGraphStage m_frameGraphStage;
};

} // namespace engine::renderer
