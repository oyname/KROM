#pragma once
// =============================================================================
// KROM Engine - addons/forward/ForwardSceneBindings.hpp
//
// Frame data through which other add-ons hand textures to the forward scene
// passes (opaque, transparent). An add-on fills the list in its scene
// extraction step; the forward pipeline binds each entry for the fragment
// stage before drawing, without knowing the add-on. An entry with an invalid
// texture clears its slot; add-ons publish their slots every frame.
// =============================================================================
#include "renderer/RendererTypes.hpp"
#include <array>
#include <cstdint>

namespace engine::renderer::addons::forward {

struct ForwardSceneTextureBinding
{
    uint32_t      slot = 0u;           // TexSlots value, e.g. TexSlots::FeatureCube0
    TextureHandle texture = TextureHandle::Invalid();
};

struct ForwardSceneBindings
{
    static constexpr const char* kFrameDataName = "forward.scene_bindings";
    static constexpr uint32_t kMaxTextures = 4u;

    std::array<ForwardSceneTextureBinding, kMaxTextures> textures{};
    uint32_t textureCount = 0u;

    // Sets the texture for a slot, replacing an earlier entry for the same slot.
    bool SetTexture(uint32_t slot, TextureHandle texture) noexcept
    {
        for (uint32_t i = 0u; i < textureCount; ++i)
        {
            if (textures[i].slot == slot)
            {
                textures[i].texture = texture;
                return true;
            }
        }
        if (textureCount >= kMaxTextures)
            return false;
        textures[textureCount++] = ForwardSceneTextureBinding{ slot, texture };
        return true;
    }

    void Clear() noexcept { textureCount = 0u; }
};

} // namespace engine::renderer::addons::forward
