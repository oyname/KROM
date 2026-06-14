#pragma once

#include "renderer/RenderFeatureInterfaces.hpp"
#include "renderer/RenderWorldViews.hpp"
#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>

namespace engine::rendergraph { struct RGExecContext; }

namespace engine::renderer {

struct DrawList;
class FramePipelineCallbacks;
struct FrameGraphRuntimeBindings;
struct FrameRecipe;

enum class PassPhase : uint8_t
{
    BeforeOpaque = 0,
    AfterOpaque  = 1,
};

struct PassBuildContext;

class IFramePass
{
public:
    virtual ~IFramePass() = default;
    virtual uint32_t GetContributorId() const noexcept = 0;
    virtual PassPhase GetPhase() const noexcept = 0;
    virtual bool DeclaresResource(std::string_view /*name*/) const noexcept { return false; }
    virtual void BuildPass(const PassBuildContext& ctx) const = 0;
};

struct PassBuildContext
{
    struct PassFrameView
    {
        uint32_t viewportWidth = 0u;
        uint32_t viewportHeight = 0u;
        const RenderQueue* renderQueue = nullptr;
        std::shared_ptr<FrameGraphRuntimeBindings> runtimeBindings;
        bool enableAmbientOcclusion = true;

        [[nodiscard]] uint32_t ActiveShadowResolution() const noexcept
        {
            return renderQueue ? renderQueue->activeShadowResolution : 0u;
        }
    };

    PassFrameView frame;
    FrameRecipe& recipe;
    FramePipelineCallbacks& callbacks;
    std::function<void(const DrawList&, const rendergraph::RGExecContext&)> drawObjects;
};

} // namespace engine::renderer
