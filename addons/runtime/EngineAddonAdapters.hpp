#pragma once

#include "core/IEngineAddon.hpp"

#include <array>
#include <memory>

namespace engine {

[[nodiscard]] std::unique_ptr<IEngineAddon> CreateCameraAddon();
[[nodiscard]] std::unique_ptr<IEngineAddon> CreateLightingAddon();
[[nodiscard]] std::unique_ptr<IEngineAddon> CreateMeshRendererAddon();
[[nodiscard]] std::unique_ptr<IEngineAddon> CreateShadowAddon();
[[nodiscard]] std::unique_ptr<IEngineAddon> CreateForwardAddon(std::array<float, 4> clearColorValue,
                                                               bool enableEnvironmentBackground = false);

} // namespace engine
