#pragma once

#include "renderer/RendererTypes.hpp"

namespace engine::renderer {

class GpuResourceRuntime;

struct FrameGraphResourceBindings
{
    GpuResourceRuntime* gpuRuntime = nullptr;
    BufferHandle perFrameCB;
    BufferBinding perFrameBinding{};
    BufferHandle perObjectArena;
    uint32_t perObjectStride = 0u;

    // Screen-space ambient occlusion from the previous frame (Option B: GTAO is
    // produced AfterOpaque, so the opaque pass samples last frame's result and
    // attenuates only the indirect/ambient term). Invalid until GTAO has run once.
    TextureHandle ambientOcclusion;
};

} // namespace engine::renderer
