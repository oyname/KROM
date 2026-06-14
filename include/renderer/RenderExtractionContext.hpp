#pragma once

#include "core/Math.hpp"
#include "ecs/World.hpp"
#include "renderer/RenderFeatureDataViews.hpp"
#include "renderer/RenderFeatureInterfaces.hpp"
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

namespace engine::jobs { class JobSystem; }
namespace engine::assets { class AssetRegistry; }

namespace engine::platform {
class IPlatformTiming;
}

namespace engine::renderer {

class IDevice;
struct FrameConstants;
struct RenderSceneSnapshot;
struct RenderView;

struct SceneExtractionContext
{
    const ecs::World&          world;
    RenderSceneSnapshot*       snapshot      = nullptr;
    RenderExtractionView       extractionView{};
    const RenderView*          view          = nullptr;
    jobs::JobSystem*           jobSystem     = nullptr;
    const assets::AssetRegistry* assetRegistry = nullptr;

    SceneExtractionContext(const ecs::World& w,
                           RenderSceneSnapshot& snap,
                           const RenderView* viewIn = nullptr,
                           jobs::JobSystem* js = nullptr) noexcept;

    SceneExtractionContext(const ecs::World& w,
                           RenderExtractionView extraction,
                           const RenderView* viewIn = nullptr,
                           jobs::JobSystem* js = nullptr) noexcept;

    void SubmitRenderables(std::vector<RenderProxy> proxies) const
    {
        extractionView.SubmitRenderables(std::move(proxies));
    }

    template<typename T>
    [[nodiscard]] T& GetOrCreateFrameData(std::string_view name) const
    {
        return extractionView.GetOrCreateFrameData<T>(name);
    }
};

class IExtractionFeature
{
public:
    virtual ~IExtractionFeature() = default;
    virtual std::string_view GetName() const noexcept = 0;
    virtual void Extract(const SceneExtractionContext& ctx) const = 0;
};

struct FrameConstantsContributionContext
{
    IDevice* device = nullptr;
    math::Mat4 projectionClipSpaceAdjustment = math::Mat4::Identity();
    math::Mat4 shadowClipSpaceAdjustment = math::Mat4::Identity();
    uint32_t viewportWidth = 0u;
    uint32_t viewportHeight = 0u;
    const RenderView& view;
    const platform::IPlatformTiming& timing;
    const RenderSceneSnapshot* snapshot = nullptr;
    RenderFrameDataView frameData{};
    const RenderQueue* renderQueue = nullptr;

    FrameConstantsContributionContext(IDevice* deviceIn,
                                      const math::Mat4& projectionAdjustment,
                                      const math::Mat4& shadowAdjustment,
                                      uint32_t viewportWidthIn,
                                      uint32_t viewportHeightIn,
                                      const RenderView& viewIn,
                                      const platform::IPlatformTiming& timingIn,
                                      const RenderSceneSnapshot& snapshotIn);

    FrameConstantsContributionContext(IDevice* deviceIn,
                                      const math::Mat4& projectionAdjustment,
                                      const math::Mat4& shadowAdjustment,
                                      uint32_t viewportWidthIn,
                                      uint32_t viewportHeightIn,
                                      const RenderView& viewIn,
                                      const platform::IPlatformTiming& timingIn,
                                      RenderFrameDataView frameDataIn,
                                      const RenderQueue* renderQueueIn,
                                      const RenderSceneSnapshot* snapshotIn = nullptr);

    [[nodiscard]] const RenderSceneSnapshot* GetSnapshot() const noexcept
    {
        return snapshot;
    }

    template<typename T>
    [[nodiscard]] const T* GetFrameData() const noexcept
    {
        return frameData.GetFrameData<T>();
    }

    [[nodiscard]] const RenderQueue* GetRenderQueueView() const noexcept
    {
        return renderQueue;
    }

    [[nodiscard]] uint32_t GetActiveShadowResolution() const noexcept
    {
        return renderQueue ? renderQueue->activeShadowResolution : 0u;
    }
};

class IFrameConstantsContributor
{
public:
    virtual ~IFrameConstantsContributor() = default;
    virtual std::string_view GetName() const noexcept = 0;
    virtual void OnDeviceShutdown() noexcept {}
    virtual void Contribute(const FrameConstantsContributionContext& context,
                            FrameConstants& frameConstants) const = 0;
};

} // namespace engine::renderer
