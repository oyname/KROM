#include "renderer/FrameGraphStage.hpp"
#include "core/Debug.hpp"

namespace engine::renderer {
namespace {

static inline void HashCombine(uint64_t& seed, uint64_t value) noexcept
{
    seed ^= value + 0x9e3779b97f4a7c15ull + (seed << 6u) + (seed >> 2u);
}

} // namespace

FrameGraphStage::FrameGraphStage()
    : m_runtimeBindings(std::make_shared<FrameGraphRuntimeBindings>())
{
}

uint64_t FrameGraphStage::ComputeStructureKey(const FrameGraphStageContext& context) const noexcept
{
    uint64_t key = 14695981039346656037ull;
    HashCombine(key, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(context.activePipeline)));
    HashCombine(key, static_cast<uint64_t>(context.viewportWidth));
    HashCombine(key, static_cast<uint64_t>(context.viewportHeight));
    for (const FramePipelineCallbackEntry& entry : context.callbacks.Entries())
    {
        HashCombine(key, std::hash<std::string>{}(entry.name));
        HashCombine(key, entry.callback ? 1ull : 0ull);
    }
    HashCombine(key, static_cast<uint64_t>(context.defaultTonemapMaterial.value));
    HashCombine(key, static_cast<uint64_t>(context.renderQueue.activeShadowResolution));
    return key;
}

void FrameGraphStage::UpdateRuntimeBindings(const FrameGraphStageContext& context)
{
    m_runtimeBindings->renderWorld = context.renderWorld;
    m_runtimeBindings->renderQueue = &context.renderQueue;
    m_runtimeBindings->gpuRuntime = &context.gpuRuntime;
    m_runtimeBindings->shaderRuntime = &context.shaderRuntime;
    m_runtimeBindings->materials = &context.materials;
    m_runtimeBindings->perFrameCB = context.perFrameCB;
    m_runtimeBindings->perFrameBinding = {};
    m_runtimeBindings->perFrameConstantsData = context.perFrameConstantsData;
    m_runtimeBindings->perObjectArena = context.perObjectArena;
    m_runtimeBindings->perObjectStride = context.perObjectStride;
    m_runtimeBindings->defaultTonemapMaterial = context.defaultTonemapMaterial;
    m_runtimeBindings->tonemapMaterialSystem = context.tonemapMaterialSystem;
    m_runtimeBindings->eventBus = context.eventBus;
    m_runtimeBindings->externalCallbacks = context.callbacks;
}

bool FrameGraphStage::Execute(const FrameGraphStageContext& context,
                              FrameGraphStageResult& result)
{
    result.renderGraph = nullptr;
    result.compiledFrame.Reset();
    result.runtimeBindings.reset();

    if (!context.activePipeline)
    {
        Debug::LogError("FrameGraphStage: no active render pipeline registered");
        return false;
    }

    UpdateRuntimeBindings(context);

    const uint64_t structureKey = ComputeStructureKey(context);
    const bool needsRebuild = !m_cache.compileValid || m_cache.structureKey != structureKey;

    if (needsRebuild)
    {
        m_cache = {};
        m_cache.structureKey = structureKey;

        RenderPipelineBuildContext pipelineContext{
            m_cache.renderGraph,
            context.viewportWidth,
            context.viewportHeight,
            context.backbufferRT,
            context.backbufferTex,
            context.renderQueue,
            context.shaderRuntime,
            context.materials,
            context.perFrameCB,
            context.defaultTonemapMaterial,
            context.tonemapMaterialSystem,
            context.eventBus,
            context.callbacks,
            m_runtimeBindings
        };
        RenderPipelineBuildResult pipelineResult{};
        if (!context.activePipeline->Build(pipelineContext, pipelineResult))
        {
            Debug::LogError("FrameGraphStage: render pipeline build failed: %s",
                            std::string(context.activePipeline->GetName()).c_str());
            return false;
        }

        m_cache.backbufferResource = pipelineResult.backbuffer;
        context.gpuRuntime.AllocateTransientTargets(m_cache.renderGraph);
        m_cache.renderGraph.SyncImportedResourceStates(context.device);
        if (!m_cache.renderGraph.Compile(result.compiledFrame))
            return false;

        m_cache.compileValid = true;
        Debug::LogVerbose("FrameGraphStage: Build - structureKey=0x%llx",
                   static_cast<unsigned long long>(structureKey));
    }
    else
    {
        if (m_cache.backbufferResource != rendergraph::RG_INVALID_RESOURCE)
            m_cache.renderGraph.SetResourceBinding(m_cache.backbufferResource, context.backbufferRT, context.backbufferTex);
        m_cache.renderGraph.ClearTransientResourceBindings();
        context.gpuRuntime.AllocateTransientTargets(m_cache.renderGraph);
        m_cache.renderGraph.SyncImportedResourceStates(context.device);
        if (!m_cache.renderGraph.ResolveCompiledFrame(result.compiledFrame))
            return false;
        Debug::LogVerbose("FrameGraphStage: Reuse - structureKey=0x%llx",
                   static_cast<unsigned long long>(structureKey));
    }

    result.renderGraph = &m_cache.renderGraph;
    result.runtimeBindings = m_runtimeBindings;
    return true;
}

} // namespace engine::renderer
