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
    AfterDepthPrepass,
    BeforeTransparent,
};

struct PassBuildContext;

class IFramePass
{
public:
    virtual ~IFramePass() = default;
    virtual uint32_t GetContributorId() const noexcept = 0;
    virtual PassPhase GetPhase() const noexcept = 0;
    virtual bool DeclaresResource(std::string_view /*name*/) const noexcept { return false; }
    // Changes whenever BuildPass would add other resources or passes (e.g. the
    // contributor was switched on or off); the frame graph is rebuilt then.
    virtual uint64_t GetStructureRevision() const noexcept { return 0u; }
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
        bool enableSelectionOutline = true;
        bool enableScreenSpaceReflections = true;
        // Sample count of the scene target (MSAA); extra opaque outputs
        // (FrameRecipe::sceneOutputs) must use it too.
        uint32_t msaaSamples = 1u;
        // Backend can bind extra colour targets (FrameRecipe::sceneOutputs).
        bool mrtSupported = false;
        // View allows a depth prepass. Contributors that list
        // FrameRecipe::opaqueInputs only do so when this is set.
        bool enableDepthPrepass = false;

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
