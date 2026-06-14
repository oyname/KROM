#pragma once

#include "core/Types.hpp"
#include "renderer/BackgroundMode.hpp"
#include "renderer/RenderFeatureInterfaces.hpp"
#include "renderer/RenderPipelineFeatureTypes.hpp"
#include <array>
#include <cstdint>
#include <vector>

namespace engine::events {
class EventBus;
}

namespace engine::rendergraph {
class RenderGraph;
}

namespace engine::renderer {

class FramePipelineCallbacks;
class MaterialSystem;
class ShaderRuntime;
struct FrameGraphRuntimeBindings;
struct RenderQueue;

struct RenderPipelineBuildContext
{
    rendergraph::RenderGraph& renderGraph;
    uint32_t viewportWidth = 0u;
    uint32_t viewportHeight = 0u;
    RenderTargetHandle backbufferRT;
    TextureHandle backbufferTex;
    bool presentOutput = true;
    const RenderQueue& renderQueue;
    ShaderRuntime& shaderRuntime;
    const MaterialSystem& materials;
    BufferHandle perFrameCB;
    MaterialHandle defaultTonemapMaterial;
    const MaterialSystem* tonemapMaterialSystem = nullptr;
    events::EventBus* eventBus = nullptr;
    const FramePipelineCallbacks& externalCallbacks;
    std::shared_ptr<FrameGraphRuntimeBindings> runtimeBindings;
    BackgroundMode backgroundMode = BackgroundMode::ClearColor;
    bool enableBloom = true;
    std::array<float, 4> clearColor{0.3f, 0.3f, 0.3f, 1.f};
    std::vector<const IPassContributor*> passContributors{};
    bool enableAmbientOcclusion = true;
};

struct RenderPipelineBuildResult
{
    uint32_t backbuffer = UINT32_MAX;
};

} // namespace engine::renderer
