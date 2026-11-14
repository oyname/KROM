#pragma once
// =============================================================================
// KROM Engine - addons/editor/EditorIcons.hpp
// Symbole aus der Icon-Schrift Lucide (assets/fonts/lucide.ttf, ISC-Lizenz,
// lucide-static 0.460.0). Die Schrift ist beim Start in die UI-Schrift
// eingemischt, die Konstanten sind daher normale UTF-8-Texte:
//   ImGui::MenuItem(ICON_LC_SAVE "  Speichern");
// Codepoints stammen aus lucide.css desselben Pakets.
// =============================================================================

#define ICON_LC_FIRST 0xE038
#define ICON_LC_LAST  0xE63A

#define ICON_LC_ACTIVITY        "\xEE\x80\xB8" // U+E038
#define ICON_LC_ATOM            "\xEE\x8F\x9B" // U+E3DB
#define ICON_LC_BOOK_OPEN       "\xEE\x81\xA3" // U+E063
#define ICON_LC_BOX             "\xEE\x81\xA5" // U+E065
#define ICON_LC_CAMERA          "\xEE\x81\xA8" // U+E068
#define ICON_LC_CHEVRON_DOWN    "\xEE\x81\xB1" // U+E071
#define ICON_LC_CHEVRON_RIGHT   "\xEE\x81\xB3" // U+E073
#define ICON_LC_CIRCLE_HELP     "\xEE\x82\x82" // U+E082
#define ICON_LC_CLOUD           "\xEE\x82\x8C" // U+E08C
#define ICON_LC_CODE            "\xEE\x82\x97" // U+E097
#define ICON_LC_CONTRAST        "\xEE\x82\xA1" // U+E0A1
#define ICON_LC_COPY            "\xEE\x82\xA2" // U+E0A2
#define ICON_LC_CUBOID          "\xEE\x94\xA9" // U+E529
#define ICON_LC_EAR             "\xEE\x8E\x86" // U+E386
#define ICON_LC_EARTH          "\xEE\x87\xB3" // U+E1F3
#define ICON_LC_EYE             "\xEE\x82\xBE" // U+E0BE
#define ICON_LC_FILE            "\xEE\x83\x84" // U+E0C4
#define ICON_LC_FILE_CODE       "\xEE\x83\x87" // U+E0C7
#define ICON_LC_FILE_PLUS       "\xEE\x83\x8D" // U+E0CD
#define ICON_LC_FILTER          "\xEE\x83\x95" // U+E0D5
#define ICON_LC_FLASHLIGHT      "\xEE\x83\x98" // U+E0D8
#define ICON_LC_FOLDER          "\xEE\x83\x9C" // U+E0DC
#define ICON_LC_FOLDER_OPEN     "\xEE\x89\x87" // U+E247
#define ICON_LC_GLOBE           "\xEE\x83\xAB" // U+E0EB
#define ICON_LC_GRID_3X3        "\xEE\x83\xAC" // U+E0EC
#define ICON_LC_IMAGE           "\xEE\x83\xB9" // U+E0F9
#define ICON_LC_INFO            "\xEE\x83\xBE" // U+E0FE
#define ICON_LC_LAYERS          "\xEE\x84\x84" // U+E104
#define ICON_LC_LAYOUT_GRID     "\xEE\x84\x85" // U+E105
#define ICON_LC_LAYOUT_TEMPLATE "\xEE\x88\x87" // U+E207
#define ICON_LC_LIGHTBULB       "\xEE\x87\x82" // U+E1C2
#define ICON_LC_LIST_TREE       "\xEE\x90\x8D" // U+E40D
#define ICON_LC_LOCATE_FIXED   "\xEE\x87\x9B" // U+E1DB
#define ICON_LC_LOCK            "\xEE\x84\x8F" // U+E10F
#define ICON_LC_LOCK_OPEN       "\xEE\x84\x90" // U+E110
#define ICON_LC_LOG_OUT         "\xEE\x84\x92" // U+E112
#define ICON_LC_MAGNET          "\xEE\x8A\xB5" // U+E2B5
#define ICON_LC_MAXIMIZE_2      "\xEE\x84\x97" // U+E117
#define ICON_LC_MONITOR         "\xEE\x84\xA1" // U+E121
#define ICON_LC_MOUSE_POINTER_2 "\xEE\x87\x83" // U+E1C3
#define ICON_LC_MOVE            "\xEE\x84\xA5" // U+E125
#define ICON_LC_MOVE_3D         "\xEE\x8B\xA5" // U+E2E5
#define ICON_LC_PACKAGE         "\xEE\x84\xAD" // U+E12D
#define ICON_LC_PALETTE         "\xEE\x87\x9D" // U+E1DD
#define ICON_LC_PAUSE           "\xEE\x84\xB2" // U+E132
#define ICON_LC_PERSON_STANDING "\xEE\x88\x9E" // U+E21E
#define ICON_LC_PLAY            "\xEE\x85\x80" // U+E140
#define ICON_LC_PLUS            "\xEE\x85\x81" // U+E141
#define ICON_LC_REDO_2          "\xEE\x8A\xA0" // U+E2A0
#define ICON_LC_REFRESH_CW      "\xEE\x85\x89" // U+E149
#define ICON_LC_ROTATE_CW       "\xEE\x85\x8D" // U+E14D
#define ICON_LC_SAVE            "\xEE\x85\x91" // U+E151
#define ICON_LC_SCALING         "\xEE\x8B\xAC" // U+E2EC
#define ICON_LC_SCAN_EYE        "\xEE\x94\xBB" // U+E53B
#define ICON_LC_SEARCH          "\xEE\x85\x95" // U+E155
#define ICON_LC_SETTINGS        "\xEE\x85\x98" // U+E158
#define ICON_LC_SPARKLES        "\xEE\x90\x97" // U+E417
#define ICON_LC_SQUARE          "\xEE\x85\xAB" // U+E16B
#define ICON_LC_SQUARE_DASHED   "\xEE\x87\x8B" // U+E1CB
#define ICON_LC_SUN             "\xEE\x85\xBC" // U+E17C
#define ICON_LC_TERMINAL        "\xEE\x86\x85" // U+E185
#define ICON_LC_TRASH_2         "\xEE\x86\x8E" // U+E18E
#define ICON_LC_UNDO_2          "\xEE\x8A\xA1" // U+E2A1
#define ICON_LC_VIDEO           "\xEE\x86\xA5" // U+E1A5
#define ICON_LC_VOLUME_2        "\xEE\x86\xAB" // U+E1AB
#define ICON_LC_WEIGHT          "\xEE\x94\xB5" // U+E535
#define ICON_LC_WRENCH          "\xEE\x86\xB1" // U+E1B1
#define ICON_LC_X               "\xEE\x86\xB2" // U+E1B2

#ifdef KROM_EDITOR_HAS_IMGUI

#include "imgui.h"

namespace engine::renderer::addons::editor {

// Zeichnet ein Symbol mittig um `center` mit der Hoehe `size` (Pixel).
inline void DrawIcon(ImDrawList* dl, const char* icon, const ImVec2& center, float size, ImU32 color)
{
    ImFont* font = ImGui::GetFont();
    const ImVec2 extent = font->CalcTextSizeA(size, FLT_MAX, 0.f, icon);
    dl->AddText(font, size, ImVec2(center.x - extent.x * 0.5f, center.y - extent.y * 0.5f), color, icon);
}

} // namespace engine::renderer::addons::editor

#endif // KROM_EDITOR_HAS_IMGUI
