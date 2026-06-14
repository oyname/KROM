#pragma once

#include "renderer/RenderWorldStorage.hpp"
#include <cstdint>

namespace engine::renderer {

struct RenderSceneSnapshot
{
    RenderSceneSnapshot() = default;
    ~RenderSceneSnapshot() = default;

    RenderSceneSnapshot(const RenderSceneSnapshot&) = delete;
    RenderSceneSnapshot& operator=(const RenderSceneSnapshot&) = delete;

    RenderSceneSnapshot(RenderSceneSnapshot&& other) noexcept = default;
    RenderSceneSnapshot& operator=(RenderSceneSnapshot&& other) noexcept = default;

    [[nodiscard]] RenderQueue& GetQueue() noexcept;
    [[nodiscard]] const RenderQueue& GetQueue() const noexcept;
    [[nodiscard]] RenderExtractionView GetExtractionView() noexcept;
    [[nodiscard]] RenderFrameDataView GetFrameDataView() const noexcept;
    [[nodiscard]] uint32_t VisibleCount() const noexcept;
    [[nodiscard]] uint32_t TotalProxyCount() const noexcept;

    void BuildDrawLists(const math::Mat4& view,
                        const math::Mat4& viewProj,
                        float nearZ,
                        float farZ,
                        const MaterialSystem& materials,
                        const RenderPassRegistry& renderPassRegistry,
                        uint32_t layerMask = 0xFFFFFFFFu,
                        jobs::JobSystem* jobSystem = nullptr);

    void Clear();

private:
    RenderSceneStorage m_storage;
};

} // namespace engine::renderer
