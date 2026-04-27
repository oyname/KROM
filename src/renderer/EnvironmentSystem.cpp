#include "renderer/EnvironmentSystem.hpp"
#include "renderer/IBLBaker.hpp"
#include "renderer/IBLCacheSerializer.hpp"
#include "renderer/IBLResourceLoader.hpp"
#include "renderer/RendererTypes.hpp"
#include "renderer/Environment.hpp"
#include "jobs/JobSystem.hpp"
#include "FloatConvert.hpp"
#include "core/Debug.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

namespace engine::renderer {
    namespace {

        // =====================================================================
        // BRDF LUT — device-level shared resource, not cached to disk.
        // All cube-map math lives in IBLBaker.cpp.
        // =====================================================================
        constexpr float kPi    = 3.14159265358979323846f;
        constexpr float kTwoPi = 6.28318530717958647692f;
        constexpr uint32_t kBrdfLutSize    = 256u;
        constexpr uint32_t kBrdfLutSamples = 256u;

        struct F2 { float x=0.f; float y=0.f; };
        struct F3 { float x=0.f; float y=0.f; float z=0.f; };

        [[nodiscard]] static float Dot3(F3 a, F3 b) noexcept { return a.x*b.x+a.y*b.y+a.z*b.z; }
        [[nodiscard]] static F3 Normalize3(F3 v) noexcept
        { const float l=std::sqrt(std::max(Dot3(v,v),0.f)); return (l>1e-12f)?F3{v.x/l,v.y/l,v.z/l}:F3{0.f,0.f,1.f}; }
        [[nodiscard]] static F3 Cross3(F3 a, F3 b) noexcept
        { return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x}; }
        [[nodiscard]] static F3 Reflect3(F3 i, F3 n) noexcept
        { const float d=2.f*Dot3(n,i); return {i.x-n.x*d,i.y-n.y*d,i.z-n.z*d}; }

        [[nodiscard]] static float RdcVdC(uint32_t b) noexcept
        {
            b=(b<<16u)|(b>>16u); b=((b&0x55555555u)<<1u)|((b&0xAAAAAAAAu)>>1u);
            b=((b&0x33333333u)<<2u)|((b&0xCCCCCCCCu)>>2u);
            b=((b&0x0F0F0F0Fu)<<4u)|((b&0xF0F0F0F0u)>>4u);
            b=((b&0x00FF00FFu)<<8u)|((b&0xFF00FF00u)>>8u);
            return static_cast<float>(b)*2.3283064365386963e-10f;
        }
        [[nodiscard]] static F2 Hammersley(uint32_t i, uint32_t n) noexcept
        { return {static_cast<float>(i)/static_cast<float>(n), RdcVdC(i)}; }

        [[nodiscard]] static F3 ImportanceSampleGGX(F2 xi, F3 n, float r) noexcept
        {
            const float a=r*r, phi=kTwoPi*xi.x;
            const float cosT=std::sqrt((1.f-xi.y)/std::max(1.f+(a*a-1.f)*xi.y,1e-6f));
            const float sinT=std::sqrt(std::max(1.f-cosT*cosT,0.f));
            const F3 hT{std::cos(phi)*sinT,std::sin(phi)*sinT,cosT};
            const F3 up=(std::abs(n.z)<0.999f)?F3{0.f,0.f,1.f}:F3{1.f,0.f,0.f};
            const F3 t=Normalize3(Cross3(up,n)), bt=Cross3(n,t);
            return Normalize3({t.x*hT.x+bt.x*hT.y+n.x*hT.z,t.y*hT.x+bt.y*hT.y+n.y*hT.z,t.z*hT.x+bt.z*hT.y+n.z*hT.z});
        }
        [[nodiscard]] static float GSchlickGGX(float NoV, float r) noexcept
        { const float k=(r+1.f)*(r+1.f)/8.f; return NoV/std::max(NoV*(1.f-k)+k,1e-6f); }

