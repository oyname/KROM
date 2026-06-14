#pragma once

#include "renderer/RenderFrameConstants.hpp"
#include "renderer/RenderPassRegistry.hpp"
#include "renderer/RendererTypes.hpp"
#include <unordered_map>
#include <vector>

namespace engine::renderer {

struct RenderProxy
{
    EntityID       entity;
    MeshHandle     mesh;
    MaterialHandle material;
    math::Mat4     worldMatrix;
    math::Mat4     worldMatrixInvT;
    math::Vec3     boundsCenter;
    math::Vec3     boundsExtents;
    float          boundsRadius  = 1.f;
    uint32_t       submeshIndex  = 0u;
    uint32_t       layerMask     = 0xFFFFFFFFu;
    bool           castShadows   = true;
    bool           visible       = true;
    BufferHandle   boneBuffer;  // invalid if not skinned
};

struct DrawItem
{
    SortKey    sortKey;

    MeshHandle     mesh;
    MaterialHandle material;
    EntityID       entity;
    uint32_t       layerMask = 0xFFFFFFFFu;

    BufferHandle gpuVertexBuffer;
    BufferHandle gpuIndexBuffer;
    BufferHandle boneBuffer;     // invalid if not skinned
    uint32_t     gpuIndexCount   = 0u;
    uint32_t     gpuVertexStride = 0u;
    uint32_t     submeshIndex    = 0u;

    uint32_t   cbOffset      = 0u;
    uint32_t   cbSize        = 0u;
    uint32_t   instanceCount = 1u;
    uint32_t   firstInstance = 0u;

    [[nodiscard]] bool hasGpuData() const noexcept
    {
        return gpuVertexBuffer.IsValid() && gpuIndexBuffer.IsValid() && gpuIndexCount > 0u;
    }

    bool operator<(const DrawItem& o) const noexcept { return sortKey < o.sortKey; }
};

struct alignas(16) PerObjectConstants
{
    float worldMatrix[16];
    float worldMatrixInvT[16];
    float entityId[4];
};

struct alignas(16) VulkanPerObjectPushConstants
{
    float worldRow0[4];
    float worldRow1[4];
    float worldRow2[4];
    float worldInvTRow0[4];
    float worldInvTRow1[4];
    float worldInvTRow2[4];
    float entityId[4];
};
static_assert(sizeof(VulkanPerObjectPushConstants) == 112u,
              "Vulkan per-object push constants must stay within the 128-byte minimum guarantee");

struct DrawList
{
    RenderPassID          passId = StandardRenderPasses::Opaque();
    std::vector<DrawItem> items;
    bool                  sorted = false;

    void Clear() { items.clear(); sorted = false; }
    void Add(DrawItem&& item) { items.push_back(std::move(item)); sorted = false; }
    void Sort();
    size_t Size() const noexcept { return items.size(); }
};

struct RenderQueue
{
    std::vector<DrawList> lists;
    std::unordered_map<RenderPassID, size_t> indices;
    std::vector<PerObjectConstants> objectConstants;
    uint32_t activeShadowResolution = 0u;

    void Clear()
    {
        lists.clear();
        indices.clear();
        objectConstants.clear();
        activeShadowResolution = 0u;
    }

    void SortAll()
    {
        for (DrawList& list : lists)
            list.Sort();
    }

    [[nodiscard]] DrawList& GetOrCreateList(RenderPassID passId) noexcept
    {
        auto it = indices.find(passId);
        if (it != indices.end())
            return lists[it->second];

        const size_t index = lists.size();
        DrawList list{};
        list.passId = passId;
        lists.push_back(std::move(list));
        indices.emplace(passId, index);
        return lists.back();
    }

    [[nodiscard]] DrawList* FindList(RenderPassID passId) noexcept
    {
        const auto it = indices.find(passId);
        return it != indices.end() ? &lists[it->second] : nullptr;
    }

    [[nodiscard]] const DrawList* FindList(RenderPassID passId) const noexcept
    {
        const auto it = indices.find(passId);
        return it != indices.end() ? &lists[it->second] : nullptr;
    }

    [[nodiscard]] std::vector<DrawList>& GetLists() noexcept { return lists; }
    [[nodiscard]] const std::vector<DrawList>& GetLists() const noexcept { return lists; }
};

} // namespace engine::renderer
