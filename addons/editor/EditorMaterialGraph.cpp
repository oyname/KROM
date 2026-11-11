#include "addons/editor/EditorMaterialGraph.hpp"

#ifdef KROM_EDITOR_HAS_IMGUI

#include "addons/editor/EditorAssetBrowser.hpp"
#include "addons/editor/EditorFeature.hpp"
#include "addons/editor/EditorLocalization.hpp"
#include "addons/editor/EditorTheme.hpp"
#include "addons/editor/EditorValueClipboard.hpp"
#include "assets/AssetRegistry.hpp"
#include "core/PathUtf8.hpp"
#include "imgui_internal.h"

#include <optional>
#include <span>

#include "addons/editor/EditorGraphCanvas.hpp"

namespace engine::renderer::addons::editor {
namespace {

using assets::MaterialGraph;
using assets::MaterialGraphNode;
using assets::MaterialNodeType;
using assets::MaterialAsset;

constexpr char kGraphLinkPayload[] = "KROM_GRAPH_LINK";
constexpr char kTexturePayload[] = "KROM_ASSET_TEXTURE";
constexpr float kNodeWidthBase = 184.f;
constexpr float kMaterialWidth = 320.f;
constexpr float kInsetBase = 9.f;

// Zoom of the canvas being drawn: every pixel size of a node scales with it,
// together with the font and the style spacing (see DrawMaterialGraph).
float g_zoom = 1.f;
float kNodeWidth = kNodeWidthBase;
float kInset = kInsetBase;

float Z(float pixels)
{
    return pixels * g_zoom;
}

struct MaterialInput
{
    uint32_t MaterialGraph::*source;
    const char* label;
    std::optional<MaterialNodeType> required;
    assets::MaterialTextureRef MaterialAsset::*texture = nullptr;
    float MaterialAsset::*scalar = nullptr;
    float maximum = 1.f;
    bool softMaximum = false; // The slider ends at maximum, typed values may exceed it.
};

constexpr float kHardScalarMaximum = 1000.f;

constexpr MaterialInput kMaterialInputs[] = {
    {&MaterialGraph::baseColor, "material.base_color", {}, &MaterialAsset::baseColorTexture},
    {&MaterialGraph::normal, "material.graph_normal", MaterialNodeType::Texture, &MaterialAsset::normalTexture},
    {&MaterialGraph::orm, "material.graph_orm", MaterialNodeType::Texture, &MaterialAsset::metallicRoughnessTexture},
    {&MaterialGraph::emissive, "material.emissive", {}, &MaterialAsset::emissiveTexture},
    {&MaterialGraph::emissiveStrength, "material.emissive_strength", MaterialNodeType::Value, nullptr,
     &MaterialAsset::emissiveStrength, 20.f, true},
    {&MaterialGraph::metallic, "material.metallic", {}, nullptr, &MaterialAsset::metallicFactor},
    {&MaterialGraph::roughness, "material.roughness", {}, nullptr, &MaterialAsset::roughnessFactor},
    {&MaterialGraph::normalStrength, "material.normal_strength", MaterialNodeType::Value, nullptr, &MaterialAsset::normalScale, 4.f},
    {&MaterialGraph::occlusion, "material.occlusion", MaterialNodeType::Value, nullptr, &MaterialAsset::occlusionStrength, 4.f},
};

constexpr struct ShaderInput
{
    assets::ShaderStage stage;
    const char* label;
    std::string MaterialAsset::*path;
} kShaderInputs[] = {
    {assets::ShaderStage::Vertex, "material.graph_vertex_shader", &MaterialAsset::vertexShaderPath},
    {assets::ShaderStage::Fragment, "material.graph_pixel_shader", &MaterialAsset::fragmentShaderPath},
};

bool ShowMaterialInput(const MaterialInput& input, EditorMaterialTemplate type)
{
    switch (type)
    {
    case EditorMaterialTemplate::PbrLit:
        return true;
    case EditorMaterialTemplate::Unlit:
        return input.source == &MaterialGraph::baseColor || input.source == &MaterialGraph::emissive ||
               input.source == &MaterialGraph::emissiveStrength;
    case EditorMaterialTemplate::LegacyLit:
        return input.source != &MaterialGraph::orm && input.source != &MaterialGraph::occlusion;
    case EditorMaterialTemplate::Custom:
        return input.texture != nullptr || input.source == &MaterialGraph::emissiveStrength;
    }
    return false;
}

struct InputLink
{
    uint32_t* source = nullptr;
    std::string* texture = nullptr;

    uint32_t Node() const { return source ? *source : 0; }
    bool Connected() const { return Node() || (texture && !texture->empty()); }

