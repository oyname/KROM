#include "addons/shadow/ShadowCasterCulling.hpp"

#include <cmath>

namespace engine::addons::shadow {

void ExtractShadowFrustumPlanes(const math::Mat4& viewProj,
                                std::array<math::Vec4, 6>& planes) noexcept
{
    // viewProj is column-major: m[col][row].
    auto row = [&](int r) -> math::Vec4
    {
        return { viewProj.m[0][r], viewProj.m[1][r], viewProj.m[2][r], viewProj.m[3][r] };
    };

    const math::Vec4 r0 = row(0);
    const math::Vec4 r1 = row(1);
    const math::Vec4 r2 = row(2);
    const math::Vec4 r3 = row(3);

    planes[0] = { r3.x + r0.x, r3.y + r0.y, r3.z + r0.z, r3.w + r0.w };
    planes[1] = { r3.x - r0.x, r3.y - r0.y, r3.z - r0.z, r3.w - r0.w };
    planes[2] = { r3.x + r1.x, r3.y + r1.y, r3.z + r1.z, r3.w + r1.w };
    planes[3] = { r3.x - r1.x, r3.y - r1.y, r3.z - r1.z, r3.w - r1.w };
    planes[4] = { r2.x,        r2.y,        r2.z,        r2.w        };
    planes[5] = { r3.x - r2.x, r3.y - r2.y, r3.z - r2.z, r3.w - r2.w };

    for (math::Vec4& plane : planes)
    {
        const float len = std::sqrt(plane.x * plane.x + plane.y * plane.y + plane.z * plane.z);
        if (len > 1e-6f)
            plane = plane * (1.0f / len);
    }
}

bool AabbIntersectsFrustumPlanes(const math::Vec3& center,
                                 const math::Vec3& extents,
                                 const std::array<math::Vec4, 6>& planes) noexcept
{
    for (const math::Vec4& plane : planes)
    {
        const float radius = std::fabs(extents.x * plane.x)
                           + std::fabs(extents.y * plane.y)
                           + std::fabs(extents.z * plane.z);
        const float distance = plane.x * center.x + plane.y * center.y + plane.z * center.z + plane.w;
        if (distance + radius < 0.f)
            return false;
    }
    return true;
}

size_t FilterShadowCastersForView(const renderer::DrawList& source,
                                  const math::Mat4& shadowViewProj,
                                  renderer::DrawList& out)
{
    out.passId = source.passId;
    out.sorted = source.sorted;
    out.items.clear();
    out.items.reserve(source.items.size());

    std::array<math::Vec4, 6> planes{};
    ExtractShadowFrustumPlanes(shadowViewProj, planes);

    size_t culled = 0u;
    for (const renderer::DrawItem& item : source.items)
    {
        if (item.hasBounds &&
            !AabbIntersectsFrustumPlanes(item.boundsCenter, item.boundsExtents, planes))
        {
            ++culled;
            continue;
        }
        out.items.push_back(item);
    }
    return culled;
}

} // namespace engine::addons::shadow