        [[nodiscard]] static std::vector<uint16_t> BuildBrdfLut()
        {
            using internal::FloatToHalf;
            std::vector<uint16_t> lut(static_cast<size_t>(kBrdfLutSize)*kBrdfLutSize*4u, 0u);
            for (uint32_t y=0u; y<kBrdfLutSize; ++y)
                for (uint32_t x=0u; x<kBrdfLutSize; ++x)
                {
                    const float NoV  = std::max((static_cast<float>(x)+0.5f)/static_cast<float>(kBrdfLutSize),1e-4f);
                    const float r    = (static_cast<float>(y)+0.5f)/static_cast<float>(kBrdfLutSize);
                    const F3 V{std::sqrt(std::max(1.f-NoV*NoV,0.f)),0.f,NoV}, N{0.f,0.f,1.f};
                    float A=0.f, B=0.f;
                    for (uint32_t i=0u; i<kBrdfLutSamples; ++i)
                    {
                        const F2 xi=Hammersley(i,kBrdfLutSamples);
                        const F3 H=ImportanceSampleGGX(xi,N,r);
                        const F3 Lr=Reflect3({-V.x,-V.y,-V.z},H);
                        const float NoL=std::max(Lr.z,0.f), NoH=std::max(H.z,0.f), HoV=std::max(Dot3(H,V),0.f);
                        if (NoL<=0.f) continue;
                        const float G=GSchlickGGX(NoV,r)*GSchlickGGX(NoL,r);
                        const float GVis=(G*HoV)/std::max(NoH*NoV,1e-4f);
                        const float Fc=std::pow(1.f-HoV,5.f);
                        A+=(1.f-Fc)*GVis; B+=Fc*GVis;
                    }
                    const size_t idx=(static_cast<size_t>(y)*kBrdfLutSize+x)*4u;
                    lut[idx+0]=FloatToHalf(A/static_cast<float>(kBrdfLutSamples));
                    lut[idx+1]=FloatToHalf(B/static_cast<float>(kBrdfLutSamples));
                    lut[idx+2]=FloatToHalf(0.f); lut[idx+3]=FloatToHalf(1.f);
                }
            return lut;
        }

        [[nodiscard]] static TextureHandle UploadBrdfLut(IDevice& device)
        {
            const std::vector<uint16_t> data = BuildBrdfLut();
            TextureDesc desc{};
            desc.width=kBrdfLutSize; desc.height=kBrdfLutSize;
            desc.format=Format::RGBA16_FLOAT;
            desc.usage=ResourceUsage::ShaderResource|ResourceUsage::CopyDest;
            desc.initialState=ResourceState::ShaderRead;
            desc.debugName="IBL_BRDFLUT";
            const TextureHandle h = device.CreateTexture(desc);
            if (h.IsValid()) device.UploadTextureData(h, data.data(), data.size()*sizeof(uint16_t), 0u, 0u);
            return h;
        }

        // =====================================================================
        // Path helpers — resolve to absolute and log diagnostics
        // =====================================================================
        [[nodiscard]] static std::string ResolveAbsoluteCacheDir(const std::string& configured,
                                                                   bool isDefault)
        {
            namespace fs = std::filesystem;

            std::error_code ec;
            std::string cwd;
            const fs::path cwdPath = fs::current_path(ec);
            if (ec)
            {
                Debug::LogWarning("IBLCache: current_path failed: %s", ec.message().c_str());
                ec.clear();
            }
            else
            {
                cwd = cwdPath.string();
            }

            const fs::path configuredPath(configured);
            const fs::path absPath = fs::absolute(configuredPath, ec);
            std::string resolved = configured;
            if (ec)
            {
                Debug::LogWarning("IBLCache: absolute() failed for '%s': %s",
                                  configured.c_str(), ec.message().c_str());
                ec.clear();
            }
            else
            {
                resolved = absPath.string();
            }

            Debug::Log("IBLCache: configured   = %s", configured.c_str());
            Debug::Log("IBLCache: working dir  = %s", cwd.empty() ? "<unavailable>" : cwd.c_str());
            if (isDefault)
                Debug::LogWarning("IBLCache: cache dir    = %s  [default — call SetCacheDirectory() to override]",
                                  resolved.c_str());
            else
                Debug::Log("IBLCache: cache dir    = %s", resolved.c_str());
            return resolved;
        }

        [[nodiscard]] static std::string BuildCachePath(const std::string& absDir, uint64_t hash)
        {
            char hashStr[32];
            std::snprintf(hashStr, sizeof(hashStr), "%016llx",
                          static_cast<unsigned long long>(hash));
            const std::string path = absDir + "/" + hashStr + ".iblcache";
            Debug::Log("IBLCache: cache file   = %s", path.c_str());
            return path;
        }

