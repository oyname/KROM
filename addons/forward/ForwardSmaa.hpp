#pragma once
// =============================================================================
// KROM Engine - addons/forward/ForwardSmaa.hpp
// SMAA 1x (Jimenez et al., "SMAA: Enhanced Subpixel Morphological Antialiasing")
// for the forward add-on's post-AA stage: shader sources of the three passes and
// the official area/search lookup textures (MIT, see licenses/SMAA.txt).
// =============================================================================
#include <cstddef>
#include <cstdint>

namespace engine::renderer::addons::forward::smaa {

// Pixel shaders (HLSL, entry "main") for the fullscreen triangle of the forward
// add-on. Bindings:
//   edges:   t8 = tonemapped colour (sRGB view), s2 = point clamp       -> RG8 edges
//   weights: t8 = edges, t9 = area, t10 = search, s1 = linear, s2 = point -> RGBA8 weights
//   blend:   t8 = tonemapped colour, t9 = weights, s1 = linear clamp      -> output
extern const char* const kEdgeDetectionPsHlsl;
extern const char* const kBlendingWeightPsHlsl;
extern const char* const kNeighborhoodBlendingPsHlsl;

// Area texture: R8G8, kAreaTexWidth x kAreaTexHeight (subsample index 0 only).
inline constexpr uint32_t kAreaTexWidth  = 160u;
inline constexpr uint32_t kAreaTexHeight = 80u;
// Search texture: R8, kSearchTexWidth x kSearchTexHeight.
inline constexpr uint32_t kSearchTexWidth  = 64u;
inline constexpr uint32_t kSearchTexHeight = 16u;

[[nodiscard]] const uint8_t* AreaTexData() noexcept;
[[nodiscard]] size_t         AreaTexSize() noexcept;
[[nodiscard]] const uint8_t* SearchTexData() noexcept;
[[nodiscard]] size_t         SearchTexSize() noexcept;

} // namespace engine::renderer::addons::forward::smaa
