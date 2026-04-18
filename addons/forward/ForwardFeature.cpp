#include "ForwardFeature.hpp"
#include "addons/lighting/LightingExtraction.hpp"
#include "addons/lighting/LightingFrameData.hpp"
#include "addons/mesh_renderer/MeshRendererExtraction.hpp"
#include "renderer/FrameGraphStage.hpp"
#include "renderer/RenderPassRegistry.hpp"
#include "renderer/StandardFramePipeline.hpp"
#include "core/Debug.hpp"
#include "renderer/RenderWorld.hpp"

namespace engine::renderer::addons::forward {
namespace {

class ForwardRenderableExtractionStep final : public ISceneExtractionStep
{
public:
    std::string_view GetName() const noexcept override { return "forward.renderables"; }

    void Extract(const ecs::World& world, RenderWorld& renderWorld) const override
    {
        engine::addons::mesh_renderer::ExtractRenderables(world, renderWorld);
    }
};

class ForwardLightExtractionStep final : public ISceneExtractionStep
{
public:
    std::string_view GetName() const noexcept override { return "forward.lights"; }

    void Extract(const ecs::World& world, RenderWorld& renderWorld) const override
    {
        engine::addons::lighting::ExtractLights(world, renderWorld);
    }
};

class ForwardRenderPipeline final : public IRenderPipeline
{
public:
    std::string_view GetName() const noexcept override { return "forward"; }

