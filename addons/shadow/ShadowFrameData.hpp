#pragma once
// =============================================================================
// KROM Engine - addons/shadow/ShadowFrameData.hpp
// Per-Frame Shadow-Laufzeitdaten mit gemeinsamem Request/View-Vertrag.
// Die CPU-Seite kann bereits mehrere Lichttypen einheitlich planen.
// Der aktuelle Renderer nutzt daraus vorerst genau einen Current-Render-Path-
// Request als Legacy-Bruecke, bis der Mehrlicht-Shadowpfad aktiv ist.
// =============================================================================
#include "addons/shadow/ShadowTypes.hpp"
#include "renderer/IDevice.hpp"
#include <cstddef>
#include <vector>

namespace engine::addons::shadow {

struct CurrentRenderPathShadowPlan
{
    std::vector<size_t> requestIndices;
    std::vector<uint32_t> visibleLightIndices;
    size_t primaryRequestIndex = static_cast<size_t>(-1);
    uint32_t primaryVisibleLightIndex = UINT32_MAX;

    void Reset() noexcept
    {
        requestIndices.clear();
        visibleLightIndices.clear();
        primaryRequestIndex = static_cast<size_t>(-1);
        primaryVisibleLightIndex = UINT32_MAX;
    }
};

struct ShadowFrameData
{
    struct GpuFrameResources
    {
        BufferHandle lightBuffer = BufferHandle::Invalid();
        BufferHandle viewBuffer = BufferHandle::Invalid();
        uint32_t lightCount = 0u;
        uint32_t viewCount = 0u;
        uint32_t lightCapacity = 0u;
        uint32_t viewCapacity = 0u;
    };

    std::vector<ShadowRequest> requests;
    std::vector<uint32_t> shadowedLightIndices;
    CurrentRenderPathShadowPlan currentRenderPath;
    GpuFrameResources gpu;

    void Reset() noexcept
    {
        requests.clear();
        shadowedLightIndices.clear();
        currentRenderPath.Reset();
        gpu.lightBuffer = BufferHandle::Invalid();
        gpu.viewBuffer = BufferHandle::Invalid();
        gpu.lightCount = 0u;
        gpu.viewCount = 0u;
        gpu.lightCapacity = 0u;
        gpu.viewCapacity = 0u;
    }

    void AddShadowedLightIndex(uint32_t visibleLightIndex) noexcept
    {
        for (uint32_t index : shadowedLightIndices)
        {
            if (index == visibleLightIndex)
                return;
        }
        shadowedLightIndices.push_back(visibleLightIndex);
    }

    [[nodiscard]] bool IsVisibleLightShadowed(uint32_t visibleLightIndex) const noexcept
    {
        for (uint32_t index : shadowedLightIndices)
        {
            if (index == visibleLightIndex)
                return true;
        }
        return false;
    }

    void ClearCurrentRenderPathRequests() noexcept
    {
        currentRenderPath.Reset();
    }

    void AddCurrentRenderPathRequest(size_t requestIndex, uint32_t visibleLightIndex) noexcept
    {
        for (size_t currentIndex : currentRenderPath.requestIndices)
        {
            if (currentIndex == requestIndex)
                return;
        }

        currentRenderPath.requestIndices.push_back(requestIndex);
        currentRenderPath.visibleLightIndices.push_back(visibleLightIndex);
        if (currentRenderPath.primaryRequestIndex >= requests.size())
        {
            currentRenderPath.primaryRequestIndex = requestIndex;
            currentRenderPath.primaryVisibleLightIndex = visibleLightIndex;
        }
    }

    [[nodiscard]] bool HasCurrentRenderPathRequests() const noexcept
    {
        return !currentRenderPath.requestIndices.empty();
    }

    [[nodiscard]] bool HasCurrentRenderPathPrimaryRequest() const noexcept
    {
        return currentRenderPath.primaryRequestIndex < requests.size();
    }

    [[nodiscard]] bool HasCurrentRenderPathPrimaryVisibleLight() const noexcept
    {
        return currentRenderPath.primaryVisibleLightIndex != UINT32_MAX;
    }

    [[nodiscard]] ShadowRequest* GetCurrentRenderPathPrimaryRequest() noexcept
    {
        return HasCurrentRenderPathPrimaryRequest() ? &requests[currentRenderPath.primaryRequestIndex] : nullptr;
    }

    [[nodiscard]] const ShadowRequest* GetCurrentRenderPathPrimaryRequest() const noexcept
    {
        return HasCurrentRenderPathPrimaryRequest() ? &requests[currentRenderPath.primaryRequestIndex] : nullptr;
    }

    [[nodiscard]] uint32_t GetCurrentRenderPathPrimaryVisibleLightIndex() const noexcept
    {
        return currentRenderPath.primaryVisibleLightIndex;
    }

    [[nodiscard]] const ShadowView* GetCurrentRenderPathPrimaryView(uint32_t index = 0u) const noexcept
    {
        const ShadowRequest* request = GetCurrentRenderPathPrimaryRequest();
        if (!request || index >= request->views.size())
            return nullptr;
        return &request->views[index];
    }
};

} // namespace engine::addons::shadow
