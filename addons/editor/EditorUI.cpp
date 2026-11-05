// =============================================================================
// KROM Engine - addons/editor/EditorUI.cpp
// Einstieg (DrawEditorPanels/DrawLightDebugLines), Layout, Transform-Mathe,
// Viewport-Picking und Editor-Kamera.
// =============================================================================
#include "addons/editor/EditorPreferences.hpp"
#include "addons/editor/EditorUIInternal.hpp"
#include "addons/editor/EditorCommands.hpp"
#include "core/PathUtf8.hpp"
#include "addons/particles/ParticleComponents.hpp"
#include <unordered_set>
#include <chrono>
#include "imgui_internal.h"

#ifdef KROM_EDITOR_HAS_IMGUI

namespace engine::renderer::addons::editor {

using namespace detail;

namespace {

struct EditorUiTimings { double materials = 0, menus = 0, gizmos = 0, assets = 0, hierarchy = 0, inspector = 0, preview = 0; };
static EditorUiTimings g_uiTimings;

constexpr float kEditorLeftPanelWidth = 288.f;
constexpr float kEditorRightPanelWidth = 448.f;
constexpr float kEditorBottomPanelHeight = 228.f;
constexpr float kEditorPanelGap = 2.f;

// Standardaufteilung: Assets unten ueber die volle Breite, Hierarchie links,
// Inspector rechts, Viewport (davor) und Game als Reiter in der Mitte.
static void BuildDefaultDockLayout(ImGuiID dockspaceId, const ImVec2& size)
{
    ImGui::DockBuilderRemoveNode(dockspaceId);
    ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspaceId, size);

    const float width = std::max(size.x, 640.f);
    const float height = std::max(size.y, 420.f);
    ImGuiID center = dockspaceId;
    const ImGuiID bottom = ImGui::DockBuilderSplitNode(
        center, ImGuiDir_Down, std::clamp(kEditorBottomPanelHeight / height, 0.1f, 0.5f), nullptr, &center);
    const ImGuiID left = ImGui::DockBuilderSplitNode(
        center, ImGuiDir_Left, std::clamp(kEditorLeftPanelWidth / width, 0.1f, 0.4f), nullptr, &center);
    const ImGuiID right = ImGui::DockBuilderSplitNode(
        center, ImGuiDir_Right,
        std::clamp(kEditorRightPanelWidth / std::max(width - kEditorLeftPanelWidth, 1.f), 0.1f, 0.5f),
        nullptr, &center);

    ImGui::DockBuilderDockWindow(TranslateLabel("menu.hierarchy", "Hierarchie##editor").c_str(), left);
    ImGui::DockBuilderDockWindow(TranslateLabel("panel.inspector", "Inspector##editor").c_str(), right);
    ImGui::DockBuilderDockWindow(TranslateLabel("panel.assets", "Assets##editor").c_str(), bottom);
    ImGui::DockBuilderDockWindow(TranslateLabel("assets.console", "Console##editor").c_str(), bottom);
    ImGui::DockBuilderDockWindow(TranslateLabel("panel.game", "Game##editor").c_str(), center);
    ImGui::DockBuilderDockWindow("Viewport##editor", center);
    ImGui::DockBuilderFinish(dockspaceId);
}

// Dockspace unter Menue- und Werkzeugleiste. Die Szene ist ein normales Panel
// ("Viewport"), daher kein durchlaessiger Mittelbereich noetig.
static void UpdateEditorDockSpace(EditorFrameContext& ctx)
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 pos(viewport->Pos.x, viewport->Pos.y + (kEditorTopHeight * ImGui::GetStyle().FontScaleMain));
    const ImVec2 size(viewport->Size.x, std::max(1.f, viewport->Size.y - (kEditorTopHeight * ImGui::GetStyle().FontScaleMain)));

    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(size);
    ImGui::SetNextWindowViewport(viewport->ID);
    constexpr ImGuiWindowFlags hostFlags =
        ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
    ImGui::Begin("##EditorDockHost", nullptr, hostFlags);
    ImGui::PopStyleVar(3);

    // Neue ID gegenueber dem Layout ohne Viewport-Panel: alte imgui.ini-Layouts
    // werden nicht weiterverwendet, das Standardlayout wird neu aufgebaut.
    const ImGuiID dockspaceId = ImGui::GetID("EditorDockSpaceViewport");
    if (ctx.resetDockLayout || ImGui::DockBuilderGetNode(dockspaceId) == nullptr)
    {
        BuildDefaultDockLayout(dockspaceId, size);
        ctx.resetDockLayout = false;
    }
    ImGui::DockSpace(dockspaceId, ImVec2(0.f, 0.f));
    ImGui::End();
}

// Prefab-Modus: "Zur Szene" oben links im Viewport. Scheitert das Speichern,
// bleibt der Prefab-Modus offen und die Meldung steht direkt am Knopf.
static void DrawViewportPrefabBar(EditorFrameContext& ctx, const ImVec2& origin)
{
    if (!IsInPrefabMode(ctx.state) || ctx.playActive)
        return;
    static std::string s_exitError;
    static double s_exitErrorUntil = 0.0;

    ImGui::SetCursorScreenPos(ImVec2(origin.x + 10.f, origin.y + 10.f));
    ImVec4 background = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
    background.w = 0.88f;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, background);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.f, 6.f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.f);
    const bool open = ImGui::BeginChild("##viewport_prefab", ImVec2(0.f, 0.f),
        ImGuiChildFlags_AutoResizeX | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding |
        ImGuiChildFlags_Borders,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    if (open)
    {
        const bool readOnly = ctx.state.prefabModeReadOnly;
        const std::string label = std::string(ICON_LC_LOG_OUT "  ") + Translate("hierarchy.back") + "##vp_prefab_back";
        if (EditorActionButton(label.c_str(), ImVec2(0.f, 0.f), EditorButtonTone::Primary))
        {
            if (ExitPrefabMode(ctx))
                s_exitError.clear();
            else
            {
                s_exitError = ctx.lastFileMessage.empty() ? std::string(Translate("prefab.exit_failed"))
                                                          : ctx.lastFileMessage;
                s_exitErrorUntil = ImGui::GetTime() + 6.0;
            }
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", readOnly ? Translate("hierarchy.back_read_only") : Translate("hierarchy.back_save"));
        if (!s_exitError.empty() && ImGui::GetTime() < s_exitErrorUntil)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, theme::kDanger);
            ImGui::TextUnformatted(s_exitError.c_str());
            ImGui::PopStyleColor();
        }
    }
    ImGui::EndChild();
}

