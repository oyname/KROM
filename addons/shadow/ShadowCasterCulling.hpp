#pragma once

#include "core/Math.hpp"
#include "renderer/RenderWorldViews.hpp"
#include <array>
#include <cstddef>

namespace engine::addons::shadow {

// Gribb-Hartmann frustum planes (left, right, bottom, top, near, far) of a
// view-projection matrix in engine clip space (depth 0..1), normalized.
void ExtractShadowFrustumPlanes(const math::Mat4& viewProj,
                                std::array<math::Vec4, 6>& planes) noexcept;

// True if the AABB is not completely behind any of the planes.
[[nodiscard]] bool AabbIntersectsFrustumPlanes(const math::Vec3& center,
                                               const math::Vec3& extents,
                                               const std::array<math::Vec4, 6>& planes) noexcept;

// Copies every item of `source` that can touch the shadow view into `out`
// (order and instancing data kept). Items without bounds are always kept.
// The shadow pass rasterizes with depth clipping, so a caster outside the
// view's frustum cannot write a single texel and is safe to skip.
// Returns the number of items that were culled.
size_t FilterShadowCastersForView(const renderer::DrawList& source,
                                  const math::Mat4& shadowViewProj,
                                  renderer::DrawList& out);

} // namespace engine::addons::shadow
