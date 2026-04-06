#include "DX11Device.hpp"
#include "core/Debug.hpp"

namespace engine::renderer::dx11 {
namespace {

// Stub-AutoRegister: registriert keinen echten Factory-Eintrag.
// Gibt stattdessen eine Warnung aus, dass DX11 auf dieser Plattform nicht verfügbar ist.
// Core muss den Stub nicht kennen — er wird durch Linken aktiviert.
struct AutoRegister
{
    AutoRegister()
    {
        Debug::LogWarning("DX11 backend stub active: DirectX11 unavailable on this platform/build.");
    }
};
static AutoRegister s_autoRegister;

} // namespace
} // namespace engine::renderer::dx11
