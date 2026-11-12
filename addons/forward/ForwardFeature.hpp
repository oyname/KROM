#pragma once

#include "renderer/RenderFeatureInterfaces.hpp"
#include "addons/forward/ForwardTonemap.hpp"
#include "addons/forward/ForwardAntiAliasing.hpp"
#include <array>
#include <cstdint>
#include <memory>

namespace engine::renderer::addons::forward {

enum class ForwardRendererMode : uint8_t
{
    Forward = 0,
    ForwardPlus,
};

enum class ForwardPlusCullingMode : uint8_t
{
    CPU = 0,    // CPU-seitiges Tile-Culling (aktuell implementiert)
    Compute,    // GPU Compute Shader Culling (vorbereitet, noch nicht implementiert)
};

struct ForwardFeatureConfig
{
    std::array<float, 4>    clearColorValue          = { 0.3f, 0.3f, 0.3f, 1.f };
    bool                    enableEnvironmentBackground = false;
    bool                    enableBloom              = false;
    float                   bloomThreshold           = 1.0f;
    float                   bloomIntensity           = 0.08f;
    float                   bloomBlurRadius          = 1.0f;
    ForwardRendererMode     mode                     = ForwardRendererMode::Forward;
    ForwardPlusCullingMode  cullingMode              = ForwardPlusCullingMode::CPU;
    // Tonemap operator and exposure. Shared so the owner (app, editor, game) can
    // change them at any time; read every frame by the tonemap pass. Null: the
    // feature keeps its own default settings.
    std::shared_ptr<TonemapSettings> tonemapSettings;
    // MSAA for the scene passes (Sky, Opaque, Transparent). Shared like the tonemap
    // settings; a change rebuilds targets and graph on the next frame. Not available
    // on OpenGL (stays at 1). Null: the feature keeps its own setting (off).
    std::shared_ptr<MsaaSettings> msaaSettings;
};

std::unique_ptr<IEngineFeature> CreateForwardFeature(ForwardFeatureConfig config = {});

} // namespace engine::renderer::addons::forward
