#include "collision/SceneQueries.hpp"
#include <cfloat>
#include <algorithm>

namespace engine::collision {
    using engine::math::Vec3;
    using engine::math::Mat4;

    static AABB BuildAABB(const Vec3& c, const Vec3& e)
    {
        return { c - e, c + e };
    }

    static Sphere BuildSphere(const Vec3& c, const Vec3& e)
    {
        return { c, e.Length() };
    }


    void SceneQueries::Build(const ecs::World& world, const assets::AssetRegistry* assets)
    {
        m_entries.clear();
        world.View<BoundsComponent, WorldTransformComponent>([&](EntityID id, const BoundsComponent& b, const WorldTransformComponent& wt) {
            Entry e{};
            e.entity = id;
            e.aabb = BuildAABB(b.centerWorld, b.extentsWorld);
            e.sphere = BuildSphere(b.centerWorld, b.extentsWorld);
            e.world = &wt.matrix;
            if (assets)
            {
                if (const auto* mc = world.Get<MeshComponent>(id)) e.mesh = assets->meshes.Get(mc->mesh);
            }
            m_entries.push_back(e);
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
        const Vec3 d = a.center - b.center;
        const float rr = a.radius + b.radius;
        return d.LengthSq() <= rr * rr;
    }

    bool SceneQueries::Intersects(const Sphere& s, const AABB& b) noexcept
    {
        float d2 = 0.f;
        const float c[3] = { s.center.x, s.center.y, s.center.z };
        const float mn[3] = { b.min.x, b.min.y, b.min.z };
        const float mx[3] = { b.max.x, b.max.y, b.max.z };
        for (int i = 0; i < 3; ++i)
        {
            float v = c[i];
            if (v < mn[i]) d2 += (mn[i] - v) * (mn[i] - v);
            else if (v > mx[i]) d2 += (v - mx[i]) * (v - mx[i]);
        }
        return d2 <= s.radius * s.radius;
    }

    bool SceneQueries::IntersectRayAABB(const Ray& ray, const AABB& box, float maxDistance, float& outT) noexcept
    {
        float tmin = 0.f, tmax = maxDistance;
        const float ro[3] = { ray.origin.x, ray.origin.y, ray.origin.z };
        const float rd[3] = { ray.direction.x, ray.direction.y, ray.direction.z };
        const float mn[3] = { box.min.x, box.min.y, box.min.z };
        const float mx[3] = { box.max.x, box.max.y, box.max.z };
        for (int i = 0; i < 3; ++i)
        {
            if (std::abs(rd[i]) < 1e-6f)
            {
                if (ro[i] < mn[i] || ro[i] > mx[i]) return false;
                continue;
            }
            const float inv = 1.f / rd[i];
            float t1 = (mn[i] - ro[i]) * inv;
            float t2 = (mx[i] - ro[i]) * inv;
            if (t1 > t2) std::swap(t1, t2);
            tmin = std::max(tmin, t1);
            tmax = std::min(tmax, t2);
            if (tmin > tmax) return false;
        }
        outT = tmin;
        return tmin <= maxDistance;
    }

    bool SceneQueries::IntersectRaySphere(const Ray& ray, const Sphere& sphere, float maxDistance, float& outT) noexcept
    {
        Vec3 oc = ray.origin - sphere.center;
        float a = Vec3::Dot(ray.direction, ray.direction);
        float b = 2.f * Vec3::Dot(oc, ray.direction);
        float c = Vec3::Dot(oc, oc) - sphere.radius * sphere.radius;
        float disc = b * b - 4.f * a * c;
        if (disc < 0.f) return false;
        float t = (-b - std::sqrt(disc)) / (2.f * a);
        if (t < 0.f) t = (-b + std::sqrt(disc)) / (2.f * a);
        if (t < 0.f || t > maxDistance) return false;
        outT = t;
        return true;
    }

    bool SceneQueries::IntersectRayTriangle(const Ray& ray, const Triangle& tri, float maxDistance, float& outT, Vec3& outNormal) noexcept
    {
        const Vec3 e1 = tri.b - tri.a;
        const Vec3 e2 = tri.c - tri.a;
        const Vec3 p = Vec3::Cross(ray.direction, e2);
        const float det = Vec3::Dot(e1, p);
        if (std::abs(det) < 1e-6f) return false;
        const float invDet = 1.f / det;
        const Vec3 t = ray.origin - tri.a;
        const float u = Vec3::Dot(t, p) * invDet;
        if (u < 0.f || u > 1.f) return false;
        const Vec3 q = Vec3::Cross(t, e1);
        const float v = Vec3::Dot(ray.direction, q) * invDet;
        if (v < 0.f || u + v > 1.f) return false;
        const float dist = Vec3::Dot(e2, q) * invDet;
        if (dist < 0.f || dist > maxDistance) return false;
        outT = dist;
        outNormal = Vec3::Cross(e1, e2).Normalized();
        return true;
    }

    bool SceneQueries::Raycast(const Ray& ray, float maxDistance, RaycastHit& outHit) const
    {
        bool found = false;
        float best = maxDistance;
        for (const auto& e : m_entries)
        {
            float tSphere = 0.f;
            if (!IntersectRaySphere(ray, e.sphere, best, tSphere)) continue;
            float tBox = 0.f;
            if (!IntersectRayAABB(ray, e.aabb, best, tBox)) continue;

            float hitT = tBox;
            Vec3 hitN = (ray.origin + ray.direction * hitT - e.sphere.center).Normalized();

            if (e.mesh && e.world)
            {
                bool triHit = false;
                for (const auto& sm : e.mesh->submeshes)
                {
                    for (size_t i = 0; i + 2 < sm.indices.size(); i += 3)
                    {
                        const uint32_t i0 = sm.indices[i + 0] * 3u;
                        const uint32_t i1 = sm.indices[i + 1] * 3u;
                        const uint32_t i2 = sm.indices[i + 2] * 3u;
                        if (i2 + 2 >= sm.positions.size()) continue;
                        Triangle tri{
                            e.world->TransformPoint(Vec3{sm.positions[i0], sm.positions[i0 + 1], sm.positions[i0 + 2]}),
                            e.world->TransformPoint(Vec3{sm.positions[i1], sm.positions[i1 + 1], sm.positions[i1 + 2]}),
                            e.world->TransformPoint(Vec3{sm.positions[i2], sm.positions[i2 + 1], sm.positions[i2 + 2]})
                        };
                        float triT = 0.f; Vec3 triN{};
                        if (IntersectRayTriangle(ray, tri, best, triT, triN))
                        {
                            hitT = triT; hitN = triN; triHit = true; best = triT;
                        }
                    }
                }
                if (!triHit && tBox >= best) continue;
            }

            if (hitT <= best)
            {
                best = hitT;
                outHit.entity = e.entity;
                outHit.distance = hitT;
                outHit.position = ray.origin + ray.direction * hitT;
                outHit.normal = hitN;
                found = true;
            }
        }
        return found;
    }

    std::vector<EntityID> SceneQueries::OverlapSphere(const Sphere& sphere) const
    {
        std::vector<EntityID> out;
        for (const auto& e : m_entries)
            if (Intersects(sphere, e.aabb)) out.push_back(e.entity);
        return out;
    }

    std::vector<EntityID> SceneQueries::OverlapAABB(const AABB& aabb) const
    {
        std::vector<EntityID> out;
        for (const auto& e : m_entries)
            if (Intersects(aabb, e.aabb)) out.push_back(e.entity);
        return out;
    }

    bool SceneQueries::SweepSphere(const Sphere& sphere, const Vec3& delta, RaycastHit& outHit) const
    {
        const float len = delta.Length();
        if (len <= 1e-6f) return false;
        Ray ray{ sphere.center, delta / len };
        bool found = false; float best = len;
        for (const auto& e : m_entries)
        {
            Sphere expanded = e.sphere; expanded.radius += sphere.radius;
            float t = 0.f;
            if (IntersectRaySphere(ray, expanded, best, t))
            {
                best = t;
                outHit.entity = e.entity;
                outHit.distance = t;
                outHit.position = ray.origin + ray.direction * t;
                outHit.normal = (outHit.position - e.sphere.center).Normalized();
                found = true;
            }
        }
        return found;
    }

}
