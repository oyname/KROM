// =============================================================================
// KROM Engine - addons/editor/EditorTheme.cpp
// =============================================================================
#include "addons/editor/EditorTheme.hpp"
#include "addons/editor/EditorPreferences.hpp"

#ifdef KROM_EDITOR_HAS_IMGUI

namespace ImGui {
// Editor opt-in implemented in the vendored imgui_widgets.cpp.
void SetKromTabAppearance(const ImVec4& topTint, const ImVec4& hoverTint,
                          const ImVec4& selectedText, const ImVec4& unselectedText);
}

namespace engine::renderer::addons::editor::theme {

static bool appliedLightTheme = false;

void ApplyEditorTheme()
{
    ImGuiStyle& style = ImGui::GetStyle();
    const auto& preferences = GetEditorPreferences();
    appliedLightTheme = preferences.lightTheme;
    const auto color = [](unsigned dark, unsigned light) { return Hex(appliedLightTheme ? light : dark); };
    kWindowBg = color(0x12171D, 0xEDF1F5);
    kPanel = color(0x1B222A, 0xF8FAFC);
    kField = color(0x0E1318, 0xFFFFFF);
    kBorder = color(0x35404B, 0xA8B6C5);
    kButton = color(0x29323C, 0xDCE7F0);
    kButtonHover = color(0x364453, 0xC5DBEC);
    kButtonActive = color(0x1D6F9D, 0x9FC7E1);
    kAccent = color(0x169BD5, 0xA2D1EA);
    kAccentHover = color(0x24B5F0, 0x71B8DB);
    kMenuHover = color(0x29485C, 0xD7E8F4);
    kMenuOpen = color(0x147BA6, 0xA9D1EA);
    kSelected = color(0x176A8C, 0xC3DFF0);
    kText = color(0xE5EBF1, 0x192938);
    kTextSecondary = color(0x9AA6B2, 0x435669);
    kTextDisabled = color(0x77828D, 0x5D7082);
    kSuccess = color(0x3BBF7A, 0x288557);
    kWarning = color(0xE8A33A, 0x96600C);
    kDanger = color(0xD9535F, 0xCA4452);
    const float dpi = style.FontScaleDpi;
    style = ImGuiStyle{};
    style.FontSizeBase = preferences.fontSize;
    style.FontScaleDpi = dpi;
    style.FontScaleMain = preferences.uiScale;
    if (appliedLightTheme)
        ImGui::StyleColorsLight(&style);
    else
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
    style.TabBarBorderSize  = 0.f;
    style.TabBarOverlineSize = 2.f; // Drawn at the bottom by the editor tab style.
    style.SeparatorTextBorderSize = 1.f;

    // The tint alpha is the blend weight for the tab's base color.
    const ImVec4 tabTopTint = appliedLightTheme ? Hex(0xF4F9FD, 0.94f) : Hex(0x0D141C, 0.98f);
    const ImVec4 tabHoverTint = Hex(0xFFFFFF, appliedLightTheme ? 0.20f : 0.025f);
    const ImVec4 tabSelectedText = color(0xFFFFFF, 0x192938);
    const ImVec4 tabUnselectedText = color(0xCCD8E3, 0x435669);
    const ImVec4 tabUnderline = color(0x91D6FF, 0xE4F6FF);
    const ImVec4 tabDimmedUnderline = color(0x7793A6, 0xDBE5ED);
    ImGui::SetKromTabAppearance(tabTopTint, tabHoverTint, tabSelectedText, tabUnselectedText);

    ImVec4* c = style.Colors;
    c[ImGuiCol_Text]                 = kText;
    c[ImGuiCol_TextDisabled]         = kTextDisabled;
    c[ImGuiCol_WindowBg]             = kWindowBg;
    c[ImGuiCol_ChildBg]              = Hex(0x000000, 0.f);
    c[ImGuiCol_PopupBg]              = kPanel;
    c[ImGuiCol_Border]               = kBorder;
    c[ImGuiCol_BorderShadow]         = Hex(0x000000, 0.f);
    c[ImGuiCol_FrameBg]              = kField;
    c[ImGuiCol_FrameBgHovered]       = color(0x1A2530, 0xDFECF6);
    c[ImGuiCol_FrameBgActive]        = color(0x1E3342, 0xC8E0F0);
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
    c[ImGuiCol_Tab]                  = color(0x2D4053, 0xDCE5ED);
    c[ImGuiCol_TabHovered]           = kMenuHover;
    c[ImGuiCol_TabSelected]          = kAccent;
    c[ImGuiCol_TabSelectedOverline]  = tabUnderline;
    c[ImGuiCol_TabDimmed]            = color(0x26384A, 0xD5DEE7);
    c[ImGuiCol_TabDimmedSelected]    = color(0x456881, 0xB9C9D7);
    c[ImGuiCol_TabDimmedSelectedOverline] = tabDimmedUnderline;
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
    c[ImGuiCol_TextLink]             = color(0x24B5F0, 0x126A9A);
    c[ImGuiCol_TextSelectedBg]       = Hex(0x147BA6, 0.55f);
    c[ImGuiCol_TreeLines]            = kBorder;
    c[ImGuiCol_DragDropTarget]       = kAccentHover;
    c[ImGuiCol_DragDropTargetBg]     = Hex(0x169BD5, 0.15f);
    c[ImGuiCol_UnsavedMarker]        = kText;
    c[ImGuiCol_NavCursor]            = color(0x24B5F0, 0x1673A3);
    c[ImGuiCol_NavWindowingHighlight] = Hex(0xFFFFFF, 0.70f);
    c[ImGuiCol_NavWindowingDimBg]    = Hex(0x000000, 0.45f);
    c[ImGuiCol_ModalWindowDimBg]     = Hex(0x000000, 0.55f);
    style.ScaleAllSizes(preferences.uiScale);
}

void RefreshEditorTheme()
{
    const auto& preferences = GetEditorPreferences();
    if (preferences.lightTheme != appliedLightTheme || ImGui::GetStyle().FontScaleMain != preferences.uiScale ||
        ImGui::GetStyle().FontSizeBase != preferences.fontSize)
        ApplyEditorTheme();
}

} // namespace engine::renderer::addons::editor::theme

#endif // KROM_EDITOR_HAS_IMGUI
