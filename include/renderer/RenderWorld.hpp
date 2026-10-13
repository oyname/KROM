#pragma once

#include "renderer/RenderWorldStorage.hpp"

namespace engine::renderer {

// Legacy facade retained for focused runtime tests and older internal code paths.
// New frame/extraction contracts should use RenderSceneSnapshot, RenderSceneStorage,
// RenderExtractionView, RenderFrameDataView, or RenderQueue directly.
class RenderWorld
{
public:
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

    [[nodiscard]] const std::vector<RenderProxy>& GetProxies() const noexcept { return m_storage.GetProxies(); }
    [[nodiscard]] RenderQueue& GetQueue() noexcept { return m_storage.GetQueue(); }
    [[nodiscard]] const RenderQueue& GetQueue() const noexcept { return m_storage.GetQueue(); }
    [[nodiscard]] uint32_t VisibleCount() const noexcept { return m_storage.VisibleCount(); }
    [[nodiscard]] uint32_t TotalProxyCount() const noexcept { return m_storage.TotalProxyCount(); }
    [[nodiscard]] RenderFeatureDataRegistry& GetFeatureDataRegistry() noexcept { return m_storage.GetFeatureDataRegistry(); }
    [[nodiscard]] const RenderFeatureDataRegistry& GetFeatureDataRegistry() const noexcept { return m_storage.GetFeatureDataRegistry(); }
    [[nodiscard]] RenderExtractionView GetExtractionView() noexcept { return m_storage.GetExtractionView(); }
    [[nodiscard]] RenderFrameDataView GetFrameDataView() const noexcept { return m_storage.GetFrameDataView(); }

    template<typename T>
    [[nodiscard]] T& GetOrCreateFeatureData(std::string_view name)
    {
        return m_storage.GetOrCreateFeatureData<T>(name);
    }

    template<typename T>
    [[nodiscard]] T* GetFeatureData() noexcept
    {
        return m_storage.GetFeatureData<T>();
    }

    template<typename T>
    [[nodiscard]] const T* GetFeatureData() const noexcept
    {
        return m_storage.GetFeatureData<T>();
    }

    void Clear() { m_storage.Clear(); }

private:
    RenderSceneStorage m_storage;
};

} // namespace engine::renderer
