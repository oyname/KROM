#include "ForwardFeature.hpp"
#include "addons/forward/StandardFramePipeline.hpp"
#include "renderer/RenderPassRegistry.hpp"
#include "core/Debug.hpp"
#include "renderer/RenderWorld.hpp"
#include "renderer/ShaderRuntime.hpp"

namespace engine::renderer::addons::forward {
    namespace {

        class ForwardRenderPipeline final : public IRenderPipeline
        {
        public:
            explicit ForwardRenderPipeline(ForwardFeatureConfig config)
                : m_config(config)
            {
            }

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
                params.clearColorValue = m_config.clearColorValue;

                FramePipelineCallbacks callbacks = context.externalCallbacks;

                auto runtime = context.runtimeBindings;

                auto executeDrawList = std::make_shared<std::function<void(const DrawList&, const rendergraph::RGExecContext&)>>(
                    [runtime](const DrawList& list, const rendergraph::RGExecContext& execCtx)
                    {
                        if (!execCtx.cmd || list.items.empty() || !runtime || !runtime->shaderRuntime || !runtime->materials)
                            return;

                        MaterialHandle lastMaterial = MaterialHandle::Invalid();
                        BufferHandle   lastVertexBuffer = BufferHandle::Invalid();
                        BufferHandle   lastIndexBuffer  = BufferHandle::Invalid();

                        for (const auto& item : list.items)
                        {
                            if (!item.hasGpuData())
                                continue;

                            const PerObjectConstants* perObjectConstants = nullptr;
                            if (runtime->renderQueue && item.cbOffset < runtime->renderQueue->objectConstants.size())
                                perObjectConstants = &runtime->renderQueue->objectConstants[item.cbOffset];

                            BufferBinding perObjBinding{};
                            if (runtime->perObjectArena.IsValid() && runtime->perObjectStride > 0u)
                            {
                                perObjBinding = BufferBinding{
                                    runtime->perObjectArena,
                                    item.cbOffset * runtime->perObjectStride,
                                    static_cast<uint32_t>(sizeof(PerObjectConstants))
                                };
                            }

                            if (item.material != lastMaterial)
                            {
                                if (!runtime->shaderRuntime->BindMaterialWithRange(*execCtx.cmd,
                                    *runtime->materials,
                                    item.material,
                                    runtime->perFrameCB,
                                    perObjBinding,
                                    {},
                                    perObjectConstants,
                                    list.passId))
                                    continue;
                                lastMaterial = item.material;
                            }
                            else
                            {
                                runtime->shaderRuntime->UpdatePerObjectBinding(*execCtx.cmd,
                                    perObjBinding,
                                    perObjectConstants);
                            }

                            if (item.gpuVertexBuffer != lastVertexBuffer)
                            {
                                execCtx.cmd->SetVertexBuffer(0u, item.gpuVertexBuffer, 0u);
                                lastVertexBuffer = item.gpuVertexBuffer;
                            }
                            if (item.gpuIndexBuffer != lastIndexBuffer)
                            {
                                execCtx.cmd->SetIndexBuffer(item.gpuIndexBuffer, true, 0u);
                                lastIndexBuffer = item.gpuIndexBuffer;
                            }
                            execCtx.cmd->DrawIndexed(item.gpuIndexCount, item.instanceCount, 0u, 0, item.firstInstance);
                        }
                    });

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

                            if (pipelineState->resources.shadowMap != rendergraph::RG_INVALID_RESOURCE)
                            {
                                const TextureHandle shadowTex =
                                    execCtx.GetTexture(pipelineState->resources.shadowMap);
                                if (shadowTex.IsValid())
                                {
                                    execCtx.cmd->SetShaderResource(TexSlots::ShadowMap,
                                        shadowTex, ShaderStageMask::Fragment);
                                    execCtx.cmd->SetShaderResource(TexSlots::PassSRV1,
                                        shadowTex, ShaderStageMask::Fragment);
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
                    callbacks.Register(StandardFrameExecutors::Tonemap,
                        [state, runtimeTonemap](const rendergraph::RGExecContext& execCtx)
                        {
                            if (!execCtx.cmd || !state->ready)
                                return;
                            TextureHandle sourceTex = execCtx.GetTexture(state->resources.hdrSceneColor);
                            if (!sourceTex.IsValid())
                            {
                                Debug::LogError("ForwardRenderPipeline: tonemap source texture invalid");
                                return;
                            }
                            const MaterialHandle material = runtimeTonemap ? runtimeTonemap->defaultTonemapMaterial
                                                                           : MaterialHandle::Invalid();
                            if (!runtimeTonemap || !runtimeTonemap->shaderRuntime || !runtimeTonemap->tonemapMaterialSystem || !material.IsValid())
                            {
                                Debug::LogError("ForwardRenderPipeline: tonemap runtime/material invalid");
                                return;
                            }

                            if (!runtimeTonemap->shaderRuntime->BindMaterial(*execCtx.cmd,
                                *runtimeTonemap->tonemapMaterialSystem,
                                material,
                                BufferHandle::Invalid(),
                                BufferHandle::Invalid(),
                                BufferHandle::Invalid()))
                            {
                                Debug::LogError("ForwardRenderPipeline: tonemap material bind failed");
                                return;
                            }

                            execCtx.cmd->SetShaderResource(TexSlots::PassSRV0, sourceTex, ShaderStageMask::Fragment);
                            execCtx.cmd->SetSampler(SamplerSlots::LinearClamp, SamplerSlots::LinearClamp, ShaderStageMask::Fragment);
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

        private:
            ForwardFeatureConfig m_config;
        };

        class ForwardFeature final : public IEngineFeature
        {
        public:
            explicit ForwardFeature(ForwardFeatureConfig config)
                : m_pipeline(std::make_shared<ForwardRenderPipeline>(config))
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

    std::unique_ptr<IEngineFeature> CreateForwardFeature(ForwardFeatureConfig config)
    {
        return std::make_unique<ForwardFeature>(config);
    }

} // namespace engine::renderer::addons::forward