    void Disconnect() const
    {
        if (Node())
            *source = 0;
        else if (texture)
            texture->clear();
    }
};

ImVec2 At(ImVec2 origin, float x, float y)
{
    return {origin.x + x, origin.y + y};
}

constexpr struct MathOperation
{
    MaterialNodeType type;
    const char* label;
} kMathOperations[] = {
    {MaterialNodeType::Add, "material.graph_add_op"},
    {MaterialNodeType::Multiply, "material.graph_multiply"},
    {MaterialNodeType::Subtract, "material.graph_subtract"},
    {MaterialNodeType::Power, "material.graph_power"},
    {MaterialNodeType::Invert, "material.graph_invert"},
    {MaterialNodeType::Clamp, "material.graph_clamp"},
    {MaterialNodeType::Remap, "material.graph_remap"},
    {MaterialNodeType::Sine, "material.graph_sine"},
};

bool IsMath(MaterialNodeType type)
{
    return std::any_of(std::begin(kMathOperations), std::end(kMathOperations),
                       [type](const MathOperation& operation) { return operation.type == type; });
}

int InputCount(MaterialNodeType type)
{
    return assets::MaterialNodeInputCount(type);
}

uint32_t& NodeInput(MaterialGraphNode& node, int i)
{
    return i == 0 ? node.a : i == 1 ? node.b : node.c;
}

uint32_t NodeInput(const MaterialGraphNode& node, int i)
{
    return i == 0 ? node.a : i == 1 ? node.b : node.c;
}

// Row of an input, in node rows. Mix has no combo above its inputs.
float InputRow(MaterialNodeType type, int i)
{
    // Mix has no combo above its inputs; a Texture takes its UV beside the image.
    if (type == MaterialNodeType::Texture)
        return 1.f;
    return (type == MaterialNodeType::Mix ? 1.f : 2.f) + static_cast<float>(i);
}

bool DependsOn(const MaterialGraph& graph, uint32_t source, uint32_t target, size_t remaining)
{
    if (source == 0 || remaining == 0)
        return false;
    if (source == target)
        return true;
    const auto* node = graph.Find(source);
    if (!node)
        return false;
    for (int i = 0; i < InputCount(node->type); ++i)
        if (DependsOn(graph, NodeInput(*node, i), target, remaining - 1))
            return true;
    return false;
}

const char* NodeLabel(MaterialNodeType type)
{
    switch (type)
    {
    case MaterialNodeType::Texture: return Translate("material.graph_texture");
    case MaterialNodeType::Value: return Translate("material.graph_value");
    case MaterialNodeType::Color: return Translate("material.graph_color");
    case MaterialNodeType::Mix: return Translate("material.graph_mix");
    case MaterialNodeType::Separate: return Translate("material.graph_separate");
    case MaterialNodeType::Time: return Translate("material.graph_time");
    case MaterialNodeType::UV: return Translate("material.graph_uv");
    case MaterialNodeType::Position: return Translate("material.graph_position");
    case MaterialNodeType::Vector2: return Translate("material.graph_vector2");
    case MaterialNodeType::Parameter: return Translate("material.graph_parameter");
    default: return Translate("material.graph_math");
    }
}

std::string LinkedNodeLabel(const MaterialGraph& graph, uint32_t nodeId, EditorMaterialTemplate type)
{
    std::string title;
    for (const auto& input : kMaterialInputs)
    {
        if (graph.*input.source != nodeId || !ShowMaterialInput(input, type))
            continue;
        if (!title.empty())
            title += " / ";
        title += Translate(input.label);
    }
    return title;
}

void AddNode(MaterialGraph& graph, MaterialNodeType type, math::Vec2 position, std::string path = {})
{
    if (graph.nodes.size() >= assets::kMaterialGraphMaxNodes)
        return;
    uint32_t id = 1;
    while (graph.Find(id))
        ++id;
    MaterialGraphNode node;
    node.id = id;
    node.type = type;
    node.position = position;
    node.texturePath = std::move(path);
    if (type == MaterialNodeType::Mix)
        node.value.z = 0.5f;
    else if (type == MaterialNodeType::Separate)
        node.value.x = 0.f;
    else if (type == MaterialNodeType::Position)
        node.value = {1.f, 0.f, 0.f}; // scale 1, plane XZ (ground)
    else if (type == MaterialNodeType::Vector2)
        node.value = {0.f, 0.f, 0.f};
    graph.nodes.push_back(std::move(node));
}

void RemoveNode(MaterialGraph& graph, uint32_t id)
{
    const auto unlink = [id](uint32_t& input) { if (input == id) input = 0; };
    for (auto& node : graph.nodes)
    {
        unlink(node.a);
        unlink(node.b);
        unlink(node.c);
    }
    for (const auto& input : kMaterialInputs)
        unlink(graph.*input.source);
    std::erase_if(graph.nodes, [id](const auto& node) { return node.id == id; });
}

ImVec2 OutputPin(ImVec2 node, float step)
{
    return At(node, kNodeWidth - Z(3.f), step * 1.5f);
}

void DrawWire(ImVec2 from, ImVec2 to, ImU32 color)
{
    const float bend = std::clamp(std::abs(to.x - from.x) * 0.45f, Z(30.f), Z(100.f));
    ImGui::GetWindowDrawList()->AddBezierCubic(from, At(from, bend, 0.f),
        At(to, -bend, 0.f), to, color, std::max(1.f, Z(2.f)));
}

void DrawSocket(const char* label, ImVec2 pin, bool linked, ImU32 color)
{
    const float hit = std::max(8.f, Z(14.f));
    ImGui::SetCursorScreenPos(At(pin, -hit * 0.5f, -hit * 0.5f));
    ImGui::InvisibleButton(label, ImVec2(hit, hit), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    auto* draw = ImGui::GetWindowDrawList();
    if (linked || ImGui::IsItemHovered())
        draw->AddCircleFilled(pin, Z(4.f), color);
    else
        draw->AddCircle(pin, Z(4.f), color, 0, 1.5f);
}

void DrawInput(MaterialGraph& graph, uint32_t node, InputLink link, const char* label,
               ImVec2 pin, ImU32 color, bool& changed, const char*& error, InputLink& detachOnRelease,
               std::optional<MaterialNodeType> required = {})
{
    DrawSocket(label, pin, link.Connected(), color);
    const char* typeHint = !required ? nullptr : *required == MaterialNodeType::Value
        ? "material.graph_value_input" : "material.graph_texture_input";
    if (typeHint)
        ImGui::SetItemTooltip("%s\n%s", Translate(typeHint), Translate("material.graph_disconnect_tip"));
    else
        ImGui::SetItemTooltip("%s", Translate("material.graph_input_tip"));
    if (link.Connected())
    {
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
        {
            link.Disconnect();
            changed = true;
        }
        else if (ImGui::IsItemDeactivated() && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
        {
            const ImVec2 drag = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
            if (drag.x != 0.f || drag.y != 0.f)
                detachOnRelease = link;
        }
    }
    if (link.source && ImGui::BeginDragDropTarget())
    {
        uint32_t& source = *link.source;
        if (const auto* payload = ImGui::AcceptDragDropPayload(kGraphLinkPayload);
            payload && payload->DataSize == sizeof(uint32_t))
        {
            const uint32_t input = *static_cast<const uint32_t*>(payload->Data);
            const auto* inputNode = graph.Find(input);
            if (inputNode && required && inputNode->type != *required)
                error = typeHint;
            else if (inputNode && input != source)
            {
                const uint32_t previous = source;
                source = input;
                if (!required && ((node != 0 && DependsOn(graph, input, node, graph.nodes.size())) ||
                                  !assets::CompileMaterialGraph(graph).valid))
                {
                    source = previous;
                    error = "material.graph_link_error";
                }
                else
                    changed = true;
            }
        }
        ImGui::EndDragDropTarget();
    }
}

void DrawDisconnectButton(InputLink link, ImVec2 row, float width, bool& changed)
{
    if (!link.Connected())
        return;
    const float buttonWidth = ImGui::GetFrameHeight();
    ImGui::SetCursorScreenPos(At(row, width - kInset - buttonWidth, 0.f));
    if (ImGui::Button("x##disconnect", ImVec2(buttonWidth, 0.f)))
    {
        link.Disconnect();
        changed = true;
    }
    ImGui::SetItemTooltip("%s", Translate("material.graph_disconnect"));
}

void DrawFrame(ImVec2 pos, float height, float width = kNodeWidth)
{
    auto* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(pos, At(pos, width, height), ImGui::GetColorU32(theme::kPanel), Z(4.f));
    draw->AddRect(pos, At(pos, width, height), ImGui::GetColorU32(theme::kBorder), Z(4.f));
}

void DrawTextureAssignment(EditorFrameContext& ctx, const std::string& path, ImVec2 pos, float width)
{
    auto* draw = ImGui::GetWindowDrawList();
    const float size = ImGui::GetFrameHeight();
    draw->AddRectFilled(pos, At(pos, size, size), ImGui::GetColorU32(theme::kField), 2.f);
    if (const auto* thumb = GetAssetTextureThumbnail(ctx, path))
        draw->AddImage(reinterpret_cast<ImTextureID>(thumb->imguiId), pos, At(pos, size, size));
    const std::string name = PathToUtf8(PathFromUtf8(path).filename());
    const ImVec2 textEnd = At(pos, width, size);
    ImGui::RenderTextEllipsis(draw, At(pos, size + Z(5.f), ImGui::GetStyle().FramePadding.y), textEnd,
                             textEnd.x, name.c_str(), name.c_str() + name.size(), nullptr);
    if (ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(pos, textEnd))
        ImGui::SetTooltip("%s", path.c_str());
}

void DrawMaterialInput(EditorFrameContext& ctx, MaterialAsset& asset, const MaterialInput& input,
                       bool custom, ImVec2 row, float step, float materialWidth, ImU32 color, bool& changed, const char*& error,
                       InputLink& detachOnRelease)
{
    auto& graph = asset.graph;
    std::string* texture = input.texture ? &(asset.*input.texture).path : nullptr;
    const InputLink link{&(graph.*input.source), texture};
    const float buttonWidth = ImGui::GetFrameHeight();
    const float fieldX = materialWidth * (132.f / kMaterialWidth);
    const float buttonX = materialWidth - kInset - buttonWidth;
    const float fieldWidth = std::max(1.f, buttonX - fieldX - kInset);
    ImGui::PushID(input.label);
    DrawInput(graph, 0, link, "##input", At(row, Z(3.f), step * 0.5f), color, changed, error,
              detachOnRelease, custom && input.texture ? std::optional(MaterialNodeType::Texture) : input.required);
    auto* draw = ImGui::GetWindowDrawList();
    draw->AddText(At(row, kInset * 2.f, ImGui::GetStyle().FramePadding.y), IM_COL32_WHITE, Translate(input.label));

    ImGui::SetCursorScreenPos(At(row, fieldX, 0.f));
    if (input.scalar)
    {
        float& fallback = asset.*input.scalar;
        float* linked = graph.FindValue(link.Node());
        if (link.Node() != 0 && !linked)
            ImGui::TextDisabled("%s", Translate("material.graph_driven"));
        else
        {
            ImGui::SetNextItemWidth(fieldWidth);
            const float hardMaximum = input.softMaximum ? kHardScalarMaximum : input.maximum;
            float value = linked ? std::clamp(*linked, 0.f, hardMaximum) : fallback;
            if (ImGui::SliderFloat("##value", &value, 0.f, input.maximum, "%.3f",
                                   input.softMaximum ? ImGuiSliderFlags_None : ImGuiSliderFlags_AlwaysClamp))
            {
                (linked ? *linked : fallback) = std::clamp(value, 0.f, hardMaximum);
                changed = true;
            }
            if (linked)
                ImGui::SetItemTooltip("%s", Translate("material.graph_linked_value"));
        }
    }
    else
    {
        constexpr auto swatch = ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel;
        const bool baseColor = input.source == &MaterialGraph::baseColor;
        const bool emissive = input.source == &MaterialGraph::emissive;
        // A linked node replaces the color; only the base color's alpha still applies.
        // Custom links only select the texture, so the factors stay available there.
        const bool colorReplaced = link.Node() != 0 && !custom;
        if (baseColor && colorReplaced)
        {
            ImGui::SetNextItemWidth(fieldWidth);
            changed = ImGui::SliderFloat("##alpha", &asset.baseColorFactor.w, 0.f, 1.f, "Alpha %.3f",
                                         ImGuiSliderFlags_AlwaysClamp) || changed;
        }
        else if (baseColor)
            changed = value_ui::ColorEdit4("##color", &asset.baseColorFactor.x, swatch) || changed;
        if (emissive && !colorReplaced)
            changed = value_ui::ColorEdit3("##color", &asset.emissiveFactor.x,
                                          swatch | ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR) || changed;
        if (!link.Node() && !texture->empty())
        {
            const float offset = baseColor || emissive ? buttonWidth + ImGui::GetStyle().ItemSpacing.x : 0.f;
            DrawTextureAssignment(ctx, *texture, At(row, fieldX + offset, 0.f), fieldWidth - offset);
        }
    }
    DrawDisconnectButton(link, row, materialWidth, changed);

    const ImRect area(row, At(row, materialWidth, step));
    if (texture && ImGui::BeginDragDropTargetCustom(area, ImGui::GetID("##direct_texture")))
    {
        if (const auto* payload = ImGui::AcceptDragDropPayload(kTexturePayload))
        {
            *texture = static_cast<const char*>(payload->Data);
            if (link.source)
                *link.source = 0;
            changed = true;
        }
        ImGui::EndDragDropTarget();
    }
    ImGui::PopID();
}

void DrawTextureSource(EditorFrameContext& ctx, MaterialGraph& graph, MaterialGraphNode& node,
                       ImVec2 pos, float step, bool showUV, bool& changed, const char*& error,
                       ImU32 color, InputLink& detachOnRelease)
{
    if (showUV)
    {
        // A linked UV node replaces the mesh UV: here, and for normal and ORM maps.
        ImGui::PushID("uv_input");
        DrawInput(graph, node.id, {&node.a}, "##uv", At(pos, Z(3.f), step * 1.5f), color, changed, error, detachOnRelease);
        ImGui::PopID();
        // Normal and ORM slots take the plain texture, so its own UV fields cannot apply
        // there; with a linked UV they are replaced anyway.
        ImGui::BeginDisabled(graph.normal == node.id || graph.orm == node.id || node.a != 0);
        const float width = kNodeWidth - 2.f * kInset - Z(50.f);
        ImGui::SetCursorScreenPos(At(pos, kInset, step * 2.8f));
        ImGui::SetNextItemWidth(width);
        changed = ImGui::DragFloat2(Translate("material.graph_uv_scale"), &node.uvScale.x, 0.01f) || changed;
        ImGui::SetCursorScreenPos(At(pos, kInset, step * 3.8f));
        ImGui::SetNextItemWidth(width);
        changed = ImGui::DragFloat2(Translate("material.graph_uv_offset"), &node.uvOffset.x, 0.01f) || changed;
        ImGui::SetCursorScreenPos(At(pos, kInset, step * 4.8f));
        ImGui::SetNextItemWidth(width);
        changed = ImGui::DragFloat2(Translate("material.graph_uv_scroll"), &node.uvScroll.x, 0.01f) || changed;
        ImGui::EndDisabled();
    }

    const ImVec2 image = At(pos, kInset, step + Z(1.f));
    ImGui::SetCursorScreenPos(image);
    const float size = step * 1.5f;
    ImGui::InvisibleButton("##texture", ImVec2(size, size));
    if (const auto* thumb = GetAssetTextureThumbnail(ctx, node.texturePath))
        ImGui::GetWindowDrawList()->AddImage(reinterpret_cast<ImTextureID>(thumb->imguiId), image, At(image, size, size));
    else
        ImGui::GetWindowDrawList()->AddRectFilled(image, At(image, size, size), ImGui::GetColorU32(theme::kField));
    ImGui::SetItemTooltip("%s", node.texturePath.empty() ? Translate("material.graph_texture_tip") : node.texturePath.c_str());
    if (ImGui::BeginDragDropTarget())
    {
        if (const auto* payload = ImGui::AcceptDragDropPayload(kTexturePayload))
        {
            const auto previous = node.texturePath;
            node.texturePath = static_cast<const char*>(payload->Data);
            if (!assets::CompileMaterialGraph(graph).valid)
            {
                node.texturePath = previous;
                error = "material.graph_link_error";
            }
            else
                changed = true;
        }
        ImGui::EndDragDropTarget();
    }
    const std::string name = node.texturePath.empty() ? Translate("common.empty")
        : PathToUtf8(PathFromUtf8(node.texturePath).filename());
    const ImVec2 textPos = At(image, size + Z(5.f), step * 0.8f);
    const ImVec2 textEnd = At(pos, kNodeWidth - kInset, step * 2.8f);
    ImGui::RenderTextEllipsis(ImGui::GetWindowDrawList(), textPos, textEnd, textEnd.x,
                             name.c_str(), name.c_str() + name.size(), nullptr);
}

} // namespace

bool DrawMaterialGraph(EditorFrameContext& ctx, MaterialAsset& asset,
                       EditorMaterialTemplate materialType,
                       const std::function<EditorMaterialTemplate()>& drawMaterialType,
                       const std::function<bool(EditorMaterialTemplate)>& drawSettings,
                       const std::function<bool(assets::ShaderStage, float)>& drawShaderSource)
{
    auto& graph = asset.graph;
    const bool canAdd = graph.nodes.size() < assets::kMaterialGraphMaxNodes;
    ImGui::BeginDisabled(!canAdd);
    const bool addFromToolbar = ImGui::Button(Translate("material.graph_add"));
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("%zu / %zu", graph.nodes.size(), assets::kMaterialGraphMaxNodes);
    ImGui::TextWrapped("%s", Translate(materialType == EditorMaterialTemplate::Custom
        ? "material.graph_custom_hint" : "material.graph_hint"));

    bool changed = false;
    // The wheel zooms and the middle button pans, so the canvas never scrolls.
    if (ImGui::BeginChild("##material_graph_canvas", ImVec2(0.f, 0.f), ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
    {
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const ImVec2 available = ImGui::GetContentRegionAvail();
        auto* draw = ImGui::GetWindowDrawList();

        // View of this material's graph: kept in the window's state storage.
        auto* viewStorage = ImGui::GetStateStorage();
        const ImGuiID panXId = ImGui::GetID("##canvas_pan_x"), panYId = ImGui::GetID("##canvas_pan_y");
        const ImGuiID zoomId = ImGui::GetID("##canvas_zoom");
        ImVec2 pan(viewStorage->GetFloat(panXId, 0.f), viewStorage->GetFloat(panYId, 0.f));
        float zoom = viewStorage->GetFloat(zoomId, 1.f);
        const bool canvasHovered = ImGui::IsWindowHovered() && !ImGui::IsAnyItemActive();
        const graph_canvas::View view = graph_canvas::Update(*draw, origin, available, pan, zoom, canvasHovered, 0.5f, 2.f);
        viewStorage->SetFloat(panXId, pan.x);
        viewStorage->SetFloat(panYId, pan.y);
        viewStorage->SetFloat(zoomId, zoom);

        // Everything in the graph scales with the zoom: font, spacing, node sizes.
        g_zoom = zoom;
        kNodeWidth = kNodeWidthBase * zoom;
        kInset = kInsetBase * zoom;
        const ImGuiStyle& baseStyle = ImGui::GetStyle();
        ImGui::PushFont(nullptr, baseStyle.FontSizeBase * zoom);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(baseStyle.FramePadding.x * zoom, baseStyle.FramePadding.y * zoom));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(baseStyle.ItemSpacing.x * zoom, baseStyle.ItemSpacing.y * zoom));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing,
                            ImVec2(baseStyle.ItemInnerSpacing.x * zoom, baseStyle.ItemInnerSpacing.y * zoom));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, baseStyle.FrameRounding * zoom);

        const float step = ImGui::GetFrameHeightWithSpacing();
        const float stepWorld = step / zoom;
        const float materialWidth = kMaterialWidth * std::max(1.f, ImGui::GetStyle().FontScaleMain) * zoom;
        const auto nodePos = [&view](const MaterialGraphNode& node) {
            return view.ToScreen(ImVec2(node.position.x, node.position.y));
        };
        // The material card lives in the graph like the nodes: it pans and zooms with
        // them and is dragged by its title row. Its place is kept for the session
        // (the .mat has no position for it); the first time it sits at the right edge.
        const ImGuiID cardXId = ImGui::GetID("##card_x"), cardYId = ImGui::GetID("##card_y");
        const ImGuiID cardInitId = ImGui::GetID("##card_init");
        if (!viewStorage->GetBool(cardInitId, false))
        {
            const ImVec2 start = view.ToWorld(At(origin, std::max(Z(220.f), available.x - materialWidth - kInset), Z(14.f)));
            viewStorage->SetFloat(cardXId, start.x);
            viewStorage->SetFloat(cardYId, start.y);
            viewStorage->SetBool(cardInitId, true);
        }
        const ImVec2 cardTop = view.ToScreen(ImVec2(viewStorage->GetFloat(cardXId), viewStorage->GetFloat(cardYId)));
        const ImVec2 material = At(cardTop, 0.f, step); // content below the title row
        ImDrawListSplitter materialChannels;
        materialChannels.Split(draw, 2);
        {
            // Title row: drag handle of the card.
            materialChannels.SetCurrentChannel(draw, 1);
            ImGui::SetCursorScreenPos(cardTop);
            ImGui::InvisibleButton("##material_card_move", ImVec2(materialWidth, step));
            if (ImGui::IsItemActivated())
            {
                viewStorage->SetFloat(ImGui::GetID("##card_drag_x"), viewStorage->GetFloat(cardXId));
                viewStorage->SetFloat(ImGui::GetID("##card_drag_y"), viewStorage->GetFloat(cardYId));
            }
            if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
            {
                const ImVec2 drag = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
                viewStorage->SetFloat(cardXId, graph_canvas::Snap(viewStorage->GetFloat(ImGui::GetID("##card_drag_x")) + drag.x / zoom));
                viewStorage->SetFloat(cardYId, graph_canvas::Snap(viewStorage->GetFloat(ImGui::GetID("##card_drag_y")) + drag.y / zoom));
            }
            if (ImGui::IsItemHovered() || ImGui::IsItemActive())
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
            draw->AddText(At(cardTop, kInset, ImGui::GetStyle().FramePadding.y), ImGui::GetColorU32(theme::kText),
                          Translate("material.graph_output"));
            materialChannels.SetCurrentChannel(draw, 0);
        }
        if (drawMaterialType)
        {
            materialChannels.SetCurrentChannel(draw, 1);
            ImGui::SetCursorScreenPos(At(material, kInset, 0.f));
            ImGui::SetNextItemWidth(materialWidth - 2.f * kInset);
            materialType = drawMaterialType();
            materialChannels.SetCurrentChannel(draw, 0);
        }

        const bool custom = materialType == EditorMaterialTemplate::Custom;
        const auto nodes = std::span(graph.nodes);
        const auto materialPin = [&](int i) { return At(material, Z(3.f), step * (i + 1.5f)); };
        const ImU32 color = ImGui::GetColorU32(theme::kAccentHover);
        const auto wire = [&](uint32_t source, ImVec2 target) {
            if (const auto* node = graph.Find(source))
                DrawWire(OutputPin(nodePos(*node), step), target, color);
        };

        for (const auto& node : nodes)
        {
            for (int i = 0; i < InputCount(node.type); ++i)
                wire(NodeInput(node, i), At(nodePos(node), Z(3.f), step * (InputRow(node.type, i) + 0.5f)));
        }
        int pinRow = custom ? IM_ARRAYSIZE(kShaderInputs) : 0;
        for (const auto& input : kMaterialInputs)
            if (ShowMaterialInput(input, materialType))
                wire(graph.*input.source, materialPin(pinRow++));
        if (const auto* payload = ImGui::GetDragDropPayload();
            payload && payload->IsDataType(kGraphLinkPayload) && payload->DataSize == sizeof(uint32_t))
        {
            if (const auto* node = graph.Find(*static_cast<const uint32_t*>(payload->Data)))
                DrawWire(OutputPin(nodePos(*node), step), ImGui::GetMousePos(), color);
        }

        float extentX = material.x - origin.x + materialWidth + 8.f;
        float extentY = 0.f;
        const char* error = nullptr;
        InputLink detachOnRelease;
        bool cardHovered = false;
        uint32_t removed = 0;
        for (auto& node : nodes)
        {
            ImGui::PushID(static_cast<int>(node.id));
            const ImVec2 pos = nodePos(node);
            const bool oneInputRow = node.type == MaterialNodeType::Separate ||
                (IsMath(node.type) && InputCount(node.type) == 1 && node.type != MaterialNodeType::Remap);
            const float height = step * (oneInputRow || node.type == MaterialNodeType::Parameter ||
                                         node.type == MaterialNodeType::Position ? 3.15f
                : IsMath(node.type) || node.type == MaterialNodeType::Mix ? 4.15f
                : node.type == MaterialNodeType::Texture ? (custom ? 2.9f : 5.85f) : 2.15f);
            cardHovered |= ImGui::IsMouseHoveringRect(At(pos, -Z(5.f), 0.f), At(pos, kNodeWidth + Z(5.f), height));
            DrawFrame(pos, height);
            extentX = std::max(extentX, node.position.x + kNodeWidth + 8.f);
            extentY = std::max(extentY, node.position.y + height + 8.f);

            ImGui::SetCursorScreenPos(pos);
            ImGui::InvisibleButton("##move", ImVec2(kNodeWidth - Z(28.f), step));
            const ImGuiID dragX = ImGui::GetID("##drag_start_x"), dragY = ImGui::GetID("##drag_start_y");
            if (ImGui::IsItemActivated())
            {
                viewStorage->SetFloat(dragX, node.position.x);
                viewStorage->SetFloat(dragY, node.position.y);
            }
            const std::string linkedTitle = LinkedNodeLabel(graph, node.id, materialType);
            const std::string title = linkedTitle.empty() ? NodeLabel(node.type) : linkedTitle;
            const ImVec2 titleEnd = At(pos, kNodeWidth - Z(28.f), step);
            ImGui::RenderTextEllipsis(ImGui::GetWindowDrawList(), At(pos, kInset, ImGui::GetStyle().FramePadding.y),
                                     titleEnd, titleEnd.x, title.c_str(), title.c_str() + title.size(), nullptr);
            if (!linkedTitle.empty())
                ImGui::SetItemTooltip("%s\n%s", linkedTitle.c_str(), NodeLabel(node.type));
            if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
            {
                const ImVec2 drag = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
                constexpr float limit = assets::kMaterialGraphCoordinateLimit;
                const float x = std::clamp(graph_canvas::Snap(viewStorage->GetFloat(dragX) + drag.x / zoom), -limit, limit);
                const float y = std::clamp(graph_canvas::Snap(viewStorage->GetFloat(dragY) + drag.y / zoom), -limit, limit);
                if (x != node.position.x || y != node.position.y)
                {
                    node.position.x = x;
                    node.position.y = y;
                    changed = true;
                }
            }
            ImGui::SetCursorScreenPos(At(pos, kNodeWidth - Z(25.f), 0.f));
            if (ImGui::Button("x##remove", ImVec2(Z(24.f), 0.f)))
                removed = node.id;
            ImGui::SetItemTooltip("%s", Translate("material.graph_remove"));

            DrawSocket("##out", OutputPin(pos, step), true, color);
            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoPreviewTooltip))
            {
                ImGui::SetDragDropPayload(kGraphLinkPayload, &node.id, sizeof(node.id));
                ImGui::EndDragDropSource();
            }

