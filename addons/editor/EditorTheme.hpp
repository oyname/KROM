#pragma once
// =============================================================================
// KROM Engine - addons/editor/EditorTheme.hpp
// Farbpalette und Grundstil des Editors. Lokale Farbwechsel in den Panels
// nehmen ihre Werte von hier, damit der Editor einheitlich bleibt.
// =============================================================================

#ifdef KROM_EDITOR_HAS_IMGUI

#include "imgui.h"

#include <cmath>

namespace engine::renderer::addons::editor::theme {

// Farbwert aus 0xRRGGBB (sRGB, wie in der Palette), optional mit Deckkraft.
// Die UI zeichnet in einen sRGB-Backbuffer (DX11/Vulkan/OpenGL), der beim
// Schreiben wieder nach sRGB umrechnet; daher hier nach linear wandeln.
inline float SrgbToLinear(float c)
{
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

inline ImVec4 Hex(unsigned rgb, float alpha = 1.f)
{
    return ImVec4(SrgbToLinear(static_cast<float>((rgb >> 16) & 0xFF) / 255.f),
                  SrgbToLinear(static_cast<float>((rgb >> 8) & 0xFF) / 255.f),
                  SrgbToLinear(static_cast<float>(rgb & 0xFF) / 255.f),
                  alpha);
}

inline ImVec4 kWindowBg      = Hex(0x12171D); // Fensterhintergrund
inline ImVec4 kPanel         = Hex(0x1B222A); // Panel / Popup / Menueeintrag
inline ImVec4 kField         = Hex(0x0E1318); // Eingabefelder (dunkler als Panel)
inline ImVec4 kBorder        = Hex(0x35404B); // Rahmen / Trennlinie
inline ImVec4 kButton        = Hex(0x29323C); // Normaler Button
inline ImVec4 kButtonHover   = Hex(0x364453);
inline ImVec4 kButtonActive  = Hex(0x1D6F9D);
inline ImVec4 kAccent        = Hex(0x169BD5); // Primaerer Button
inline ImVec4 kAccentHover   = Hex(0x24B5F0);
inline ImVec4 kMenuHover     = Hex(0x29485C);
inline ImVec4 kMenuOpen      = Hex(0x147BA6); // Aktiv / geoeffnet
inline ImVec4 kSelected      = Hex(0x176A8C); // Ausgewaehlter Eintrag
inline ImVec4 kText          = Hex(0xE5EBF1);
inline ImVec4 kTextSecondary = Hex(0x9AA6B2);
inline ImVec4 kTextDisabled  = Hex(0x77828D);
inline ImVec4 kSuccess       = Hex(0x3BBF7A); // Erfolg / Play
inline ImVec4 kWarning       = Hex(0xE8A33A);
inline ImVec4 kDanger        = Hex(0xD9535F); // Fehler / Loeschen

inline ImU32 U32(const ImVec4& c) { return ImGui::ColorConvertFloat4ToU32(c); }

// Setzt Farben und Abstaende des aktuellen ImGui-Kontexts. Einmal nach
// ImGui::CreateContext aufrufen.
void ApplyEditorTheme();
void RefreshEditorTheme();

} // namespace engine::renderer::addons::editor::theme

#endif // KROM_EDITOR_HAS_IMGUI
