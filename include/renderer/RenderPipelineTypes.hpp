#pragma once

#include "rendergraph/RenderGraph.hpp"
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace engine::renderer {

using rendergraph::RGExecContext;
using rendergraph::RGResourceID;
using rendergraph::RG_INVALID_RESOURCE;

struct FramePipelineResources
{
    RGResourceID shadowMap       = RG_INVALID_RESOURCE;
    RGResourceID hdrSceneColor   = RG_INVALID_RESOURCE;

    RGResourceID bloomInput      = RG_INVALID_RESOURCE;
    RGResourceID bloomExtracted  = RG_INVALID_RESOURCE;
    RGResourceID bloomBlurH      = RG_INVALID_RESOURCE;
    RGResourceID bloomBlurV      = RG_INVALID_RESOURCE;

    RGResourceID tonemapped      = RG_INVALID_RESOURCE;
    RGResourceID uiOverlay       = RG_INVALID_RESOURCE;
    RGResourceID backbuffer      = RG_INVALID_RESOURCE;

    RGResourceID depthBuffer     = RG_INVALID_RESOURCE;
};

using FramePipelinePassCallback = std::function<void(const RGExecContext&)>;

struct FramePipelineCallbackEntry
{
    std::string name;
    FramePipelinePassCallback callback;
};

class FramePipelineCallbacks
{
public:
    FramePipelineCallbacks() = default;

    void Register(std::string_view name, FramePipelinePassCallback callback)
    {
        for (FramePipelineCallbackEntry& entry : m_entries)
        {
            if (entry.name == name)
            {
                entry.callback = std::move(callback);
                return;
            }
        }

        FramePipelineCallbackEntry entry{};
        entry.name = std::string(name);
        entry.callback = std::move(callback);
        m_entries.push_back(std::move(entry));
    }

    [[nodiscard]] bool Has(std::string_view name) const noexcept
    {
        return Find(name) != nullptr;
    }

    [[nodiscard]] const FramePipelinePassCallback* Find(std::string_view name) const noexcept
    {
        for (const FramePipelineCallbackEntry& entry : m_entries)
        {
            if (entry.name == name && static_cast<bool>(entry.callback))
                return &entry.callback;
        }
        return nullptr;
    }

    [[nodiscard]] const std::vector<FramePipelineCallbackEntry>& Entries() const noexcept
    {
        return m_entries;
    }

private:
    std::vector<FramePipelineCallbackEntry> m_entries;
};

} // namespace engine::renderer
