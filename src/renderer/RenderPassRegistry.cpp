#include "renderer/RenderPassRegistry.hpp"

namespace engine::renderer {

RenderPassRegistry::RenderPassRegistry()
{
    // Fullscreen/Postprocess: DepthTest, DepthWrite und Cull müssen zwingend OFF sein.
    // Sonst verwirft der Rasterizer den Fullscreen-Triangle → schwarzes Bild auf OpenGL.
    PassLocks postprocessLocks{};
    postprocessLocks.depthTestLocked  = true;  postprocessLocks.depthTestValue  = false;
    postprocessLocks.depthWriteLocked = true;  postprocessLocks.depthWriteValue = false;
    postprocessLocks.cullModeLocked   = true;  postprocessLocks.cullModeValue   = CullMode::None;

    // UI: gleiche Regel — kein Depth-Test, kein Cull (UI liegt immer vor allem anderen).
    PassLocks uiLocks{};
    uiLocks.depthTestLocked  = true;  uiLocks.depthTestValue  = false;
    uiLocks.depthWriteLocked = true;  uiLocks.depthWriteValue = false;
    uiLocks.cullModeLocked   = true;  uiLocks.cullModeValue   = CullMode::None;

    // Editor-Gizmos liegen ueber der Szene, sollen aber untereinander korrekt
    // tiefensortieren. Der Overlay-Pass leert dafuer seinen Depth-Buffer.
    PassLocks editorGizmoLocks{};
    editorGizmoLocks.depthTestLocked  = true;  editorGizmoLocks.depthTestValue  = true;
    editorGizmoLocks.depthWriteLocked = true;  editorGizmoLocks.depthWriteValue = true;
    editorGizmoLocks.cullModeLocked   = true;  editorGizmoLocks.cullModeValue   = CullMode::None;

    // Shadow: DepthTest und DepthWrite müssen ON sein — sonst keine Shadow Map.
    // CullMode::Front (Front-Face-Culling) ist der Industriestandard gegen
    // Peter-Panning: es werden die RÜCKseiten der Caster in die Shadow-Map
    // gerendert, wodurch die gespeicherte Tiefe hinter dem Objekt liegt. Dadurch
    // klebt der Schatten an der Objektkante (kein Durchscheinen am Kontaktpunkt)
    // und Self-Shadow-Acne wandert ins Objektinnere. Funktioniert für solide,
    // geschlossene Geometrie; rein einseitige (thin) Caster casten dann von ihrer
    // Rückseite — für übliche Wände/Boxen ist das korrekt.
    PassLocks shadowLocks{};
    shadowLocks.depthTestLocked  = true;  shadowLocks.depthTestValue  = true;
    shadowLocks.depthWriteLocked = true;  shadowLocks.depthWriteValue = true;
    shadowLocks.cullModeLocked   = true;  shadowLocks.cullModeValue   = CullMode::Front;

    m_passes = {
        { StandardRenderPasses::Opaque(),      "forward.opaque",      RenderPassSortMode::FrontToBack,    {} },
        { StandardRenderPasses::AlphaCutout(), "forward.alpha_cutout",RenderPassSortMode::FrontToBack,    {} },
        { StandardRenderPasses::Transparent(), "forward.transparent", RenderPassSortMode::BackToFront,    {} },
        { StandardRenderPasses::Shadow(),      "forward.shadow",      RenderPassSortMode::FrontToBack,    shadowLocks },
        { StandardRenderPasses::UI(),          "forward.ui",          RenderPassSortMode::SubmissionOrder,uiLocks },
        { StandardRenderPasses::Postprocess(), "forward.postprocess", RenderPassSortMode::SubmissionOrder,postprocessLocks },
        { StandardRenderPasses::EditorGizmo(), "forward.editor_gizmo",RenderPassSortMode::FrontToBack,    editorGizmoLocks },
    };
}

RenderPassID RenderPassRegistry::Register(std::string name, RenderPassSortMode sortMode)
{
    for (const RenderPassDesc& pass : m_passes)
    {
        if (pass.name == name)
            return pass.id;
    }

    const uint16_t nextId = static_cast<uint16_t>(m_passes.size() + 1u);
    RenderPassDesc pass{};
    pass.id = RenderPassID{nextId};
    pass.name = std::move(name);
    pass.sortMode = sortMode;
    m_passes.push_back(std::move(pass));
    return m_passes.back().id;
}

RenderPassID RenderPassRegistry::FindByName(std::string_view name) const noexcept
{
    for (const RenderPassDesc& pass : m_passes)
    {
        if (pass.name == name)
            return pass.id;
    }
    return RenderPassID::Invalid();
}

const RenderPassDesc* RenderPassRegistry::Get(RenderPassID id) const noexcept
{
    if (!id.IsValid())
        return nullptr;

    const size_t index = static_cast<size_t>(id.value - 1u);
    if (index >= m_passes.size())
        return nullptr;
    return &m_passes[index];
}

std::string_view RenderPassRegistry::GetName(RenderPassID id) const noexcept
{
    const RenderPassDesc* pass = Get(id);
    return pass ? std::string_view(pass->name) : std::string_view("Invalid");
}

RenderPassSortMode RenderPassRegistry::GetSortMode(RenderPassID id) const noexcept
{
    const RenderPassDesc* pass = Get(id);
    return pass ? pass->sortMode : RenderPassSortMode::FrontToBack;
}

const PassLocks* RenderPassRegistry::GetLocks(RenderPassID id) const noexcept
{
    const RenderPassDesc* pass = Get(id);
    return pass ? &pass->locks : nullptr;
}

void RenderPassRegistry::CopyFrom(const RenderPassRegistry& other)
{
    if (this == &other)
        return;
    m_passes = other.m_passes;
}

} // namespace engine::renderer
