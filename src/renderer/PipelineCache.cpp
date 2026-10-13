// =============================================================================
// KROM Engine - src/renderer/PipelineCache.cpp
// =============================================================================
#include "renderer/PipelineCache.hpp"
#include "core/Debug.hpp"

namespace engine::renderer {

PipelineHandle PipelineCache::GetOrCreate(
    const PipelineKey& key,
    const std::function<PipelineHandle(const PipelineKey&)>& factory)
{
    auto it = m_cache.find(key);
    if (it != m_cache.end())
    {
        ++m_hits;
        return it->second;
    }

    ++m_misses;
    PipelineHandle handle = factory(key);
    if (handle.IsValid())
    {
        m_cache.emplace(key, handle);
        Debug::LogVerbose("PipelineCache.cpp: Miss - created pipeline %u "
            "(cache size=%zu)", handle.value, m_cache.size());
    }
    else
    {
        Debug::LogError("PipelineCache.cpp: factory returned invalid handle");
    }
    return handle;
}

void PipelineCache::Insert(const PipelineKey& key, PipelineHandle handle)
{
    m_cache[key] = handle;
}

bool PipelineCache::Contains(const PipelineKey& key) const noexcept
{
    return m_cache.count(key) > 0u;
}

void PipelineCache::Clear() noexcept
{
    Debug::Log("PipelineCache.cpp: Clear - %zu entries invalidated", m_cache.size());
    m_cache.clear();
}

} // namespace engine::renderer
