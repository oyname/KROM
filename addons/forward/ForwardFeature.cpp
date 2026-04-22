#include "ForwardFeature.hpp"
#include "addons/forward/StandardFramePipeline.hpp"
#include "renderer/RenderPassRegistry.hpp"
#include "core/Debug.hpp"
#include "renderer/RenderWorld.hpp"

namespace engine::renderer::addons::forward {
    namespace {

        class ForwardRenderPipeline final : public IRenderPipeline
        {
        public:
            std::string_view GetName() const noexcept override { return "forward"; }

            bool Build(const RenderPipelineBuildContext& context,
                RenderPipelineBuildResult& result) const override
            {
                StandardFrameRecipeBuilder::BuildParams params;
                params.viewportWidth = context.viewportWidth;
                params.viewportHeight = context.viewportHeight;
                params.bloomWidth = context.viewportWidth > 1u ? context.viewportWidth / 2u : 1u;
                params.bloomHeight = context.viewportHeight > 1u ? context.viewportHeight / 2u : 1u;
                params.backbufferRT = context.backbufferRT;
                params.backbufferTex = context.backbufferTex;
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

                // Gemeinsamer State: Pipeline-Ressourcen bekannt nach Build(), genutzt von
                // Opaque-Executor (Shadow-Map-Binding) und Tonemap-Executor (HDR-Tex).
                struct PipelineState
                {
                    StandardFrameBuildResult resources{};
                    bool ready = false;
                };
                auto pipelineState = std::make_shared<PipelineState>();

                if (!callbacks.Has(StandardFrameExecutors::Opaque))
                {
                    callbacks.Register(StandardFrameExecutors::Opaque,
                        [runtime, executeDrawList, pipelineState](const rendergraph::RGExecContext& execCtx)
                        {
                            if (!runtime || !runtime->renderQueue || !pipelineState->ready)
                                return;

                            // Shadow-Map (t4) binden wenn verfügbar – engine-seitiges Binding,
                            // nicht über das Materialsystem geleitet.
                            if (pipelineState->resources.shadowMap != rendergraph::RG_INVALID_RESOURCE)
                            {
                                const TextureHandle shadowTex =
                                    execCtx.GetTexture(pipelineState->resources.shadowMap);
                                if (shadowTex.IsValid())
                                {
                                    execCtx.cmd->SetShaderResource(TexSlots::ShadowMap,
                                        shadowTex, ShaderStageMask::Fragment);
                                    // Debug-Mode 5 liest die gleiche Shadow-Map roh ueber einen
                                    // separaten non-comparison Texture-Slot.
                                    execCtx.cmd->SetShaderResource(TexSlots::PassSRV1,
                                        shadowTex, ShaderStageMask::Fragment);
                                    // Shadow-Map und Sampler sind globale Engine-Bindings.
                                    execCtx.cmd->SetSampler(SamplerSlots::ShadowPCF,
                                        SamplerSlots::ShadowPCF,
                                        ShaderStageMask::Fragment);
                                    execCtx.cmd->SetSampler(SamplerSlots::PointClamp,
                                        SamplerSlots::PointClamp,
                                        ShaderStageMask::Fragment);
                                }
                            }

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

                if (!callbacks.Has(StandardFrameExecutors::Present))
                {
                    callbacks.Register(StandardFrameExecutors::Present,
                        [](const rendergraph::RGExecContext& /*execCtx*/) {});
                }

                if (!callbacks.Has(StandardFrameExecutors::Tonemap) && context.defaultTonemapMaterial.IsValid() && context.tonemapMaterialSystem)
                {
                    auto state = pipelineState;
                    auto runtimeTonemap = runtime;
                    const MaterialHandle material = context.defaultTonemapMaterial;
                    callbacks.Register(StandardFrameExecutors::Tonemap,
                        [state, runtimeTonemap, material](const rendergraph::RGExecContext& execCtx)
                        {
                            if (!execCtx.cmd || !state->ready)
                                return;
                            TextureHandle sourceTex = execCtx.GetTexture(state->resources.hdrSceneColor);
                            if (!sourceTex.IsValid())
                            {
                                Debug::LogError("ForwardRenderPipeline: tonemap source texture invalid");
                                return;
                            }
                            execCtx.cmd->SetShaderResource(TexSlots::PassSRV0, sourceTex, ShaderStageMask::Fragment);
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

                const StandardFrameBuildResult builtResources =
                    StandardFrameRecipeBuilder::Build(context.renderGraph, params, callbacks);
                result.backbuffer = builtResources.backbuffer;
                pipelineState->resources = builtResources;
                pipelineState->ready = true;
                return true;
            }
        };

        class ForwardFeature final : public IEngineFeature
        {
        public:
            ForwardFeature()
                : m_pipeline(std::make_shared<ForwardRenderPipeline>())
            {
            }

            std::string_view GetName() const noexcept override { return "krom-forward"; }
            FeatureID GetID() const noexcept override { return FeatureID::FromString("krom-forward"); }

            void Register(FeatureRegistrationContext& context) override
            {
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
            RenderPipelinePtr m_pipeline;
        };

    } // namespace

    std::unique_ptr<IEngineFeature> CreateForwardFeature()
    {
        return std::make_unique<ForwardFeature>();
    }

} // namespace engine::renderer::addons::forward
