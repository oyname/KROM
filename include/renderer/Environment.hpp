#pragma once
#include "core/Types.hpp"

namespace engine::renderer {

// Environment-Vertrag:
// - sourceTexture ist die geladene 2D-HDR-Quelle im equirectangular Layout.
// - EnvironmentSystem bleibt alleiniger Besitzer der daraus abgeleiteten GPU-Ressourcen.
// - Aus der Quelle werden diffuse irradiance, specular prefilter und BRDF-LUT
//   backendneutral erzeugt und an den Runtime-Pfad gebunden.
struct EnvironmentDesc
{
    TextureHandle sourceTexture = TextureHandle::Invalid();
    float intensity = 1.0f;
    bool enableIBL = true;
};

struct EnvironmentHandle
{
    uint32_t id = 0u;
    [[nodiscard]] bool IsValid() const noexcept { return id != 0u; }
    static EnvironmentHandle Invalid() noexcept { return {}; }
    bool operator==(const EnvironmentHandle&) const noexcept = default;
};

// ShaderRuntime konsumiert diesen Zustand nur noch als Binder/Fallback-Schicht.
// irradiance und prefiltered bleiben getrennte Runtime-Felder fuer einen sauberen
// API-neutralen IBL-Vertrag ueber DX11, OpenGL, Vulkan und spaeter DX12 hinweg.
struct EnvironmentRuntimeState
{
    TextureHandle irradiance = TextureHandle::Invalid();
    TextureHandle prefiltered = TextureHandle::Invalid();
    TextureHandle brdfLut = TextureHandle::Invalid();
    float intensity = 1.0f;
    bool active = false;
};

} // namespace engine::renderer
