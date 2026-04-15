#include "assets/MeshTangents.hpp"

#include "core/Math.hpp"
#include <cmath>
#include <vector>

namespace engine::assets {
using engine::math::Vec2;
using engine::math::Vec3;

namespace {

constexpr float kEpsilon = 1e-8f;

static bool HasChannel(const std::vector<float>& data, size_t vertexCount, size_t components) noexcept
{
    return data.size() >= vertexCount * components;
}

static Vec3 LoadVec3(const std::vector<float>& data, size_t index) noexcept
{
    const size_t o = index * 3u;
    return { data[o + 0u], data[o + 1u], data[o + 2u] };
}

static Vec2 LoadVec2(const std::vector<float>& data, size_t index) noexcept
{
    const size_t o = index * 2u;
    return { data[o + 0u], data[o + 1u] };
}

static void StoreTangent(std::vector<float>& data, size_t index, const Vec3& tangent, float sign) noexcept
{
    const size_t o = index * 4u;
    data[o + 0u] = tangent.x;
    data[o + 1u] = tangent.y;
    data[o + 2u] = tangent.z;
    data[o + 3u] = sign;
}

static Vec3 BuildFallbackTangent(const Vec3& normal) noexcept
{
    const Vec3 axis = (std::fabs(normal.y) < 0.999f) ? Vec3{0.f, 1.f, 0.f}
                                                     : Vec3{1.f, 0.f, 0.f};
    Vec3 tangent = Vec3::Cross(axis, normal).Normalized();
    if (tangent.LengthSq() <= kEpsilon)
        tangent = Vec3{1.f, 0.f, 0.f};
    return tangent;
}

} // namespace

bool HasValidTangents(const SubMeshData& subMesh) noexcept
{
    const size_t vertexCount = subMesh.positions.size() / 3u;
    return vertexCount > 0u && subMesh.tangents.size() >= vertexCount * 4u;
}

bool GenerateTangents(SubMeshData& subMesh, const TangentGenerationOptions& options) noexcept
{
    const size_t vertexCount = subMesh.positions.size() / 3u;
    if (vertexCount == 0u || !HasChannel(subMesh.positions, vertexCount, 3u))
        return false;
    if (options.requireNormals && !HasChannel(subMesh.normals, vertexCount, 3u))
        return false;
    if (options.requireTexcoords && !HasChannel(subMesh.uvs, vertexCount, 2u))
        return false;
    if (!options.overwriteExistingTangents && HasValidTangents(subMesh))
        return true;

    std::vector<Vec3> tan1(vertexCount, Vec3{});
    std::vector<Vec3> tan2(vertexCount, Vec3{});

    auto accumulateTriangle = [&](uint32_t i0, uint32_t i1, uint32_t i2)
    {
        if (i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount)
            return;

        const Vec3 p0 = LoadVec3(subMesh.positions, i0);
        const Vec3 p1 = LoadVec3(subMesh.positions, i1);
        const Vec3 p2 = LoadVec3(subMesh.positions, i2);
        const Vec2 w0 = LoadVec2(subMesh.uvs, i0);
        const Vec2 w1 = LoadVec2(subMesh.uvs, i1);
        const Vec2 w2 = LoadVec2(subMesh.uvs, i2);

        const Vec3 e1 = p1 - p0;
        const Vec3 e2 = p2 - p0;
        const Vec2 d1 = w1 - w0;
        const Vec2 d2 = w2 - w0;

        const float det = d1.x * d2.y - d1.y * d2.x;
        if (std::fabs(det) <= kEpsilon)
            return;

        const float invDet = 1.0f / det;
        const Vec3 sdir = (e1 * d2.y - e2 * d1.y) * invDet;
        const Vec3 tdir = (e2 * d1.x - e1 * d2.x) * invDet;

        tan1[i0] += sdir; tan1[i1] += sdir; tan1[i2] += sdir;
        tan2[i0] += tdir; tan2[i1] += tdir; tan2[i2] += tdir;
    };

    if (!subMesh.indices.empty())
    {
        for (size_t i = 0; i + 2u < subMesh.indices.size(); i += 3u)
            accumulateTriangle(subMesh.indices[i + 0u], subMesh.indices[i + 1u], subMesh.indices[i + 2u]);
    }
    else
    {
        for (uint32_t i = 0u; i + 2u < static_cast<uint32_t>(vertexCount); i += 3u)
            accumulateTriangle(i + 0u, i + 1u, i + 2u);
    }

    subMesh.tangents.assign(vertexCount * 4u, 0.f);
    for (size_t v = 0u; v < vertexCount; ++v)
    {
        Vec3 n = LoadVec3(subMesh.normals, v).Normalized();
        if (n.LengthSq() <= kEpsilon)
            n = Vec3{0.f, 1.f, 0.f};

        Vec3 t = tan1[v];
        t = t - n * Vec3::Dot(n, t);
        if (t.LengthSq() <= kEpsilon)
            t = BuildFallbackTangent(n);
        else
            t = t.Normalized();

        Vec3 b = Vec3::Cross(n, t);
        const float sign = (Vec3::Dot(b, tan2[v]) < 0.0f) ? -1.0f : 1.0f;
        StoreTangent(subMesh.tangents, v, t, sign);
    }

    return true;
}

bool EnsureTangents(SubMeshData& subMesh, const TangentGenerationOptions& options) noexcept
{
    if (HasValidTangents(subMesh) && !options.overwriteExistingTangents)
        return true;
    return GenerateTangents(subMesh, options);
}

} // namespace engine::assets
