// =============================================================================
// KROM Engine - addons/editor/EditorTheme.cpp
// =============================================================================
#include "addons/editor/EditorTheme.hpp"

#ifdef KROM_EDITOR_HAS_IMGUI

namespace engine::renderer::addons::editor::theme {

void ApplyEditorTheme()
{
    ImGuiStyle& style = ImGui::GetStyle();
    ImGui::StyleColorsDark(&style);

    style.WindowPadding     = ImVec2(10.f, 10.f);
    style.FramePadding      = ImVec2(8.f, 5.f);
    style.ItemSpacing       = ImVec2(8.f, 6.f);
    style.ItemInnerSpacing  = ImVec2(6.f, 4.f);
    style.IndentSpacing     = 18.f;
    style.ScrollbarSize     = 12.f;
    style.GrabMinSize       = 10.f;
    style.WindowRounding    = 6.f;
    style.ChildRounding     = 4.f;
    style.FrameRounding     = 4.f;
    style.PopupRounding     = 6.f;
    style.ScrollbarRounding = 6.f;
    style.GrabRounding      = 4.f;
    style.TabRounding       = 4.f;
    style.WindowBorderSize  = 1.f;
    style.ChildBorderSize   = 1.f;
    style.PopupBorderSize   = 1.f;
    style.FrameBorderSize   = 1.f;
    style.TabBorderSize     = 0.f;
    style.SeparatorTextBorderSize = 1.f;

    ImVec4* c = style.Colors;
    c[ImGuiCol_Text]                 = kText;
    c[ImGuiCol_TextDisabled]         = kTextDisabled;
    c[ImGuiCol_WindowBg]             = kWindowBg;
    c[ImGuiCol_ChildBg]              = Hex(0x000000, 0.f);
    c[ImGuiCol_PopupBg]              = kPanel;
    c[ImGuiCol_Border]               = kBorder;
    c[ImGuiCol_BorderShadow]         = Hex(0x000000, 0.f);
    c[ImGuiCol_FrameBg]              = kField;
    c[ImGuiCol_FrameBgHovered]       = Hex(0x1A2530);
    c[ImGuiCol_FrameBgActive]        = Hex(0x1E3342);
    c[ImGuiCol_TitleBg]              = kWindowBg;
    c[ImGuiCol_TitleBgActive]        = kWindowBg;
    c[ImGuiCol_TitleBgCollapsed]     = kWindowBg;
    c[ImGuiCol_MenuBarBg]            = kWindowBg;
    c[ImGuiCol_ScrollbarBg]          = Hex(0x000000, 0.f);
    c[ImGuiCol_ScrollbarGrab]        = kButton;
    c[ImGuiCol_ScrollbarGrabHovered] = kButtonHover;
    c[ImGuiCol_ScrollbarGrabActive]  = kButtonActive;
    c[ImGuiCol_CheckMark]            = kText;
    c[ImGuiCol_CheckboxSelectedBg]   = kMenuOpen;
    c[ImGuiCol_SliderGrab]           = kAccent;
    c[ImGuiCol_SliderGrabActive]     = kAccentHover;
    c[ImGuiCol_Button]               = kButton;
    c[ImGuiCol_ButtonHovered]        = kButtonHover;
    c[ImGuiCol_ButtonActive]         = kButtonActive;
    c[ImGuiCol_Header]               = kSelected;
    c[ImGuiCol_HeaderHovered]        = kMenuHover;
    c[ImGuiCol_HeaderActive]         = kMenuOpen;
    c[ImGuiCol_Separator]            = kBorder;
    c[ImGuiCol_SeparatorHovered]     = kAccent;
    c[ImGuiCol_SeparatorActive]      = kAccentHover;
    c[ImGuiCol_ResizeGrip]           = Hex(0x000000, 0.f);
    c[ImGuiCol_ResizeGripHovered]    = kAccent;
    c[ImGuiCol_ResizeGripActive]     = kAccentHover;
    c[ImGuiCol_InputTextCursor]      = kText;
    c[ImGuiCol_Tab]                  = kWindowBg;
    c[ImGuiCol_TabHovered]           = kMenuHover;
    c[ImGuiCol_TabSelected]          = kPanel;
    c[ImGuiCol_TabSelectedOverline]  = kAccent;
    c[ImGuiCol_TabDimmed]            = kWindowBg;
    c[ImGuiCol_TabDimmedSelected]    = kPanel;
    c[ImGuiCol_TabDimmedSelectedOverline] = Hex(0x000000, 0.f);
    c[ImGuiCol_DockingPreview]       = Hex(0x169BD5, 0.55f);
    c[ImGuiCol_DockingEmptyBg]       = kWindowBg;
    c[ImGuiCol_PlotLines]            = kAccent;
    c[ImGuiCol_PlotLinesHovered]     = kAccentHover;
    c[ImGuiCol_PlotHistogram]        = kAccent;
    c[ImGuiCol_PlotHistogramHovered] = kAccentHover;
    c[ImGuiCol_TableHeaderBg]        = kPanel;
    c[ImGuiCol_TableBorderStrong]    = kBorder;
    c[ImGuiCol_TableBorderLight]     = kButton;
    c[ImGuiCol_TableRowBg]           = Hex(0x000000, 0.f);
    c[ImGuiCol_TableRowBgAlt]        = Hex(0xFFFFFF, 0.03f);
    c[ImGuiCol_TextLink]             = kAccentHover;
    c[ImGuiCol_TextSelectedBg]       = Hex(0x147BA6, 0.55f);
    c[ImGuiCol_TreeLines]            = kBorder;
    c[ImGuiCol_DragDropTarget]       = kAccentHover;
    c[ImGuiCol_DragDropTargetBg]     = Hex(0x169BD5, 0.15f);
    c[ImGuiCol_UnsavedMarker]        = kText;
    c[ImGuiCol_NavCursor]            = kAccentHover;
    c[ImGuiCol_NavWindowingHighlight] = Hex(0xFFFFFF, 0.70f);
    c[ImGuiCol_NavWindowingDimBg]    = Hex(0x000000, 0.45f);
    c[ImGuiCol_ModalWindowDimBg]     = Hex(0x000000, 0.55f);
}

} // namespace engine::renderer::addons::editor::theme

#endif // KROM_EDITOR_HAS_IMGUI
