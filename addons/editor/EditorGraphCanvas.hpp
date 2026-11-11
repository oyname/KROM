#pragma once
// =============================================================================
// KROM Engine - addons/editor/EditorGraphCanvas.hpp
// Shared canvas of the node editors (controller graph, material graph):
// background, grid (every fifth line stronger), pan with the middle mouse
// button, zoom with the wheel around the mouse, snapping to the grid (Alt
// places freely). Pan and zoom belong to the caller, so each editor keeps its
// own view.
// =============================================================================
#ifdef KROM_EDITOR_HAS_IMGUI

#include "addons/editor/EditorTheme.hpp"
#include "imgui.h"

#include <algorithm>
#include <cmath>

namespace engine::renderer::addons::editor::graph_canvas {

inline constexpr float kGrid = 20.f;

struct View
{
    ImVec2 origin;      // top left of the canvas on screen
    ImVec2 size;
    ImVec2 pan;         // world offset, in world units
    float  zoom = 1.f;

    [[nodiscard]] ImVec2 ToScreen(ImVec2 world) const
    {
        return ImVec2(origin.x + (world.x + pan.x) * zoom, origin.y + (world.y + pan.y) * zoom);
    }
    [[nodiscard]] ImVec2 ToWorld(ImVec2 screen) const
    {
        return ImVec2((screen.x - origin.x) / zoom - pan.x, (screen.y - origin.y) / zoom - pan.y);
    }
    [[nodiscard]] ImVec2 Max() const { return ImVec2(origin.x + size.x, origin.y + size.y); }
};

// A world coordinate on the grid; with Alt held it stays where it is.
[[nodiscard]] inline float Snap(float value)
{
    return ImGui::GetIO().KeyAlt ? value : std::round(value / kGrid) * kGrid;
}

// Applies pan (middle mouse) and zoom (wheel, around the mouse) while `hovered`,
// then draws background and grid into `drawList`. Colors come from the theme
// (linear), so the grid stays faint on every display.
inline View Update(ImDrawList& drawList, ImVec2 origin, ImVec2 size, ImVec2& pan, float& zoom, bool hovered,
                   float minZoom = 0.35f, float maxZoom = 2.5f)
{
    ImGuiIO& io = ImGui::GetIO();
    View view{origin, size, pan, zoom};
    if (hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.f))
    {
        pan.x += io.MouseDelta.x / zoom;
        pan.y += io.MouseDelta.y / zoom;
    }
    if (hovered && io.MouseWheel != 0.f)
    {
        const ImVec2 before = view.ToWorld(io.MousePos);
        zoom = std::clamp(zoom * std::pow(1.12f, io.MouseWheel), minZoom, maxZoom);
        view.zoom = zoom;
        const ImVec2 after = view.ToWorld(io.MousePos);
        pan.x += after.x - before.x;
        pan.y += after.y - before.y;
    }
    view.pan = pan;
    view.zoom = zoom;

    const ImVec2 max = view.Max();
    drawList.AddRectFilled(origin, max, theme::U32(theme::kField));
    const float step = kGrid * zoom;
    const ImU32 minor = theme::U32(theme::Hex(0x35404B, 0.22f));
    const ImU32 major = theme::U32(theme::Hex(0x35404B, 0.55f));
    // Index of the grid line at a screen offset, to make every fifth one stronger.
    const auto lineIndex = [&](float offset, float panAxis) {
        return static_cast<long>(std::lround((offset / zoom - panAxis) / kGrid));
    };
    for (float x = std::fmod(pan.x * zoom, step); x < size.x; x += step)
        drawList.AddLine(ImVec2(origin.x + x, origin.y), ImVec2(origin.x + x, max.y), lineIndex(x, pan.x) % 5 == 0 ? major : minor);
    for (float y = std::fmod(pan.y * zoom, step); y < size.y; y += step)
        drawList.AddLine(ImVec2(origin.x, origin.y + y), ImVec2(max.x, origin.y + y), lineIndex(y, pan.y) % 5 == 0 ? major : minor);
    return view;
}

} // namespace engine::renderer::addons::editor::graph_canvas

#endif // KROM_EDITOR_HAS_IMGUI
