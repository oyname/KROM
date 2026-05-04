#pragma once
#include "assets/AssetRegistry.hpp"
#include "renderer/Environment.hpp"
#include "renderer/IBLBaker.hpp"
#include "renderer/IDevice.hpp"
#include "renderer/GpuResourceRuntime.hpp"
#include <string>
#include <unordered_map>

// Forward declaration — full header only needed in EnvironmentSystem.cpp.
namespace engine::jobs { class JobSystem; }

namespace engine::renderer {

class EnvironmentSystem
{
public:
    bool Initialize(IDevice& device, assets::AssetRegistry* assets);
    void Shutdown();

    void SetAssetRegistry(assets::AssetRegistry* assets) noexcept { m_assets = assets; }
    void SetGpuResourceRuntime(GpuResourceRuntime* runtime) noexcept { m_gpuRuntime = runtime; }

    // Set the directory where .iblcache files are stored.
    // Relative paths are resolved to an absolute path at first use.
    // Call before CreateEnvironment. If not called, the engine will assert/warn.
    void SetCacheDirectory(std::string dir) noexcept { m_cacheDir = std::move(dir); }

    // Optional: provide a running JobSystem to parallelize CPU baking.
    // Must outlive this EnvironmentSystem. Can be set at any time before baking.
    void SetJobSystem(jobs::JobSystem* js) noexcept { m_jobSystem = js; }

    [[nodiscard]] EnvironmentHandle CreateEnvironment(const EnvironmentDesc& desc);
    void DestroyEnvironment(EnvironmentHandle handle);

    void SetActiveEnvironment(EnvironmentHandle handle) noexcept;
    [[nodiscard]] EnvironmentHandle GetActiveEnvironment() const noexcept { return m_active; }

    [[nodiscard]] EnvironmentRuntimeState ResolveRuntimeState() const noexcept;

private:
    struct EnvironmentEntry
    {
        EnvironmentDesc desc{};
        TextureHandle environment = TextureHandle::Invalid();
        TextureHandle irradiance  = TextureHandle::Invalid();
        TextureHandle prefiltered = TextureHandle::Invalid();
        TextureHandle brdfLut     = TextureHandle::Invalid();
        IBLRuntimeMode iblMode    = IBLRuntimeMode::HDR;
    };

    IDevice*               m_device    = nullptr;
    GpuResourceRuntime*     m_gpuRuntime = nullptr;
    assets::AssetRegistry* m_assets    = nullptr;
    jobs::JobSystem*       m_jobSystem = nullptr;
    std::unordered_map<uint32_t, EnvironmentEntry> m_entries;
    EnvironmentHandle      m_active{};
    EnvironmentEntry       m_retiredActive{};
    TextureHandle          m_sharedBrdfLut = TextureHandle::Invalid();
    uint32_t               m_nextId        = 1u;
    std::string            m_cacheDir = ".krom/ibl";  // override via SetCacheDirectory()

    void DestroyEntry(EnvironmentEntry& entry);
};

} // namespace engine::renderer
