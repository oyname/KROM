#pragma once

#include "renderer/BackgroundMode.hpp"
#include "renderer/RenderFeatureDataViews.hpp"
#include "renderer/RendererTypes.hpp"
#include "rendergraph/CompiledFrame.hpp"
#include "rendergraph/RenderGraph.hpp"
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace engine::events { class EventBus; }

namespace engine::renderer {

class FramePipelineCallbacks;
class GpuResourceRuntime;
class IDevice;
class IFramePass;
class IRenderPipelineFeature;
class MaterialSystem;
class ShaderRuntime;
struct FrameConstants;
struct FrameGraphRuntimeBindings;

using IRenderPipeline = IRenderPipelineFeature;
using IPassContributor = IFramePass;

struct FrameGraphStageResult
{
    const rendergraph::RenderGraph* renderGraph = nullptr;
    rendergraph::CompiledFrame compiledFrame;
    std::shared_ptr<FrameGraphRuntimeBindings> runtimeBindings;
};

struct FrameGraphStageContext
{
    IDevice& device;
    uint32_t viewportWidth  = 0u;
    uint32_t viewportHeight = 0u;
    RenderTargetHandle backbufferRT;
    TextureHandle backbufferTex;
    bool presentOutput = true;
    RenderFrameDataView frameData{};
    const RenderQueue& renderQueue;
    const IRenderPipeline* activePipeline = nullptr;
    ShaderRuntime& shaderRuntime;
    const MaterialSystem& materials;
    const FramePipelineCallbacks& callbacks;
    events::EventBus* eventBus             = nullptr;
    GpuResourceRuntime& gpuRuntime;
    BufferHandle perFrameCB;
    const FrameConstants* perFrameConstantsData = nullptr;
    BufferHandle perObjectArena;
    uint32_t     perObjectStride           = 0u;
    MaterialHandle defaultTonemapMaterial;
    const MaterialSystem* tonemapMaterialSystem = nullptr;
    BackgroundMode backgroundMode = BackgroundMode::ClearColor;
    bool enableBloom = true;
    std::array<float, 4> clearColor{0.3f, 0.3f, 0.3f, 1.f};
    // Vom FeatureRegistry eingesammelte Pass-Contributors (leer = kein Contributor aktiv).
    std::vector<const IPassContributor*> passContributors{};
    bool enableAmbientOcclusion = true;
};

class FrameGraphStage
{
public:
    FrameGraphStage();

    [[nodiscard]] bool Execute(const FrameGraphStageContext& context,
                               FrameGraphStageResult& result);

private:
    struct StructuralCache
    {
        uint64_t structureKey                              = 0ull;
        rendergraph::RenderGraph renderGraph;
        rendergraph::RGResourceID backbufferResource       = rendergraph::RG_INVALID_RESOURCE;
        bool compileValid                                  = false;
    };

    [[nodiscard]] uint64_t ComputeStructureKey(const FrameGraphStageContext& context) const noexcept;
    void UpdateRuntimeBindings(const FrameGraphStageContext& context);

    std::shared_ptr<FrameGraphRuntimeBindings> m_runtimeBindings;
    StructuralCache m_cache;
};

} // namespace engine::renderer
