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
// The near plane is not tested: the shadow pass clamps depth, so casters in
// front of it still write depth 0 (where the backend supports depth clamping;
// otherwise they are clipped by the rasterizer, which is merely a wasted draw).
// A caster outside the side or far planes cannot write a texel and is skipped.
// Returns the number of items that were culled.
size_t FilterShadowCastersForView(const renderer::DrawList& source,
                                  const math::Mat4& shadowViewProj,
                                  renderer::DrawList& out);

} // namespace engine::addons::shadow
