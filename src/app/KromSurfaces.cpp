#include "KromSurfaces.hpp"

#include "addons/mesh_renderer/MeshBounds.hpp"
#include "addons/mesh_renderer/MeshRendererComponents.hpp"

namespace Krom::internal {

engine::assets::SubMeshData* GetSurfaceSubmesh(const SurfaceContext& ctx, LPSURFACE surface)
{
    const auto recordIt = ctx.surfaces.find(surface.id);
    if (recordIt == ctx.surfaces.end())
        return nullptr;

    engine::assets::MeshAsset* mesh = ctx.assetRegistry.meshes.Get(recordIt->second.mesh);
    if (!mesh || recordIt->second.submeshIndex >= mesh->submeshes.size())
        return nullptr;
    return &mesh->submeshes[recordIt->second.submeshIndex];
}

const SurfaceRecord* GetSurfaceRecord(const SurfaceContext& ctx, LPSURFACE surface)
{
    const auto it = ctx.surfaces.find(surface.id);
    return it != ctx.surfaces.end() ? &it->second : nullptr;
}

void TouchSurface(const SurfaceContext& ctx, RuntimeMaterialCache& runtimeMaterialCache, LPSURFACE surface)
{
    const SurfaceRecord* record = GetSurfaceRecord(ctx, surface);
    if (!record)
        return;

    engine::assets::MeshAsset* mesh = ctx.assetRegistry.meshes.Get(record->mesh);
    if (!mesh || record->submeshIndex >= mesh->submeshes.size())
        return;

    engine::assets::SubMeshData& submesh = mesh->submeshes[record->submeshIndex];
    submesh.rawInterleavedBytes.clear();
    submesh.rawVertexStride = 0u;
    mesh->gpuStatus.dirty = true;
    mesh->gpuStatus.uploaded = false;
    std::erase_if(runtimeMaterialCache, [&](const auto& item) {
        return static_cast<uint32_t>(item.first & 0xFFFFFFFFull) == record->mesh.value;
    });

    if (ctx.world && ctx.world->IsAlive(record->entity))
    {
        if (!ctx.world->Has<engine::BoundsComponent>(record->entity))
            ctx.world->Add<engine::BoundsComponent>(record->entity);
        engine::mesh_renderer::UpdateLocalBoundsForEntity(*ctx.world, record->entity, ctx.assetRegistry);
    }
}

LPSURFACE CreateSurface(const SurfaceContext& ctx, LPENTITY meshEntity)
{
    if (!ctx.world || !ctx.world->IsAlive(meshEntity))
        return NULL_SURFACE;

    engine::MeshComponent* meshComponent = ctx.world->Get<engine::MeshComponent>(meshEntity);
    if (!meshComponent || !meshComponent->mesh.IsValid())
        return NULL_SURFACE;

    engine::assets::MeshAsset* mesh = ctx.assetRegistry.meshes.Get(meshComponent->mesh);
    if (!mesh)
        return NULL_SURFACE;

    engine::assets::SubMeshData submesh;
    submesh.materialIndex = static_cast<uint32_t>(mesh->submeshes.size());
    mesh->submeshes.push_back(std::move(submesh));
    mesh->gpuStatus.dirty = true;
    mesh->gpuStatus.uploaded = false;

    const uint32_t id = ctx.nextSurfaceId++;
    ctx.surfaces[id] = SurfaceRecord{meshEntity, meshComponent->mesh, static_cast<uint32_t>(mesh->submeshes.size() - 1u)};
    return LPSURFACE{id};
}

int AddVertex(const SurfaceContext& ctx, RuntimeMaterialCache& runtimeMaterialCache,
              LPSURFACE surface, float x, float y, float z, float u, float v)
{
    engine::assets::SubMeshData* submesh = GetSurfaceSubmesh(ctx, surface);
    if (!submesh)
        return -1;

    const int index = static_cast<int>(submesh->positions.size() / 3u);
    submesh->positions.insert(submesh->positions.end(), {x, y, z});
    submesh->normals.insert(submesh->normals.end(), {0.0f, 0.0f, 0.0f});
    submesh->uvs.insert(submesh->uvs.end(), {u, v});
    submesh->colors.insert(submesh->colors.end(), {1.0f, 1.0f, 1.0f, 1.0f});
    TouchSurface(ctx, runtimeMaterialCache, surface);
    return index;
}

bool AddTriangle(const SurfaceContext& ctx, RuntimeMaterialCache& runtimeMaterialCache,
                 LPSURFACE surface, int v0, int v1, int v2)
{
    engine::assets::SubMeshData* submesh = GetSurfaceSubmesh(ctx, surface);
    if (!submesh || v0 < 0 || v1 < 0 || v2 < 0)
        return false;

    const uint32_t vertexCount = static_cast<uint32_t>(submesh->positions.size() / 3u);
    const uint32_t i0 = static_cast<uint32_t>(v0);
    const uint32_t i1 = static_cast<uint32_t>(v1);
    const uint32_t i2 = static_cast<uint32_t>(v2);
    if (i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount)
        return false;

    submesh->indices.insert(submesh->indices.end(), {i0, i1, i2});
    TouchSurface(ctx, runtimeMaterialCache, surface);
    return true;
}

bool VertexNormal(const SurfaceContext& ctx, RuntimeMaterialCache& runtimeMaterialCache,
                  LPSURFACE surface, int index, float nx, float ny, float nz)
{
    engine::assets::SubMeshData* submesh = GetSurfaceSubmesh(ctx, surface);
    if (!submesh || index < 0)
        return false;

    const uint32_t vertex = static_cast<uint32_t>(index);
    if (vertex >= submesh->positions.size() / 3u)
        return false;

    if (submesh->normals.size() < submesh->positions.size())
        submesh->normals.resize(submesh->positions.size(), 0.0f);
    const size_t offset = static_cast<size_t>(vertex) * 3u;
    submesh->normals[offset] = nx;
    submesh->normals[offset + 1u] = ny;
    submesh->normals[offset + 2u] = nz;
    TouchSurface(ctx, runtimeMaterialCache, surface);
    return true;
}

bool VertexTexCoords(const SurfaceContext& ctx, RuntimeMaterialCache& runtimeMaterialCache,
                     LPSURFACE surface, int index, float u, float v)
{
    engine::assets::SubMeshData* submesh = GetSurfaceSubmesh(ctx, surface);
    if (!submesh || index < 0)
        return false;

    const uint32_t vertex = static_cast<uint32_t>(index);
    if (vertex >= submesh->positions.size() / 3u)
        return false;

    const size_t vertexCount = submesh->positions.size() / 3u;
    if (submesh->uvs.size() < vertexCount * 2u)
        submesh->uvs.resize(vertexCount * 2u, 0.0f);
    const size_t offset = static_cast<size_t>(vertex) * 2u;
    submesh->uvs[offset] = u;
    submesh->uvs[offset + 1u] = v;
    TouchSurface(ctx, runtimeMaterialCache, surface);
    return true;
}

int CountVertices(const SurfaceContext& ctx, LPSURFACE surface)
{
    const engine::assets::SubMeshData* submesh = GetSurfaceSubmesh(ctx, surface);
    return submesh ? static_cast<int>(submesh->positions.size() / 3u) : 0;
}

int CountTriangles(const SurfaceContext& ctx, LPSURFACE surface)
{
    const engine::assets::SubMeshData* submesh = GetSurfaceSubmesh(ctx, surface);
    return submesh ? static_cast<int>(submesh->indices.size() / 3u) : 0;
}

} // namespace Krom::internal