            if (node.type == MaterialNodeType::Texture)
                DrawTextureSource(ctx, graph, node, pos, step, !custom, changed, error, color, detachOnRelease);
            else
            {
                ImGui::SetCursorScreenPos(At(pos, kInset, step));
                ImGui::SetNextItemWidth(kNodeWidth - 3.f * kInset);
                if (node.type == MaterialNodeType::Value)
                    changed = ImGui::DragFloat("##value", &node.value.x, 0.01f) || changed;
                else if (node.type == MaterialNodeType::Color)
                    changed = ImGui::ColorEdit3("##color", &node.value.x, ImGuiColorEditFlags_Float) || changed;
                else if (IsMath(node.type))
                {
                    const char* operations[IM_ARRAYSIZE(kMathOperations)];
                    int op = 0;
                    for (int i = 0; i < IM_ARRAYSIZE(kMathOperations); ++i)
                    {
                        operations[i] = Translate(kMathOperations[i].label);
                        if (kMathOperations[i].type == node.type)
                            op = i;
                    }
                    if (ImGui::Combo("##operation", &op, operations, IM_ARRAYSIZE(operations)))
                    {
                        node.type = kMathOperations[op].type;
                        if (InputCount(node.type) < 2)
                            node.b = 0;
                        if (node.type == MaterialNodeType::Remap)
                        {
                            node.value.y = 0.f;
                            node.value.z = 1.f;
                        }
                        changed = true;
                    }
                }
                else if (node.type == MaterialNodeType::Time)
                    changed = ImGui::DragFloat(Translate("material.graph_speed"), &node.value.x, 0.01f) || changed;
                else if (node.type == MaterialNodeType::UV)
                {
                    changed = ImGui::DragFloat2("##tiling", &node.value.x, 0.01f) || changed;
                    ImGui::SetItemTooltip("%s", Translate("material.graph_tiling"));
                }
                else if (node.type == MaterialNodeType::Vector2)
                    changed = ImGui::DragFloat2("##vector2", &node.value.x, 0.01f) || changed;
                else if (node.type == MaterialNodeType::Position)
                {
                    const char* planes[] = {"XZ", "XY", "YZ"};
                    int plane = std::clamp(static_cast<int>(node.value.y), 0, 2);
                    if (ImGui::Combo("##plane", &plane, planes, IM_ARRAYSIZE(planes)))
                    {
                        node.value.y = static_cast<float>(plane);
                        changed = true;
                    }
                    ImGui::SetItemTooltip("%s", Translate("material.graph_plane"));
                    ImGui::SetCursorScreenPos(At(pos, kInset, step * 2.f));
                    ImGui::SetNextItemWidth(kNodeWidth - 3.f * kInset);
                    changed = ImGui::DragFloat("##scale", &node.value.x, 0.01f) || changed;
                    ImGui::SetItemTooltip("%s", Translate("material.graph_scale"));
                }
                else if (node.type == MaterialNodeType::Parameter)
                {
                    char name[64];
                    std::snprintf(name, sizeof(name), "%s", node.texturePath.c_str());
                    ImGui::SetNextItemWidth(kNodeWidth - 2.f * kInset);
                    if (ImGui::InputTextWithHint("##name", Translate("material.graph_parameter_name"), name, sizeof(name)))
                    {
                        node.texturePath = name;
                        changed = true;
                    }
                    ImGui::SetCursorScreenPos(At(pos, kInset, step * 2.f));
                    ImGui::SetNextItemWidth(kNodeWidth - 2.f * kInset);
                    changed = ImGui::DragFloat3("##default", &node.value.x, 0.01f) || changed;
                    ImGui::SetItemTooltip("%s", Translate("material.graph_parameter_default"));
                }
                else if (node.type == MaterialNodeType::Separate)
                {
                    const char* channels[] = {"R", "G", "B", "A"};
                    int channel = std::clamp(static_cast<int>(node.value.x), 0, 3);
                    if (ImGui::Combo("##channel", &channel, channels, IM_ARRAYSIZE(channels)))
                    {
                        node.value.x = static_cast<float>(channel);
                        changed = true;
                    }
                }

                for (int i = 0; i < InputCount(node.type); ++i)
                {
                    ImGui::PushID(i);
                    uint32_t& input = NodeInput(node, i);
                    const float row = InputRow(node.type, i);
                    const uint32_t previousInput = input;
                    DrawInput(graph, node.id, {&input}, "##in", At(pos, Z(3.f), step * (row + 0.5f)),
                              color, changed, error, detachOnRelease);
                    const ImVec2 field = At(pos, 2.f * kInset, step * row);
                    if (node.type == MaterialNodeType::Separate || node.type == MaterialNodeType::Remap)
                    {
                        ImGui::GetWindowDrawList()->AddText(At(field, 0.f, ImGui::GetStyle().FramePadding.y),
                                                            IM_COL32_WHITE, Translate("material.graph_source"));
                        if (node.type == MaterialNodeType::Remap)
                        {
                            ImGui::SetCursorScreenPos(At(pos, 2.f * kInset, step * 3.f));
                            ImGui::SetNextItemWidth(kNodeWidth - 5.f * kInset - Z(16.f));
                            changed = ImGui::DragFloat2(Translate("material.graph_remap_range"), &node.value.y, 0.01f) || changed;
                        }
                        ImGui::PopID();
                        continue;
                    }
                    float& ownValue = i == 0 ? node.value.x : i == 1 ? node.value.y : node.value.z;
                    if (input != previousInput && input != 0)
                        ownValue = 1.f; // A new link starts at full strength.
                    const bool mask = i == 2;
                    const char* name = mask ? Translate("material.graph_mix_factor") : i == 0 ? "A" : "B";
                    ImGui::SetCursorScreenPos(field);
                    ImGui::SetNextItemWidth(kNodeWidth - 5.f * kInset - (mask ? Z(32.f) : 0.f));
                    auto* source = graph.Find(input);
                    if (mask && source && source->type != MaterialNodeType::Value && source->type != MaterialNodeType::Color)
                        ImGui::TextDisabled("%s", Translate("material.graph_driven"));
                    else if (source && source->type == MaterialNodeType::Color)
                        changed = ImGui::ColorEdit3(name, &source->value.x, ImGuiColorEditFlags_Float) || changed;
                    else
                    {
                        float* linkedValue = graph.FindValue(input);
                        // Zero limits leave DragFloat unbounded.
                        const float limit = mask && !linkedValue ? 1.f : 0.f;
                        changed = ImGui::DragFloat(name, linkedValue ? linkedValue : &ownValue, 0.01f, 0.f, limit) || changed;
                        if (source)
                            ImGui::SetItemTooltip("%s", Translate(linkedValue ? "material.graph_linked_value"
                                                                              : "material.graph_input_strength"));
                    }
                    ImGui::PopID();
                }
            }
            ImGui::PopID();
        }

