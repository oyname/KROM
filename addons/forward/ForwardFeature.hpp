#pragma once

#include "renderer/RenderFeatureInterfaces.hpp"
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
};

std::unique_ptr<IEngineFeature> CreateForwardFeature(ForwardFeatureConfig config = {});

} // namespace engine::renderer::addons::forward
