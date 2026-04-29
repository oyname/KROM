#pragma once

#include "renderer/RendererTypes.hpp"
#include <cstdint>

namespace engine::renderer::pbr {

enum class MaterialChannel : uint32_t
{
    R = 0,
    G = 1,
    B = 2,
    A = 3,
};

struct PbrScalarTextureInput
{
    TextureHandle   texture;
    MaterialChannel channel = MaterialChannel::R;
    float           factor  = 1.0f;
    float           bias    = 0.0f;

    [[nodiscard]] bool HasTexture() const noexcept { return texture.IsValid(); }
};

} // namespace engine::renderer::pbr
