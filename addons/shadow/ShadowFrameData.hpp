#pragma once
// =============================================================================
// KROM Engine - addons/shadow/ShadowFrameData.hpp
// Per-Frame Shadow-Laufzeitdaten mit gemeinsamem Request/View-Vertrag.
// Der aktuelle Renderer konsumiert daraus vorerst einen ausgewaehlten Request,
// die CPU-Seite kann aber bereits mehrere Lichttypen einheitlich planen.
// =============================================================================
#include "addons/shadow/ShadowTypes.hpp"
#include <cstddef>
#include <vector>

namespace engine::addons::shadow {

struct ShadowFrameData
{
    std::vector<ShadowRequest> requests;
    size_t selectedRequestIndex = static_cast<size_t>(-1);

    void Reset() noexcept
    {
        requests.clear();
        selectedRequestIndex = static_cast<size_t>(-1);
    }

    [[nodiscard]] bool HasSelectedRequest() const noexcept
    {
        return selectedRequestIndex < requests.size();
    }

    [[nodiscard]] bool active() const noexcept
    {
        return HasSelectedRequest();
    }

    [[nodiscard]] ShadowRequest* GetSelectedRequest() noexcept
    {
        return HasSelectedRequest() ? &requests[selectedRequestIndex] : nullptr;
    }

    [[nodiscard]] const ShadowRequest* GetSelectedRequest() const noexcept
    {
        return HasSelectedRequest() ? &requests[selectedRequestIndex] : nullptr;
    }

    [[nodiscard]] const ShadowView* GetSelectedView(uint32_t index = 0u) const noexcept
    {
        const ShadowRequest* request = GetSelectedRequest();
        if (!request || index >= request->views.size())
            return nullptr;
        return &request->views[index];
    }
};

} // namespace engine::addons::shadow