        if (custom && drawShaderSource)
        {
            const float height = step * 3.2f;
            for (int i = 0; i < IM_ARRAYSIZE(kShaderInputs); ++i)
            {
                const auto& input = kShaderInputs[i];
                const ImVec2 pos = view.ToScreen(ImVec2(14.f, 14.f + i * (height / zoom + kInsetBase)));
                const bool assigned = !(asset.*input.path).empty();
                if (assigned)
                    DrawWire(OutputPin(pos, step), materialPin(i), color);
                DrawFrame(pos, height);
                cardHovered |= ImGui::IsMouseHoveringRect(pos, At(pos, kNodeWidth + Z(5.f), height));
                draw->AddText(At(pos, kInset, ImGui::GetStyle().FramePadding.y), IM_COL32_WHITE, Translate(input.label));
                draw->AddCircleFilled(OutputPin(pos, step), Z(4.f),
                    assigned ? color : ImGui::GetColorU32(ImGuiCol_TextDisabled));
                ImGui::PushID(input.label);
                ImGui::SetCursorScreenPos(At(pos, kInset, step));
                const float indent = pos.x - origin.x + kInset;
                ImGui::Indent(indent);
                changed = drawShaderSource(input.stage, kNodeWidth - 3.f * kInset) || changed;
                ImGui::Unindent(indent);
                ImGui::PopID();
                extentY = std::max(extentY, pos.y - origin.y + height + 8.f);
            }
        }
        materialChannels.SetCurrentChannel(draw, 1);
        int rowCount = 0;
        if (custom)
        {
            for (const auto& input : kShaderInputs)
            {
                const ImVec2 row = At(material, 0.f, step * (++rowCount));
                const InputLink link{nullptr, &(asset.*input.path)};
                ImGui::PushID(input.label);
                DrawInput(graph, 0, link, "##shader_input", At(row, Z(3.f), step * 0.5f),
                          color, changed, error, detachOnRelease);
                draw->AddText(At(row, 2.f * kInset, ImGui::GetStyle().FramePadding.y), IM_COL32_WHITE, Translate(input.label));
                DrawDisconnectButton(link, row, materialWidth, changed);
                ImGui::PopID();
            }
        }
        for (const auto& input : kMaterialInputs)
            if (ShowMaterialInput(input, materialType))
                DrawMaterialInput(ctx, asset, input, custom, At(material, 0.f, step * (++rowCount)),
                                  step, materialWidth, color, changed, error, detachOnRelease);
        const float inputsHeight = step * (rowCount + 1.f);
        float materialHeight = inputsHeight + kInset;
        if (drawSettings)
        {
            ImGui::SetCursorScreenPos(At(material, kInset, inputsHeight));
            ImGui::Indent(material.x - origin.x + kInset);
            ImGui::PushItemWidth(std::max(1.f, materialWidth - 2.f * kInset -
                ImGui::CalcTextSize(Translate("material.box_tile_size")).x - ImGui::GetStyle().ItemInnerSpacing.x));
            changed = drawSettings(materialType) || changed;
            ImGui::PopItemWidth();
            ImGui::Unindent(material.x - origin.x + kInset);
            materialHeight = ImGui::GetCursorScreenPos().y - material.y + kInset;
        }
        materialChannels.SetCurrentChannel(draw, 0);
        DrawFrame(cardTop, materialHeight + step, materialWidth);
        draw->AddLine(At(cardTop, 0.f, step - Z(2.f)), At(cardTop, materialWidth, step - Z(2.f)), ImGui::GetColorU32(theme::kBorder));
        materialChannels.Merge(draw);
        cardHovered |= ImGui::IsMouseHoveringRect(At(cardTop, -Z(5.f), 0.f), At(material, materialWidth, materialHeight));
        extentY = std::max(extentY, material.y - origin.y + materialHeight + 8.f);

