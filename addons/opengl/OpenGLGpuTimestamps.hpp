#pragma once
// =============================================================================
// KROM Engine - addons/opengl/OpenGLGpuTimestamps.hpp
// GPU timestamps for OpenGL (GL 3.3 core: glQueryCounter with GL_TIMESTAMP).
// A ring of query sets, one per device frame. Results are read only once the
// last query of a frame reports GL_QUERY_RESULT_AVAILABLE, so the CPU never
// waits for the GPU. Needs a current GL context for every call.
// =============================================================================
#include "renderer/IDevice.hpp"
#include <cstdint>
#include <vector>

namespace engine::renderer::opengl {

class OpenGLGpuTimestamps
{
public:
    static constexpr uint32_t kCapacity = 256u;
    static constexpr uint32_t kFrameSets = 5u;

    [[nodiscard]] static bool IsSupported() noexcept;

    void SetEnabled(bool enabled);
    [[nodiscard]] bool IsEnabled() const noexcept { return m_enabled; }
    // Deletes the queries if a GL context is current, otherwise forgets them.
    void Release() noexcept;

    void BeginFrame(uint64_t frameId);
    void EndFrame();
    uint32_t Write();
    void TakeCompleted(std::vector<GpuTimestampFrame>& out);

private:
    struct FrameSet
    {
        std::vector<uint32_t> queries;
        uint64_t              frameId = 0u;
        uint32_t              written = 0u;
        bool                  open    = false;
        bool                  pending = false;
    };

    std::vector<FrameSet> m_sets;
    uint32_t              m_current = 0u;
    bool                  m_enabled = false;
};

} // namespace engine::renderer::opengl
