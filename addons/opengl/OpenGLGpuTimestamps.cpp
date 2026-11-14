// =============================================================================
// KROM Engine - addons/opengl/OpenGLGpuTimestamps.cpp
// =============================================================================
#include "OpenGLGpuTimestamps.hpp"
#include "core/Debug.hpp"
#include <algorithm>
#ifdef _WIN32
#   include "OpenGLWin32Loader.hpp"
#endif

#ifndef GL_TIMESTAMP
#   define GL_TIMESTAMP 0x8E28
#endif
#ifndef GL_QUERY_RESULT
#   define GL_QUERY_RESULT 0x8866
#endif
#ifndef GL_QUERY_RESULT_AVAILABLE
#   define GL_QUERY_RESULT_AVAILABLE 0x8867
#endif

namespace engine::renderer::opengl {

bool OpenGLGpuTimestamps::IsSupported() noexcept
{
#ifdef _WIN32
    return krom_glGenQueries && krom_glDeleteQueries && krom_glQueryCounter
        && krom_glGetQueryObjectiv && krom_glGetQueryObjectui64v;
#else
    return false;
#endif
}

void OpenGLGpuTimestamps::SetEnabled(bool enabled)
{
#ifdef _WIN32
    if (enabled == m_enabled)
        return;
    if (!enabled)
    {
        Release();
        return;
    }
    if (!IsSupported() || !wglGetCurrentContext())
    {
        Debug::LogWarning("OpenGLGpuTimestamps.cpp: timer queries unavailable, GPU timing stays off");
        return;
    }
    m_sets.assign(kFrameSets, FrameSet{});
    for (FrameSet& set : m_sets)
    {
        set.queries.assign(kCapacity, 0u);
        glGenQueries(static_cast<GLsizei>(kCapacity), set.queries.data());
    }
    m_current = 0u;
    m_enabled = true;
#else
    (void)enabled;
#endif
}

void OpenGLGpuTimestamps::Release() noexcept
{
#ifdef _WIN32
    if (IsSupported() && wglGetCurrentContext())
        for (FrameSet& set : m_sets)
            if (!set.queries.empty())
                glDeleteQueries(static_cast<GLsizei>(set.queries.size()), set.queries.data());
#endif
    m_sets.clear();
    m_current = 0u;
    m_enabled = false;
}

void OpenGLGpuTimestamps::BeginFrame(uint64_t frameId)
{
    if (!m_enabled || m_sets.empty())
        return;
    EndFrame();
    m_current = (m_current + 1u) % static_cast<uint32_t>(m_sets.size());
    FrameSet& set = m_sets[m_current];
    set.pending = false;  // results not read in time are lost
    set.frameId = frameId;
    set.written = 0u;
    set.open = true;
}

void OpenGLGpuTimestamps::EndFrame()
{
    if (!m_enabled || m_sets.empty())
        return;
    FrameSet& set = m_sets[m_current];
    if (!set.open)
        return;
    set.open = false;
    set.pending = set.written > 0u;
}

uint32_t OpenGLGpuTimestamps::Write()
{
#ifdef _WIN32
    if (!m_enabled || m_sets.empty())
        return kInvalidGpuTimestamp;
    FrameSet& set = m_sets[m_current];
    if (!set.open || set.written >= kCapacity)
        return kInvalidGpuTimestamp;
    glQueryCounter(set.queries[set.written], GL_TIMESTAMP);
    return set.written++;
#else
    return kInvalidGpuTimestamp;
#endif
}

void OpenGLGpuTimestamps::TakeCompleted(std::vector<GpuTimestampFrame>& out)
{
#ifdef _WIN32
    if (!m_enabled)
        return;
    std::vector<FrameSet*> ready;
    for (FrameSet& set : m_sets)
        if (set.pending)
            ready.push_back(&set);
    std::sort(ready.begin(), ready.end(), [](const FrameSet* a, const FrameSet* b) { return a->frameId < b->frameId; });

    for (FrameSet* set : ready)
    {
        // Queries complete in order: the last one available means all are.
        GLint available = 0;
        glGetQueryObjectiv(set->queries[set->written - 1u], GL_QUERY_RESULT_AVAILABLE, &available);
        if (!available)
            break;
        set->pending = false;

        GpuTimestampFrame frame;
        frame.frameId = set->frameId;
        frame.timestampsMs.assign(set->written, 0.0);
        frame.valid.assign(set->written, true);
        GLuint64 origin = 0u;
        for (uint32_t i = 0u; i < set->written; ++i)
        {
            GLuint64 ns = 0u;
            glGetQueryObjectui64v(set->queries[i], GL_QUERY_RESULT, &ns);
            if (i == 0u)
                origin = ns;
            frame.timestampsMs[i] = static_cast<double>(static_cast<int64_t>(ns - origin)) * 1e-6;
        }
        out.push_back(std::move(frame));
    }
#else
    (void)out;
#endif
}

} // namespace engine::renderer::opengl
