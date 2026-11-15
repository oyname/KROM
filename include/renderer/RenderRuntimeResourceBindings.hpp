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
    BufferHandle perObjectStructured;
};

} // namespace engine::renderer
