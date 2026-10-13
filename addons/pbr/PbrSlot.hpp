#pragma once

#include "renderer/RendererTypes.hpp"
#include "PbrChannelInput.hpp"  // MaterialChannel enum
#include <string>

namespace engine::renderer::pbr {

// ---------------------------------------------------------------------------
// PbrSlotValue
// Represents the value for one PBR slot: either a scalar/color constant
// or a texture source (with channel selection, scale and bias).
//
// channel = -1 is the sentinel for "constant mode" in the KROM_CHANNEL_MAP
// shader path — the scalar factor is used directly without sampling.
// ---------------------------------------------------------------------------
struct PbrSlotValue
{
    enum class Mode : uint8_t { Constant, Texture };

    Mode            mode     = Mode::Constant;
    math::Vec4      constant = {1.f, 1.f, 1.f, 1.f};
    TextureHandle   texture;
    MaterialChannel channel  = MaterialChannel::R;
    float           scale    = 1.0f;
    float           bias     = 0.0f;

    [[nodiscard]] static PbrSlotValue FromConstant(float v) noexcept
    {
        PbrSlotValue s;
        s.mode = Mode::Constant;
        s.constant = {v, 0.f, 0.f, 1.f};
        return s;
    }

    [[nodiscard]] static PbrSlotValue FromConstant(math::Vec4 v) noexcept
    {
        PbrSlotValue s;
        s.mode = Mode::Constant;
        s.constant = v;
        return s;
    }

    [[nodiscard]] static PbrSlotValue FromTexture(TextureHandle t,
                                                   MaterialChannel ch = MaterialChannel::R,
                                                   float sc = 1.f,
                                                   float b  = 0.f) noexcept
    {
        PbrSlotValue s;
        s.mode    = Mode::Texture;
        s.texture = t;
        s.channel = ch;
        s.scale   = sc;
        s.bias    = b;
        return s;
    }

    [[nodiscard]] bool HasTexture() const noexcept
    {
        return mode == Mode::Texture && texture.IsValid();
    }
};

// ---------------------------------------------------------------------------
// PbrSlotDesc
// Metadata per slot — used by the editor for introspection.
// ---------------------------------------------------------------------------
struct PbrSlotDesc
{
    enum class DataType : uint8_t { Float, Vec3, Vec4 };

    std::string id;           // internal id:   "roughness"
    std::string displayName;  // editor label:  "Roughness"
    DataType    dataType       = DataType::Float;
    bool        acceptsTexture = true;
    math::Vec4  defaultValue   = {1.f, 1.f, 1.f, 1.f};
    math::Vec4  minValue       = {0.f, 0.f, 0.f, 0.f};
    math::Vec4  maxValue       = {1.f, 1.f, 1.f, 1.f};
};

} // namespace engine::renderer::pbr
