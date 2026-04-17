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
    const float a = Vec3::Dot(ray.direction, ray.direction);
    const float b = 2.f * Vec3::Dot(oc, ray.direction);
    const float c = Vec3::Dot(oc, oc) - sphere.radius * sphere.radius;
    const float discriminant = b * b - 4.f * a * c;
    if (discriminant < 0.f)
        return false;

    float t = (-b - std::sqrt(discriminant)) / (2.f * a);
    if (t < 0.f)
        t = (-b + std::sqrt(discriminant)) / (2.f * a);
    if (t < 0.f || t > maxDistance)
        return false;

    outT = t;
    return true;
}

bool SceneQueries::IntersectRayTriangle(const Ray& ray, const Triangle& tri, float maxDistance, float& outT, Vec3& outNormal) noexcept
{
    const Vec3 edge1 = tri.b - tri.a;
    const Vec3 edge2 = tri.c - tri.a;
    const Vec3 p = Vec3::Cross(ray.direction, edge2);
    const float det = Vec3::Dot(edge1, p);
    if (std::abs(det) < 1e-6f)
        return false;

    const float invDet = 1.f / det;
    const Vec3 t = ray.origin - tri.a;
    const float u = Vec3::Dot(t, p) * invDet;
    if (u < 0.f || u > 1.f)
        return false;

    const Vec3 q = Vec3::Cross(t, edge1);
    const float v = Vec3::Dot(ray.direction, q) * invDet;
    if (v < 0.f || u + v > 1.f)
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
    for (const Entry& entry : m_entries)
    {
        float sphereT = 0.f;
        if (!IntersectRaySphere(ray, entry.sphere, bestDistance, sphereT))
            continue;

        float boxT = 0.f;
        if (!IntersectRayAABB(ray, entry.aabb, bestDistance, boxT))
            continue;

        bestDistance = boxT;
        outHit.entity = entry.entity;
        outHit.distance = boxT;
        outHit.position = ray.origin + ray.direction * boxT;
        outHit.normal = (outHit.position - entry.sphere.center).Normalized();
        found = true;
    }

    return found;
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
