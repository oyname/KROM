#pragma once

// Compatibility header for shader-reflection code. The owning parameter-layout
// definition is material-facing and lives in MaterialParameterLayout.hpp.
#include "renderer/MaterialParameterLayout.hpp"

namespace engine::renderer {

using ParameterType = MaterialParameterType;
using ParameterSlot = MaterialParameterSlot;
using ShaderParameterLayout = MaterialParameterLayout;

[[nodiscard]] uint64_t HashShaderParameterLayout(const ShaderParameterLayout& layout) noexcept;

} // namespace engine::renderer
