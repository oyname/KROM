#include "addons/editor/EditorControllerGraph.hpp"

#ifdef KROM_EDITOR_HAS_IMGUI

#include "addons/animation/AnimationComponents.hpp"
#include "addons/editor/EditorAnimationControllers.hpp"
#include "addons/editor/EditorAnimationWindow.hpp"
#include "addons/editor/EditorCard.hpp"
#include "addons/editor/EditorCommands.hpp"
#include "addons/editor/EditorIcons.hpp"
#include "addons/editor/EditorLocalization.hpp"
#include "addons/editor/EditorModelClips.hpp"
#include "addons/editor/EditorPreferences.hpp"
#include "addons/editor/EditorTheme.hpp"
#include "addons/editor/EditorValueClipboard.hpp"
#include "animation/ModelClipSettings.hpp"
#include "assets/AssetPipeline.hpp"
#include "core/AtomicTextFile.hpp"
#include "core/PathUtf8.hpp"
#include "imgui_internal.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <vector>

namespace engine::renderer::addons::editor {

namespace {

namespace anim = ::engine::addons::animation;
using anim::AnimationConditionOp;
using anim::AnimationParameterType;

constexpr float kNodeWidth = 170.f;
constexpr float kNodeHeight = 46.f;
constexpr float kSpecialWidth = 110.f;
constexpr float kGrid = 20.f;
constexpr const char* kEntryNode = "#entry";

struct GraphState
{
    std::string path;                         // relative to Assets
    anim::AnimationControllerAsset asset;     // working copy
    bool dirty = false;
    uint64_t version = 0;                     // bumped by every change (compile cache)
    std::string selection;                    // state, transition or parameter id

    // View
    ImVec2 pan{40.f, 40.f};
    float zoom = 1.f;

    // Interaction
    std::string dragNode;                     // state id, kEntryNode or kAnyStateId
    ImVec2 dragOffset{};
    std::string linkFrom;                     // pending "create transition"
    ImVec2 popupWorld{};
    std::string popupNode;
    std::string popupTransition;

    // Undo: the state before the running edit, recorded once it ends.
    bool gestureActive = false;
    anim::AnimationControllerAsset gestureBefore;

    // Unsaved changes question on close
    bool askUnsaved = false;

    // Screen positions of the last frame (state centres, arrow middles).
    std::vector<std::pair<std::string, ImVec2>> drawnAt;

