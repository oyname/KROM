#include "ForwardFeature.hpp"
#include "renderer/FrameGraphStage.hpp"
#include "core/Debug.hpp"
#include "ecs/Components.hpp"
#include "renderer/ECSExtractor.hpp"
#include "renderer/RenderWorld.hpp"

namespace engine::renderer::addons::forward {
namespace {

class ForwardRenderableExtractionStep final : public ISceneExtractionStep
{
public:
    std::string_view GetName() const noexcept override { return "forward.renderables"; }

    void Extract(const ecs::World& world, SceneSnapshot& snapshot) const override
    {
        world.View<WorldTransformComponent, MeshComponent, MaterialComponent>(
            [&](EntityID id,
                const WorldTransformComponent& wt,
                const MeshComponent& mesh,
                const MaterialComponent& mat)
            {
                if (!ECSExtractor::IsEntityActive(world, id))
                    return;
                if (!mesh.mesh.IsValid())
                    return;

                RenderableEntry e{};
                e.entity = id;
                e.mesh = mesh.mesh;
                e.material = mat.material;
                e.submeshIndex = mat.submeshIndex;
                e.worldMatrix = wt.matrix;
                e.worldMatrixInvT = wt.inverse.Transposed();
                e.layerMask = mesh.layerMask;
                e.castShadows = mesh.castShadows;
                e.receiveShadows = mesh.receiveShadows;

                if (const auto* b = world.Get<BoundsComponent>(id))
                {
                    e.boundsCenter = b->centerWorld;
                    e.boundsExtents = b->extentsWorld;
                    e.boundsRadius = b->boundingSphere;
                }
                else
                {
                    e.boundsCenter = math::Vec3(wt.matrix.m[3][0], wt.matrix.m[3][1], wt.matrix.m[3][2]);
                    e.boundsRadius = 0.f;
                }

                snapshot.renderables.push_back(e);
            });
    }
};

class ForwardLightExtractionStep final : public ISceneExtractionStep
{
public:
    std::string_view GetName() const noexcept override { return "forward.lights"; }

    void Extract(const ecs::World& world, SceneSnapshot& snapshot) const override
    {
        world.View<WorldTransformComponent, LightComponent>(
            [&](EntityID id,
                const WorldTransformComponent& wt,
                const LightComponent& lc)
            {
                if (!ECSExtractor::IsEntityActive(world, id))
                    return;

                LightEntry e{};
                e.entity = id;
                e.lightType = lc.type;
                e.color = lc.color;
                e.intensity = lc.intensity;
                e.range = lc.range;
                e.spotInnerDeg = lc.spotInnerDeg;
                e.spotOuterDeg = lc.spotOuterDeg;
                e.castShadows = lc.castShadows;
                e.positionWorld = wt.matrix.TransformPoint(math::Vec3(0.f, 0.f, 0.f));
                e.directionWorld = wt.matrix.TransformDirection(math::Vec3(0.f, 0.f, -1.f)).Normalized();
                snapshot.lights.push_back(e);
            });
    }
};

class ForwardRenderPipeline final : public IRenderPipeline
{
public:
    std::string_view GetName() const noexcept override { return "forward"; }

    bool Build(const RenderPipelineBuildContext& context,
               RenderPipelineBuildResult& result) const override
    {
        rendergraph::FramePipelineBuilder::BuildParams params;
        params.viewportWidth  = context.viewportWidth;
        params.viewportHeight = context.viewportHeight;
        params.bloomWidth     = context.viewportWidth > 1u ? context.viewportWidth / 2u : 1u;
        params.bloomHeight    = context.viewportHeight > 1u ? context.viewportHeight / 2u : 1u;
        params.backbufferRT   = context.backbufferRT;
        params.backbufferTex  = context.backbufferTex;
        params.shadowEnabled = true;
        params.transparentEnabled = true;
        params.particleEnabled = true;
        params.uiEnabled = (context.externalCallbacks.onUI != nullptr)
                        || (context.externalCallbacks.onPresent != nullptr);

        rendergraph::FramePipelineCallbacks callbacks = context.externalCallbacks;

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

                    // DIAG: Buffer-Handle und Rotation-Element (alle 60 Frames)
                    static uint32_t s_diagFrame = 0u;
                    if ((++s_diagFrame % 60u) == 0u)
                        Debug::LogVerbose("DIAG ForwardFeat frame=%u arena=0x%x stride=%u binding.valid=%d",
                            s_diagFrame, runtime->perObjectArena.value,
                            runtime->perObjectStride, static_cast<int>(perObjBinding.IsValid()));

                    if (!runtime->shaderRuntime->BindMaterialWithRange(*execCtx.cmd,
                                                                        *runtime->materials,
                                                                        item.material,
                                                                        runtime->perFrameCB,
                                                                        perObjBinding,
                                                                        {},
                                                                        list.passTag))
                    {
                        continue;
                    }
                    execCtx.cmd->SetVertexBuffer(0u, item.gpuVertexBuffer, 0u);
                    execCtx.cmd->SetIndexBuffer(item.gpuIndexBuffer, true, 0u);
                    execCtx.cmd->DrawIndexed(item.gpuIndexCount, item.instanceCount, 0u, 0, item.firstInstance);
                }
            });

        if (!callbacks.onOpaquePass)
        {
            callbacks.onOpaquePass = [runtime, executeDrawList](const rendergraph::RGExecContext& execCtx)
            {
                if (runtime && runtime->renderQueue) (*executeDrawList)(runtime->renderQueue->opaque, execCtx);
            };
        }

        if (!callbacks.onShadowPass)
        {
            callbacks.onShadowPass = [runtime, executeDrawList](const rendergraph::RGExecContext& execCtx)
            {
                if (runtime && runtime->renderQueue) (*executeDrawList)(runtime->renderQueue->shadow, execCtx);
            };
        }

        if (!callbacks.onTransparentPass)
        {
            callbacks.onTransparentPass = [runtime, executeDrawList](const rendergraph::RGExecContext& execCtx)
            {
                if (runtime && runtime->renderQueue) (*executeDrawList)(runtime->renderQueue->transparent, execCtx);
            };
        }

        struct TonemapState
        {
            rendergraph::FramePipelineResources resources{};
            bool ready = false;
        };
        auto tonemapState = std::make_shared<TonemapState>();

        if (!callbacks.onTonemap && context.defaultTonemapMaterial.IsValid() && context.tonemapMaterialSystem)
        {
            auto state = tonemapState;
            auto runtimeTonemap = runtime;
            const MaterialHandle material = context.defaultTonemapMaterial;
            callbacks.onTonemap = [state, runtimeTonemap, material](const rendergraph::RGExecContext& execCtx)
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
            };
        }

        result.resources = rendergraph::FramePipelineBuilder::Build(context.renderGraph, params, callbacks);
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
        , m_pipeline(std::make_shared<ForwardRenderPipeline>())
    {
    }

    std::string_view GetName() const noexcept override { return "krom-forward"; }
    FeatureID GetID() const noexcept override { return FeatureID::FromString("krom-forward"); }

    void Register(FeatureRegistrationContext& context) override
    {
        context.RegisterSceneExtractionStep(m_renderableStep);
        context.RegisterSceneExtractionStep(m_lightStep);
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
    RenderPipelinePtr m_pipeline;
};

} // namespace

std::unique_ptr<IEngineFeature> CreateForwardFeature()
{
    return std::make_unique<ForwardFeature>();
}

} // namespace engine::renderer::addons::forward
