#pragma once

#include <cstdint>
#include <memory>
#include <string_view>

namespace engine::renderer {

struct RenderPipelineBuildContext;
struct RenderPipelineBuildResult;

class IRenderPipelineFeature
{
public:
    virtual ~IRenderPipelineFeature() = default;
    virtual std::string_view GetName() const noexcept = 0;
    virtual void OnDeviceShutdown() noexcept {}
    virtual bool Build(const RenderPipelineBuildContext& context,
                       RenderPipelineBuildResult& result) const = 0;
    // Changes whenever the pipeline's own settings change the graph it builds
    // (targets, passes). Part of the cached graph's key, so a change rebuilds
    // the graph without restarting.
    [[nodiscard]] virtual uint64_t GetStructureRevision() const noexcept { return 0u; }
};

using IRenderPipeline = IRenderPipelineFeature;
using RenderPipelinePtr = std::shared_ptr<const IRenderPipeline>;

} // namespace engine::renderer