        // =====================================================================
        // Timing
        // =====================================================================
        using Clock = std::chrono::steady_clock;
        [[nodiscard]] static float ElapsedMs(Clock::time_point t0) noexcept
        { return std::chrono::duration<float,std::milli>(Clock::now()-t0).count(); }

    } // namespace

    // =========================================================================
    // EnvironmentSystem — public methods
    // =========================================================================

    bool EnvironmentSystem::Initialize(IDevice& device, assets::AssetRegistry* assets)
    {
        m_device = &device;
        m_assets = assets;
        if (!m_sharedBrdfLut.IsValid())
            m_sharedBrdfLut = UploadBrdfLut(device);
        return m_sharedBrdfLut.IsValid();
    }

    void EnvironmentSystem::DestroyEntry(EnvironmentEntry& entry)
    {
        if (!m_device) return;

        const uint64_t retireFence = m_gpuRuntime ? m_gpuRuntime->GetRetirementFenceForCurrentFrame() : 0u;
        auto destroyTexture = [&](TextureHandle texture)
        {
            if (!texture.IsValid())
                return;
            if (m_gpuRuntime)
                m_gpuRuntime->ScheduleDestroy(texture, retireFence);
            else
                m_device->DestroyTexture(texture);
        };

        destroyTexture(entry.environment);
        destroyTexture(entry.irradiance);
        destroyTexture(entry.prefiltered);
        entry = {};
    }

    void EnvironmentSystem::Shutdown()
    {
        for (auto& [_, entry] : m_entries) DestroyEntry(entry);
        m_entries.clear();
        DestroyEntry(m_retiredActive);
        if (m_device && m_sharedBrdfLut.IsValid())
            m_device->DestroyTexture(m_sharedBrdfLut);
        m_device        = nullptr;
        m_gpuRuntime    = nullptr;
        m_assets        = nullptr;
        m_sharedBrdfLut = TextureHandle::Invalid();
        m_active        = {};
        m_nextId        = 1u;
    }

    EnvironmentHandle EnvironmentSystem::CreateEnvironment(const EnvironmentDesc& desc)
    {
        if (!m_device)             return {};
        if (desc.mode == EnvironmentMode::None) return {};

        // -----------------------------------------------------------------
        // Validate source for Texture mode
        // -----------------------------------------------------------------
        const assets::TextureAsset* sourceAsset = nullptr;
        if (desc.mode == EnvironmentMode::Texture)
        {
            if (!m_assets || !desc.sourceTexture.IsValid())
            {
                Debug::LogError("EnvironmentSystem: Texture mode requires a valid sourceTexture");
                return {};
            }
            sourceAsset = m_assets->textures.Get(desc.sourceTexture);
            if (!sourceAsset || sourceAsset->state != assets::AssetState::Loaded
                             || sourceAsset->pixelData.empty())
            {
                Debug::LogError("EnvironmentSystem: source texture not loaded or empty");
                return {};
            }
        }

        // -----------------------------------------------------------------
        // Absolute path resolution + diagnostics (logged once per call)
        // -----------------------------------------------------------------
        constexpr std::string_view kDefaultCacheDir = ".krom/ibl";
        const std::string absDir = ResolveAbsoluteCacheDir(m_cacheDir, m_cacheDir == kDefaultCacheDir);
        const uint32_t removedCaches = IBLCacheSerializer::CleanupDirectory(absDir);
        if (removedCaches > 0u)
            Debug::Log("IBLCache: cleanup removed %u stale/incompatible file(s)", removedCaches);

        // -----------------------------------------------------------------
        // Cache key
        // -----------------------------------------------------------------
        const IBLBakeParams params{ .jobSystem = m_jobSystem };
        IBLCacheKey key{};
        key.environmentSize = params.environmentSize;
        key.irradianceSize  = params.irradianceSize;
        key.prefilterSize   = params.prefilterBaseSize;
        key.mipCount        = params.prefilterMipCount;

        if (desc.mode == EnvironmentMode::Texture)
            key.sourceHash = IBLBaker::HashTextureSource(*sourceAsset, desc.intensity);
        else
            key.sourceHash = IBLBaker::HashProceduralSky(desc.skyDesc, desc.intensity);

        const std::string cachePath = BuildCachePath(absDir, key.sourceHash);

        // -----------------------------------------------------------------
        // Cache lookup
        // -----------------------------------------------------------------
        IBLBakedData data{};

        // Distinguish: file doesn't exist vs. file exists but is invalid
        {
            namespace fs = std::filesystem;
            std::error_code ec;
            const bool fileExists = fs::exists(cachePath, ec);

            if (!fileExists)
            {
                Debug::Log("IBLCache: MISS  '%s' — file not found", cachePath.c_str());
            }
            else if (IBLCacheSerializer::IsValid(cachePath, key))
            {
                const auto t0 = Clock::now();
                if (IBLCacheSerializer::Read(cachePath, data))
                {
                    Debug::Log("IBLCache: HIT   '%s' (%.0f ms)", cachePath.c_str(), ElapsedMs(t0));
                }
                else
                {
                    Debug::LogWarning("IBLCache: read failed '%s' — rebaking", cachePath.c_str());
                    data = {};
                }
            }
            // else: IsValid already logged the INVALID reason
        }

        // -----------------------------------------------------------------
        // Bake (on miss or invalid)
        // -----------------------------------------------------------------
        if (!data.IsValid())
        {
            const uint32_t workers = (m_jobSystem && m_jobSystem->IsParallel())
                ? m_jobSystem->WorkerCount() : 0u;
            Debug::Log("IBLCache: baking  (threads: %u) ...", workers);

            const auto t0 = Clock::now();
            if (desc.mode == EnvironmentMode::Texture)
                data = IBLBaker::BakeFromTexture(*sourceAsset, desc.intensity, params);
            else
                data = IBLBaker::BakeFromProceduralSky(desc.skyDesc, desc.intensity, params);

            Debug::Log("IBLCache: bake    %.0f ms", ElapsedMs(t0));

            if (!data.IsValid())
            {
                Debug::LogError("EnvironmentSystem: bake produced no data");
                return {};
            }

            // Write cache (IBLCacheSerializer logs success/failure + file size)
            const bool wroteCache = IBLCacheSerializer::Write(cachePath, data, key);
            (void)wroteCache;
        }

        // -----------------------------------------------------------------
        // GPU upload
        // -----------------------------------------------------------------
        const auto t0 = Clock::now();
        IBLGpuResources gpu = IBLResourceLoader::Upload(*m_device, data);
        Debug::Log("IBLCache: upload  %.0f ms", ElapsedMs(t0));

        if (!gpu.IsValid())
        {
            IBLResourceLoader::Destroy(*m_device, gpu);
            Debug::LogError("EnvironmentSystem: GPU upload failed");
            return {};
        }

        // -----------------------------------------------------------------
        // Store entry
        // -----------------------------------------------------------------
        EnvironmentEntry entry{};
        entry.desc        = desc;
        entry.environment = gpu.environment;
        entry.irradiance  = gpu.irradiance;
        entry.prefiltered = gpu.prefiltered;
        entry.brdfLut     = m_sharedBrdfLut;

        EnvironmentHandle handle{ m_nextId++ };
        m_entries.emplace(handle.id, std::move(entry));
        return handle;
    }

    void EnvironmentSystem::DestroyEnvironment(EnvironmentHandle handle)
    {
        const auto it = m_entries.find(handle.id);
        if (it == m_entries.end()) return;

        if (m_active == handle)
        {
            DestroyEntry(m_retiredActive);
            m_retiredActive = std::move(it->second);
            m_active = {};
            m_entries.erase(it);
            return;
        }

        DestroyEntry(it->second);
        m_entries.erase(it);
    }

    void EnvironmentSystem::SetActiveEnvironment(EnvironmentHandle handle) noexcept
    {
        if (!handle.IsValid())
        {
            m_active = {};
            DestroyEntry(m_retiredActive);
            return;
        }

        if (m_entries.find(handle.id) != m_entries.end())
        {
            m_active = handle;
            DestroyEntry(m_retiredActive);
        }
    }

    EnvironmentRuntimeState EnvironmentSystem::ResolveRuntimeState() const noexcept
    {
        EnvironmentRuntimeState state{};

        const EnvironmentEntry* entry = nullptr;
        const auto it = m_entries.find(m_active.id);
        if (it != m_entries.end())
            entry = &it->second;
        else if (m_retiredActive.environment.IsValid()
              || m_retiredActive.irradiance.IsValid()
              || m_retiredActive.prefiltered.IsValid()
              || m_retiredActive.brdfLut.IsValid())
            entry = &m_retiredActive;

        if (!entry)
            return state;

        state.environment = entry->environment;
        state.irradiance  = entry->irradiance;
        state.prefiltered = entry->prefiltered;
        state.brdfLut     = entry->brdfLut;
        state.intensity   = entry->desc.intensity;
        state.mode        = entry->desc.mode;
        state.active      = entry->desc.enableIBL
                         && state.environment.IsValid()
                         && state.irradiance.IsValid()
                         && state.prefiltered.IsValid()
                         && state.brdfLut.IsValid();
        return state;
    }

} // namespace engine::renderer
