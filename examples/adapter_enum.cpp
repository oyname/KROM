#include "krom.h"
#include "core/Debug.hpp"

int main()
{
    engine::Debug::ResetMinLevelForBuild();

    const int backends[] = {
        Krom::Renderer::DX11,
        Krom::Renderer::DX12,
        Krom::Renderer::OpenGL,
        Krom::Renderer::Vulkan,
    };

    for (int backend : backends)
    {
        if (!Krom::IsRendererAvailable(backend))
        {
            engine::Debug::Log("%-8s  nicht verfuegbar", Krom::RendererName(backend));
            continue;
        }

        const auto adapters = Krom::EnumerateAdapters(backend);
        engine::Debug::Log("=== %s (%zu Adapter) ===",
            Krom::RendererName(backend), adapters.size());

        for (const auto& a : adapters)
        {
            const size_t vramMB = a.dedicatedVRAM / (1024u * 1024u);
            engine::Debug::Log("  [%u] %-40s  %s  VRAM: %4zu MB  FeatureLevel: %d",
                a.index,
                a.name.c_str(),
                a.isDiscrete ? "discrete  " : "integrated",
                vramMB,
                a.featureLevel);
        }
    }

    return 0;
}