    // Compiled view for problems (clip missing etc.)
    uint64_t compiledVersion = ~0ull;
    anim::CompiledAnimationController compiled;
};

GraphState& G()
{
    static GraphState state;
    return state;
}

// ---------------------------------------------------------------------------
// Edits and undo
// ---------------------------------------------------------------------------

void BeginEdit()
{
    GraphState& g = G();
    if (!g.gestureActive)
    {
        g.gestureBefore = g.asset;
        g.gestureActive = true;
    }
    g.dirty = true;
    ++g.version;
}

void ApplySnapshot(const std::string& path, const anim::AnimationControllerAsset& asset)
{
    GraphState& g = G();
    if (g.path != path)
        return;
    g.asset = asset;
    g.gestureActive = false;
    g.dirty = true;
    ++g.version;
}

bool SaveGraph(EditorFrameContext& ctx)
{
    GraphState& g = G();
    if (g.path.empty() || !ctx.assetPipeline)
        return false;
    const std::filesystem::path file = ctx.assetPipeline->GetAssetRoot() / PathFromUtf8(g.path);
    if (!WriteTextFileAtomically(file, anim::SerializeAnimationController(g.asset)))
    {
        ctx.lastFileMessage = Translate("status.save_failed");
        return false;
    }
    g.dirty = false;
    return true;
}

// One undo step per finished edit (drag, typing, a menu command).
void CommitEdit(EditorFrameContext& ctx)
{
    GraphState& g = G();
    if (!g.gestureActive || ImGui::IsAnyMouseDown() || ImGui::IsAnyItemActive())
        return;
    g.gestureActive = false;
    if (g.gestureBefore == g.asset)
        return;
    RecordEditorValueChange(ctx, "history.edit_controller",
        [path = g.path, before = g.gestureBefore] { ApplySnapshot(path, before); },
        [path = g.path, after = g.asset] { ApplySnapshot(path, after); });
    if (GetEditorPreferences().autoSaveEditors)
        SaveGraph(ctx);
}

anim::AnimationStateDesc* FindState(const std::string& id)
{
    for (auto& state : G().asset.states)
        if (state.id == id)
            return &state;
    return nullptr;
}

anim::AnimationTransitionDesc* FindTransition(const std::string& id)
{
    for (auto& transition : G().asset.transitions)
        if (transition.id == id)
            return &transition;
    return nullptr;
}

anim::AnimationParameterDesc* FindParameter(const std::string& id)
{
    for (auto& parameter : G().asset.parameters)
        if (parameter.id == id)
            return &parameter;
    return nullptr;
}

std::string UniqueStateName(const std::string& base)
{
    const auto taken = [](const std::string& name) {
        const auto& states = G().asset.states;
        return std::any_of(states.begin(), states.end(), [&](const auto& s) { return s.name == name; });
    };
    if (!taken(base))
        return base;
    for (int i = 2;; ++i)
        if (const std::string name = base + " " + std::to_string(i); !taken(name))
            return name;
}

std::string UniqueParameterName(const std::string& base)
{
    const auto taken = [](const std::string& name) {
        const auto& parameters = G().asset.parameters;
        return std::any_of(parameters.begin(), parameters.end(), [&](const auto& p) { return p.name == name; });
    };
    if (!taken(base))
        return base;
    for (int i = 2;; ++i)
        if (const std::string name = base + std::to_string(i); !taken(name))
            return name;
}

float Snap(float value)
{
    return ImGui::GetIO().KeyAlt ? value : std::round(value / kGrid) * kGrid;
}

// Operators that fit a parameter type, first = default.
std::vector<AnimationConditionOp> OpsFor(AnimationParameterType type)
{
    switch (type)
    {
    case AnimationParameterType::Int:
        return {AnimationConditionOp::Greater, AnimationConditionOp::Less, AnimationConditionOp::Equals, AnimationConditionOp::NotEquals};
    case AnimationParameterType::Bool:    return {AnimationConditionOp::IsTrue, AnimationConditionOp::IsFalse};
    case AnimationParameterType::Trigger: return {AnimationConditionOp::Triggered};
    default:                              return {AnimationConditionOp::Greater, AnimationConditionOp::Less};
    }
}

const char* OpSymbol(AnimationConditionOp op)
{
    switch (op)
    {
    case AnimationConditionOp::Greater:   return ">";
    case AnimationConditionOp::Less:      return "<";
    case AnimationConditionOp::Equals:    return "==";
    case AnimationConditionOp::NotEquals: return "!=";
    case AnimationConditionOp::IsTrue:    return "= true";
    case AnimationConditionOp::IsFalse:   return "= false";
    default:                              return "";
    }
}

const char* TypeLabel(AnimationParameterType type)
{
    switch (type)
    {
    case AnimationParameterType::Int:     return "Int";
    case AnimationParameterType::Bool:    return "Bool";
    case AnimationParameterType::Trigger: return "Trigger";
    default:                              return "Float";
    }
}

bool TransitionValid(const anim::AnimationTransitionDesc& t)
{
    return std::all_of(t.conditions.begin(), t.conditions.end(), [](const auto& c) { return FindParameter(c.parameterId) != nullptr; });
}

// Short form of the conditions for the arrow: "speed > 2.5 & attack".
std::string ConditionText(const anim::AnimationTransitionDesc& t)
{
    std::string text;
    for (const auto& c : t.conditions)
    {
        if (!text.empty())
            text += " & ";
        const auto* p = FindParameter(c.parameterId);
        const std::string name = p ? p->name : std::string("?");
        if (c.op == AnimationConditionOp::Triggered)
            text += name;
        else if (c.op == AnimationConditionOp::IsTrue)
            text += name;
        else if (c.op == AnimationConditionOp::IsFalse)
            text += "!" + name;
        else
        {
            char value[32];
            if (p && p->type == AnimationParameterType::Int)
                std::snprintf(value, sizeof(value), "%d", static_cast<int>(std::lround(c.value)));
            else
                std::snprintf(value, sizeof(value), "%g", c.value);
            text += name + " " + OpSymbol(c.op) + " " + value;
        }
    }
    if (t.hasExitTime)
    {
        char exit[32];
        std::snprintf(exit, sizeof(exit), "%s@%g", text.empty() ? "" : " ", t.exitTime);
        text += exit;
    }
    return text;
}

const anim::CompiledAnimationController& Compiled(EditorFrameContext& ctx)
{
    GraphState& g = G();
    if (g.compiledVersion != g.version)
    {
        g.compiled = anim::CompileAnimationController(g.asset, MakeEditorClipResolver(ctx));
        g.compiledVersion = g.version;
    }
    return g.compiled;
}

bool StateClipFound(EditorFrameContext& ctx, const std::string& stateId)
{
    const auto& c = Compiled(ctx);
    const auto& states = G().asset.states;
    for (size_t i = 0; i < states.size() && i < c.states.size(); ++i)
        if (states[i].id == stateId)
            return c.states[i].clipFound;
    return false;
}

bool StateHasExit(EditorFrameContext& ctx, const anim::AnimationStateDesc& state)
{
    const auto& g = G();
    if (g.asset.states.size() < 2u)
        return true;
    for (const auto& t : g.asset.transitions)
        if (t.from == state.id || t.from == anim::kAnyStateId)
            return true;
    const auto& c = Compiled(ctx);
    for (size_t i = 0; i < g.asset.states.size() && i < c.states.size(); ++i)
        if (g.asset.states[i].id == state.id)
            return c.states[i].looping;
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// Public edit functions
// ---------------------------------------------------------------------------

const std::string& ControllerGraphPath() { return G().path; }

const anim::AnimationControllerAsset* ControllerGraphAsset()
{
    return G().path.empty() ? nullptr : &G().asset;
}

bool ControllerGraphDirty() { return G().dirty; }

uint64_t ControllerGraphVersion() { return G().version; }

std::string ControllerGraphAddState(EditorFrameContext& ctx, const std::string& clipAssetPath, float x, float y)
{
    GraphState& g = G();
    if (g.path.empty())
        return {};
    BeginEdit();
    anim::AnimationStateDesc state;
    state.id = g.asset.NewId('s');
    const std::string label = EditorClipLabel(ctx, clipAssetPath);
    state.name = UniqueStateName(label.empty() ? "State" : label);
    state.clipAssetPath = clipAssetPath;
    state.posX = x;
    state.posY = y;
    if (g.asset.states.empty() || !g.asset.FindState(g.asset.entryState))
        g.asset.entryState = state.id;
    g.asset.states.push_back(state);
    g.selection = state.id;
    return state.id;
}

std::string ControllerGraphAddTransition(EditorFrameContext&, const std::string& fromId, const std::string& toId)
{
    GraphState& g = G();
    if (g.path.empty() || !g.asset.FindState(toId) || (fromId != anim::kAnyStateId && !g.asset.FindState(fromId)))
        return {};
    BeginEdit();
    anim::AnimationTransitionDesc transition;
    transition.id = g.asset.NewId('t');
    transition.from = fromId;
    transition.to = toId;
    g.asset.transitions.push_back(transition);
    g.selection = transition.id;
    return transition.id;
}

std::string ControllerGraphAddParameter(EditorFrameContext&, AnimationParameterType type)
{
    GraphState& g = G();
    if (g.path.empty())
        return {};
    BeginEdit();
    anim::AnimationParameterDesc parameter;
    parameter.id = g.asset.NewId('p');
    parameter.type = type;
    parameter.name = UniqueParameterName(type == AnimationParameterType::Trigger ? "trigger"
                                       : type == AnimationParameterType::Bool    ? "flag"
                                       : type == AnimationParameterType::Int     ? "count" : "value");
    g.asset.parameters.push_back(parameter);
    g.selection = parameter.id;
    return parameter.id;
}

bool ControllerGraphSetEntry(EditorFrameContext&, const std::string& stateId)
{
    GraphState& g = G();
    if (!g.asset.FindState(stateId) || g.asset.entryState == stateId)
        return false;
    BeginEdit();
    g.asset.entryState = stateId;
    return true;
}

bool ControllerGraphEdit(EditorFrameContext&, const std::function<void(anim::AnimationControllerAsset&)>& edit)
{
    GraphState& g = G();
    if (g.path.empty() || !edit)
        return false;
    BeginEdit();
    edit(g.asset);
    return true;
}

void ControllerGraphSelect(const std::string& id)
{
    G().selection = id;
}

const std::string& ControllerGraphSelection()
{
    return G().selection;
}

bool ControllerGraphDeleteSelection(EditorFrameContext&)
{
    GraphState& g = G();
    auto& a = g.asset;
    const std::string id = g.selection;
    if (id.empty())
        return false;
    if (a.FindState(id))
    {
        BeginEdit();
        a.states.erase(std::remove_if(a.states.begin(), a.states.end(), [&](const auto& s) { return s.id == id; }), a.states.end());
        a.transitions.erase(std::remove_if(a.transitions.begin(), a.transitions.end(),
                                           [&](const auto& t) { return t.from == id || t.to == id; }), a.transitions.end());
        if (a.entryState == id)
            a.entryState = a.states.empty() ? std::string{} : a.states.front().id;
    }
    else if (FindTransition(id))
    {
        BeginEdit();
        a.transitions.erase(std::remove_if(a.transitions.begin(), a.transitions.end(), [&](const auto& t) { return t.id == id; }),
                            a.transitions.end());
    }
    else if (FindParameter(id))
    {
        // Conditions keep the id: they show up red instead of disappearing silently.
        BeginEdit();
        a.parameters.erase(std::remove_if(a.parameters.begin(), a.parameters.end(), [&](const auto& p) { return p.id == id; }),
                           a.parameters.end());
    }
    else
        return false;
    g.selection.clear();
    return true;
}

bool ControllerGraphScreenPosition(const std::string& id, float& x, float& y)
{
    for (const auto& [drawn, at] : G().drawnAt)
        if (drawn == id)
        {
            x = at.x;
            y = at.y;
            return true;
        }
    return false;
}

bool SaveOpenControllerIfChanged(EditorFrameContext& ctx)
{
    return G().dirty && SaveGraph(ctx);
}

bool OpenControllerGraph(EditorFrameContext& ctx, const std::string& assetPath)
{
    GraphState& g = G();
    if (!ctx.assetPipeline)
        return false;
    ctx.state.controllerWindowOpen = true;
    ImGui::SetWindowFocus(TranslateLabel("controller.title", "Controller##editor").c_str());
    if (g.path == assetPath)
        return true;
    if (g.dirty)
        SaveGraph(ctx);
    std::ifstream in(ctx.assetPipeline->GetAssetRoot() / PathFromUtf8(assetPath), std::ios::binary);
    const std::string source((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    anim::AnimationControllerAsset asset;
    std::string error;
    if (source.empty() || !anim::ParseAnimationController(source, asset, &error))
    {
        ctx.lastFileMessage = Translate("animation.controller_invalid") + std::string(" ") + assetPath + (error.empty() ? "" : " (" + error + ")");
        return false;
    }
    g = GraphState{};
    g.path = assetPath;
    g.asset = std::move(asset);
    return true;
}

namespace {

// ---------------------------------------------------------------------------
// Canvas
// ---------------------------------------------------------------------------

struct NodeView
{
    std::string id;          // state id, kEntryNode or kAnyStateId
    ImVec2 min, max;
};

ImVec2 NodeWorldPos(const std::string& id)
{
    auto& a = G().asset;
    if (id == kEntryNode)
        return {a.entryPosX, a.entryPosY};
    if (id == anim::kAnyStateId)
        return {a.anyPosX, a.anyPosY};
    if (const auto* s = a.FindState(id))
        return {s->posX, s->posY};
    return {};
}

void SetNodeWorldPos(const std::string& id, ImVec2 pos)
{
    auto& a = G().asset;
    if (id == kEntryNode)       { a.entryPosX = pos.x; a.entryPosY = pos.y; }
    else if (id == anim::kAnyStateId) { a.anyPosX = pos.x; a.anyPosY = pos.y; }
    else if (auto* s = FindState(id)) { s->posX = pos.x; s->posY = pos.y; }
}

ImVec2 ClipToRect(const ImVec2& center, const ImVec2& towards, const ImVec2& half)
{
    const float dx = towards.x - center.x, dy = towards.y - center.y;
    if (std::fabs(dx) < 1e-4f && std::fabs(dy) < 1e-4f)
        return center;
    const float sx = std::fabs(dx) > 1e-4f ? half.x / std::fabs(dx) : FLT_MAX;
    const float sy = std::fabs(dy) > 1e-4f ? half.y / std::fabs(dy) : FLT_MAX;
    const float s = std::min(sx, sy);
    return {center.x + dx * s, center.y + dy * s};
}

float DistanceToSegment(const ImVec2& p, const ImVec2& a, const ImVec2& b)
{
    const ImVec2 ab(b.x - a.x, b.y - a.y);
    const float len2 = ab.x * ab.x + ab.y * ab.y;
    float t = len2 > 0.f ? ((p.x - a.x) * ab.x + (p.y - a.y) * ab.y) / len2 : 0.f;
    t = std::clamp(t, 0.f, 1.f);
    const float x = a.x + ab.x * t - p.x, y = a.y + ab.y * t - p.y;
    return std::sqrt(x * x + y * y);
}

void DrawArrowHead(ImDrawList& dl, const ImVec2& at, const ImVec2& dir, float size, ImU32 color)
{
    const ImVec2 n(-dir.y, dir.x);
    dl.AddTriangleFilled(ImVec2(at.x + dir.x * size, at.y + dir.y * size),
                         ImVec2(at.x - dir.x * size * 0.6f + n.x * size * 0.7f, at.y - dir.y * size * 0.6f + n.y * size * 0.7f),
                         ImVec2(at.x - dir.x * size * 0.6f - n.x * size * 0.7f, at.y - dir.y * size * 0.6f - n.y * size * 0.7f), color);
}

void DrawDashed(ImDrawList& dl, ImVec2 a, ImVec2 b, ImU32 color, float thickness)
{
    const float dx = b.x - a.x, dy = b.y - a.y;
    const float len = std::sqrt(dx * dx + dy * dy);
    const int dashes = std::max(1, static_cast<int>(len / 8.f));
    for (int i = 0; i < dashes; i += 2)
    {
        const float t0 = static_cast<float>(i) / dashes, t1 = std::min(1.f, static_cast<float>(i + 1) / dashes);
        dl.AddLine(ImVec2(a.x + dx * t0, a.y + dy * t0), ImVec2(a.x + dx * t1, a.y + dy * t1), color, thickness);
    }
}

// What the graph shows live: the animation window's preview copy outside Play,
// the selected object in Play. Its compiled controller may be older than the
// working copy (Play reads the saved file), so states and transitions are
// matched by name and target, parameters by name.
constexpr float kEventShowSeconds = 1.2f;   // a fired clip event stays visible this long

struct LiveView
{
    AnimationPlayerComponent* player = nullptr;
    const anim::CompiledAnimationController* compiled = nullptr;
    std::string currentState;     // state ids in the working copy, "" when unknown
    std::string previousState;
    std::string transition;       // transition of the running cross-fade
    float currentFraction = 0.f;  // normalized time in [0, 1] (one pass)
    float previousFraction = 0.f;
    float blend = 0.f;            // cross-fade progress
    std::string eventName;        // clip event the preview fired lately
    float eventAge = 0.f;         // seconds since

    explicit operator bool() const noexcept { return player != nullptr; }
};

std::string LiveStateId(const anim::CompiledAnimationController& c, int32_t index)
{
    if (index < 0 || static_cast<size_t>(index) >= c.states.size())
        return {};
    const auto& states = G().asset.states;
    const std::string& name = c.states[static_cast<size_t>(index)].name;
    if (static_cast<size_t>(index) < states.size() && states[static_cast<size_t>(index)].name == name)
        return states[static_cast<size_t>(index)].id;
    for (const auto& s : states)
        if (s.name == name)
            return s.id;
    return {};
}

std::string LiveTransitionId(const anim::CompiledAnimationController& c, int32_t index)
{
    if (index < 0 || static_cast<size_t>(index) >= c.transitions.size())
        return {};
    const auto& compiled = c.transitions[static_cast<size_t>(index)];
    const auto& transitions = G().asset.transitions;
    if (compiled.order >= transitions.size())
        return {};
    const auto& t = transitions[compiled.order];
    const std::string from = compiled.from < 0 ? std::string(anim::kAnyStateId) : LiveStateId(c, compiled.from);
    return t.to == LiveStateId(c, compiled.to) && t.from == from ? t.id : std::string{};
}

float StateFraction(const anim::CompiledAnimationController& c, int32_t state, float time)
{
    if (state < 0 || static_cast<size_t>(state) >= c.states.size())
        return 0.f;
    const float normalized = anim::NormalizedStateTime(c, state, time);
    return c.states[static_cast<size_t>(state)].looping ? normalized - std::floor(normalized)
                                                        : std::clamp(normalized, 0.f, 1.f);
}

LiveView Live(EditorFrameContext& ctx)
{
    LiveView live;
    live.player = ControllerLivePlayer(ctx, G().path);
    if (!live.player)
        return live;
    const auto& rt = live.player->controllerState;
    live.compiled = rt.controller.get();
    const auto& in = rt.instance;
    live.currentState = LiveStateId(*live.compiled, in.current);
    live.currentFraction = StateFraction(*live.compiled, in.current, in.currentTime);
    if (in.Blending())
    {
        live.previousState = LiveStateId(*live.compiled, in.previous);
        live.previousFraction = StateFraction(*live.compiled, in.previous, in.previousTime);
        live.transition = LiveTransitionId(*live.compiled, in.blendTransition);
        live.blend = in.CurrentWeight();
    }
    if (!ctx.playActive && ControllerPreviewLastEvent(live.eventName, live.eventAge) && live.eventAge > kEventShowSeconds)
        live.eventName.clear();
    return live;
}

// While a preview or game runs, the value field shows and sets the value it uses
// (in place of the default). False when the running version lacks the parameter.
bool DrawLiveParameter(const LiveView& live, const anim::AnimationParameterDesc& p)
{
    const int32_t index = live.compiled->FindParameter(p.name);
    if (index < 0)
        return false;
    const AnimationParameterType type = live.compiled->parameters[static_cast<size_t>(index)].type;
    float value = live.player->GetParameter(index);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, theme::kSelected);
    switch (type)
    {
    case AnimationParameterType::Float:
        if (ImGui::DragFloat("##live", &value, 0.01f, 0.f, 0.f, "%.2f"))
            live.player->SetParameter(index, value);
        break;
    case AnimationParameterType::Int:
    {
        int v = static_cast<int>(std::lround(value));
        if (ImGui::DragInt("##live", &v, 0.1f))
            live.player->SetParameter(index, static_cast<float>(v));
        break;
    }
    case AnimationParameterType::Bool:
    {
        bool v = value != 0.f;
        if (ImGui::Checkbox("##live", &v))
            live.player->SetParameter(index, v ? 1.f : 0.f);
        break;
    }
    case AnimationParameterType::Trigger:
    {
        // Set until a transition consumes it; the button shows that it waits.
        const bool pending = value != 0.f;
        if (pending)
            ImGui::PushStyleColor(ImGuiCol_Button, theme::kAccent);
        if (ImGui::Button((std::string(Translate("controller.fire")) + "##live").c_str(), ImVec2(-FLT_MIN, 0.f)))
            live.player->SetParameter(index, 1.f);
        if (pending)
            ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", Translate(pending ? "controller.fire_pending" : "controller.fire_tip"));
        break;
    }
    }
    ImGui::PopStyleColor();
    return true;
}

void DrawParameterPanel(EditorFrameContext& ctx)
{
    GraphState& g = G();
    const LiveView live = Live(ctx);
    ImGui::TextUnformatted(Translate("animation.parameters"));
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight() + ImGui::GetCursorPosX() - 4.f);
    if (ImGui::Button("+##ctrl_add_param", ImVec2(ImGui::GetFrameHeight(), 0.f)))
        ImGui::OpenPopup("##ctrl_param_types");
    if (ImGui::BeginPopup("##ctrl_param_types"))
    {
        for (const AnimationParameterType type : {AnimationParameterType::Float, AnimationParameterType::Int,
                                                  AnimationParameterType::Bool, AnimationParameterType::Trigger})
            if (ImGui::MenuItem(TypeLabel(type)))
                ControllerGraphAddParameter(ctx, type);
        ImGui::EndPopup();
    }
    ImGui::Separator();
    if (live)
    {
        // The value fields show the preview's values, or the running game's in Play.
        ImGui::TextColored(theme::kAccentHover, "%s", Translate(ctx.playActive ? "controller.live_play" : "controller.live_preview"));
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", Translate(ctx.playActive ? "controller.live_play_tip" : "controller.live_preview_tip"));
        ImGui::Separator();
    }
    if (g.asset.parameters.empty())
        value_ui::TextDisabled("%s", Translate("animation.no_parameters"));

    std::string remove;
    for (auto& p : g.asset.parameters)
    {
        ImGui::PushID(p.id.c_str());
        const bool selected = g.selection == p.id;
        // Name (committed when the field is left), type, default value.
        char name[64];
        std::snprintf(name, sizeof(name), "%s", p.name.c_str());
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.48f);
        if (selected)
            ImGui::PushStyleColor(ImGuiCol_FrameBg, theme::kSelected);
        ImGui::InputText("##name", name, sizeof(name));
        if (selected)
            ImGui::PopStyleColor();
        if (ImGui::IsItemActivated())
            g.selection = p.id;
        if (ImGui::IsItemDeactivatedAfterEdit())
        {
            std::string next(name);
            if (!next.empty() && next != p.name)
            {
                BeginEdit();
                p.name = UniqueParameterName(next);
            }
        }
        if (ImGui::BeginPopupContextItem("##param_ctx"))
        {
            if (ImGui::MenuItem(TranslateLabel("common.delete", "Delete").c_str()))
                remove = p.id;
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (live && DrawLiveParameter(live, p))
        {
            ImGui::PopID();
            continue;
        }
        switch (p.type)
        {
        case AnimationParameterType::Float:
        {
            float value = p.defaultValue;
            if (ImGui::DragFloat("##default", &value, 0.05f, 0.f, 0.f, "%.2f") && value != p.defaultValue)
            {
                BeginEdit();
                p.defaultValue = value;
            }
            break;
        }
        case AnimationParameterType::Int:
        {
            int value = static_cast<int>(std::lround(p.defaultValue));
            if (ImGui::DragInt("##default", &value, 0.1f) && value != static_cast<int>(std::lround(p.defaultValue)))
            {
                BeginEdit();
                p.defaultValue = static_cast<float>(value);
            }
            break;
        }
        case AnimationParameterType::Bool:
        {
            bool value = p.defaultValue != 0.f;
            if (ImGui::Checkbox("##default", &value))
            {
                BeginEdit();
                p.defaultValue = value ? 1.f : 0.f;
            }
            break;
        }
        case AnimationParameterType::Trigger:
            value_ui::TextDisabled("Trigger");
            break;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", TypeLabel(p.type));
        ImGui::PopID();
    }
    if (!remove.empty())
    {
        g.selection = remove;
        ControllerGraphDeleteSelection(ctx);
    }
}

void DrawCanvas(EditorFrameContext& ctx)
{
    GraphState& g = G();
    ImGuiIO& io = ImGui::GetIO();
    const LiveView live = Live(ctx);
    const ImU32 liveColor = theme::U32(theme::kAccent);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 size(std::max(64.f, ImGui::GetContentRegionAvail().x), std::max(64.f, ImGui::GetContentRegionAvail().y));
    ImGui::InvisibleButton("##ctrl_canvas", size,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    const bool activated = ImGui::IsItemActivated() && ImGui::IsMouseDown(ImGuiMouseButton_Left);
    ImDrawList& dl = *ImGui::GetWindowDrawList();
    const ImVec2 canvasMax(origin.x + size.x, origin.y + size.y);
    dl.PushClipRect(origin, canvasMax, true);
    dl.AddRectFilled(origin, canvasMax, theme::U32(theme::kField));

    const auto toScreen = [&](ImVec2 w) { return ImVec2(origin.x + (w.x + g.pan.x) * g.zoom, origin.y + (w.y + g.pan.y) * g.zoom); };
    const auto toWorld = [&](ImVec2 s) { return ImVec2((s.x - origin.x) / g.zoom - g.pan.x, (s.y - origin.y) / g.zoom - g.pan.y); };

    // Grid: faint lines, every fifth a little stronger. Theme colors are linear
    // (the editor renders in linear space), raw sRGB values would look washed out.
    const float step = kGrid * g.zoom;
    const ImU32 gridColor = theme::U32(theme::Hex(0x35404B, 0.22f));
    const ImU32 gridMajor = theme::U32(theme::Hex(0x35404B, 0.55f));
    const auto lineIndex = [&](float screen, float pan) {
        return static_cast<long>(std::lround((screen / g.zoom - pan) / kGrid));
    };
    for (float x = std::fmod(g.pan.x * g.zoom, step); x < size.x; x += step)
        dl.AddLine(ImVec2(origin.x + x, origin.y), ImVec2(origin.x + x, canvasMax.y),
                   lineIndex(x, g.pan.x) % 5 == 0 ? gridMajor : gridColor);
    for (float y = std::fmod(g.pan.y * g.zoom, step); y < size.y; y += step)
        dl.AddLine(ImVec2(origin.x, origin.y + y), ImVec2(canvasMax.x, origin.y + y),
                   lineIndex(y, g.pan.y) % 5 == 0 ? gridMajor : gridColor);

    // Pan (middle button) and zoom (wheel) around the mouse.
    if (hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.f))
    {
        g.pan.x += io.MouseDelta.x / g.zoom;
        g.pan.y += io.MouseDelta.y / g.zoom;
    }
    if (hovered && io.MouseWheel != 0.f)
    {
        const ImVec2 before = toWorld(io.MousePos);
        g.zoom = std::clamp(g.zoom * std::pow(1.12f, io.MouseWheel), 0.35f, 2.5f);
        const ImVec2 after = toWorld(io.MousePos);
        g.pan.x += after.x - before.x;
        g.pan.y += after.y - before.y;
    }

    // Nodes
    std::vector<NodeView> nodes;
    const auto addNode = [&](const std::string& id, float width) {
        const ImVec2 p = toScreen(NodeWorldPos(id));
        nodes.push_back({id, p, ImVec2(p.x + width * g.zoom, p.y + kNodeHeight * g.zoom)});
    };
    addNode(kEntryNode, kSpecialWidth);
    addNode(anim::kAnyStateId, kSpecialWidth);
    for (const auto& s : g.asset.states)
        addNode(s.id, kNodeWidth);
    const auto findNode = [&](const std::string& id) -> const NodeView* {
        for (const auto& n : nodes)
            if (n.id == id)
                return &n;
        return nullptr;
    };
    const auto center = [](const NodeView& n) { return ImVec2((n.min.x + n.max.x) * 0.5f, (n.min.y + n.max.y) * 0.5f); };
    const auto half = [](const NodeView& n) { return ImVec2((n.max.x - n.min.x) * 0.5f, (n.max.y - n.min.y) * 0.5f); };

    std::string hoveredNode;
    for (const auto& n : nodes)
        if (hovered && ImGui::IsMouseHoveringRect(n.min, n.max))
            hoveredNode = n.id;
    g.drawnAt.clear();
    g.drawnAt.emplace_back("#canvasMin", origin);
    g.drawnAt.emplace_back("#canvasMax", canvasMax);
    for (const auto& n : nodes)
        g.drawnAt.emplace_back(n.id, ImVec2((n.min.x + n.max.x) * 0.5f, (n.min.y + n.max.y) * 0.5f));

    // Transitions: arrows between node borders; several between the same pair sit side by side.
    const ImU32 lineColor = theme::U32(theme::kTextSecondary);
    const ImU32 selectedColor = theme::U32(theme::kAccentHover);
    const ImU32 dangerColor = theme::U32(theme::kDanger);
    std::string hoveredTransition;
    const auto pairKey = [](const std::string& a, const std::string& b) { return a < b ? a + "|" + b : b + "|" + a; };
    // Geometry and hover first, drawing second: the arrow under the mouse is highlighted.
    struct Arrow { size_t index; ImVec2 a, b; bool loop; ImVec2 loopCenter; float loopRadius; };
    std::vector<Arrow> arrows;
    for (size_t i = 0; i < g.asset.transitions.size(); ++i)
    {
        const auto& t = g.asset.transitions[i];
        const NodeView* from = findNode(t.from);
        const NodeView* to = findNode(t.to);
        if (!from || !to)
            continue;
        if (t.from == t.to)
        {
            // Self transition: a loop above the node.
            const ImVec2 c = center(*from);
            const float r = 12.f * g.zoom;
            const ImVec2 loop(c.x + half(*from).x * 0.5f, from->min.y - r * 0.6f);
            arrows.push_back({i, {}, {}, true, loop, r});
            const float d = std::sqrt((io.MousePos.x - loop.x) * (io.MousePos.x - loop.x) + (io.MousePos.y - loop.y) * (io.MousePos.y - loop.y));
            if (hovered && hoveredNode.empty() && std::fabs(d - r) < 7.f)
                hoveredTransition = t.id;
            continue;
        }
        // Offset among the arrows of this pair, perpendicular to the canonical direction.
        int index = 0, count = 0;
        const std::string key = pairKey(t.from, t.to);
        for (size_t j = 0; j < g.asset.transitions.size(); ++j)
        {
            const auto& o = g.asset.transitions[j];
            if (o.from == o.to || pairKey(o.from, o.to) != key)
                continue;
            if (j < i)
                ++index;
            ++count;
        }
        const ImVec2 ca = t.from < t.to ? center(*from) : center(*to);
        const ImVec2 cb = t.from < t.to ? center(*to) : center(*from);
        float dx = cb.x - ca.x, dy = cb.y - ca.y;
        const float len = std::max(1e-3f, std::sqrt(dx * dx + dy * dy));
        dx /= len; dy /= len;
        const float offset = (static_cast<float>(index) - (count - 1) * 0.5f) * 12.f * g.zoom;
        const ImVec2 shift(-dy * offset, dx * offset);
        const ImVec2 fromC(center(*from).x + shift.x, center(*from).y + shift.y);
        const ImVec2 toC(center(*to).x + shift.x, center(*to).y + shift.y);
        const ImVec2 a = ClipToRect(fromC, toC, half(*from));
        const ImVec2 b = ClipToRect(toC, fromC, half(*to));
        arrows.push_back({i, a, b, false, {}, 0.f});
        if (hovered && hoveredNode.empty() && DistanceToSegment(io.MousePos, a, b) < 7.f)
            hoveredTransition = t.id;
    }
    for (const Arrow& arrow : arrows)
    {
        const auto& t = g.asset.transitions[arrow.index];
        const bool valid = TransitionValid(t);
        const bool selected = g.selection == t.id;
        const bool hot = hoveredTransition == t.id;
        const ImU32 color = !valid ? dangerColor : (selected || hot) ? selectedColor : lineColor;
        const float thickness = selected ? 2.5f : hot ? 2.f : 1.5f;
        // A running cross-fade fills its arrow up to its progress.
        const bool fading = live && !live.transition.empty() && live.transition == t.id;
        if (arrow.loop)
        {
            dl.AddCircle(arrow.loopCenter, arrow.loopRadius, color, 16, thickness);
            if (fading)
            {
                constexpr float kPi = 3.14159265f;
                dl.PathArcTo(arrow.loopCenter, arrow.loopRadius, kPi, kPi + 2.f * kPi * live.blend, 24);
                dl.PathStroke(liveColor, 0, 4.f);
            }
            DrawArrowHead(dl, ImVec2(arrow.loopCenter.x - arrow.loopRadius, arrow.loopCenter.y), ImVec2(0.f, 1.f), 5.f * g.zoom, color);
            g.drawnAt.emplace_back(t.id, ImVec2(arrow.loopCenter.x, arrow.loopCenter.y - arrow.loopRadius));
            continue;
        }
        const ImVec2 a = arrow.a, b = arrow.b;
        if (valid)
            dl.AddLine(a, b, color, thickness);
        else
            DrawDashed(dl, a, b, color, thickness);
        if (fading)
        {
            const ImVec2 fill(a.x + (b.x - a.x) * live.blend, a.y + (b.y - a.y) * live.blend);
            dl.AddLine(a, fill, liveColor, 4.f);
            g.drawnAt.emplace_back(t.id + "#fill", fill);
        }
        float ux = b.x - a.x, uy = b.y - a.y;
        const float ulen = std::max(1e-3f, std::sqrt(ux * ux + uy * uy));
        ux /= ulen; uy /= ulen;
        const ImVec2 mid((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
        g.drawnAt.emplace_back(t.id, mid);
        DrawArrowHead(dl, mid, ImVec2(ux, uy), 6.f * g.zoom, color);
        // Condition text and priority beside the arrow.
        std::string label = ConditionText(t);
        if (t.priority != 0)
            label = "P" + std::to_string(t.priority) + (label.empty() ? "" : "  " + label);
        if (!label.empty() && g.zoom > 0.55f)
            dl.AddText(ImVec2(mid.x + uy * 8.f + 4.f, mid.y - ux * 8.f - ImGui::GetFontSize() * 0.5f),
                       valid ? theme::U32(theme::kTextDisabled) : dangerColor, label.c_str());
        if (!valid && hot)
            ImGui::SetTooltip("%s", Translate("controller.deleted_parameter"));
    }

    // Entry arrow (not a transition).
    if (const NodeView* entry = findNode(kEntryNode))
        if (const NodeView* target = findNode(g.asset.entryState))
        {
            const ImVec2 a = ClipToRect(center(*entry), center(*target), half(*entry));
            const ImVec2 b = ClipToRect(center(*target), center(*entry), half(*target));
            const ImU32 green = theme::U32(theme::kSuccess);
            dl.AddLine(a, b, green, 1.5f);
            // Not a transition: it only marks the start state.
            if (hovered && hoveredNode.empty() && hoveredTransition.empty() && DistanceToSegment(io.MousePos, a, b) < 7.f)
                ImGui::SetTooltip("%s", Translate("controller.entry_arrow_tip"));
            float ux = b.x - a.x, uy = b.y - a.y;
            const float ulen = std::max(1e-3f, std::sqrt(ux * ux + uy * uy));
            DrawArrowHead(dl, ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f), ImVec2(ux / ulen, uy / ulen), 6.f * g.zoom, green);
        }

    // Node boxes
    for (const auto& n : nodes)
    {
        const bool isEntry = n.id == kEntryNode, isAny = n.id == anim::kAnyStateId;
        const anim::AnimationStateDesc* state = (isEntry || isAny) ? nullptr : g.asset.FindState(n.id);
        const bool selected = g.selection == n.id;
        const float rounding = 6.f * g.zoom;
        // Dark fills with light text; Entry and Any State are told apart by a colored edge.
        ImU32 fill = theme::U32(theme::kButton);
        if (isEntry) fill = theme::U32(theme::Hex(0x1F3F2E));
        if (isAny)   fill = theme::U32(theme::Hex(0x1C3A3F));
        dl.AddRectFilled(n.min, n.max, fill, rounding);
        const bool missingClip = state && !StateClipFound(ctx, state->id);
        ImU32 border = theme::U32(theme::kBorder);
        if (isEntry) border = theme::U32(theme::kSuccess);
        if (isAny)   border = theme::U32(theme::Hex(0x3FA7B0));
        if (missingClip) border = dangerColor;
        if (selected)    border = selectedColor;
        if (state && state->id == g.asset.entryState && !selected && !missingClip)
            border = theme::U32(theme::kSuccess);
        // Live: the active state is outlined, a bar shows its normalized time.
        const bool active = live && state && state->id == live.currentState;
        const bool fadingOut = live && state && !active && state->id == live.previousState;
        if (active && !selected)
            border = liveColor;
        dl.AddRect(n.min, n.max, border, rounding, 0,
                   active ? 3.f : selected || missingClip ? 2.5f : (isEntry || isAny) ? 1.5f : 1.f);
        if (active || fadingOut)
        {
            const float inset = 4.f * g.zoom, height = std::max(2.f, 3.f * g.zoom);
            const ImVec2 barMin(n.min.x + inset, n.max.y - inset - height);
            const ImVec2 barMax(n.max.x - inset, n.max.y - inset);
            const float fraction = active ? live.currentFraction : live.previousFraction;
            dl.AddRectFilled(barMin, barMax, theme::U32(theme::kField), height * 0.5f);
            dl.AddRectFilled(barMin, ImVec2(barMin.x + (barMax.x - barMin.x) * fraction, barMax.y),
                             active ? liveColor : theme::U32(theme::kTextDisabled), height * 0.5f);
            g.drawnAt.emplace_back(state->id + "#bar", ImVec2(barMin.x + (barMax.x - barMin.x) * fraction, barMin.y));
        }
        // A clip event the preview just fired shows above the active state and fades out.
        if (active && !live.eventName.empty())
        {
            const float alpha = std::clamp(1.f - live.eventAge / kEventShowSeconds, 0.f, 1.f);
            ImVec4 color = theme::kWarning;
            color.w = alpha;
            const std::string label = std::string(ICON_LC_ACTIVITY " ") + live.eventName;
            const ImVec2 at(n.min.x, n.min.y - ImGui::GetFontSize() - 4.f * g.zoom);
            dl.AddText(at, theme::U32(color), label.c_str());
            g.drawnAt.emplace_back("#event", at);
        }
        dl.PushClipRect(n.min, n.max, true);
        const char* title = isEntry ? Translate("controller.entry") : isAny ? Translate("controller.any_state") : state->name.c_str();
        const ImVec2 titleSize = ImGui::CalcTextSize(title);
        const float pad = 6.f * g.zoom;
        if (state)
        {
            dl.AddText(ImVec2(n.min.x + pad, n.min.y + pad * 0.6f), theme::U32(theme::kText), title);
            const std::string clip = state->clipAssetPath.empty() ? std::string(Translate("controller.no_clip"))
                                                                  : EditorClipLabel(ctx, state->clipAssetPath);
            dl.AddText(ImVec2(n.min.x + pad, n.min.y + pad * 0.6f + ImGui::GetFontSize() + 2.f),
                       missingClip ? dangerColor : theme::U32(theme::kTextDisabled), clip.c_str());
        }
        else
        {
            dl.AddText(ImVec2((n.min.x + n.max.x - titleSize.x) * 0.5f, (n.min.y + n.max.y - titleSize.y) * 0.5f),
                       theme::U32(theme::kText), title);
        }
        dl.PopClipRect();
        if (state && !StateHasExit(ctx, *state))
        {
            const ImVec2 at(n.max.x - 10.f * g.zoom, n.min.y + 4.f * g.zoom);
            dl.AddText(at, theme::U32(theme::kWarning), "!");
            if (hoveredNode == n.id && io.MousePos.x > at.x - 6.f)
                ImGui::SetTooltip("%s", Translate("controller.no_exit"));
        }
        if (missingClip && hoveredNode == n.id)
            ImGui::SetTooltip("%s", Translate("controller.clip_missing"));
    }

    // Pending "create transition": a line to the mouse until a state is clicked.
    if (!g.linkFrom.empty())
    {
        if (const NodeView* from = findNode(g.linkFrom))
            dl.AddLine(center(*from), io.MousePos, selectedColor, 2.f);
        if (ImGui::IsKeyPressed(ImGuiKey_Escape) || (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)))
            g.linkFrom.clear();
        else if (activated)
        {
            if (!hoveredNode.empty() && hoveredNode != kEntryNode && hoveredNode != anim::kAnyStateId)
                ControllerGraphAddTransition(ctx, g.linkFrom, hoveredNode);
            g.linkFrom.clear();
        }
        dl.PopClipRect();
        return;
    }

    // Selection and dragging
    if (activated)
    {
        if (!hoveredNode.empty())
        {
            if (hoveredNode != kEntryNode && hoveredNode != anim::kAnyStateId)
                g.selection = hoveredNode;
            else
                g.selection.clear();
            g.dragNode = hoveredNode;
            const ImVec2 world = toWorld(io.MousePos);
            const ImVec2 pos = NodeWorldPos(hoveredNode);
            g.dragOffset = ImVec2(world.x - pos.x, world.y - pos.y);
        }
        else
            g.selection = hoveredTransition;
    }
    if (!g.dragNode.empty())
    {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
            g.dragNode.clear();
        else if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.f))
        {
            const ImVec2 world = toWorld(io.MousePos);
            const ImVec2 next(Snap(world.x - g.dragOffset.x), Snap(world.y - g.dragOffset.y));
            const ImVec2 current = NodeWorldPos(g.dragNode);
            if (next.x != current.x || next.y != current.y)
            {
                BeginEdit();
                SetNodeWorldPos(g.dragNode, next);
            }
        }
    }

    // Context menus
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
    {
        g.popupWorld = toWorld(io.MousePos);
        g.popupNode = hoveredNode;
        g.popupTransition = hoveredNode.empty() ? hoveredTransition : std::string{};
        if (!hoveredNode.empty() && hoveredNode != kEntryNode && hoveredNode != anim::kAnyStateId)
            g.selection = hoveredNode;
        else if (!g.popupTransition.empty())
            g.selection = g.popupTransition;
        ImGui::OpenPopup("##ctrl_canvas_menu");
    }
    if (ImGui::BeginPopup("##ctrl_canvas_menu"))
    {
        const bool onState = !g.popupNode.empty() && g.popupNode != kEntryNode && g.popupNode != anim::kAnyStateId;
        if (g.popupNode.empty() && g.popupTransition.empty())
        {
            if (ImGui::MenuItem(TranslateLabel("controller.new_state", "New State").c_str()))
                ControllerGraphAddState(ctx, {}, Snap(g.popupWorld.x), Snap(g.popupWorld.y));
        }
        if (onState || g.popupNode == anim::kAnyStateId)
            if (ImGui::MenuItem(TranslateLabel("controller.make_transition", "Make Transition").c_str()))
                g.linkFrom = g.popupNode;
        if (onState)
        {
            if (ImGui::MenuItem(TranslateLabel("controller.set_entry", "Set as Start State").c_str(), nullptr, false,
                                g.asset.entryState != g.popupNode))
                ControllerGraphSetEntry(ctx, g.popupNode);
            if (ImGui::MenuItem(TranslateLabel("controller.duplicate", "Duplicate").c_str()))
                if (const auto* s = g.asset.FindState(g.popupNode))
                {
                    const anim::AnimationStateDesc copy = *s;
                    const std::string id = ControllerGraphAddState(ctx, copy.clipAssetPath, copy.posX + 2.f * kGrid, copy.posY + 2.f * kGrid);
                    if (auto* added = FindState(id))
                    {
                        added->speed = copy.speed;
                        added->name = UniqueStateName(copy.name);
                    }
                }
        }
        if (onState || !g.popupTransition.empty())
        {
            ImGui::Separator();
            ImGui::PushStyleColor(ImGuiCol_Text, theme::kDanger);
            if (ImGui::MenuItem(TranslateLabel("common.delete", "Delete").c_str(), "Del"))
                ControllerGraphDeleteSelection(ctx);
            ImGui::PopStyleColor();
        }
        ImGui::EndPopup();
    }

    // A clip dropped from the asset browser becomes a state with that clip.
    if (ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("KROM_ASSET_ANIMATION"))
            if (const char* dropped = static_cast<const char*>(payload->Data); dropped && dropped[0] != '\0')
            {
                const ImVec2 world = toWorld(io.MousePos);
                ControllerGraphAddState(ctx, dropped, Snap(world.x), Snap(world.y));
            }
        ImGui::EndDragDropTarget();
    }

    dl.PopClipRect();
}

// ---------------------------------------------------------------------------
// Inspector cards
// ---------------------------------------------------------------------------

// A clip of the same file: model clips by name, .anim clips by name.
void DrawClipChoice(EditorFrameContext& ctx, anim::AnimationStateDesc& state)
{
    value_ui::RowLabel("Clip");
    const std::string label = state.clipAssetPath.empty() ? std::string(Translate("controller.no_clip")) : EditorClipLabel(ctx, state.clipAssetPath);
    if (ImGui::BeginCombo("##state_clip", label.c_str()))
    {
        const std::string file = state.clipAssetPath.substr(0, state.clipAssetPath.rfind('#'));
        if (::engine::animation::IsModelFilePath(file) && ctx.assetPipeline)
        {
            for (const auto& entry : GetModelClips(ctx.assetPipeline->GetAssetRoot(), file).settings.clips)
            {
                const std::string path = ::engine::animation::ModelClipAssetPath(file, entry.id);
                if (ImGui::Selectable(entry.name.c_str(), path == state.clipAssetPath) && path != state.clipAssetPath)
                {
                    (void)PersistModelClips(ctx.assetPipeline->GetAssetRoot(), file);
                    BeginEdit();
                    state.clipAssetPath = path;
                }
            }
        }
        else if (AnimationFileExists(ctx, file))
        {
            for (const std::string& name : AnimationClipNames(ctx, file))
            {
                const std::string path = file + "#" + name;
                if (ImGui::Selectable(name.c_str(), path == state.clipAssetPath) && path != state.clipAssetPath)
                {
                    BeginEdit();
                    state.clipAssetPath = path;
                }
            }
        }
        else
            value_ui::TextDisabled("%s", Translate("controller.drop_clip_hint"));
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered() && !state.clipAssetPath.empty())
        ImGui::SetTooltip("%s", state.clipAssetPath.c_str());
    if (ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("KROM_ASSET_ANIMATION"))
            if (const char* dropped = static_cast<const char*>(payload->Data); dropped && dropped[0] != '\0')
            {
                BeginEdit();
                state.clipAssetPath = dropped;
            }
        ImGui::EndDragDropTarget();
    }
}

void DrawStateCard(EditorFrameContext& ctx, anim::AnimationStateDesc& state)
{
    GraphState& g = G();
    value_ui::RowLabel(Translate("common.name"));
    char name[64];
    std::snprintf(name, sizeof(name), "%s", state.name.c_str());
    ImGui::InputText("##state_name", name, sizeof(name));
    if (ImGui::IsItemDeactivatedAfterEdit() && name[0] != '\0' && state.name != name)
    {
        BeginEdit();
        state.name = UniqueStateName(name);
    }
    DrawClipChoice(ctx, state);
    if (!state.clipAssetPath.empty() && !StateClipFound(ctx, state.id))
        ImGui::TextColored(theme::kDanger, "%s", Translate("controller.clip_missing"));
    float speed = state.speed;
    if (value_ui::DragFloat(TranslateLabel("animation.speed", "Speed##state_speed").c_str(), &speed, 0.01f, -10.f, 10.f, "%.2f") &&
        speed != state.speed)
    {
        BeginEdit();
        state.speed = speed;
    }
    if (g.asset.entryState != state.id)
    {
        if (ImGui::Button(TranslateLabel("controller.set_entry", "Set as Start State").c_str(), ImVec2(-FLT_MIN, 0.f)))
            ControllerGraphSetEntry(ctx, state.id);
    }
    else
        ImGui::TextColored(theme::kSuccess, "%s", Translate("controller.is_entry"));

    // Outgoing transitions in the order they are checked (Any State ones also apply).
    std::vector<const anim::AnimationTransitionDesc*> outgoing;
    for (size_t i = 0; i < g.asset.transitions.size(); ++i)
        if (g.asset.transitions[i].from == state.id || g.asset.transitions[i].from == anim::kAnyStateId)
            outgoing.push_back(&g.asset.transitions[i]);
    std::stable_sort(outgoing.begin(), outgoing.end(), [](const auto* a, const auto* b) { return a->priority > b->priority; });
    ImGui::Spacing();
    value_ui::TextDisabled("%s", Translate("controller.outgoing"));
    std::string select;
    for (const auto* t : outgoing)
    {
        const auto* to = g.asset.FindState(t->to);
        std::string row = std::string(t->from == anim::kAnyStateId ? "(Any) " : "") + "-> " + (to ? to->name : "?") +
                          "   P" + std::to_string(t->priority);
        const std::string condition = ConditionText(*t);
        if (!condition.empty())
            row += "   " + condition;
        ImGui::PushID(t->id.c_str());
        if (!TransitionValid(*t))
            ImGui::PushStyleColor(ImGuiCol_Text, theme::kDanger);
        if (ImGui::Selectable(row.c_str(), false))
            select = t->id;
        if (!TransitionValid(*t))
            ImGui::PopStyleColor();
        ImGui::PopID();
    }
    if (outgoing.empty())
        value_ui::TextDisabled("%s", Translate("controller.none"));
    if (!select.empty())
        g.selection = select;
}

void DrawTransitionCard(EditorFrameContext&, anim::AnimationTransitionDesc& t)
{
    GraphState& g = G();
    const auto stateName = [&](const std::string& id) -> std::string {
        if (id == anim::kAnyStateId)
            return Translate("controller.any_state");
        const auto* s = g.asset.FindState(id);
        return s ? s->name : std::string("?");
    };
    // From and To
    value_ui::RowLabel(Translate("controller.from"));
    if (ImGui::BeginCombo("##t_from", stateName(t.from).c_str()))
    {
        if (ImGui::Selectable(Translate("controller.any_state"), t.from == anim::kAnyStateId) && t.from != anim::kAnyStateId)
        {
            BeginEdit();
            t.from = anim::kAnyStateId;
        }
        for (const auto& s : g.asset.states)
            if (ImGui::Selectable((s.name + "##" + s.id).c_str(), t.from == s.id) && t.from != s.id)
            {
                BeginEdit();
                t.from = s.id;
            }
        ImGui::EndCombo();
    }
    value_ui::RowLabel(Translate("controller.to"));
    if (ImGui::BeginCombo("##t_to", stateName(t.to).c_str()))
    {
        for (const auto& s : g.asset.states)
            if (ImGui::Selectable((s.name + "##" + s.id).c_str(), t.to == s.id) && t.to != s.id)
            {
                BeginEdit();
                t.to = s.id;
            }
        ImGui::EndCombo();
    }
    int priority = t.priority;
    if (value_ui::DragInt(TranslateLabel("controller.priority", "Priority##t_priority").c_str(), &priority, 0.1f) &&
        priority != t.priority)
    {
        BeginEdit();
        t.priority = priority;
    }
    float blend = t.blendDuration;
    if (value_ui::DragFloat(TranslateLabel("controller.blend", "Blend (s)##t_blend").c_str(), &blend, 0.01f, 0.f, 10.f, "%.2f",
                            ImGuiSliderFlags_AlwaysClamp) && blend != t.blendDuration)
    {
        BeginEdit();
        t.blendDuration = blend;
    }
    bool hasExit = t.hasExitTime;
    if (value_ui::Checkbox(TranslateLabel("controller.exit_time", "Exit Time##t_has_exit").c_str(), &hasExit))
    {
        BeginEdit();
        t.hasExitTime = hasExit;
    }
    if (t.hasExitTime)
    {
        float exit = t.exitTime;
        if (value_ui::DragFloat(TranslateLabel("controller.exit_at", "  at (normalized)##t_exit").c_str(), &exit, 0.01f, 0.f, 100.f,
                                "%.2f", ImGuiSliderFlags_AlwaysClamp) && exit != t.exitTime)
        {
            BeginEdit();
            t.exitTime = exit;
        }
    }
    bool interruptible = t.interruptible;
    if (value_ui::Checkbox(TranslateLabel("controller.interruptible", "Interruptible##t_interrupt").c_str(), &interruptible))
    {
        BeginEdit();
        t.interruptible = interruptible;
    }

    // Conditions: parameter - operator - value.
    ImGui::Spacing();
    value_ui::TextDisabled("%s", Translate("controller.conditions"));
    int remove = -1;
    for (size_t i = 0; i < t.conditions.size(); ++i)
    {
        auto& c = t.conditions[i];
        ImGui::PushID(static_cast<int>(i));
        const auto* p = FindParameter(c.parameterId);
        const float rowWidth = ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight() - ImGui::GetStyle().ItemSpacing.x;
        ImGui::SetNextItemWidth(rowWidth * 0.42f);
        if (!p)
            ImGui::PushStyleColor(ImGuiCol_Text, theme::kDanger);
        if (ImGui::BeginCombo("##c_param", p ? p->name.c_str() : Translate("controller.deleted")))
        {
            for (const auto& candidate : g.asset.parameters)
                if (ImGui::Selectable((candidate.name + "##" + candidate.id).c_str(), candidate.id == c.parameterId) &&
                    candidate.id != c.parameterId)
                {
                    BeginEdit();
                    c.parameterId = candidate.id;
                    const auto ops = OpsFor(candidate.type);
                    if (std::find(ops.begin(), ops.end(), c.op) == ops.end())
                        c.op = ops.front();
                }
            ImGui::EndCombo();
        }
        if (!p)
            ImGui::PopStyleColor();
        if (p)
        {
            const auto ops = OpsFor(p->type);
            if (ops.size() > 1u)
            {
                ImGui::SameLine();
                ImGui::SetNextItemWidth(rowWidth * 0.24f);
                if (ImGui::BeginCombo("##c_op", OpSymbol(c.op)))
                {
                    for (const auto op : ops)
                        if (ImGui::Selectable(OpSymbol(op), op == c.op) && op != c.op)
                        {
                            BeginEdit();
                            c.op = op;
                        }
                    ImGui::EndCombo();
                }
            }
            if (p->type == AnimationParameterType::Float || p->type == AnimationParameterType::Int)
            {
                ImGui::SameLine();
                ImGui::SetNextItemWidth(rowWidth * 0.30f);
                if (p->type == AnimationParameterType::Int)
                {
                    int value = static_cast<int>(std::lround(c.value));
                    if (ImGui::DragInt("##c_value", &value, 0.1f) && value != static_cast<int>(std::lround(c.value)))
                    {
                        BeginEdit();
                        c.value = static_cast<float>(value);
                    }
                }
                else
                {
                    float value = c.value;
                    if (ImGui::DragFloat("##c_value", &value, 0.05f, 0.f, 0.f, "%.2f") && value != c.value)
                    {
                        BeginEdit();
                        c.value = value;
                    }
                }
            }
        }
        ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::GetFrameHeight());
        if (ImGui::Button("x##c_remove", ImVec2(ImGui::GetFrameHeight(), 0.f)))
            remove = static_cast<int>(i);
        ImGui::PopID();
    }
    if (remove >= 0)
    {
        BeginEdit();
        t.conditions.erase(t.conditions.begin() + remove);
    }
    ImGui::BeginDisabled(g.asset.parameters.empty());
    if (ImGui::Button(TranslateLabel("controller.add_condition", "+ Condition").c_str(), ImVec2(-FLT_MIN, 0.f)))
    {
        BeginEdit();
        anim::AnimationConditionDesc condition;
        condition.parameterId = g.asset.parameters.front().id;
        condition.op = OpsFor(g.asset.parameters.front().type).front();
        t.conditions.push_back(condition);
    }
    ImGui::EndDisabled();
    if (g.asset.parameters.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", Translate("controller.add_parameter_first"));
}

} // namespace

