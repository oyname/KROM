#pragma once

#include "core/Math.hpp"
#include "renderer/GpuResourceRuntime.hpp"
#include "renderer/RenderWorld.hpp"
#include "rendergraph/CompiledFrame.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace engine::renderer {

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
};

struct RenderStats
{
    uint64_t frameIndex = 0u;
    uint32_t totalProxyCount = 0u;
    uint32_t visibleProxyCount = 0u;
    uint32_t opaqueDraws = 0u;
    uint32_t transparentDraws = 0u;
    uint32_t shadowDraws = 0u;
    uint32_t particleDraws = 0u;
    uint32_t backendDrawCalls = 0u;
    uint32_t graphPassCount = 0u;
    uint32_t graphTransitionCount = 0u;
    uint32_t pooledTransientTargets = 0u;
    uint64_t uploadedBytes = 0u;
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
    SceneSnapshot snapshot;
};

struct FramePreparationStageResult
{
    math::Mat4 projectionForBackend = math::Mat4::Identity();
    math::Mat4 viewProjForBackend = math::Mat4::Identity();
    FrameConstants frameConstants{};
    BufferHandle perFrameCB;
    // Per-Object Constant Buffer Arena (alle Objekte dieses Frames, suballoziiert).
    BufferHandle perObjectArena;
    uint32_t     perObjectStride = 0u; // alignierter Byte-Abstand pro Slot
    std::vector<ShaderHandle> shaderRequests;
    std::vector<MaterialHandle> materialRequests;
    std::vector<GpuResourceRuntime::MeshUploadRequest> meshUploadRequests;
};

struct FrameGraphStageResult
{
    const rendergraph::RenderGraph* renderGraph = nullptr;
    rendergraph::CompiledFrame compiledFrame;
};

struct FrameExecutionStageResult
{
    uint64_t submittedFenceValue = 0u;
    RenderStats stats{};
};

} // namespace engine::renderer