// Partikel-Leiste unten links im Viewport. Wirkt auf die Emitter, die die
// Vorschau gerade abspielt: Auswahl samt Kindern, oder alle mit "Alle".
static void DrawViewportParticleBar(EditorFrameContext& ctx, const ImVec2& origin, float width, float height)
{
    if (ctx.playActive || width < 120.f || height < 80.f)
        return;

    std::vector<EntityID> emitters;
    if (ctx.state.playAllParticles)
    {
        ctx.world.View<ParticleEmitterComponent>([&](EntityID id, ParticleEmitterComponent&) {
            emitters.push_back(id);
        });
    }
    else
    {
        std::unordered_set<EntityID> seen;
        std::vector<EntityID> pending(ctx.state.multiSelection.begin(), ctx.state.multiSelection.end());
        pending.push_back(ctx.state.selectedEntity);
        while (!pending.empty())
        {
            const EntityID id = pending.back();
            pending.pop_back();
            if (!id.IsValid() || !ctx.world.IsAlive(id) || !seen.insert(id).second)
                continue;
            if (ctx.world.Get<ParticleEmitterComponent>(id))
                emitters.push_back(id);
            if (const auto* children = ctx.world.Get<ChildrenComponent>(id))
                pending.insert(pending.end(), children->children.begin(), children->children.end());
        }
    }
    // Ohne Emitter nur zeigen, wenn "Alle" an ist, damit man es wieder ausschalten kann.
    if (emitters.empty() && !ctx.state.playAllParticles)
        return;

    size_t active = 0u, looping = 0u;
    uint32_t particles = 0u;
    for (const EntityID id : emitters)
    {
        const auto* emitter = ctx.world.Get<ParticleEmitterComponent>(id);
        active += emitter->playing ? 1u : 0u;
        looping += emitter->looping ? 1u : 0u;
        if (ctx.particleCount)
            particles += ctx.particleCount(id);
    }

    const ImVec2 padding(8.f, 6.f);
    const float barHeight = ImGui::GetFrameHeight() + padding.y * 2.f;
    ImGui::SetCursorScreenPos(ImVec2(origin.x + 10.f, origin.y + height - barHeight - 10.f));
    ImVec4 background = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
    background.w = 0.88f;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, background);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, padding);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.f);
    const bool open = ImGui::BeginChild("##viewport_particles", ImVec2(0.f, 0.f),
        ImGuiChildFlags_AutoResizeX | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding |
        ImGuiChildFlags_Borders,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    if (open)
    {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(ICON_LC_SPARKLES);
        ImGui::SameLine();

        ImGui::BeginDisabled(emitters.empty() || !ctx.restartParticleEmitter);
        const std::string restart = std::string(ICON_LC_REFRESH_CW "  ") + Translate("common.restart") + "##vp_pe_restart";
        if (ImGui::Button(restart.c_str()))
            for (const EntityID id : emitters)
                ctx.restartParticleEmitter(id);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", Translate("particles.restart_tip"));

        // Gemischte Werte zeigt ImGui als Strich; ein Klick setzt alle gleich.
        const auto toggle = [&](const char* key, const char* id, size_t on, bool ParticleEmitterComponent::* member,
                                const char* tip) {
            ImGui::SameLine();
            bool value = !emitters.empty() && on == emitters.size();
            const bool mixed = on > 0u && on < emitters.size();
            if (mixed)
                ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true);
            const bool clicked = ImGui::Checkbox(TranslateLabel(key, id).c_str(), &value);
            if (mixed)
                ImGui::PopItemFlag();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s", Translate(tip));
            if (!clicked)
                return;
            const std::string historyKey = std::string("viewport_particles") + id;
            BeginPendingSceneEdit(ctx, historyKey, "history.edit_particles");
            for (const EntityID entity : emitters)
                if (auto* emitter = ctx.world.Get<ParticleEmitterComponent>(entity))
                    emitter->*member = value;
            CommitPendingSceneEdit(ctx, historyKey);
        };
        toggle("field.enabled", "##vp_pe_active", active, &ParticleEmitterComponent::playing, "particles.bar_active_tip");
        toggle("field.loop", "##vp_pe_loop", looping, &ParticleEmitterComponent::looping, "particles.bar_loop_tip");
        ImGui::EndDisabled();

        ImGui::SameLine();
        ImGui::Checkbox(TranslateLabel("particles.bar_all", "##vp_pe_all").c_str(), &ctx.state.playAllParticles);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", Translate("toolbar.particles_tip"));

        ImGui::SameLine();
        const std::string status = std::to_string(emitters.size()) + " " + Translate("particles.bar_emitters") +
                                   "  \xC2\xB7  " + std::to_string(particles) + " " + Translate("particles.bar_particles");
        ImGui::TextDisabled("%s", status.c_str());
    }
    ImGui::EndChild();
}

