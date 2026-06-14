#pragma once

#include "renderer/RenderWorldViews.hpp"
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <typeindex>
#include <typeinfo>
#include <vector>

namespace engine::renderer {

using RenderFeatureDataSlot = uint32_t;

class RenderFeatureDataRegistry
{
public:
    struct Entry
    {
        std::type_index type = typeid(void);
        std::string name;
    };

    [[nodiscard]] RenderFeatureDataSlot Register(std::type_index type, std::string name);
    [[nodiscard]] const Entry* Get(RenderFeatureDataSlot slot) const noexcept;

    template<typename T>
    [[nodiscard]] RenderFeatureDataSlot Register(std::string_view name)
    {
        return Register(std::type_index(typeid(T)), std::string(name));
    }

    [[nodiscard]] RenderFeatureDataSlot Find(std::type_index type) const noexcept;

    template<typename T>
    [[nodiscard]] RenderFeatureDataSlot Find() const noexcept
    {
        return Find(std::type_index(typeid(T)));
    }

private:
    std::vector<Entry> m_entries;
};

struct FeatureDataStorageBase
{
    virtual ~FeatureDataStorageBase() = default;
};

template<typename T>
struct FeatureDataStorage final : FeatureDataStorageBase
{
    T value{};
};

struct RenderFrameDataView
{
    const RenderFeatureDataRegistry* featureRegistry = nullptr;
    const std::vector<std::unique_ptr<FeatureDataStorageBase>>* featureData = nullptr;

    template<typename T>
    [[nodiscard]] const T* GetFrameData() const noexcept
    {
        if (!featureRegistry || !featureData)
            return nullptr;
        const RenderFeatureDataSlot slot = featureRegistry->Find<T>();
        if (slot == static_cast<RenderFeatureDataSlot>(-1))
            return nullptr;
        if (slot >= featureData->size() || !(*featureData)[slot])
            return nullptr;
        return &static_cast<const FeatureDataStorage<T>*>((*featureData)[slot].get())->value;
    }
};

struct RenderExtractionView
{
    std::vector<RenderProxy>* proxies = nullptr;
    RenderFeatureDataRegistry* featureRegistry = nullptr;
    std::vector<std::unique_ptr<FeatureDataStorageBase>>* featureData = nullptr;
    uint32_t* activeShadowResolution = nullptr;

    void SubmitRenderables(std::vector<RenderProxy> submittedProxies) const
    {
        if (!proxies)
            return;
        proxies->insert(proxies->end(),
                        std::make_move_iterator(submittedProxies.begin()),
                        std::make_move_iterator(submittedProxies.end()));
    }

    void AddRenderable(EntityID entity, MeshHandle mesh, MaterialHandle material,
                       const math::Mat4& worldMatrix, const math::Mat4& worldMatrixInvT,
                       const math::Vec3& boundsCenter, const math::Vec3& boundsExtents,
                       float boundsRadius, uint32_t layerMask, bool castShadows) const
    {
        if (!proxies)
            return;

        RenderProxy proxy;
        proxy.entity = entity;
        proxy.mesh = mesh;
        proxy.material = material;
        proxy.worldMatrix = worldMatrix;
        proxy.worldMatrixInvT = worldMatrixInvT;
        proxy.boundsCenter = boundsCenter;
        proxy.boundsExtents = boundsExtents;
        proxy.boundsRadius = boundsRadius;
        proxy.layerMask = layerMask;
        proxy.castShadows = castShadows;
        proxies->push_back(proxy);
    }

    template<typename T>
    [[nodiscard]] T& GetOrCreateFrameData(std::string_view name) const
    {
        const RenderFeatureDataSlot slot = featureRegistry->Register<T>(name);
        if (slot >= featureData->size())
            featureData->resize(slot + 1u);
        if (!(*featureData)[slot])
            (*featureData)[slot] = std::make_unique<FeatureDataStorage<T>>();
        return static_cast<FeatureDataStorage<T>&>(*(*featureData)[slot]).value;
    }

    template<typename T>
    [[nodiscard]] T* GetFrameData() const noexcept
    {
        const RenderFeatureDataSlot slot = featureRegistry->Find<T>();
        if (slot == static_cast<RenderFeatureDataSlot>(-1))
            return nullptr;
        if (slot >= featureData->size() || !(*featureData)[slot])
            return nullptr;
        return &static_cast<FeatureDataStorage<T>*>((*featureData)[slot].get())->value;
    }

    [[nodiscard]] RenderFrameDataView GetFrameDataView() const noexcept
    {
        return RenderFrameDataView{ featureRegistry, featureData };
    }

    void SetActiveShadowResolution(uint32_t resolution) const noexcept
    {
        if (activeShadowResolution)
            *activeShadowResolution = resolution;
    }
};

} // namespace engine::renderer
