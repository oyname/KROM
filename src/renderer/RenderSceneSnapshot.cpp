#include "renderer/RenderSceneSnapshot.hpp"

namespace engine::renderer {

RenderQueue& RenderSceneSnapshot::GetQueue() noexcept
{
    return m_storage.GetQueue();
}

const RenderQueue& RenderSceneSnapshot::GetQueue() const noexcept
{
    return m_storage.GetQueue();
}

RenderExtractionView RenderSceneSnapshot::GetExtractionView() noexcept
{
    return m_storage.GetExtractionView();
}

RenderFrameDataView RenderSceneSnapshot::GetFrameDataView() const noexcept
{
    return m_storage.GetFrameDataView();
}

uint32_t RenderSceneSnapshot::VisibleCount() const noexcept
{
    return m_storage.VisibleCount();
}

uint32_t RenderSceneSnapshot::TotalProxyCount() const noexcept
{
    return m_storage.TotalProxyCount();
}

void RenderSceneSnapshot::BuildDrawLists(const math::Mat4& view,
                                         const math::Mat4& viewProj,
                                         float nearZ,
                                         float farZ,
                                         const MaterialSystem& materials,
                                         const RenderPassRegistry& renderPassRegistry,
                                         uint32_t layerMask,
                                         jobs::JobSystem* jobSystem)
{
    m_storage.BuildDrawLists(view,
                             viewProj,
                             nearZ,
                             farZ,
                             materials,
                             renderPassRegistry,
                             layerMask,
                             jobSystem);
}

void RenderSceneSnapshot::Clear()
{
    m_storage.Clear();
}

} // namespace engine::renderer
