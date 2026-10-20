#pragma once
// =============================================================================
// KROM Engine - addons/script/ITextService.hpp
// Schnittstelle, ueber die Scripts Bildschirmtext zeichnen. Ohne Abhaengigkeit
// zum Text-Addon: die Runtime stellt einen Adapter auf TextRenderer bereit.
// Ohne Text ist ScriptServices::text null und die Helfer sind neutral.
// =============================================================================
#include "core/Math.hpp"

#include <string_view>

namespace engine::script {

class ITextService
{
public:
    virtual ~ITextService() = default;

    // UTF-8-Text fuer diesen Frame; x/y = linke obere Ecke in Pixeln
    // (0,0 = oben links), size = Zeilenhoehe in Pixeln, color in sRGB.
    virtual void Draw(std::string_view text, float x, float y, float size,
                      const math::Vec4& color) = 0;
    // Breite der laengsten Zeile und Gesamthoehe in Pixeln.
    [[nodiscard]] virtual math::Vec2 Measure(std::string_view text, float size) = 0;
    // Groesse des Bildschirms (Backbuffer) in Pixeln.
    [[nodiscard]] virtual math::Vec2 ScreenSize() const = 0;
};

} // namespace engine::script
