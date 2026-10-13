#include "addons/editor/EditorFeature.hpp"
#include "addons/editor/EditorUI.hpp"

#include "renderer/RenderFramePassInterfaces.hpp"
#include "renderer/RenderLayers.hpp"
#include "renderer/RenderPipelineTypes.hpp"
#include "renderer/RenderPipelineRecipe.hpp"
#include "rendergraph/RenderGraph.hpp"
#include <algorithm>
#include <string>

namespace engine::renderer::addons::editor {
namespace {

constexpr const char* kEditorGizmoDepthExecutor   = "editor.gizmo_depth";
constexpr const char* kEditorGizmoOverlayExecutor = "editor.gizmo_overlay";

class EditorGizmoOverlayPass final : public IFramePass
{
public:
    uint32_t  GetContributorId() const noexcept override { return 0xED170001u; }
    PassPhase GetPhase()         const noexcept override { return PassPhase::AfterOpaque; }

    void BuildPass(const PassBuildContext& ctx) const override
    {
        const uint32_t w = ctx.frame.viewportWidth;
        const uint32_t h = ctx.frame.viewportHeight;
        const bool hasGtao = std::any_of(ctx.recipe.resources.begin(), ctx.recipe.resources.end(),
            [](const FrameRecipeResourceDesc& r) { return r.name == "GTAOComposite"; });
        const std::string sceneColorTarget = hasGtao
            ? "GTAOComposite"
            : "HDRSceneColor";

        // Pass 1: Depth-getestete Gizmos (z.B. Kamera-Modell) — kein Depth-Clear,
        //         werden von normaler Szenengeometrie verdeckt.
        {
            FrameRecipePassDesc pass{};
            pass.name         = "EditorGizmoDepthPass";
            pass.executorName = kEditorGizmoDepthExecutor;
            pass.accesses.push_back({ sceneColorTarget, FrameRecipeAccessKind::WriteRenderTarget });
            pass.renderPass.enabled            = true;
            pass.renderPass.targetResourceName = sceneColorTarget;
            pass.renderPass.viewportWidth      = w;
            pass.renderPass.viewportHeight     = h;
            pass.renderPass.clearColor         = false;
            pass.renderPass.clearDepth         = false;
            ctx.recipe.passes.push_back(std::move(pass));
        }

        // Pass 2: Immer-sichtbare Gizmos (Transform-Pfeile, Licht-Icons usw.) —
        //         Depth-Buffer wird gecleared damit sie nie verdeckt werden.
        {
            FrameRecipePassDesc pass{};
            pass.name         = "EditorGizmoOverlayPass";
            pass.executorName = kEditorGizmoOverlayExecutor;
            pass.accesses.push_back({ sceneColorTarget, FrameRecipeAccessKind::WriteRenderTarget });
            pass.renderPass.enabled            = true;
            pass.renderPass.targetResourceName = sceneColorTarget;
            pass.renderPass.viewportWidth      = w;
            pass.renderPass.viewportHeight     = h;
            pass.renderPass.clearColor         = false;
            pass.renderPass.clearDepth         = true;
            ctx.recipe.passes.push_back(std::move(pass));
        }

        const RenderQueue* renderQueue = ctx.frame.renderQueue;
        auto drawObjects = ctx.drawObjects;

        ctx.callbacks.Register(kEditorGizmoDepthExecutor,
            [renderQueue, drawObjects](const rendergraph::RGExecContext& exec)
            {
                if (!exec.cmd || !renderQueue || !drawObjects)
                    return;

                DrawList list{};
                list.passId = StandardRenderPasses::EditorGizmo();
                list.sorted = true;

                for (const DrawList& dl : renderQueue->GetLists())
                {
                    if (dl.passId == StandardRenderPasses::Shadow())
                        continue;
                    for (const DrawItem& item : dl.items)
                    {
                        if ((item.layerMask & LAYER_EDITOR_GIZMO_DEPTH) == 0u)
                            continue;
                        list.items.push_back(item);
                    }
                }

                if (!list.items.empty())
                    drawObjects(list, exec);
            });

        ctx.callbacks.Register(kEditorGizmoOverlayExecutor,
            [renderQueue, drawObjects](const rendergraph::RGExecContext& exec)
            {
                if (!exec.cmd || !renderQueue || !drawObjects)
                    return;

                DrawList overlay{};
                overlay.passId = StandardRenderPasses::EditorGizmo();
                overlay.sorted = true;

                for (const DrawList& dl : renderQueue->GetLists())
                {
                    if (dl.passId == StandardRenderPasses::Shadow())
                        continue;
                    for (const DrawItem& item : dl.items)
                    {
                        if ((item.layerMask & LAYER_EDITOR_GIZMO) == 0u)
                            continue;
                        overlay.items.push_back(item);
                    }
                }

                if (!overlay.items.empty())
                    drawObjects(overlay, exec);
            });
    }
};

class EditorPass final : public IFramePass
{
public:
    explicit EditorPass(EditorBackendCallbacks callbacks)
        : m_callbacks(std::move(callbacks))
    {}

