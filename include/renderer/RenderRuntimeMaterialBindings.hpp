#pragma once

#include "renderer/RendererTypes.hpp"

namespace engine::renderer {

class MaterialSystem;
class ShaderRuntime;

struct FrameGraphMaterialBindings
{
    ShaderRuntime* shaderRuntime = nullptr;
    const MaterialSystem* materials = nullptr;
    MaterialHandle defaultTonemapMaterial;
    const MaterialSystem* tonemapMaterialSystem = nullptr;
};

} // namespace engine::renderer