        const ImRect canvas = ImGui::GetCurrentWindow()->InnerClipRect;
        const bool freeCanvas = !cardHovered && canvas.Contains(ImGui::GetMousePos()) &&
            ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
        if (freeCanvas && detachOnRelease.Connected())
        {
            detachOnRelease.Disconnect();
            changed = true;
        }
        if (custom)
        {
            if (asset.vertexShaderPath.empty())
                asset.vertexShader = ShaderHandle::Invalid();
            if (asset.fragmentShaderPath.empty())
                asset.fragmentShader = ShaderHandle::Invalid();
        }

        if (removed != 0)
        {
            RemoveNode(graph, removed);
            changed = true;
        }

        if (freeCanvas && ImGui::BeginDragDropTargetCustom(canvas, ImGui::GetID("##graph_texture_drop")))
        {
            if (const auto* payload = ImGui::AcceptDragDropPayload(kTexturePayload);
                payload && graph.nodes.size() < assets::kMaterialGraphMaxNodes)
            {
                const ImVec2 world = view.ToWorld(ImGui::GetMousePos());
                AddNode(graph, MaterialNodeType::Texture,
                        {graph_canvas::Snap(world.x - kNodeWidthBase * 0.5f), graph_canvas::Snap(world.y - stepWorld * 0.5f)},
                        static_cast<const char*>(payload->Data));
                changed = true;
            }
            ImGui::EndDragDropTarget();
        }

