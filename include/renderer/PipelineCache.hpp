#pragma once
// =============================================================================
// KROM Engine - renderer/PipelineCache.hpp
// API-neutraler PipelineCache: PipelineKey → PipelineHandle.
//
// Backends cachen erzeugte Pipeline-Objekte (D3D12 PSO, Vulkan Pipeline,
// DX11 ShaderSet+BlendState+etc.) hinter diesem Cache. PipelineKey dient
// als deterministischer Hash-Schlüssel.
//
// Nutzung im Backend:
//   PipelineHandle GetOrCreate(const PipelineKey& key,
//       std::function<PipelineHandle(const PipelineKey&)> factory);
//
// Nutzung in der Draw-Schleife (Null-Backend oder echtes):
//   auto pipeline = m_pipelineCache.GetOrCreate(mat.pipelineKey,
//       [&](const PipelineKey& k) { return device.CreatePipeline(BuildDesc(k)); });
//   cmd.SetPipeline(pipeline);
//
// Deklaration. Implementierung: src/renderer/PipelineCache.cpp
// =============================================================================
#include "renderer/MaterialSystem.hpp"
#include <functional>
#include <unordered_map>
#include <vector>

namespace engine::renderer {

class PipelineCache
{
public:
    PipelineCache()  = default;
    ~PipelineCache() = default;

    // Gibt gecachten Handle zurück oder ruft factory() auf und cached das Ergebnis.
    [[nodiscard]] PipelineHandle GetOrCreate(
        const PipelineKey& key,
        const std::function<PipelineHandle(const PipelineKey&)>& factory);

    // Explizite Eintrag-Einfügung (für Pre-Warming)
    void Insert(const PipelineKey& key, PipelineHandle handle);

    // Prüft ob bereits gecacht
    [[nodiscard]] bool Contains(const PipelineKey& key) const noexcept;

    // Alle Handles ungültig machen (z.B. bei Device-Reset)
    void Clear() noexcept;

    [[nodiscard]] size_t Size()    const noexcept { return m_cache.size(); }
    [[nodiscard]] size_t HitCount()  const noexcept { return m_hits;   }
    [[nodiscard]] size_t MissCount() const noexcept { return m_misses; }
    void ResetStats() noexcept { m_hits = 0u; m_misses = 0u; }

    // Iteriert alle gecachten Einträge (z.B. für Shutdown-Cleanup)
    template<typename Func>
    void ForEach(Func&& fn) const
    {
        for (const auto& [key, handle] : m_cache)
            fn(key, handle);
    }

private:
    std::unordered_map<PipelineKey, PipelineHandle> m_cache;
    size_t m_hits   = 0u;
    size_t m_misses = 0u;
};

} // namespace engine::renderer
