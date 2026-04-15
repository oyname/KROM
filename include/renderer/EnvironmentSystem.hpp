#pragma once
#include "assets/AssetRegistry.hpp"
#include "renderer/Environment.hpp"
#include "renderer/IDevice.hpp"
#include <unordered_map>

namespace engine::renderer {

class EnvironmentSystem
{
public:
    bool Initialize(IDevice& device, assets::AssetRegistry* assets);
    void Shutdown();

    void SetAssetRegistry(assets::AssetRegistry* assets) noexcept { m_assets = assets; }

    [[nodiscard]] EnvironmentHandle CreateEnvironment(const EnvironmentDesc& desc);
    void DestroyEnvironment(EnvironmentHandle handle);

    void SetActiveEnvironment(EnvironmentHandle handle) noexcept;
    [[nodiscard]] EnvironmentHandle GetActiveEnvironment() const noexcept { return m_active; }

    [[nodiscard]] EnvironmentRuntimeState ResolveRuntimeState() const noexcept;

private:
    struct EnvironmentEntry
    {
        EnvironmentDesc desc{};
        // GPU-abgeleitete IBL-Ressourcen aus einer 2D-Equirect-Quelle.
        // irradiance und prefiltered bleiben explizit getrennt fuer alle Backends
        // und spaeter auch fuer DX12.
        TextureHandle irradiance = TextureHandle::Invalid();
        TextureHandle prefiltered = TextureHandle::Invalid();
        TextureHandle brdfLut = TextureHandle::Invalid();
    };

    IDevice* m_device = nullptr;
    assets::AssetRegistry* m_assets = nullptr;
    std::unordered_map<uint32_t, EnvironmentEntry> m_entries;
    EnvironmentHandle m_active{};
    TextureHandle m_sharedBrdfLut = TextureHandle::Invalid();
    uint32_t m_nextId = 1u;

    void DestroyEntry(EnvironmentEntry& entry);
};

} // namespace engine::renderer
