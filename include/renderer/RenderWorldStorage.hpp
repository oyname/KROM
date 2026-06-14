#pragma once

#include "renderer/RenderFeatureDataViews.hpp"
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace engine::jobs { class JobSystem; }

namespace engine::renderer {

class MaterialSystem;
class RenderPassRegistry;

// Owns extracted renderables, feature frame data, and built draw queues for one frame.
// This is the concrete storage behind RenderSceneSnapshot; it intentionally does
// not expose orchestration, devices, shader runtime, or backend state.
class RenderSceneStorage
{
public:
    RenderSceneStorage() = default;
    ~RenderSceneStorage() = default;

    RenderSceneStorage(const RenderSceneStorage&) = delete;
    RenderSceneStorage& operator=(const RenderSceneStorage&) = delete;
    RenderSceneStorage(RenderSceneStorage&&) noexcept = default;
    RenderSceneStorage& operator=(RenderSceneStorage&&) noexcept = default;

    void AddRenderable(EntityID entity, MeshHandle mesh, MaterialHandle material,
                       const math::Mat4& worldMatrix, const math::Mat4& worldMatrixInvT,
                       const math::Vec3& boundsCenter, const math::Vec3& boundsExtents,
                       float boundsRadius, uint32_t layerMask, bool castShadows);

    void AddRenderablesBulk(std::vector<RenderProxy> proxies);

    void BuildDrawLists(const math::Mat4& view,
                        const math::Mat4& viewProj,
                        float nearZ,
                        float farZ,
                        const MaterialSystem& materials,
                        const RenderPassRegistry& renderPassRegistry,
                        uint32_t layerMask = 0xFFFFFFFFu,
                        jobs::JobSystem* jobSystem = nullptr);

    [[nodiscard]] const std::vector<RenderProxy>& GetProxies() const noexcept { return m_proxies; }
    [[nodiscard]] RenderQueue& GetQueue() noexcept { return m_queue; }
    [[nodiscard]] const RenderQueue& GetQueue() const noexcept { return m_queue; }
    [[nodiscard]] uint32_t VisibleCount() const noexcept { return m_visibleCount; }
    [[nodiscard]] uint32_t TotalProxyCount() const noexcept { return static_cast<uint32_t>(m_proxies.size()); }
    [[nodiscard]] RenderFeatureDataRegistry& GetFeatureDataRegistry() noexcept { return m_featureDataRegistry; }
    [[nodiscard]] const RenderFeatureDataRegistry& GetFeatureDataRegistry() const noexcept { return m_featureDataRegistry; }

    [[nodiscard]] RenderExtractionView GetExtractionView() noexcept
    {
        return RenderExtractionView{ &m_proxies, &m_featureDataRegistry, &m_featureData, &m_queue.activeShadowResolution };
    }

    [[nodiscard]] RenderFrameDataView GetFrameDataView() const noexcept
    {
        return RenderFrameDataView{ &m_featureDataRegistry, &m_featureData };
    }

    template<typename T>
    [[nodiscard]] T& GetOrCreateFeatureData(std::string_view name)
    {
        return GetExtractionView().GetOrCreateFrameData<T>(name);
    }

    template<typename T>
    [[nodiscard]] T* GetFeatureData() noexcept
    {
        const RenderFeatureDataSlot slot = m_featureDataRegistry.Find<T>();
        if (slot == static_cast<RenderFeatureDataSlot>(-1))
            return nullptr;
        if (slot >= m_featureData.size() || !m_featureData[slot])
            return nullptr;
        return &static_cast<FeatureDataStorage<T>*>(m_featureData[slot].get())->value;
    }

    template<typename T>
    [[nodiscard]] const T* GetFeatureData() const noexcept
    {
        const RenderFeatureDataSlot slot = m_featureDataRegistry.Find<T>();
        if (slot == static_cast<RenderFeatureDataSlot>(-1))
            return nullptr;
        if (slot >= m_featureData.size() || !m_featureData[slot])
            return nullptr;
        return &static_cast<const FeatureDataStorage<T>*>(m_featureData[slot].get())->value;
    }

    void Clear()
    {
        m_proxies.clear();
        m_featureData.clear();
        m_queue.Clear();
        m_visibleCount = 0u;
    }

private:
    std::vector<RenderProxy> m_proxies;
    RenderFeatureDataRegistry m_featureDataRegistry;
    std::vector<std::unique_ptr<FeatureDataStorageBase>> m_featureData;
    RenderQueue m_queue;
    uint32_t m_visibleCount = 0u;

    static bool FrustumTest(const math::Vec3& center,
                            const math::Vec3& extents,
                            const math::Vec4 planes[6]) noexcept;

    static void ExtractFrustumPlanes(const math::Mat4& viewProj,
                                     math::Vec4 outPlanes[6]) noexcept;

    static float ComputeLinearDepth(const math::Vec3& worldPos,
                                    const math::Mat4& view,
                                    float nearZ,
                                    float farZ) noexcept;

    [[nodiscard]] DrawItem BuildDrawItem(const RenderProxy& proxy,
                                         const MaterialSystem& materials,
                                         const RenderPassRegistry& renderPassRegistry,
                                         float linearDepth,
                                         bool isShadow,
                                         uint32_t submissionOrder) const noexcept;
};

} // namespace engine::renderer