namespace {

// Opens the animation window, which then plays the controller on the selected
// animator. Disabled without such an object; marked while the preview runs.
void DrawPreviewButton(EditorFrameContext& ctx)
{
    const GraphState& g = G();
    if (ctx.playActive)
    {
        if (ControllerLivePlayer(ctx, g.path))
            ImGui::TextColored(theme::kAccentHover, "%s", Translate("controller.live_play"));
        return;
    }
    // One button: starts the preview (opens the animation window), then pauses
    // and resumes it. Restart sits beside it while the preview runs.
    const bool running = ControllerPreviewRunning(g.path) && ctx.state.animationWindowOpen;
    const bool available = ControllerPreviewCandidate(ctx, g.path).IsValid();
    const bool paused = running && ControllerPreviewPaused();
    ImGui::BeginDisabled(!available && !running);
    if (running && !paused)
        ImGui::PushStyleColor(ImGuiCol_Button, theme::kSelected);
    const char* icon = running && !paused ? ICON_LC_PAUSE " " : ICON_LC_PLAY " ";
    if (ImGui::Button((std::string(icon) + Translate("controller.preview") + "##ctrl_preview").c_str()))
    {
        if (running)
            SetControllerPreviewPaused(!paused);
        else
            StartControllerPreview(ctx);
    }
    if (running && !paused)
        ImGui::PopStyleColor();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", Translate(running ? (paused ? "controller.preview_resume" : "controller.preview_pause")
                                                  : available ? "controller.preview_tip" : "controller.preview_select_tip"));
    if (!running)
        return;
    ImGui::SameLine();
    if (ImGui::Button(ICON_LC_REFRESH_CW "##ctrl_preview_restart"))
        RestartControllerPreview(ctx);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", Translate("controller.preview_restart_tip"));
}

} // namespace

