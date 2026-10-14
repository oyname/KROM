#include "collision/SceneQueries.hpp"
#include "ecs/Components.hpp"
#include <algorithm>
#include <cmath>

namespace engine::collision {
using engine::math::Vec3;

static AABB BuildAABB(const Vec3& center, const Vec3& extents)
{
    return { center - extents, center + extents };
}

static Sphere BuildSphere(const Vec3& center, const Vec3& extents)
{
    return { center, extents.Length() };
}

AABB ComputeWorldAABB(const OBBComponent& obb, const WorldTransformComponent& transform) noexcept
{
    constexpr float kBig = 3.402823466e+38f;
    AABB out{ { kBig, kBig, kBig }, { -kBig, -kBig, -kBig } };
    for (int i = 0; i < 8; ++i)
    {
        const Vec3 corner{
            (i & 1) ? obb.halfExtents.x : -obb.halfExtents.x,
            (i & 2) ? obb.halfExtents.y : -obb.halfExtents.y,
            (i & 4) ? obb.halfExtents.z : -obb.halfExtents.z };
        const Vec3 local = obb.centerOffset + obb.orientation.Rotate(corner);
        const Vec3 scaled{ local.x * transform.scale.x, local.y * transform.scale.y, local.z * transform.scale.z };
        const Vec3 world = transform.position + transform.rotation.Rotate(scaled);
        out.min = { std::min(out.min.x, world.x), std::min(out.min.y, world.y), std::min(out.min.z, world.z) };
        out.max = { std::max(out.max.x, world.x), std::max(out.max.y, world.y), std::max(out.max.z, world.z) };
    }
    return out;
}

void SceneQueries::Build(const ecs::World& world)
{
    m_entries.clear();
    world.View<BoundsComponent>([&](EntityID id, const BoundsComponent& bounds) {
        Entry entry{};
        entry.entity = id;
        entry.aabb = BuildAABB(bounds.centerWorld, bounds.extentsWorld);
        entry.sphere = BuildSphere(bounds.centerWorld, bounds.extentsWorld);
        m_entries.push_back(entry);
    });
}

void SceneQueries::BuildColliders(const ecs::World& world)
{
    m_entries.clear();
    world.View<BoundsComponent>([&](EntityID id, const BoundsComponent& bounds) {
        if (world.Has<OBBComponent>(id))
            return;  // OBB ist die Kollisionsform dieser Entity
        Entry entry{};
        entry.entity = id;
        entry.aabb = BuildAABB(bounds.centerWorld, bounds.extentsWorld);
        entry.sphere = BuildSphere(bounds.centerWorld, bounds.extentsWorld);
        m_entries.push_back(entry);
    });
    world.View<OBBComponent>([&](EntityID id, const OBBComponent& obb) {
        if (obb.isTrigger)
            return;
        const auto* transform = world.Get<WorldTransformComponent>(id);
        if (!transform)
            return;
        Entry entry{};
        entry.entity = id;
        entry.aabb = ComputeWorldAABB(obb, *transform);
        const Vec3 center = (entry.aabb.min + entry.aabb.max) * 0.5f;
        entry.sphere = BuildSphere(center, entry.aabb.max - center);
        m_entries.push_back(entry);
    });
}

bool SceneQueries::Intersects(const AABB& a, const AABB& b) noexcept
{
    return a.min.x <= b.max.x && a.max.x >= b.min.x &&
        a.min.y <= b.max.y && a.max.y >= b.min.y &&
        a.min.z <= b.max.z && a.max.z >= b.min.z;
}

bool SceneQueries::Intersects(const Sphere& a, const Sphere& b) noexcept
{
    const Vec3 delta = a.center - b.center;
    const float radiusSum = a.radius + b.radius;
    return delta.LengthSq() <= radiusSum * radiusSum;
}

bool SceneQueries::Intersects(const Sphere& sphere, const AABB& box) noexcept
{
    float distanceSq = 0.f;
    const float c[3] = { sphere.center.x, sphere.center.y, sphere.center.z };
    const float mn[3] = { box.min.x, box.min.y, box.min.z };
    const float mx[3] = { box.max.x, box.max.y, box.max.z };
    for (int i = 0; i < 3; ++i)
    {
        const float value = c[i];
        if (value < mn[i])
            distanceSq += (mn[i] - value) * (mn[i] - value);
        else if (value > mx[i])
            distanceSq += (value - mx[i]) * (value - mx[i]);
    }
    return distanceSq <= sphere.radius * sphere.radius;
}

bool SceneQueries::IntersectRayAABB(const Ray& ray, const AABB& box, float maxDistance, float& outT) noexcept
{
    float tmin = 0.f;
    float tmax = maxDistance;
    const float ro[3] = { ray.origin.x, ray.origin.y, ray.origin.z };
    const float rd[3] = { ray.direction.x, ray.direction.y, ray.direction.z };
    const float mn[3] = { box.min.x, box.min.y, box.min.z };
    const float mx[3] = { box.max.x, box.max.y, box.max.z };
    for (int i = 0; i < 3; ++i)
    {
        if (std::abs(rd[i]) < 1e-6f)
        {
            if (ro[i] < mn[i] || ro[i] > mx[i])
                return false;
            continue;
        }

        const float inv = 1.f / rd[i];
        float t1 = (mn[i] - ro[i]) * inv;
        float t2 = (mx[i] - ro[i]) * inv;
        if (t1 > t2)
            std::swap(t1, t2);
        tmin = std::max(tmin, t1);
        tmax = std::min(tmax, t2);
        if (tmin > tmax)
            return false;
    }

    outT = tmin;
    return tmin <= maxDistance;
}

bool SceneQueries::IntersectRaySphere(const Ray& ray, const Sphere& sphere, float maxDistance, float& outT) noexcept
{
    const Vec3 oc = ray.origin - sphere.center;
    const double radius = static_cast<double>(sphere.radius);
    const double radiusSq = radius * radius;
    const double ocLenSq =
        static_cast<double>(oc.x) * static_cast<double>(oc.x) +
        static_cast<double>(oc.y) * static_cast<double>(oc.y) +
        static_cast<double>(oc.z) * static_cast<double>(oc.z);

    if (ocLenSq <= radiusSq)
    {
        outT = 0.f;
        return maxDistance >= 0.f;
    }

    const double dirLenSq =
        static_cast<double>(ray.direction.x) * static_cast<double>(ray.direction.x) +
        static_cast<double>(ray.direction.y) * static_cast<double>(ray.direction.y) +
        static_cast<double>(ray.direction.z) * static_cast<double>(ray.direction.z);
    if (dirLenSq <= 1e-12)
        return false;

    const double tClosest = -(
        static_cast<double>(oc.x) * static_cast<double>(ray.direction.x) +
        static_cast<double>(oc.y) * static_cast<double>(ray.direction.y) +
        static_cast<double>(oc.z) * static_cast<double>(ray.direction.z)) / dirLenSq;

    const double closestDistSq = ocLenSq - tClosest * tClosest * dirLenSq;
    if (closestDistSq > radiusSq)
        return false;

    const double halfChord = std::sqrt(std::max(0.0, (radiusSq - closestDistSq) / dirLenSq));
    double t = tClosest - halfChord;
    if (t < 0.0)
        t = tClosest + halfChord;
    if (t < 0.0 || t > static_cast<double>(maxDistance))
        return false;

    outT = static_cast<float>(t);
    return true;
}

bool SceneQueries::IntersectRayTriangle(const Ray& ray, const Triangle& tri, float maxDistance, float& outT, Vec3& outNormal) noexcept
{
    const Vec3 edge1 = tri.b - tri.a;
    const Vec3 edge2 = tri.c - tri.a;
    const Vec3 p = Vec3::Cross(ray.direction, edge2);
    const float det = Vec3::Dot(edge1, p);
    // Relatives Epsilon: ein absolutes 1e-6 verwirft kleine Dreiecke (z.B. skalierte
    // High-Poly-Modelle mit Kantenlaengen ~1e-3) komplett, obwohl der Strahl sie trifft.
    const float parallelEps = 1e-7f * edge1.Length() * edge2.Length();
    if (std::abs(det) <= parallelEps || det == 0.f)
        return false;

    // Kleine Toleranz auf den Kanten, damit Strahlen exakt auf gemeinsamen
    // Kanten nicht durch beide Nachbardreiecke fallen.
    constexpr float kEdgeEps = 1e-5f;
    const float invDet = 1.f / det;
    const Vec3 t = ray.origin - tri.a;
    const float u = Vec3::Dot(t, p) * invDet;
    if (u < -kEdgeEps || u > 1.f + kEdgeEps)
        return false;

    const Vec3 q = Vec3::Cross(t, edge1);
    const float v = Vec3::Dot(ray.direction, q) * invDet;
    if (v < -kEdgeEps || u + v > 1.f + kEdgeEps)
        return false;

    const float distance = Vec3::Dot(edge2, q) * invDet;
    if (distance < 0.f || distance > maxDistance)
        return false;

    outT = distance;
    outNormal = Vec3::Cross(edge1, edge2).Normalized();
    return true;
}

bool SceneQueries::Raycast(const Ray& ray, float maxDistance, RaycastHit& outHit) const
{
    bool found = false;
    float bestDistance = maxDistance;
    for (const RaycastCandidate& candidate : CollectRaycastCandidates(ray, maxDistance))
    {
        if (candidate.boundsDistance > bestDistance)
            break;

        bestDistance = candidate.boundsDistance;
        outHit.entity = candidate.entity;
        outHit.distance = candidate.boundsDistance;
        outHit.position = ray.origin + ray.direction * candidate.boundsDistance;
        outHit.normal = (outHit.position - candidate.sphere.center).Normalized();
        found = true;
    }

    return found;
}

std::vector<RaycastCandidate> SceneQueries::CollectRaycastCandidates(const Ray& ray, float maxDistance) const
{
    std::vector<RaycastCandidate> candidates;
    for (const Entry& entry : m_entries)
    {
        float sphereT = 0.f;
        if (!IntersectRaySphere(ray, entry.sphere, maxDistance, sphereT))
            continue;

        float boxT = 0.f;
        if (!IntersectRayAABB(ray, entry.aabb, maxDistance, boxT))
            continue;

        candidates.push_back(RaycastCandidate{
            entry.entity,
            entry.aabb,
            entry.sphere,
            sphereT,
            boxT
        });
    }

    std::sort(candidates.begin(), candidates.end(),
        [](const RaycastCandidate& a, const RaycastCandidate& b)
    {
        return a.boundsDistance < b.boundsDistance;
    });
    return candidates;
}

std::vector<EntityID> SceneQueries::OverlapSphere(const Sphere& sphere) const
{
    std::vector<EntityID> out;
    for (const Entry& entry : m_entries)
        if (Intersects(sphere, entry.aabb))
            out.push_back(entry.entity);
    return out;
}

std::vector<EntityID> SceneQueries::OverlapAABB(const AABB& aabb) const
{
    std::vector<EntityID> out;
    for (const Entry& entry : m_entries)
        if (Intersects(aabb, entry.aabb))
            out.push_back(entry.entity);
    return out;
}

bool SceneQueries::SweepSphere(const Sphere& sphere, const Vec3& delta, RaycastHit& outHit) const
{
    const float length = delta.Length();
    if (length <= 1e-6f)
        return false;

    const Ray ray{ sphere.center, delta / length };
    bool found = false;
    float bestDistance = length;
    for (const Entry& entry : m_entries)
    {
        Sphere expanded = entry.sphere;
        expanded.radius += sphere.radius;
        float t = 0.f;
        if (!IntersectRaySphere(ray, expanded, bestDistance, t))
            continue;

        bestDistance = t;
        outHit.entity = entry.entity;
        outHit.distance = t;
        outHit.position = ray.origin + ray.direction * t;
        outHit.normal = (outHit.position - entry.sphere.center).Normalized();
        found = true;
    }

    return found;
}

} // namespace engine::collision
