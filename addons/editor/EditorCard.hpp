#pragma once
// Shared look of the inspector: sections drawn as cards with a header (fold
// triangle, coloured icon, title), and the save button with its disk icon.
// Editors outside the inspector use these so all windows follow one design.

#ifdef KROM_EDITOR_HAS_IMGUI

#include "addons/editor/EditorIcons.hpp"
#include "addons/editor/EditorTheme.hpp"
#include "imgui.h"

#include <algorithm>
#include <string>

namespace engine::renderer::addons::editor::card {

inline constexpr float kInset = 7.f;

struct Card
{
    ImDrawListSplitter splitter;
    ImDrawList* drawList = nullptr;
    ImVec2 min{};
    float right = 0.f;
    float headerBottom = 0.f;
    bool open = false;
};

// Starts a card. `width` <= 0 takes the available width. Returns whether the
// card is open; End() must be called either way.
inline bool Begin(Card& card, const char* id, const char* title, const char* glyph, ImU32 iconColor,
                  float width = 0.f, bool defaultOpen = true)
{
    ImGui::Dummy(ImVec2(0.f, 4.f));
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float contentWidth = width > 0.f ? width : ImGui::GetContentRegionAvail().x;
    card.drawList = ImGui::GetWindowDrawList();
    card.min = ImVec2(start.x - 2.f, start.y);
    card.right = start.x + contentWidth + 3.f;
    card.splitter.Split(card.drawList, 2);
    card.splitter.SetCurrentChannel(card.drawList, 1);

    ImGui::PushID(id);
    bool& open = *ImGui::GetStateStorage()->GetBoolRef(ImGui::GetID("##card_open"), defaultOpen);
    const float headerHeight = ImGui::GetFontSize() + 14.f;
    ImGui::SetCursorScreenPos(ImVec2(start.x + kInset, start.y + kInset * 0.5f));
    if (ImGui::InvisibleButton("##card_header", ImVec2(std::max(1.f, contentWidth - kInset), headerHeight)))
        open = !open;
    const bool hovered = ImGui::IsItemHovered();
    ImGui::PopID();

    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    const float mid = (min.y + max.y) * 0.5f;
    const ImU32 textColor = ImGui::GetColorU32(ImGuiCol_Text);
    ImDrawList* dl = card.drawList;
    dl->AddRectFilled(min, max, ImGui::GetColorU32(hovered ? theme::kMenuHover : theme::kPanel),
                      ImGui::GetStyle().FrameRounding);
    if (open)
        dl->AddTriangleFilled(ImVec2(min.x + 10, mid - 3), ImVec2(min.x + 18, mid - 3), ImVec2(min.x + 14, mid + 3), textColor);
    else
        dl->AddTriangleFilled(ImVec2(min.x + 11, mid - 4), ImVec2(min.x + 11, mid + 4), ImVec2(min.x + 17, mid), textColor);
    float textX = min.x + 26.f;
    if (glyph)
    {
        DrawIcon(dl, glyph, ImVec2(min.x + 33.f, mid), 17.f, iconColor);
        textX = min.x + 49.f;
    }
    dl->PushClipRect(min, max, true);
    dl->AddText(ImVec2(textX, mid - ImGui::GetFontSize() * 0.5f), textColor, title);
    dl->PopClipRect();

    card.headerBottom = max.y;
    card.open = open;
    ImGui::SetCursorScreenPos(ImVec2(start.x, max.y));
    ImGui::Dummy(ImVec2(0.f, 2.f));
    if (open)
        ImGui::Indent(kInset);
    return open;
}

inline void End(Card& card)
{
    float bottom = card.headerBottom + kInset * 0.5f;
    if (card.open)
    {
        ImGui::Dummy(ImVec2(0.f, 2.f));
        ImGui::Unindent(kInset);
        bottom = ImGui::GetCursorScreenPos().y - ImGui::GetStyle().ItemSpacing.y + 2.f;
    }
    card.splitter.SetCurrentChannel(card.drawList, 0);
    ImVec4 fill = ImGui::GetStyleColorVec4(ImGuiCol_FrameBg);
    fill.w *= 0.45f;
    const ImVec2 max(card.right, bottom);
    card.drawList->AddRectFilled(card.min, max, ImGui::GetColorU32(fill), 5.f);
    card.drawList->AddRect(card.min, max, ImGui::GetColorU32(theme::kBorder), 5.f);
    card.splitter.Merge(card.drawList);
}

// The one save button of all editors: disk icon and label.
inline bool SaveButton(const std::string& label)
{
    return ImGui::Button((std::string(ICON_LC_SAVE "  ") + label).c_str());
}

} // namespace engine::renderer::addons::editor::card

#endif // KROM_EDITOR_HAS_IMGUI