bool DrawControllerGraphCard(EditorFrameContext& ctx)
{
    GraphState& g = G();
    if (!ctx.state.controllerWindowOpen || g.path.empty() || g.selection.empty())
        return false;
    auto* state = FindState(g.selection);
    auto* transition = state ? nullptr : FindTransition(g.selection);
    if (!state && !transition)
        return false;
    value_ui::InlineLabelScope rows;
    card::Card c;
    const char* title = state ? "State" : "Transition";
    if (card::Begin(c, "##controller_card", title, ICON_LC_LIST_TREE, IM_COL32(110, 190, 235, 255)))
    {
        ImGui::PushID(g.selection.c_str());
        if (state)
            DrawStateCard(ctx, *state);
        else
            DrawTransitionCard(ctx, *transition);
        ImGui::PopID();
    }
    card::End(c);
    return true;
}

void DrawControllerGraphWindow(EditorFrameContext& ctx)
{
    GraphState& g = G();
    const std::string title = TranslateLabel("controller.title", "Controller##editor");
    if (!ctx.state.controllerWindowOpen)
    {
        if (g.dirty && !GetEditorPreferences().autoSaveEditors)
        {
            ctx.state.controllerWindowOpen = true; // closing waits for the answer
            g.askUnsaved = true;
        }
        else
        {
            if (g.dirty)
                SaveGraph(ctx);
            return;
        }
    }
    CommitEdit(ctx);

    // First use: a tab beside the viewport, where the graph has room.
    if (const auto* viewport = ImGui::FindWindowSettingsByID(ImHashStr("Viewport##editor"));
        viewport && viewport->DockId && ImGui::DockBuilderGetNode(viewport->DockId))
        ImGui::SetNextWindowDockID(viewport->DockId, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(900.f, 560.f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin(title.c_str(), &ctx.state.controllerWindowOpen))
    {
        if (g.path.empty())
        {
            ImGui::TextWrapped("%s", Translate("controller.empty_hint"));
        }
        else
        {
            // Save line as in the material and animation editors.
            if (card::SaveButton(TranslateLabel("common.save", "Save##controller")))
                SaveGraph(ctx);
            ImGui::SameLine();
            if (g.dirty)
                ImGui::TextColored(ImVec4(0.96f, 0.82f, 0.30f, 1.f), "%s", Translate("common.unsaved_changes"));
            else
                value_ui::TextDisabled("%s", Translate(GetEditorPreferences().autoSaveEditors ? "material.autosaved" : "material.saved"));
            ImGui::SameLine();
            DrawPreviewButton(ctx);
            ImGui::SameLine();
            value_ui::TextDisabled("   %s", g.path.c_str());
            for (const std::string& problem : Compiled(ctx).problems)
                if (problem.find("no way out") == std::string::npos && problem.find("is missing") == std::string::npos)
                    ImGui::TextColored(theme::kWarning, "%s", problem.c_str());
            ImGui::Separator();

            const float panelWidth = std::min(240.f, ImGui::GetContentRegionAvail().x * 0.35f);
            if (ImGui::BeginChild("##ctrl_params", ImVec2(panelWidth, 0.f), ImGuiChildFlags_Borders))
                DrawParameterPanel(ctx);
            ImGui::EndChild();
            ImGui::SameLine();
            if (ImGui::BeginChild("##ctrl_graph", ImVec2(0.f, 0.f), ImGuiChildFlags_None,
                                  ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
                DrawCanvas(ctx);
            ImGui::EndChild();
            // Delete removes the selected state, transition or parameter while this
            // window has the focus (scene objects only from hierarchy or viewport).
            if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput &&
                ImGui::IsKeyPressed(ImGuiKey_Delete, false))
                ControllerGraphDeleteSelection(ctx);
        }

        if (g.askUnsaved)
        {
            g.askUnsaved = false;
            ImGui::OpenPopup("##ctrl_unsaved");
        }
        if (ImGui::BeginPopupModal("##ctrl_unsaved", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar))
        {
            ImGui::TextUnformatted(Translate("controller.unsaved"));
            const auto close = [&] {
                ImGui::CloseCurrentPopup();
                ctx.state.controllerWindowOpen = false;
            };
            if (ImGui::Button(TranslateLabel("common.save", "Save##ctrl_unsaved_save").c_str()) && SaveGraph(ctx))
                close();
            ImGui::SameLine();
            if (ImGui::Button(TranslateLabel("animation.discard", "Discard").c_str()))
            {
                const std::string path = g.path;
                g.dirty = false;
                g.path.clear();
                OpenControllerGraph(ctx, path); // back to the file on disk
                close();
            }
            ImGui::SameLine();
            if (ImGui::Button(TranslateLabel("common.cancel", "Cancel").c_str()))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }
    ImGui::End();
}

} // namespace engine::renderer::addons::editor

#else

namespace engine::renderer::addons::editor {
void DrawControllerGraphWindow(EditorFrameContext&) {}
bool OpenControllerGraph(EditorFrameContext&, const std::string&) { return false; }
bool DrawControllerGraphCard(EditorFrameContext&) { return false; }
bool SaveOpenControllerIfChanged(EditorFrameContext&) { return false; }
const std::string& ControllerGraphPath() { static const std::string empty; return empty; }
const engine::addons::animation::AnimationControllerAsset* ControllerGraphAsset() { return nullptr; }
bool ControllerGraphDirty() { return false; }
uint64_t ControllerGraphVersion() { return 0; }
std::string ControllerGraphAddState(EditorFrameContext&, const std::string&, float, float) { return {}; }
std::string ControllerGraphAddTransition(EditorFrameContext&, const std::string&, const std::string&) { return {}; }
std::string ControllerGraphAddParameter(EditorFrameContext&, engine::addons::animation::AnimationParameterType) { return {}; }
bool ControllerGraphSetEntry(EditorFrameContext&, const std::string&) { return false; }
bool ControllerGraphEdit(EditorFrameContext&, const std::function<void(engine::addons::animation::AnimationControllerAsset&)>&) { return false; }
void ControllerGraphSelect(const std::string&) {}
const std::string& ControllerGraphSelection() { static const std::string empty; return empty; }
bool ControllerGraphDeleteSelection(EditorFrameContext&) { return false; }
bool ControllerGraphScreenPosition(const std::string&, float&, float&) { return false; }
} // namespace engine::renderer::addons::editor

#endif