        if (error)
        {
            ImGui::GetStateStorage()->SetVoidPtr(ImGui::GetID("##graph_link_error_message"),
                                                const_cast<char*>(error));
            ImGui::OpenPopup("##graph_link_error");
        }
        if (ImGui::BeginPopup("##graph_link_error"))
        {
            const auto* message = static_cast<const char*>(
                ImGui::GetStateStorage()->GetVoidPtr(ImGui::GetID("##graph_link_error_message")));
            ImGui::TextUnformatted(Translate(message ? message : "material.graph_link_error"));
            ImGui::EndPopup();
        }

        auto* storage = ImGui::GetStateStorage();
        const ImGuiID addX = ImGui::GetID("##add_node_x");
        const ImGuiID addY = ImGui::GetID("##add_node_y");
        const ImGuiID addType = ImGui::GetID("##add_node_type");
        const bool addFromCanvas = freeCanvas && ImGui::IsWindowHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Right) &&
            !ImGui::IsMouseDragPastThreshold(ImGuiMouseButton_Right) &&
            canvas.Contains(ImGui::GetIO().MouseClickedPos[ImGuiMouseButton_Right]);
        if (addFromToolbar || addFromCanvas)
        {
            const size_t i = graph.nodes.size();
            const size_t columns = material.x - origin.x >= Z(404.f) ? 2u : 1u;
            const ImVec2 mouse = view.ToWorld(ImGui::GetMousePos());
            // Custom materials keep their shader source cards at the top left.
            const float firstY = custom ? 14.f + IM_ARRAYSIZE(kShaderInputs) * (stepWorld * 3.2f + kInsetBase) : 14.f;
            constexpr float limit = assets::kMaterialGraphCoordinateLimit;
            storage->SetFloat(addX, addFromCanvas ? std::clamp(graph_canvas::Snap(mouse.x), -limit, limit)
                : 14.f + static_cast<float>(i % columns) * 195.f);
            storage->SetFloat(addY, addFromCanvas ? std::clamp(graph_canvas::Snap(mouse.y), -limit, limit)
                : firstY + static_cast<float>(i / columns) * stepWorld * 4.2f);
            ImGui::OpenPopup("##add_material_node");
        }
        if (ImGui::BeginPopup("##add_material_node"))
        {
            constexpr MaterialNodeType types[] = {
                MaterialNodeType::Texture, MaterialNodeType::Value, MaterialNodeType::Color, MaterialNodeType::Multiply,
                MaterialNodeType::Mix, MaterialNodeType::Separate, MaterialNodeType::Time, MaterialNodeType::Parameter,
                MaterialNodeType::UV, MaterialNodeType::Position, MaterialNodeType::Vector2,
            };
            const char* labels[IM_ARRAYSIZE(types)];
            for (int i = 0; i < IM_ARRAYSIZE(types); ++i)
                labels[i] = NodeLabel(types[i]);
            // Custom shaders can only use Texture nodes, which come first in the list.
            const int typeCount = custom ? 1 : IM_ARRAYSIZE(labels);
            int type = std::min(storage->GetInt(addType), typeCount - 1);
            ImGui::TextUnformatted(Translate("material.graph_add"));
            ImGui::SetNextItemWidth(kNodeWidth);
            if (ImGui::Combo("##node_type", &type, labels, typeCount))
                storage->SetInt(addType, type);
            ImGui::BeginDisabled(!canAdd);
            if (ImGui::Button(Translate("material.graph_create")))
            {
                AddNode(graph, types[type], {storage->GetFloat(addX), storage->GetFloat(addY)});
                changed = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
            ImGui::EndPopup();
        }

        (void)extentX;
        (void)extentY;
        ImGui::PopStyleVar(4);
        ImGui::PopFont();
        g_zoom = 1.f;
        kNodeWidth = kNodeWidthBase;
        kInset = kInsetBase;
        ImGui::SetCursorScreenPos(origin);
        ImGui::Dummy(ImVec2(std::max(1.f, available.x), std::max(1.f, available.y)));
    }
    ImGui::EndChild();
    return changed;
}

} // namespace engine::renderer::addons::editor

#endif
