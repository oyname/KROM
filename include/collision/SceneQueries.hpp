#pragma once
#include "ecs/World.hpp"
#include "ecs/Components.hpp"
#include "assets/AssetRegistry.hpp"
#include <vector>

namespace engine::collision {

using engine::math::Vec3;
using engine::math::Mat4;

struct Ray
{
    Vec3 origin{};
    Vec3 direction{0.f,0.f,-1.f};
};

struct AABB
{
    Vec3 min{};
    Vec3 max{};
};

struct Sphere
{
    Vec3 center{};
    float radius = 0.f;
};

struct Triangle
{
    Vec3 a{}, b{}, c{};
};

struct RaycastHit
{
    EntityID entity = NULL_ENTITY;
    float distance = 0.f;
    Vec3 position{};
    Vec3 normal{};
};

class SceneQueries
{
public:
    void Build(const ecs::World& world, const assets::AssetRegistry* assets = nullptr);

    bool Raycast(const Ray& ray, float maxDistance, RaycastHit& outHit) const;
    std::vector<EntityID> OverlapSphere(const Sphere& sphere) const;
    std::vector<EntityID> OverlapAABB(const AABB& aabb) const;
    bool SweepSphere(const Sphere& sphere, const Vec3& delta, RaycastHit& outHit) const;

    static bool Intersects(const AABB& a, const AABB& b) noexcept;
    static bool Intersects(const Sphere& a, const Sphere& b) noexcept;
    static bool Intersects(const Sphere& s, const AABB& b) noexcept;
    static bool IntersectRayAABB(const Ray& ray, const AABB& box, float maxDistance, float& outT) noexcept;
    static bool IntersectRaySphere(const Ray& ray, const Sphere& sphere, float maxDistance, float& outT) noexcept;
    static bool IntersectRayTriangle(const Ray& ray, const Triangle& tri, float maxDistance, float& outT, Vec3& outNormal) noexcept;

private:
    struct Entry
    {
        EntityID entity = NULL_ENTITY;
        AABB aabb{};
        Sphere sphere{};
        const assets::MeshAsset* mesh = nullptr;
        const Mat4* world = nullptr;
    };

    std::vector<Entry> m_entries;
};

}
