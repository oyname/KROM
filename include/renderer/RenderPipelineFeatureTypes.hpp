#pragma once

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
};

using IRenderPipeline = IRenderPipelineFeature;
using RenderPipelinePtr = std::shared_ptr<const IRenderPipeline>;

} // namespace engine::renderer
