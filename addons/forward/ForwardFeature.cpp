#include "ForwardFeature.hpp"
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
        params.shadowEnabled  = !context.renderQueue.shadow.items.empty();
        params.transparentEnabled = !context.renderQueue.transparent.items.empty();
        params.particleEnabled = !context.renderQueue.particles.items.empty();
        params.uiEnabled = !context.renderQueue.ui.items.empty();

        rendergraph::FramePipelineCallbacks callbacks = context.externalCallbacks;

        const RenderQueue* queue = &context.renderQueue;
        ShaderRuntime* shaderRuntime = &context.shaderRuntime;
        const MaterialSystem* materials = &context.materials;
        const BufferHandle perFrameCB = context.perFrameCB;

        auto executeDrawList = std::make_shared<std::function<void(const DrawList&, const rendergraph::RGExecContext&)>>(
            [shaderRuntime, materials, perFrameCB](const DrawList& list, const rendergraph::RGExecContext& execCtx)
            {
                if (!execCtx.cmd || list.items.empty())
                    return;

                for (const auto& item : list.items)
                {
                    if (!item.hasGpuData())
                        continue;
                    if (!shaderRuntime->BindMaterial(*execCtx.cmd,
                                                     *materials,
                                                     item.material,
                                                     perFrameCB,
                                                     BufferHandle::Invalid()))
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
            callbacks.onOpaquePass = [queue, executeDrawList](const rendergraph::RGExecContext& execCtx)
            {
                (*executeDrawList)(queue->opaque, execCtx);
            };
        }

        if (!callbacks.onShadowPass)
        {
            callbacks.onShadowPass = [queue, executeDrawList](const rendergraph::RGExecContext& execCtx)
            {
                (*executeDrawList)(queue->shadow, execCtx);
            };
        }

        if (!callbacks.onTransparentPass)
        {
            callbacks.onTransparentPass = [queue, executeDrawList](const rendergraph::RGExecContext& execCtx)
            {
                (*executeDrawList)(queue->transparent, execCtx);
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
            auto* tonemapShaderRuntime = shaderRuntime;
            auto* materialSystem = context.tonemapMaterialSystem;
            const MaterialHandle material = context.defaultTonemapMaterial;
            callbacks.onTonemap = [state, tonemapShaderRuntime, materialSystem, material](const rendergraph::RGExecContext& execCtx)
            {
                if (!execCtx.cmd || !state->ready)
                    return;
                const TextureHandle hdrTex = execCtx.GetTexture(state->resources.hdrSceneColor);
                if (!hdrTex.IsValid())
                {
                    Debug::LogError("ForwardRenderPipeline: hdrSceneColor texture invalid");
                    return;
                }
                execCtx.cmd->SetShaderResource(0u, hdrTex, ShaderStageMask::Fragment);
                if (!tonemapShaderRuntime->BindMaterial(*execCtx.cmd,
                                                        *materialSystem,
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