// Szene als Bild im Panel "Viewport". Die App rendert die Szene in ein Ziel in
// genau dieser Groesse (sceneViewRequest*) und liefert es als sceneViewTexture.
static void DrawSceneViewportPanel(EditorFrameContext& ctx)
{
    const bool requestFocus = ctx.focusScenePanel;
    if (requestFocus)
    {
        ImGui::SetNextWindowCollapsed(false);
        ImGui::SetNextWindowFocus();
        ctx.viewportPanelVisible = true;
    }
    ctx.sceneViewHovered = false;
    ctx.sceneViewVisible = false;
    // Closed panel: keep the target size like a hidden tab, so reopening does
    // not recreate the render target.
    if (!ctx.viewportPanelVisible)
        return;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
    const bool visible = ImGui::Begin("Viewport##editor", &ctx.viewportPanelVisible,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    // Gespeicherte Dock-Reiter werden beim ersten Begin erst aufgebaut. Den
    // Start-Fokus behalten, bis der Viewport nach diesem Aufbau sichtbar ist.
    if (requestFocus && visible && !ImGui::IsWindowAppearing())
        ctx.focusScenePanel = false;
    if (!visible)
    {
        // Verdeckter Reiter: Zielgroesse und Bildbereich behalten (kein Umschalten),
        // die Maus gehoert dann dem verdeckenden Panel.
        ImGui::End();
        return;
    }

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = std::max(1.f, std::floor(avail.x));
    const float height = std::max(1.f, std::floor(avail.y));
    ctx.sceneViewVisible = true;
    ctx.sceneViewRequestWidth = static_cast<uint32_t>(width);
    ctx.sceneViewRequestHeight = static_cast<uint32_t>(height);
    ctx.sceneViewRectValid = true;
    ctx.sceneViewX = origin.x;
    ctx.sceneViewY = origin.y;
    ctx.sceneViewWidth = width;
    ctx.sceneViewHeight = height;

    if (ctx.sceneViewTexture)
    {
        const ImVec2 uv0 = ctx.sceneViewFlipY ? ImVec2(0.f, 1.f) : ImVec2(0.f, 0.f);
        const ImVec2 uv1 = ctx.sceneViewFlipY ? ImVec2(1.f, 0.f) : ImVec2(1.f, 1.f);
        ImGui::Image(reinterpret_cast<ImTextureID>(ctx.sceneViewTexture), ImVec2(width, height), uv0, uv1);
    }
    else
    {
        ImGui::Dummy(ImVec2(width, height));
    }
    ctx.sceneViewHovered = ImGui::IsItemHovered();
    RenderPendingEditorIcons(ctx, ImGui::GetWindowDrawList());
    DrawViewportParticleBar(ctx, origin, width, height);
    DrawViewportPrefabBar(ctx, origin);
    ImGui::End();
}

// Bild der Spielkamera im Panel "Game"; waehrend Play mit dem Bildschirmtext der
// Scripts. Muss jeden Frame eingereicht werden, auch wenn der Reiter verdeckt ist,
// sonst wechselt der Dock-Knoten jeden Frame den aktiven Reiter (Flimmern).
static void DrawGamePanel(EditorFrameContext& ctx)
{
    ctx.gameViewVisible = false;
    ctx.gameViewFocused = false;
    if (ctx.focusGamePanel)
    {
        ImGui::SetNextWindowCollapsed(false);
        ImGui::SetNextWindowFocus();
        ctx.focusGamePanel = false;
        ctx.gamePanelVisible = true;
    }
    if (!ctx.gamePanelVisible)
        return;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
    const bool visible = ImGui::Begin(TranslateLabel("panel.game", "Game##editor").c_str(), &ctx.gamePanelVisible,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::PopStyleVar();
    if (!visible)
    {
        // Verdeckter Reiter: Zielgroesse behalten (kein Neuanlegen beim Zurueckschalten).
        ImGui::End();
        return;
    }

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = std::max(1.f, std::floor(avail.x));
    const float height = std::max(1.f, std::floor(avail.y));
    ctx.gameViewVisible = true;
    ctx.gameViewX = origin.x;
    ctx.gameViewY = origin.y;
    ctx.gameViewWidth = width;
    ctx.gameViewHeight = height;
    ctx.gameViewRequestWidth = static_cast<uint32_t>(width);
    ctx.gameViewRequestHeight = static_cast<uint32_t>(height);

    if (ctx.gameViewTexture)
    {
        const ImVec2 uv0 = ctx.gameViewFlipY ? ImVec2(0.f, 1.f) : ImVec2(0.f, 0.f);
        const ImVec2 uv1 = ctx.gameViewFlipY ? ImVec2(1.f, 0.f) : ImVec2(1.f, 1.f);
        ImGui::Image(reinterpret_cast<ImTextureID>(ctx.gameViewTexture), ImVec2(width, height), uv0, uv1);
    }
    else
    {
        ImGui::Dummy(ImVec2(width, height));
    }

    // Bildschirmtext der Scripts (Pixel des Spielbilds, oben links) ueber dem Bild.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(origin, ImVec2(origin.x + width, origin.y + height), true);
    for (const EditorFrameContext::GameOverlayText& line : ctx.gameOverlayText)
    {
        const ImVec4 color(line.color.x, line.color.y, line.color.z, line.color.w);
        dl->AddText(ImGui::GetFont(), line.size, ImVec2(origin.x + line.x, origin.y + line.y),
                    ImGui::ColorConvertFloat4ToU32(color), line.text.c_str());
    }
    dl->PopClipRect();

    if (!ctx.gameViewTexture || !ctx.playActive)
    {
        const char* hint = !ctx.gameViewTexture ? "Keine Hauptkamera in der Szene."
                                                : "Start spielt die Szene hier.";
        const ImVec2 size = ImGui::CalcTextSize(hint);
        const ImVec2 pos(origin.x + (width - size.x) * 0.5f, origin.y + height - size.y - 16.f);
        // Eigener Hintergrund: auf hellem Spielbild sonst kaum lesbar.
        dl->AddRectFilled(ImVec2(pos.x - 10.f, pos.y - 5.f), ImVec2(pos.x + size.x + 10.f, pos.y + size.y + 5.f),
                          ImGui::GetColorU32(ImGuiCol_PopupBg, 0.9f), 4.f);
        dl->AddText(pos, ImGui::GetColorU32(ImGuiCol_Text), hint);
    }

    // Waehrend Play gehen Tastatur und Maus an das Spiel, solange der Reiter den
    // Fokus hat (Klick ins Bild). Escape gibt den Fokus an den Editor zurueck.
    if (ctx.playActive && !ctx.playPaused && ImGui::IsWindowFocused())
    {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            ImGui::SetWindowFocus(nullptr);
        else
            ctx.gameViewFocused = true;
    }
    if (ctx.gameViewFocused)
    {
        dl->AddRect(origin, ImVec2(origin.x + width, origin.y + height),
                    ImGui::GetColorU32(ImGuiCol_TabSelectedOverline), 0.f, 0, 2.f);
    }
    ImGui::End();
}

static void UpdateEditorLayout(EditorFrameContext& ctx)
{
    if (ctx.renderViewWidth > 1.f && ctx.renderViewHeight > 1.f)
        return;

    const EditorLayoutRects rects = ComputeEditorLayout(ctx, ImGui::GetIO().DisplaySize);
    ctx.renderViewX = rects.renderPos.x;
    ctx.renderViewY = rects.renderPos.y;
    ctx.renderViewWidth = rects.renderSize.x;
    ctx.renderViewHeight = rects.renderSize.y;
}

static bool IsMouseInRenderView(const EditorFrameContext& ctx)
{
    // Verdeckter Viewport-Reiter: sein Rechteck gehoert dann einem anderen Panel
    // (z. B. Game) - keine Kamerasteuerung mit WASD ueber dem Spiel.
    if (ctx.sceneViewRectValid && !ctx.sceneViewVisible)
        return false;
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    return mouse.x >= ctx.renderViewX &&
           mouse.y >= ctx.renderViewY &&
           mouse.x < ctx.renderViewX + ctx.renderViewWidth &&
           mouse.y < ctx.renderViewY + ctx.renderViewHeight;
}

static float EditorPickMaxDistance(const EditorCameraState& cam) noexcept
{
    return std::max(cam.farPlane, 100000.f);
}
static math::Mat4 CurrentWorldMatrix(EditorFrameContext& ctx, EntityID entity) noexcept;

// Unity-Inspector-Konvention:
// Euler-Werte X/Y/Z werden in der Reihenfolge Z, dann X, dann Y angewendet.
// Quaternion nach Euler-Winkeln (Grad), Reihenfolge passend zu Quat::FromEulerDeg.
static math::Vec3 QuatToEulerDeg(const math::Quat& q) noexcept
{
    float sinPitch = 2.f * (q.w * q.x - q.y * q.z);
    float clamped  = std::clamp(sinPitch, -1.f, 1.f);
    float pitch    = std::asin(clamped);
    float yaw, roll;
    if (std::abs(sinPitch) < 0.9999f)
    {
        yaw  = std::atan2(2.f * (q.x * q.z + q.w * q.y),
                          1.f - 2.f * (q.x * q.x + q.y * q.y));
        roll = std::atan2(2.f * (q.x * q.y + q.w * q.z),
                          1.f - 2.f * (q.x * q.x + q.z * q.z));
    }
    else
    {
        yaw  = std::atan2(-2.f * (q.x * q.z - q.w * q.y),
                           1.f -  2.f * (q.y * q.y + q.z * q.z));
        roll = 0.f;
    }
    return { pitch * math::RAD_TO_DEG, yaw * math::RAD_TO_DEG, roll * math::RAD_TO_DEG };
}

static float NormalizeAngleDeg(float angle) noexcept
{
    while (angle > 360.f)  angle -= 360.f;
    while (angle < 0.f)    angle += 360.f;
    return angle;
}

static math::Vec3 NormalizeEulerDeg(const math::Vec3& euler) noexcept
{
    return {
        NormalizeAngleDeg(euler.x),
        NormalizeAngleDeg(euler.y),
        NormalizeAngleDeg(euler.z)
    };
}

static math::Vec3 MatrixTranslation(const math::Mat4& m) noexcept
{
    return {m.m[3][0], m.m[3][1], m.m[3][2]};
}

static math::Quat QuatFromRotationMatrix(const math::Mat4& m) noexcept
{
    const float trace = m.m[0][0] + m.m[1][1] + m.m[2][2];
    if (trace > 0.f)
    {
        const float s = std::sqrt(trace + 1.f) * 2.f;
        return { (m.m[1][2] - m.m[2][1]) / s,
                 (m.m[2][0] - m.m[0][2]) / s,
                 (m.m[0][1] - m.m[1][0]) / s,
                 0.25f * s };
    }
    if (m.m[0][0] > m.m[1][1] && m.m[0][0] > m.m[2][2])
    {
        const float s = std::sqrt(1.f + m.m[0][0] - m.m[1][1] - m.m[2][2]) * 2.f;
        return { 0.25f * s,
                 (m.m[1][0] + m.m[0][1]) / s,
                 (m.m[2][0] + m.m[0][2]) / s,
                 (m.m[1][2] - m.m[2][1]) / s };
    }
    if (m.m[1][1] > m.m[2][2])
    {
        const float s = std::sqrt(1.f + m.m[1][1] - m.m[0][0] - m.m[2][2]) * 2.f;
        return { (m.m[1][0] + m.m[0][1]) / s,
                 0.25f * s,
                 (m.m[2][1] + m.m[1][2]) / s,
                 (m.m[2][0] - m.m[0][2]) / s };
    }

    const float s = std::sqrt(1.f + m.m[2][2] - m.m[0][0] - m.m[1][1]) * 2.f;
    return { (m.m[2][0] + m.m[0][2]) / s,
             (m.m[2][1] + m.m[1][2]) / s,
             0.25f * s,
             (m.m[0][1] - m.m[1][0]) / s };
}

static math::Vec3 MatrixScale(const math::Mat4& m) noexcept
{
    const math::Vec3 x{m.m[0][0], m.m[0][1], m.m[0][2]};
    const math::Vec3 y{m.m[1][0], m.m[1][1], m.m[1][2]};
    const math::Vec3 z{m.m[2][0], m.m[2][1], m.m[2][2]};
    return {x.Length(), y.Length(), z.Length()};
}

static void DecomposeTRS(const math::Mat4& m,
                         math::Vec3& outTranslation,
                         math::Quat& outRotation,
                         math::Vec3& outScale) noexcept
{
    outTranslation = MatrixTranslation(m);
    outScale = MatrixScale(m);

    math::Mat4 rotationMatrix = math::Mat4::Identity();
    if (outScale.x > math::EPSILON)
    {
        rotationMatrix.m[0][0] = m.m[0][0] / outScale.x;
        rotationMatrix.m[0][1] = m.m[0][1] / outScale.x;
        rotationMatrix.m[0][2] = m.m[0][2] / outScale.x;
    }
    if (outScale.y > math::EPSILON)
    {
        rotationMatrix.m[1][0] = m.m[1][0] / outScale.y;
        rotationMatrix.m[1][1] = m.m[1][1] / outScale.y;
        rotationMatrix.m[1][2] = m.m[1][2] / outScale.y;
    }
    if (outScale.z > math::EPSILON)
    {
        rotationMatrix.m[2][0] = m.m[2][0] / outScale.z;
        rotationMatrix.m[2][1] = m.m[2][1] / outScale.z;
        rotationMatrix.m[2][2] = m.m[2][2] / outScale.z;
    }

    outRotation = QuatFromRotationMatrix(rotationMatrix).Normalized();
}

static bool HasNonUniformScale(const math::Vec3& scale) noexcept
{
    return std::abs(scale.x - scale.y) > 1e-4f ||
           std::abs(scale.x - scale.z) > 1e-4f ||
           std::abs(scale.y - scale.z) > 1e-4f;
}

static math::Mat4 CurrentWorldMatrix(EditorFrameContext& ctx, EntityID entity) noexcept
{
    if (const auto* wt = ctx.world.Get<WorldTransformComponent>(entity))
        return wt->matrix;

    const auto* t = ctx.world.Get<TransformComponent>(entity);
    if (!t)
        return math::Mat4::Identity();

    return math::Mat4::TRS(WorldPosition(ctx, entity), WorldRotation(ctx, entity), WorldScale(ctx, entity));
}

static bool IsEditorHandleActiveOrHovered(const EditorState& state) noexcept
{
    // Nur der Gizmo des aktiven Modus zählt; veraltete Zustände eines anderen
    // Modus dürfen die Selektion nicht dauerhaft sperren.
    bool gizmoActive = false;
    switch (state.gizmoMode)
    {
    case GizmoMode::Position:
        gizmoActive = state.gizmoDragAxis >= 0 || state.gizmoHoveredAxis >= 0;
        break;
    case GizmoMode::Rotation:
        gizmoActive = state.rotGizmoDragAxis >= 0 || state.rotGizmoHoveredAxis >= 0;
        break;
    case GizmoMode::Scale:
        gizmoActive = state.sclGizmoDragAxis >= 0 || state.sclGizmoHoveredAxis >= 0;
        break;
    default:
        break;
    }
    return gizmoActive ||
           state.obbDragFaceIdx >= 0 || state.obbHoveredFace >= 0;
}

// Strahl gegen eine lokale Box [-half, +half] unter der Transformation localToWorld.
// t bleibt der Welt-Strahlparameter (affine Abbildung erhaelt ihn).
static bool IntersectRayLocalBox(const collision::Ray& ray, const math::Mat4& localToWorld,
                                 const math::Vec3& half, float maxDistance, float& outT)
{
    const math::Mat4 worldToLocal = localToWorld.Inverse();
    const math::Vec3 o = worldToLocal.TransformPoint(ray.origin);
    const math::Vec3 d = worldToLocal.TransformDirection(ray.direction);
    // Kamera steht in der Box → nicht picken, sonst faengt sie jeden Klick ab.
    if (std::fabs(o.x) <= half.x && std::fabs(o.y) <= half.y && std::fabs(o.z) <= half.z)
        return false;
    float tMin = 0.f, tMax = maxDistance;
    const float oa[3] = { o.x, o.y, o.z }, da[3] = { d.x, d.y, d.z }, ha[3] = { half.x, half.y, half.z };
    for (int i = 0; i < 3; ++i)
    {
        if (std::fabs(da[i]) < 1e-8f)
        {
            if (oa[i] < -ha[i] || oa[i] > ha[i])
                return false;
            continue;
        }
        float t0 = (-ha[i] - oa[i]) / da[i];
        float t1 = ( ha[i] - oa[i]) / da[i];
        if (t0 > t1) std::swap(t0, t1);
        tMin = std::max(tMin, t0);
        tMax = std::min(tMax, t1);
        if (tMin > tMax)
            return false;
    }
    outT = tMin;
    return true;
}

// Collider ohne Mesh (z.B. Kollisions-Kinder "Collider") trifft der
// Mesh-Raycast nicht — sie werden hier ueber ihre Form anklickbar.
static bool PickMeshlessColliders(EditorFrameContext& ctx, const collision::Ray& ray,
                                  float maxDistance, EntityID& outEntity, float& outT)
{
    float best = maxDistance;
    EntityID bestEntity = NULL_ENTITY;
    auto consider = [&](EntityID id, const math::Mat4& localToWorld, const math::Vec3& half) {
        float t = 0.f;
        if (IntersectRayLocalBox(ray, localToWorld, half, best, t))
        {
            best = t;
            bestEntity = id;
        }
    };
    auto eligible = [&](EntityID id) -> const WorldTransformComponent* {
        if (ctx.world.Has<MeshComponent>(id) || !IsScenePickEntity(ctx.world, id))
            return nullptr;
        if (const auto* active = ctx.world.Get<ActiveComponent>(id); active && !active->active)
            return nullptr;
        return ctx.world.Get<WorldTransformComponent>(id);
    };

    ctx.world.View<OBBComponent>([&](EntityID id, const OBBComponent& obb) {
        if (obb.isTrigger)
            return;   // Trigger-Zonen umschliessen oft Objekte und wuerden Klicks schlucken
        if (const auto* wt = eligible(id))
            consider(id, wt->matrix * math::Mat4::TRS(obb.centerOffset, obb.orientation, {1.f, 1.f, 1.f}),
                     obb.halfExtents);
    });
    ctx.world.View<SphereColliderComponent>([&](EntityID id, const SphereColliderComponent& sphere) {
        const auto* wt = eligible(id);
        if (!wt)
            return;
        const float s = std::max({ std::fabs(wt->scale.x), std::fabs(wt->scale.y), std::fabs(wt->scale.z) });
        float t = 0.f;
        if (collision::SceneQueries::IntersectRaySphere(
                ray, { wt->matrix.TransformPoint(sphere.centerOffset), sphere.radius * s }, best, t))
        {
            best = t;
            bestEntity = id;
        }
    });
    ctx.world.View<CapsuleColliderComponent>([&](EntityID id, const CapsuleColliderComponent& capsule) {
        if (const auto* wt = eligible(id))
        {
            // Naeherung als Box um die Kapsel.
            const float h = std::max(capsule.radius, capsule.height * 0.5f);
            math::Vec3 half{ capsule.radius, capsule.radius, capsule.radius };
            (&half.x)[static_cast<int>(capsule.axis)] = h;
            consider(id, wt->matrix * math::Mat4::TRS(capsule.centerOffset, math::Quat::Identity(), {1.f, 1.f, 1.f}),
                     half);
        }
    });

    if (!bestEntity.IsValid())
        return false;
    outEntity = bestEntity;
    outT = best;
    return true;
}

static void SelectEntityUnderMouse(EditorFrameContext& ctx)
{
    // Gizmo- oder OBB-Drag hat Vorrang — Entity-Selektion überspringen.
    // Auch Hover reicht: beim nächsten Klick wird der Gizmo-Drag gestartet,
    // nicht eine andere Entity selektiert.
    if (IsEditorHandleActiveOrHovered(ctx.state))
        return;

    ImGuiIO& io = ImGui::GetIO();
    if (SceneMouseBlocked(ctx) || ImGui::IsAnyItemHovered() || !ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        return;

    if (ShouldCancelSelectionChangeForActiveInput())
        return;

    if (!IsMouseInRenderView(ctx) ||
        ctx.renderViewWidth <= 1.f || ctx.renderViewHeight <= 1.f)
    {
        return;
    }

    const ImVec2 mouse = io.MousePos;

    // Editor-Symbole (Licht, Partikel, Audio) liegen ueber der Szene und haben Vorrang.
    if (const EntityID icon = PickEditorIcon(ctx, mouse, /*cycleFromSelection=*/!io.KeyCtrl); icon.IsValid())
    {
        if (io.KeyCtrl)
            AddToMultiSelection(ctx, icon);
        else
        {
            ClearMultiSelection(ctx);
            ctx.state.selectedEntity  = icon;
            ctx.state.selectionSource = SelectionSource::Viewport;
        }
        return;
    }

    const SceneScreenRect view = GetSceneScreenRect(ctx);
    const float ndcX = ((mouse.x - view.min.x) / view.size.x) * 2.f - 1.f;
    const float ndcY = 1.f - ((mouse.y - view.min.y) / view.size.y) * 2.f;

    const EditorCameraState& cam = ctx.state.editorCamera;
    const float aspect = ctx.renderViewWidth / ctx.renderViewHeight;
    const float tanHalfFov = std::tan((cam.fovDeg * math::DEG_TO_RAD) * 0.5f);
    const math::Quat rot = math::Quat::FromEulerDeg(cam.pitchDeg, cam.yawDeg, 0.f);
    const math::Vec3 rayDir = rot.Rotate({
        ndcX * aspect * tanHalfFov,
        ndcY * tanHalfFov,
        -1.f
    }).Normalized();

    const collision::Ray ray{ cam.position, rayDir };
    const float kMaxDist = EditorPickMaxDistance(cam);

    collision::RaycastHit hit{};
    mesh_renderer::MeshCollisionPipeline pickPipeline;
    pickPipeline.Build(ctx.world);
    mesh_renderer::MeshCollisionRaycastOptions pickOptions{};
    pickOptions.boundsFallbackAfterTriangleMiss = true;
    pickOptions.includeEntity = [&](EntityID entity) {
        return IsScenePickEntity(ctx.world, entity);
    };
    bool found = pickPipeline.Raycast(ctx.world, ctx.registry, ray, kMaxDist, hit, pickOptions);

    // Kamera-Gizmo Picking: Kugeltest pro Gizmo (CPU-Mesh meist nicht verfügbar)
    {
        float bestDist = found ? hit.distance : kMaxDist;
        ctx.world.View<EditorCameraGizmoComponent, WorldTransformComponent>(
            [&](EntityID id,
                const EditorCameraGizmoComponent&,
                const WorldTransformComponent& wt)
        {
            const auto* bounds = ctx.world.Get<BoundsComponent>(id);
            const float pickRadius = std::max(0.5f, bounds ? bounds->boundingSphere : 0.5f);

            float gizmoT = 0.f;
            if (collision::SceneQueries::IntersectRaySphere(
                    ray, { wt.position, pickRadius }, bestDist, gizmoT))
            {
                bestDist   = gizmoT;
                hit.entity = id;
                hit.distance = gizmoT;
                found = true;
            }
        });
    }

    // Collider ohne Mesh: naeher als der Mesh-Treffer → hat Vorrang
    // (das Kollisions-Kind liegt meist im/am Mesh seines Parents).
    {
        EntityID colliderEntity = NULL_ENTITY;
        float colliderT = 0.f;
        if (PickMeshlessColliders(ctx, ray, found ? hit.distance : kMaxDist, colliderEntity, colliderT))
        {
            hit.entity   = colliderEntity;
            hit.distance = colliderT;
            found = true;
        }
    }

    if (found && hit.entity.IsValid() && ctx.world.IsAlive(hit.entity))
    {
        EntityID pickedEntity = hit.entity;
        // Kamera-Gizmo angeklickt → zur zugehörigen Kamera-Entity umleiten.
        // Variante A: modernes Gizmo mit EditorCameraGizmoComponent
        if (ctx.world.Get<EditorMaterialPreviewComponent>(hit.entity) != nullptr)
        {
            return;
        }
        else if (ctx.world.Get<EditorDpadGizmoComponent>(hit.entity) != nullptr)
        {
            return;
        }
        else if (ctx.world.Get<EditorRotateGizmoComponent>(hit.entity) != nullptr)
        {
            return;
        }
        else if (ctx.world.Get<EditorScaleGizmoComponent>(hit.entity) != nullptr)
        {
            return;
        }
        else if (ctx.world.Get<EditorAssetThumbnailComponent>(hit.entity) != nullptr)
        {
            return;
        }
        else if (const auto* gizmo = ctx.world.Get<EditorCameraGizmoComponent>(hit.entity))
        {
            if (gizmo->cameraEntity.IsValid() && ctx.world.IsAlive(gizmo->cameraEntity))
                pickedEntity = gizmo->cameraEntity;
        }
        // Variante B: Legacy-Gizmo (kein Component-Tag) — nächste lebende Kamera suchen
        else if (const auto* mc = ctx.world.Get<MeshComponent>(hit.entity))
        {
            if (mc->layerMask == renderer::LAYER_EDITOR_GIZMO)
            {
                // Nächste Kamera-Entity an derselben Position finden
                const auto* hitWtc = ctx.world.Get<WorldTransformComponent>(hit.entity);
                if (hitWtc)
                {
                    float    bestDist = 1e9f;
                    EntityID bestCam  = NULL_ENTITY;
                    ctx.world.ForEachAlive([&](EntityID id) {
                        if (!ctx.world.Has<CameraComponent>(id)) return;
                        const auto* wtc = ctx.world.Get<WorldTransformComponent>(id);
                        if (!wtc) return;
                        const math::Vec3 d = wtc->position - hitWtc->position;
                        const float dist = d.x*d.x + d.y*d.y + d.z*d.z;
                        if (dist < bestDist) { bestDist = dist; bestCam = id; }
                    });
                    if (bestCam.IsValid()) pickedEntity = bestCam;
                }
            }
        }

        if (ImGui::GetIO().KeyCtrl)
            AddToMultiSelection(ctx, pickedEntity);
        else
        {
            ClearMultiSelection(ctx);
            ctx.state.selectedEntity  = pickedEntity;
            ctx.state.selectionSource = SelectionSource::Viewport;

            // Kamera per Viewport angeklickt → Vorschau einmalig öffnen.
            // Guard zurücksetzen damit DrawCameraPreviewWindow das Fenster aufmacht,
            // auch wenn es vorher manuell geschlossen wurde.
            // (Einmaliger Reset hier statt dauerhaft in DrawCameraPreviewWindow —
            //  sonst kann der User das Fenster nicht schließen.)
            if (ctx.world.Has<CameraComponent>(pickedEntity))
                ctx.state.cameraPreviewLastAutoOpenSelection = NULL_ENTITY;
        }
        return;
    }

    if (!ImGui::GetIO().KeyCtrl)
    {
        ClearMultiSelection(ctx);
        ctx.state.selectedEntity = NULL_ENTITY;
    }
}

static void UpdateEditorCamera(EditorFrameContext& ctx)
{
    ImGuiIO& io = ImGui::GetIO();
    const bool mouseInRenderView = IsMouseInRenderView(ctx);

    EditorCameraState& cam = ctx.state.editorCamera;

    const math::Quat rot     = math::Quat::FromEulerDeg(cam.pitchDeg, cam.yawDeg, 0.f);
    const math::Vec3 forward = rot.Rotate({0.f, 0.f, -1.f});
    const math::Vec3 right   = rot.Rotate({1.f, 0.f,  0.f});

    if (mouseInRenderView && !SceneMouseBlocked(ctx))
    {
        if (io.MouseWheel != 0.f)
        {
            const float boost = ImGui::IsKeyDown(ImGuiKey_LeftShift) || ImGui::IsKeyDown(ImGuiKey_RightShift) ? 5.f : 1.f;
            cam.position += forward * (io.MouseWheel * cam.moveSpeed * 0.1f * boost);
        }

        if (io.MouseDown[1])
        {
            const auto& preferences = GetEditorPreferences();
            const float sensitivity = .25f * preferences.mouseSensitivity;
            cam.yawDeg   -= io.MouseDelta.x * sensitivity;
            cam.pitchDeg -= io.MouseDelta.y * sensitivity * (preferences.invertMouseY ? -1.f : 1.f);
        }
    }

    // WASD + Pfeiltasten: gesperrt nur wenn ein Texteingabefeld aktiv ist
    if (mouseInRenderView && !io.WantTextInput)
    {
        const float dt    = ctx.deltaSeconds;
        const float boost = ImGui::IsKeyDown(ImGuiKey_LeftShift) || ImGui::IsKeyDown(ImGuiKey_RightShift) ? 5.f : 1.f;
        const float speed = cam.moveSpeed * boost;

        if (ImGui::IsKeyDown(ImGuiKey_W)) cam.position += forward * (speed * dt);
        if (ImGui::IsKeyDown(ImGuiKey_S)) cam.position -= forward * (speed * dt);
        if (ImGui::IsKeyDown(ImGuiKey_A)) cam.position -= right   * (speed * dt);
        if (ImGui::IsKeyDown(ImGuiKey_D)) cam.position += right   * (speed * dt);
        if (ImGui::IsKeyDown(ImGuiKey_E)) cam.position.y += speed * dt;
        if (ImGui::IsKeyDown(ImGuiKey_Q)) cam.position.y -= speed * dt;

        const float rotSpeed = 60.f * dt * boost;
        if (ImGui::IsKeyDown(ImGuiKey_LeftArrow))  cam.yawDeg   += rotSpeed;
        if (ImGui::IsKeyDown(ImGuiKey_RightArrow)) cam.yawDeg   -= rotSpeed;
        if (ImGui::IsKeyDown(ImGuiKey_UpArrow))    cam.pitchDeg += rotSpeed;
        if (ImGui::IsKeyDown(ImGuiKey_DownArrow))  cam.pitchDeg -= rotSpeed;
    }

    cam.pitchDeg = std::clamp(cam.pitchDeg, -89.f, 89.f);
}

static void DrawCameraPreviewWindow(EditorFrameContext& ctx)
{
    const EntityID selected = ctx.state.selectedEntity;
    if (!ctx.state.showCameraPreview)
    {
        ctx.state.cameraPreviewWindowOpen = false;
        ctx.state.cameraPreviewLastAutoOpenSelection = NULL_ENTITY;
        return;
    }
    if (selected.IsValid() && ctx.world.Has<CameraComponent>(selected))
    {
        if (ctx.state.cameraPreviewLastAutoOpenSelection != selected)
        {
            ctx.state.cameraPreviewWindowOpen = true;
            ctx.state.cameraPreviewLastAutoOpenSelection = selected;
        }
    }
    else
    {
        ctx.state.cameraPreviewWindowOpen = false;
        ctx.state.cameraPreviewLastAutoOpenSelection = NULL_ENTITY;
    }

    if (!ctx.state.cameraPreviewWindowOpen)
        return;

    // Selektierte Kamera für Vorschau nutzen — fallback auf Main-Kamera
    EntityID previewCameraEntity = selected;
    if (!previewCameraEntity.IsValid() || !ctx.world.Has<CameraComponent>(previewCameraEntity))
        return;

    const auto* camera = ctx.world.Get<CameraComponent>(previewCameraEntity);
    if (!camera)
        return;

    ImGui::SetNextWindowPos(ImVec2(290.f, 10.f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(340.f, 230.f), ImGuiCond_FirstUseEver);
    const bool wasOpen = ctx.state.cameraPreviewWindowOpen;
    if (!ImGui::Begin(TranslateLabel("panel.camera_preview", "Camera Preview##editor").c_str(), &ctx.state.cameraPreviewWindowOpen))
    {
        ImGui::End();
        return;
    }
    if (wasOpen && !ctx.state.cameraPreviewWindowOpen)
        ctx.state.cameraPreviewLastAutoOpenSelection = ctx.state.selectedEntity;

    ImGui::Separator();

    if (ctx.cameraPreviewTexture)
    {
        void* texId = ctx.cameraPreviewTexture(previewCameraEntity);
        if (texId)
        {
            const ImVec2 avail  = ImGui::GetContentRegionAvail();
            const float  aspect = camera->aspectRatio > 1e-6f ? camera->aspectRatio : (16.f / 9.f);
            const float  w      = avail.x;
            const float  h      = w / aspect;
            const ImVec2 uv0 = ctx.previewFlipY ? ImVec2(0.f, 1.f) : ImVec2(0.f, 0.f);
            const ImVec2 uv1 = ctx.previewFlipY ? ImVec2(1.f, 0.f) : ImVec2(1.f, 1.f);
            ImGui::Image(reinterpret_cast<ImTextureID>(texId), ImVec2(w, h), uv0, uv1);
        }
        else
        {
            ImGui::TextDisabled(Translate("camera.no_preview"));
        }
    }
    else
    {
        ImGui::TextDisabled(Translate("camera.no_preview_source"));
    }

    ImGui::End();
}

static void DrawPerfOverlay(const EditorFrameContext& ctx)
{
    if (!ctx.state.showPerfOverlay || !ctx.renderStats)
        return;

    const RenderStats& s = *ctx.renderStats;

    const ImGuiIO& io  = ImGui::GetIO();
    const float pad    = 8.f;
    const ImVec2 pos   = ctx.sceneViewRectValid
        ? ImVec2(ctx.sceneViewX + ctx.sceneViewWidth - pad, ctx.sceneViewY + pad)
        : ImVec2(io.DisplaySize.x - pad, (kEditorTopHeight * ImGui::GetStyle().FontScaleMain) + pad);
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always, ImVec2(1.f, 0.f));
    ImGui::SetNextWindowBgAlpha(0.55f);
    ImGui::SetNextWindowSize(ImVec2(0.f, 0.f)); // auto-size
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
        ImGuiWindowFlags_NoMove       | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing;
    // Ohne NoBringToFrontOnFocus: solche Fenster legt ImGui beim Anlegen ganz nach hinten,
    // dann verdeckt sie das Viewport-Panel. NoInputs verhindert ohnehin jeden Fokus.

    if (ImGui::Begin("##perfoverlay", nullptr, flags))
    {
        const auto* lighting =
            ctx.frameData.GetFrameData<engine::addons::lighting::LightingFrameData>();
        const auto* shadow =
            ctx.frameData.GetFrameData<engine::addons::shadow::ShadowFrameData>();
        uint32_t shadowViewCount = 0u;
        if (shadow)
        {
            for (size_t requestIndex : shadow->currentRenderPath.requestIndices)
            {
                if (requestIndex < shadow->requests.size())
                    shadowViewCount += static_cast<uint32_t>(shadow->requests[requestIndex].views.size());
            }
        }

        ImGui::TextColored(ImVec4(1.f, 0.85f, 0.2f, 1.f),
            "Frame %5.1f ms  (%4.0f FPS)",
            io.Framerate > 0.f ? 1000.f / io.Framerate : 0.f, io.Framerate);
        if (s.backendGpuFrameMs > 0.f)
            ImGui::Text("GPU          %5.1f ms", s.backendGpuFrameMs);
        else
            ImGui::TextDisabled(Translate("perf.gpu_na"));
        ImGui::Text(Translate("perf.app_update"), ctx.appCpuTimings[0], ctx.appCpuTimings[1], ctx.appCpuTimings[2]);
        ImGui::Text(Translate("perf.gizmo_build"), ctx.appCpuTimings[3], ctx.appCpuTimings[4]);
        ImGui::Text(Translate("perf.render"), ctx.appCpuTimings[5], s.backendBeginFrameMs, s.backendAcquireMs);
        ImGui::Text(Translate("perf.prepare"), s.prepareFrameMs, s.collectShadersMs, s.collectMaterialsMs);
        ImGui::Text(Translate("perf.cpu_execute"), s.executeMs);
        ImGui::Text(Translate("perf.record"), s.executeRecordMs);
        ImGui::Text(Translate("perf.submit"), s.executeSubmitMs);
        ImGui::Text(Translate("perf.present"), s.executePresentMs);
        ImGui::Text(Translate("perf.opaque_setup"), s.opaqueCpu.setupMs);
        ImGui::Text(Translate("perf.material"), s.opaqueCpu.materialMs, s.opaqueCpu.materialBinds);
        ImGui::Text(Translate("perf.draw_calls"), s.opaqueCpu.drawMs, s.opaqueCpu.draws);
        for (const auto& pass : s.slowestPasses)
        {
            if (pass.name[0] != '\0')
                ImGui::Text("%s  %.1f ms", pass.name.data(), pass.cpuMs);
        }
        ImGui::Text(Translate("perf.inspector"), g_uiTimings.inspector, g_uiTimings.gizmos);
        ImGui::Text(Translate("perf.hierarchy"), g_uiTimings.assets, g_uiTimings.hierarchy);
        ImGui::Text(Translate("perf.materials"), g_uiTimings.materials, g_uiTimings.menus, g_uiTimings.preview);
        ImGui::Text(Translate("perf.build_graph"), s.buildGraphMs);
        ImGui::Text(Translate("perf.parallel"), s.parallelSectionMs, s.peakActiveWorkers);
        ImGui::Text("Upload       %5.1f ms  (%u KB)",
            s.collectUploadsMs + s.commitUploadsMs,
            static_cast<uint32_t>(s.uploadedBytes / 1024u));
        ImGui::Separator();
        ImGui::Text(Translate("perf.draws"),
            s.opaqueDraws, s.shadowDraws, s.transparentDraws);
        ImGui::Text(Translate("perf.instancing"),
            s.instancedDraws,
            s.instancedObjects,
            s.instancingSavedDraws);
        if (lighting)
        {
            ImGui::Text(Translate("perf.lights"),
                lighting->extractedCount,
                lighting->gpu.lightCount,
                lighting->shadowCastingCount);
        }
        if (shadow)
        {
            ImGui::Text(Translate("perf.shadow_requests"),
                static_cast<uint32_t>(shadow->requests.size()),
                static_cast<uint32_t>(shadow->currentRenderPath.requestIndices.size()),
                shadowViewCount);
            ImGui::Text(Translate("perf.shadow_culled"),
                shadow->culledReceiverCount,
                shadow->culledBudgetCount);
        }
        ImGui::Text(Translate("perf.shadow_draws"),
            s.shadowPass.views,
            s.shadowPass.drawn,
            s.shadowPass.culled);
        ImGui::Text(Translate("perf.passes"), s.graphPassCount, s.graphTransitionCount);
    }
    ImGui::End();
}

} // namespace

namespace detail {

EditorLayoutRects ComputeEditorLayout(const EditorFrameContext& ctx, const ImVec2& displaySize)
{
    const float width = std::max(displaySize.x, 640.f);
    const float height = std::max(displaySize.y, 420.f);
    const float sidePanelWidth = ctx.hierarchyPanelVisible
        ? std::clamp(ctx.hierarchyPanelWidth, 180.f, width * 0.4f) : 0.f;
    const float inspectorWidth = ctx.inspectorPanelVisible
        ? std::clamp(ctx.inspectorPanelWidth, 240.f, width * 0.45f) : 0.f;
    const bool bottomVisible = ctx.assetsPanelVisible || ctx.consolePanelVisible;
    const float bottomHeight = bottomVisible
        ? std::clamp(ctx.assetsPanelHeight, 120.f, height * 0.5f) : 0.f;
    const float top = (kEditorTopHeight * ImGui::GetStyle().FontScaleMain) + kEditorPanelGap;
    const float bottomGap = bottomVisible ? kEditorPanelGap : 0.f;
    const float leftGap = ctx.hierarchyPanelVisible ? kEditorPanelGap : 0.f;
    const float rightGap = ctx.inspectorPanelVisible ? kEditorPanelGap : 0.f;
    const float centerHeight = std::max(96.f, height - top - bottomHeight - bottomGap);
    const float centerWidth = std::max(96.f, width - sidePanelWidth - inspectorWidth - leftGap - rightGap);

    EditorLayoutRects rects{};
    rects.hierarchyPos = {0.f, top};
    rects.hierarchySize = {sidePanelWidth, centerHeight};
    rects.renderPos = {sidePanelWidth + leftGap, top};
    rects.renderSize = {centerWidth, centerHeight};
    rects.inspectorPos = {rects.renderPos.x + centerWidth + rightGap, top};
    rects.inspectorSize = {inspectorWidth, centerHeight};
    rects.assetsPos = {0.f, top + centerHeight + bottomGap};
    rects.assetsSize = {width, bottomHeight};
    if (ctx.sceneViewRectValid)
    {
        rects.renderPos = {ctx.sceneViewX, ctx.sceneViewY};
        rects.renderSize = {ctx.sceneViewWidth, ctx.sceneViewHeight};
    }
    return rects;
}

void SetFixedEditorWindow(const ImVec2& pos, const ImVec2& size)
{
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
}

void PushSolidEditorPanelStyle()
{
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 1.f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, theme::kWindowBg);
    ImGui::PushStyleColor(ImGuiCol_ChildBg,  theme::kWindowBg);
}

void PopSolidEditorPanelStyle()
{
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();
}

bool ShouldCancelSelectionChangeForActiveInput()
{
    ImGuiIO& io = ImGui::GetIO();
    return io.WantTextInput;
}

bool IsScenePickEntity(const ecs::World& world, EntityID entity)
{
    if (!entity.IsValid() || !world.IsAlive(entity))
        return false;
    if (world.Has<EditorMaterialPreviewComponent>(entity)) return false;
    if (world.Has<EditorDpadGizmoComponent>(entity)) return false;
    if (world.Has<EditorRotateGizmoComponent>(entity)) return false;
    if (world.Has<EditorScaleGizmoComponent>(entity)) return false;
    if (world.Has<EditorAssetThumbnailComponent>(entity)) return false;
    if (world.Has<EditorPrefabPreviewComponent>(entity)) return false;
    if (world.Has<EditorPreviewLightComponent>(entity)) return false;
    if (world.Has<EditorRuntimeGizmoTag>(entity)) return false;
    return true;
}

math::Vec3 PrepareRotationEditEuler(EditorFrameContext& ctx,
                                    EntityID entity,
                                    EditorTransformSpace space,
                                    const math::Quat& rotation) noexcept
{
    EditorState& state = ctx.state;
    const float quatDot = std::abs(
        state.rotationEditQuat.x * rotation.x +
        state.rotationEditQuat.y * rotation.y +
        state.rotationEditQuat.z * rotation.z +
        state.rotationEditQuat.w * rotation.w);
    const bool needsRefresh =
        !state.rotationEditInitialized ||
        state.rotationEditEntity != entity ||
        state.rotationEditSpace != space ||
        (!state.rotationEditActive && quatDot < 0.9999f);

    if (needsRefresh)
    {
        state.rotationEditEntity      = entity;
        state.rotationEditSpace       = space;
        state.rotationEditEuler       = NormalizeEulerDeg(QuatToEulerDeg(rotation));
        state.rotationEditQuat        = rotation;
        state.rotationEditInitialized = true;
    }

    return state.rotationEditEuler;
}

void CommitRotationEditState(EditorFrameContext& ctx,
                             EntityID entity,
                             EditorTransformSpace space,
                             const math::Vec3& euler,
                             const math::Quat& rotation) noexcept
{
    EditorState& state = ctx.state;
    state.rotationEditEntity      = entity;
    state.rotationEditSpace       = space;
    state.rotationEditEuler       = NormalizeEulerDeg(euler);
    state.rotationEditQuat        = rotation;
    state.rotationEditActive      = ImGui::IsItemActive();
    state.rotationEditInitialized = true;
}

bool HasNonUniformScaledParent(EditorFrameContext& ctx, EntityID entity) noexcept
{
    const auto* transform = ctx.world.Get<TransformComponent>(entity);
    if (!transform || !transform->inheritParentScale)
        return false;

    const auto* parent = ctx.world.Get<ParentComponent>(entity);
    if (!parent || !parent->parent.IsValid() || !ctx.world.IsAlive(parent->parent))
        return false;

    for (EntityID cursor = parent->parent; cursor.IsValid() && ctx.world.IsAlive(cursor); )
    {
        const auto* wt = ctx.world.Get<WorldTransformComponent>(cursor);
        if (wt && HasNonUniformScale(wt->scale))
            return true;

        const auto* next = ctx.world.Get<ParentComponent>(cursor);
        cursor = next ? next->parent : NULL_ENTITY;
    }

    return false;
}

void SetWorldTransform(EditorFrameContext& ctx,
                       EntityID entity,
                       TransformComponent& transform,
                       const math::Mat4& desiredWorldMatrix)
{
    math::Vec3 worldPosition{};
    math::Quat worldRotation = math::Quat::Identity();
    math::Vec3 worldScale{1.f, 1.f, 1.f};
    DecomposeTRS(desiredWorldMatrix, worldPosition, worldRotation, worldScale);

    if (const auto* parent = ctx.world.Get<ParentComponent>(entity))
    {
        if (parent->parent.IsValid())
        {
            if (const auto* parentWorld = ctx.world.Get<WorldTransformComponent>(parent->parent))
            {
                const math::Quat invParentRotation = parentWorld->rotation.Conjugate().Normalized();
                transform.localPosition = invParentRotation.Rotate(worldPosition - parentWorld->position);
                if (transform.inheritParentScale)
                {
                    transform.localPosition = {
                        std::abs(parentWorld->scale.x) > math::EPSILON ? transform.localPosition.x / parentWorld->scale.x : transform.localPosition.x,
                        std::abs(parentWorld->scale.y) > math::EPSILON ? transform.localPosition.y / parentWorld->scale.y : transform.localPosition.y,
                        std::abs(parentWorld->scale.z) > math::EPSILON ? transform.localPosition.z / parentWorld->scale.z : transform.localPosition.z
                    };
                }
                transform.localRotation = (invParentRotation * worldRotation).Normalized();
                if (transform.inheritParentScale)
                {
                    transform.localScale = {
                        std::abs(parentWorld->scale.x) > math::EPSILON ? worldScale.x / parentWorld->scale.x : worldScale.x,
                        std::abs(parentWorld->scale.y) > math::EPSILON ? worldScale.y / parentWorld->scale.y : worldScale.y,
                        std::abs(parentWorld->scale.z) > math::EPSILON ? worldScale.z / parentWorld->scale.z : worldScale.z
                    };
                }
                else
                {
                    transform.localScale = worldScale;
                }
                transform.dirty = true;
                return;
            }
        }
    }

    transform.localPosition = worldPosition;
    transform.localRotation = worldRotation;
    transform.localScale    = worldScale;
    transform.dirty = true;
}


math::Vec3 WorldPosition(EditorFrameContext& ctx, EntityID entity)
{
    // Weltposition exakt wie TransformSystem: parent.WTC * localPosition.
    // Nicht aus child.WTC lesen – das kann veraltet sein wenn child.dirty=true
    // aber TransformSystem noch nicht gelaufen ist (gleicher Frame).
    const auto* t = ctx.world.Get<TransformComponent>(entity);
    if (!t)
    {
        if (const auto* wt = ctx.world.Get<WorldTransformComponent>(entity))
            return wt->position;
        return {};
    }
    if (const auto* p = ctx.world.Get<ParentComponent>(entity))
    {
        if (p->parent.IsValid() && ctx.world.IsAlive(p->parent))
        {
            if (const auto* parentWtc = ctx.world.Get<WorldTransformComponent>(p->parent))
            {
                const math::Vec3 localOffset = t->inheritParentScale
                    ? math::Vec3{
                        parentWtc->scale.x * t->localPosition.x,
                        parentWtc->scale.y * t->localPosition.y,
                        parentWtc->scale.z * t->localPosition.z
                    }
                    : t->localPosition;
                return parentWtc->position + parentWtc->rotation.Rotate(localOffset);
            }
        }
    }
    // Root-Entity: localPosition == Weltposition
    return t->localPosition;
}

math::Quat WorldRotation(EditorFrameContext& ctx, EntityID entity)
{
    // Rotation direkt aus der Quaternion-Kette berechnen (nicht aus der Weltmatrix),
    // weil MatrixRotation() bei gescherten Matrizen (nicht-uniforme Parent-Skalierung
    // + Rotation) falsche Ergebnisse liefert – Spaltennormalisierung gibt nur eine
    // Näherung der Rotation. Die Quat-Komposition ist immer exakt.
    const auto* t = ctx.world.Get<TransformComponent>(entity);
    if (!t) return math::Quat::Identity();

    if (const auto* p = ctx.world.Get<ParentComponent>(entity))
        if (p->parent.IsValid() && ctx.world.IsAlive(p->parent))
            return (WorldRotation(ctx, p->parent) * t->localRotation).Normalized();

    return t->localRotation;
}

math::Vec3 WorldScale(EditorFrameContext& ctx, EntityID entity)
{
    // Komponentenweise Weltskala: parentWorldScale * localScale.
    // MatrixScale(child.WTC) wäre bei gescherten Matrizen (nicht-uniformer
    // Parent-Scale + Rotation) ungenau – Spaltenlängen sind dann verzerrt.
    const auto* t = ctx.world.Get<TransformComponent>(entity);
    if (!t)
    {
        if (const auto* wt = ctx.world.Get<WorldTransformComponent>(entity))
            return wt->scale;
        return {1.f, 1.f, 1.f};
    }
    if (!t->inheritParentScale)
        return t->localScale;
    if (const auto* p = ctx.world.Get<ParentComponent>(entity))
    {
        if (p->parent.IsValid() && ctx.world.IsAlive(p->parent))
        {
            if (const auto* parentWtc = ctx.world.Get<WorldTransformComponent>(p->parent))
            {
                const math::Vec3 ps = parentWtc->scale;
                return { ps.x * t->localScale.x,
                         ps.y * t->localScale.y,
                         ps.z * t->localScale.z };
            }
        }
    }
    return t->localScale;
}

void SetWorldScale(EditorFrameContext& ctx, EntityID entity, TransformComponent& transform, const math::Vec3& worldScale)
{
    transform.localScale = worldScale;
    if (transform.inheritParentScale)
        if (const auto* parent = ctx.world.Get<ParentComponent>(entity))
            if (const auto* parentWorld = ctx.world.Get<WorldTransformComponent>(parent->parent))
                transform.localScale = {
                    std::abs(parentWorld->scale.x) > math::EPSILON ? worldScale.x / parentWorld->scale.x : worldScale.x,
                    std::abs(parentWorld->scale.y) > math::EPSILON ? worldScale.y / parentWorld->scale.y : worldScale.y,
                    std::abs(parentWorld->scale.z) > math::EPSILON ? worldScale.z / parentWorld->scale.z : worldScale.z
                };
    transform.dirty = true;
    ++transform.localVersion;
}

void SetWorldPosition(EditorFrameContext& ctx, EntityID entity, TransformComponent& transform, const math::Vec3& worldPosition)
{
    const math::Mat4 currentWorld = CurrentWorldMatrix(ctx, entity);
    const math::Mat4 desiredWorld = math::Mat4::TRS(
        worldPosition,
        WorldRotation(ctx, entity),
        MatrixScale(currentWorld));
    SetWorldTransform(ctx, entity, transform, desiredWorld);
}

void SetWorldRotation(EditorFrameContext& ctx, EntityID entity, TransformComponent& transform, const math::Quat& worldRotation)
{
    const math::Mat4 currentWorld = CurrentWorldMatrix(ctx, entity);
    const math::Mat4 desiredWorld = math::Mat4::TRS(
        MatrixTranslation(currentWorld),
        worldRotation,
        MatrixScale(currentWorld));
    SetWorldTransform(ctx, entity, transform, desiredWorld);
}

} // namespace detail

void DrawLightDebugLines(EditorFrameContext& ctx)
{
#ifdef KROM_EDITOR_HAS_IMGUI
    DrawSnapGrid(ctx);
    DrawLightGizmos(ctx);
    DrawParticleEmitterGizmo(ctx);
    DrawOBBDebugLines(ctx);
    DrawPhysicsColliderLines(ctx);
    DrawTransformGizmo(ctx);
    DrawRotationGizmo(ctx);
    DrawScaleGizmo(ctx);
#else
    (void)ctx;
#endif
}

void DrawEditorDockSpace(EditorFrameContext& ctx)
{
    UpdateEditorDockSpace(ctx);
}

void DrawEditorPanels(EditorFrameContext& ctx)
{
    if (!ctx.state.pendingStartProjectFile.empty())
    {
        DrawNoProjectScreen(ctx);
        return;
    }
    // Wege, die nicht ueber ExecuteSceneMutation laufen (z. B. Drop aus dem
    // Asset-Browser), landen spaetestens hier unter der Prefab-Wurzel.
    if (!ctx.sceneLoadActive)
        EnforcePrefabModeSingleRoot(ctx);
    // Beim Laden keine Panels auf die teilweise aufgebaute Szene zugreifen lassen.
    if (ctx.sceneLoadActive)
    {
        DrawSceneLoadStatus(ctx);
        return;
    }
    // Kein Projekt geladen → nur Startbildschirm zeigen, keinen Editor-Panel-Inhalt.
    if (ctx.currentProjectName.empty())
    {
        DrawNoProjectScreen(ctx);
        return;
    }

    auto uiMark = std::chrono::steady_clock::now();
    auto measureUi = [&](double& result) {
        const auto now = std::chrono::steady_clock::now();
        result = std::chrono::duration<double, std::milli>(now - uiMark).count();
        uiMark = now;
    };
    g_uiTimings = {};
    FlushPendingMaterialOpen(ctx);
    TickMaterialTextureSync(ctx);
    UpdateEditorLayout(ctx);
    measureUi(g_uiTimings.materials);

    DrawFileMenuBar(ctx);
    UpdateEditorDockSpace(ctx);
    DrawDeleteConfirmDialog(ctx);
    measureUi(g_uiTimings.menus);
    UpdateSnapAxis(ctx);
    // Griffe der Kollisionsbox zuerst: sie liegen oft auf einer Gizmo-Achse und sind
    // kleiner, daher gewinnen sie, wenn die Maus direkt auf ihnen ist.
    UpdateOBBHandles(ctx);
    UpdateTransformGizmoInput(ctx);   // Position-Gizmo vor Selektion
    UpdateRotationGizmoInput(ctx);    // Rotations-Gizmo ebenfalls vor Selektion
    // Vor UpdateEditorCamera: das 3D-Bild dieses Frames nutzt noch die alte Kamera,
    // sonst laeuft das Symbol beim Kamerabewegen einen Frame voraus (Zittern).
    DrawEditorIcons(ctx);
    SelectEntityUnderMouse(ctx);
    UpdateEditorCamera(ctx);

    // G: Snap to Surface — danach Bewegung sperren bis Maus losgelassen
    if (!ImGui::GetIO().WantTextInput && !ctx.gameViewFocused &&
        ImGui::IsKeyPressed(ImGuiKey_G, /*repeat=*/false))
    {
        SnapEntityToSurface(ctx);
        ctx.state.gizmoDragAxis   = -1;   // laufenden Drag abbrechen
        ctx.state.snapMoveLocked  = true; // keine neue Bewegung bis Maus los
    }

    measureUi(g_uiTimings.gizmos);
    DrawProjectSettingsDialog(ctx);
    DrawRuntimeBuildStatus(ctx);
    DrawMaterialLibrary(ctx);
    measureUi(g_uiTimings.menus);

    DrawSceneViewportPanel(ctx);
    DrawGamePanel(ctx);

    if (ctx.state.assetBrowser && ctx.assetsPanelVisible)
        DrawAssetBrowser(ctx, *ctx.state.assetBrowser);
    if (ctx.consolePanelVisible)
        DrawEditorConsole(ctx);
    measureUi(g_uiTimings.assets);
    if (ctx.hierarchyPanelVisible)
        DrawEntityList(ctx);
    measureUi(g_uiTimings.hierarchy);
    if (ctx.inspectorPanelVisible)
        DrawInspector(ctx);
    measureUi(g_uiTimings.inspector);
    DrawCameraPreviewWindow(ctx);
    measureUi(g_uiTimings.preview);
    DrawPerfOverlay(ctx);
}

} // namespace engine::renderer::addons::editor

#else // KROM_EDITOR_HAS_IMGUI

namespace engine::renderer::addons::editor {
void DrawEditorDockSpace(EditorFrameContext&) {}
void DrawEditorPanels(EditorFrameContext&) {}
} // namespace engine::renderer::addons::editor

#endif // KROM_EDITOR_HAS_IMGUI