    bool Build(const RenderPipelineBuildContext& context,
               RenderPipelineBuildResult& result) const override
    {
        StandardFrameRecipeBuilder::BuildParams params;
        params.viewportWidth  = context.viewportWidth;
        params.viewportHeight = context.viewportHeight;
        params.bloomWidth     = context.viewportWidth > 1u ? context.viewportWidth / 2u : 1u;
        params.bloomHeight    = context.viewportHeight > 1u ? context.viewportHeight / 2u : 1u;
        params.backbufferRT   = context.backbufferRT;
        params.backbufferTex  = context.backbufferTex;
        params.shadowEnabled = true;
        params.transparentEnabled = true;
        params.uiEnabled = context.externalCallbacks.Has(StandardFrameExecutors::UI)
                        || context.externalCallbacks.Has(StandardFrameExecutors::Present);

        FramePipelineCallbacks callbacks = context.externalCallbacks;

        auto runtime = context.runtimeBindings;

        auto executeDrawList = std::make_shared<std::function<void(const DrawList&, const rendergraph::RGExecContext&)>>(
            [runtime](const DrawList& list, const rendergraph::RGExecContext& execCtx)
            {
                if (!execCtx.cmd || list.items.empty() || !runtime || !runtime->shaderRuntime || !runtime->materials)
                    return;

                for (const auto& item : list.items)
                {
                    if (!item.hasGpuData())
                        continue;

                    // Per-Object BufferBinding aus dem Frame-Arena ableiten.
                    // cbOffset ist der Slot-Index; Stride ist alignment-konform (kConstantBufferAlignment).
                    BufferBinding perObjBinding{};
                    if (runtime->perObjectArena.IsValid() && runtime->perObjectStride > 0u)
                    {
                        perObjBinding = BufferBinding{
                            runtime->perObjectArena,
                            item.cbOffset * runtime->perObjectStride,
                            static_cast<uint32_t>(sizeof(PerObjectConstants))
                        };
                    }

                    if (!runtime->shaderRuntime->BindMaterialWithRange(*execCtx.cmd,
                                                                        *runtime->materials,
                                                                        item.material,
                                                                        runtime->perFrameCB,
                                                                        perObjBinding,
                                                                        {},
                                                                        list.passId))
                    {
                        continue;
                    }
                    execCtx.cmd->SetVertexBuffer(0u, item.gpuVertexBuffer, 0u);
                    execCtx.cmd->SetIndexBuffer(item.gpuIndexBuffer, true, 0u);
                    execCtx.cmd->DrawIndexed(item.gpuIndexCount, item.instanceCount, 0u, 0, item.firstInstance);
                }
            });

        if (!callbacks.Has(StandardFrameExecutors::Opaque))
        {
            callbacks.Register(StandardFrameExecutors::Opaque,
                [runtime, executeDrawList](const rendergraph::RGExecContext& execCtx)
            {
                if (!runtime || !runtime->renderQueue)
                    return;
                const DrawList* list = runtime->renderQueue->FindList(StandardRenderPasses::Opaque());
                if (list)
                    (*executeDrawList)(*list, execCtx);
            });
        }

        if (!callbacks.Has(StandardFrameExecutors::Shadow))
        {
            callbacks.Register(StandardFrameExecutors::Shadow,
                [runtime, executeDrawList](const rendergraph::RGExecContext& execCtx)
            {
                if (!runtime || !runtime->renderQueue)
                    return;
                const DrawList* list = runtime->renderQueue->FindList(StandardRenderPasses::Shadow());
                if (list)
                    (*executeDrawList)(*list, execCtx);
            });
        }

        if (!callbacks.Has(StandardFrameExecutors::Transparent))
        {
            callbacks.Register(StandardFrameExecutors::Transparent,
                [runtime, executeDrawList](const rendergraph::RGExecContext& execCtx)
            {
                if (!runtime || !runtime->renderQueue)
                    return;
                const DrawList* list = runtime->renderQueue->FindList(StandardRenderPasses::Transparent());
                if (list)
                    (*executeDrawList)(*list, execCtx);
            });
        }

        struct TonemapState
        {
            FramePipelineResources resources{};
            bool ready = false;
        };
        auto tonemapState = std::make_shared<TonemapState>();

        if (!callbacks.Has(StandardFrameExecutors::Tonemap) && context.defaultTonemapMaterial.IsValid() && context.tonemapMaterialSystem)
        {
            auto state = tonemapState;
            auto runtimeTonemap = runtime;
            const MaterialHandle material = context.defaultTonemapMaterial;
            callbacks.Register(StandardFrameExecutors::Tonemap,
                [state, runtimeTonemap, material](const rendergraph::RGExecContext& execCtx)
            {
                if (!execCtx.cmd || !state->ready)
                    return;
                const TextureHandle hdrTex = execCtx.GetTexture(state->resources.hdrSceneColor);
                if (!hdrTex.IsValid())
                {
                    Debug::LogError("ForwardRenderPipeline: hdrSceneColor texture invalid");
                    return;
                }
                execCtx.cmd->SetShaderResource(TexSlots::PassSRV0, hdrTex, ShaderStageMask::Fragment);
                if (!runtimeTonemap || !runtimeTonemap->shaderRuntime || !runtimeTonemap->tonemapMaterialSystem ||
                    !runtimeTonemap->shaderRuntime->BindMaterial(*execCtx.cmd,
                                                                 *runtimeTonemap->tonemapMaterialSystem,
                                                                 material,
                                                                 BufferHandle::Invalid(),
                                                                 BufferHandle::Invalid()))
                {
                    Debug::LogError("ForwardRenderPipeline: tonemap material bind failed");
                    return;
                }
                execCtx.cmd->Draw(3u, 1u, 0u, 0u);
            });
        }

        result.resources = StandardFrameRecipeBuilder::Build(context.renderGraph, params, callbacks);
        tonemapState->resources = result.resources;
        tonemapState->ready = true;
        return true;
    }
};

class ForwardFeature final : public IEngineFeature
{
public:
    ForwardFeature()
        : m_renderableStep(std::make_shared<ForwardRenderableExtractionStep>())
        , m_lightStep(std::make_shared<ForwardLightExtractionStep>())
        , m_lightingFrameContributor(engine::addons::lighting::CreateLightingFrameConstantsContributor())
        , m_pipeline(std::make_shared<ForwardRenderPipeline>())
    {
    }

    std::string_view GetName() const noexcept override { return "krom-forward"; }
    FeatureID GetID() const noexcept override { return FeatureID::FromString("krom-forward"); }

    void Register(FeatureRegistrationContext& context) override
    {
        context.RegisterSceneExtractionStep(m_renderableStep);
        context.RegisterSceneExtractionStep(m_lightStep);
        context.RegisterFrameConstantsContributor(m_lightingFrameContributor);
        context.RegisterRenderPipeline(m_pipeline, true);
    }

    bool Initialize(const FeatureInitializationContext& context) override
    {
        (void)context;
        return true;
    }

    void Shutdown(const FeatureShutdownContext& context) override
    {
        (void)context;
    }

private:
    SceneExtractionStepPtr m_renderableStep;
    SceneExtractionStepPtr m_lightStep;
    FrameConstantsContributorPtr m_lightingFrameContributor;
    RenderPipelinePtr m_pipeline;
};

} // namespace

std::unique_ptr<IEngineFeature> CreateForwardFeature()
{
    return std::make_unique<ForwardFeature>();
}

} // namespace engine::renderer::addons::forward
