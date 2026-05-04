#pragma once

#include "core/Math.hpp"
#include "renderer/GpuResourceRuntime.hpp"
#include "renderer/RenderWorld.hpp"
#include "rendergraph/CompiledFrame.hpp"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace engine::renderer {

struct FrameGraphRuntimeBindings;

struct RenderView
{
    math::Mat4 view = math::Mat4::Identity();
    math::Mat4 projection = math::Mat4::Identity();
    math::Vec3 cameraPosition{0.f, 0.f, 0.f};
    math::Vec3 cameraForward{0.f, 0.f, 1.f};
    math::Vec3 ambientColor{0.03f, 0.03f, 0.03f};
    float ambientIntensity = 1.f;
    float nearPlane = 0.1f;
    float farPlane = 1000.f;
    uint32_t debugFlags = 0u;  // DebugFlags-Bitfeld (siehe enum DebugFlags in RenderWorld.hpp)
};

struct RenderStats
{
    uint64_t frameIndex = 0u;
    uint32_t totalProxyCount = 0u;
    uint32_t visibleProxyCount = 0u;
    uint32_t opaqueDraws = 0u;
    uint32_t transparentDraws = 0u;
    uint32_t shadowDraws = 0u;
    uint32_t graphPassCount = 0u;
    uint32_t graphTransitionCount = 0u;
    uint32_t pooledTransientTargets = 0u;
    uint32_t peakActiveWorkers = 0u;
    uint64_t uploadedBytes = 0u;
    float collectUploadsMs = 0.0f;
    float commitUploadsMs = 0.0f;
    float buildGraphMs = 0.0f;
    float prepareFrameMs = 0.0f;
    float collectShadersMs = 0.0f;
    float collectMaterialsMs = 0.0f;
    float parallelSectionMs = 0.0f;
    float executeMs = 0.0f;
    float executeRecordMs = 0.0f;
    float executeSubmitMs = 0.0f;
    float executePresentMs = 0.0f;
    float backendBeginFrameMs = 0.0f;
    float backendAcquireMs = 0.0f;
    float backendQueueSubmitMs = 0.0f;
    float backendPresentMs = 0.0f;
    uint32_t backendDescriptorRematerializations = 0u;
    uint32_t backendDescriptorSetAllocations = 0u;
    uint32_t backendDescriptorSetUpdates = 0u;
    uint32_t backendDescriptorSetBinds = 0u;
};

struct FrameStageStatus
{
    bool succeeded = false;
    std::string errorMessage;

    void MarkSucceeded()
    {
        succeeded = true;
        errorMessage.clear();
    }

    void MarkFailed(std::string message)
    {
        succeeded = false;
        errorMessage = std::move(message);
    }
};

struct FrameExtractionStageResult
{
    RenderSceneSnapshot snapshot{};
};

struct FrameConstantsResult
{
    math::Mat4 projectionForBackend = math::Mat4::Identity();
    math::Mat4 viewProjForBackend = math::Mat4::Identity();
    FrameConstants frameConstants{};
};

struct FrameShaderResult
{
    std::vector<ShaderHandle> shaderRequests;
    std::vector<MaterialHandle> materialRequests;
};

struct FrameUploadResult
{
    BufferHandle perFrameCB;
    BufferHandle perObjectArena;
    uint32_t perObjectStride = 0u;
    std::vector<GpuResourceRuntime::MeshUploadRequest> meshUploadRequests;
};

struct FrameGraphStageResult
{
    const rendergraph::RenderGraph* renderGraph = nullptr;
    rendergraph::CompiledFrame compiledFrame;
    std::shared_ptr<FrameGraphRuntimeBindings> runtimeBindings;
};

struct FrameExecutionStageResult
{
    uint64_t submittedFenceValue = 0u;
    RenderStats stats{};
};

} // namespace engine::renderer