    uint32_t  GetContributorId() const noexcept override { return 0xED170000u; }
    PassPhase GetPhase()         const noexcept override { return PassPhase::AfterOpaque; }

    // Signalisiert ForwardFeature, dass der UI-Pass ins Rezept gehoert.
    // Entspricht StandardFrameResources::UIOverlay ("UIOverlay").
    bool DeclaresResource(std::string_view name) const noexcept override
    {
        return name == "UIOverlay";
    }

    void BuildPass(const PassBuildContext& ctx) const override
    {
        ctx.callbacks.Register("frame.ui", [this](const rendergraph::RGExecContext& exec) {
            ICommandList* cmd = exec.cmd;

            if (m_callbacks.newFrame)
                m_callbacks.newFrame();

            if (m_callbacks.getFrameContext)
            {
                if (EditorFrameContext* frameCtx = m_callbacks.getFrameContext())
                {
                    DrawEditorPanels(*frameCtx);

                    if (m_callbacks.drawUI)
                        m_callbacks.drawUI(*frameCtx);
                }
            }

            if (m_callbacks.renderDrawData && cmd)
                m_callbacks.renderDrawData(*cmd);
        });
    }

private:
    EditorBackendCallbacks m_callbacks;
};

class EditorFeature final : public IEngineFeature
{
public:
    explicit EditorFeature(EditorBackendCallbacks callbacks)
        : m_callbacks(std::move(callbacks))
        , m_gizmoOverlayPass(std::make_shared<EditorGizmoOverlayPass>())
        , m_pass(std::make_shared<EditorPass>(m_callbacks))
    {}

    std::string_view GetName() const noexcept override { return "krom-editor"; }
    FeatureID        GetID()   const noexcept override { return FeatureID::FromString("krom-editor"); }

    void Register(FeatureRegistrationContext& context) override
    {
        context.RegisterPassContributor(m_gizmoOverlayPass);
        context.RegisterPassContributor(m_pass);
    }

    bool Initialize(const FeatureInitializationContext& context) override
    {
        (void)context;
        return m_callbacks.init ? m_callbacks.init() : true;
    }

    void Shutdown(const FeatureShutdownContext& context) override
    {
        (void)context;
        if (m_callbacks.shutdown)
            m_callbacks.shutdown();
    }

private:
    EditorBackendCallbacks      m_callbacks;
    std::shared_ptr<EditorGizmoOverlayPass> m_gizmoOverlayPass;
    std::shared_ptr<EditorPass> m_pass;
};

} // namespace

std::unique_ptr<IEngineFeature> CreateEditorFeature(EditorBackendCallbacks callbacks)
{
    return std::make_unique<EditorFeature>(std::move(callbacks));
}

} // namespace engine::renderer::addons::editor
