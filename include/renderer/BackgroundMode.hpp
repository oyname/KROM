#pragma once

#include <cstdint>

namespace engine {

enum class BackgroundMode : uint8_t
{
    ClearColor   = 0,  // feste Farbe (clearColor der Kamera)
    Skybox       = 1,  // Environment-Cubemap sichtbar rendern
    Transparent  = 2,  // Alpha 0 — fuer RTT/Compositing
    KeepPrevious = 3,  // Framebuffer nicht leeren (Overlay, Editor-Spezialfaelle)

    Solid        = 0,  // Rueckwaertskompatibilitaets-Alias fuer ClearColor
};

} // namespace engine
