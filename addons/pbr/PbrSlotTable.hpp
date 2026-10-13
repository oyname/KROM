#pragma once

#include "PbrSlot.hpp"
#include "renderer/RendererTypes.hpp"

// Internal header — not part of the public addon API.
// Single source of truth for all PBR slot definitions.
// Adding a slot = one row in kSlots, plus one method in PbrInstanceBuilder.

namespace engine::renderer::pbr::detail {

struct SlotDef
{
    // Display metadata (consumed by BuildSlotDescs)
    const char*           id;
    const char*           displayName;
    PbrSlotDesc::DataType dataType;
    bool                  acceptsTexture;
    math::Vec4            defaultValue;
    math::Vec4            minValue;
    math::Vec4            maxValue;
    // Permutation flag set when this slot has a texture
    ShaderVariantFlag     variantFlag;
    // CB / texture binding (consumed by SetSlotValue)
    const char* texParam;           // bind texture here; nullptr = no texture mode
    const char* scaleParam;         // SetFloat in tex mode (nullptr = skip)
    const char* channelParam;       // SetInt in tex mode; -1 sentinel in constant mode
    const char* biasParam;          // SetFloat in tex mode; 0 in constant mode (nullptr = skip)
    const char* constantVec4Param;  // SetVec4 in constant mode (nullptr = skip)
    const char* constantFloatParam; // SetFloat(x) in constant mode (nullptr = skip)
};

//                      id           displayName   dataType                     tex?   default             min               max
//                      variantFlag
//                      texParam        scaleParam         channelParam        biasParam      vec4Param           floatParam
inline constexpr SlotDef kSlots[] = {
    { "baseColor", "Base Color", PbrSlotDesc::DataType::Vec4,  true, {1,1,1,1},    {0,0,0,0}, {1,1,1,1},
      ShaderVariantFlag::BaseColorMap,
      "albedo",    nullptr,           nullptr,             nullptr,        "baseColorFactor", nullptr           },

    { "emissive",  "Emissive",   PbrSlotDesc::DataType::Vec3,  true, {0,0,0,0},    {0,0,0,0}, {100,100,100,1},
      ShaderVariantFlag::EmissiveMap,
      "emissive",  nullptr,           nullptr,             nullptr,        "emissiveFactor",  nullptr           },

    { "normal",    "Normal",     PbrSlotDesc::DataType::Vec3,  true, {0,0,1,0}, {-1,-1,0,0}, {1,1,1,1},
      ShaderVariantFlag::NormalMap,
      "normal",    "normalStrength",  nullptr,             nullptr,        nullptr,           nullptr           },

    { "roughness", "Roughness",  PbrSlotDesc::DataType::Float, true, {0.5f,0,0,0}, {0,0,0,0}, {1,1,1,1},
      ShaderVariantFlag::ChannelMap,
      "orm",       "roughnessFactor", "roughnessChannel",  "roughnessBias", nullptr,          "roughnessFactor" },

    { "metallic",  "Metallic",   PbrSlotDesc::DataType::Float, true, {0,0,0,0},    {0,0,0,0}, {1,1,1,1},
      ShaderVariantFlag::ChannelMap,
      "orm",       "metallicFactor",  "metallicChannel",   "metallicBias",  nullptr,          "metallicFactor"  },

    { "occlusion", "Occlusion",  PbrSlotDesc::DataType::Float, true, {1,0,0,0},    {0,0,0,0}, {1,1,1,1},
      ShaderVariantFlag::ChannelMap,
      "orm",       "occlusionStrength","occlusionChannel", "occlusionBias", nullptr,          "occlusionStrength"},

    { "opacity",   "Opacity",    PbrSlotDesc::DataType::Float, false,{1,0,0,0},    {0,0,0,0}, {1,1,1,1},
      ShaderVariantFlag::None,
      nullptr,     nullptr,           nullptr,             nullptr,        nullptr,           "opacityFactor"   },
};

} // namespace engine::renderer::pbr::detail
