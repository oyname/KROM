#include "renderer/ShaderRuntime.hpp"
#include "jobs/JobSystem.hpp"
#include <algorithm>
#include <cstring>

namespace engine::renderer {

    namespace {

    // "pbr_forward_plus.ps.hlsl" -> "pbr_forward_plus.ps" for the progress display.
    std::string ShaderDisplayName(const assets::ShaderAsset& asset)
    {
        std::string name = asset.debugName.empty() ? asset.path : asset.debugName;
        if (const size_t slash = name.find_last_of("/\\"); slash != std::string::npos)
            name.erase(0u, slash + 1u);
        for (const char* ext : { ".hlsl", ".glsl" })
        {
            const size_t len = std::strlen(ext);
            if (name.size() > len && name.compare(name.size() - len, len, ext) == 0)
            {
                name.erase(name.size() - len);
                break;
            }
        }
        return name;
    }

    std::string JoinDefines(ShaderVariantFlag flags)
    {
        std::string out;
        for (const std::string& define : ShaderCompiler::VariantFlagsToDefines(flags))
        {
            if (!out.empty())
                out += ' ';
            out += define;
        }
        return out;
    }

    } // namespace

    ShaderHandle ShaderRuntime::GetOrCreateVariant(ShaderHandle shaderAssetHandle,
        ShaderPassType pass,
        ShaderVariantFlag flags,
        bool* outPending)
    {
        if (outPending)
            *outPending = false;
        if (!m_device || !m_assets || !shaderAssetHandle.IsValid())
            return ShaderHandle::Invalid();

        auto* shaderAsset = m_assets->shaders.Get(shaderAssetHandle);
        if (!shaderAsset)
            return ShaderHandle::Invalid();

        const ShaderVariantKey key = ShaderVariantKey{ shaderAssetHandle, pass, flags }.Normalized();
        const assets::ShaderTargetProfile target = ShaderCompiler::ResolveTargetProfile(*m_device);
        // Nothing to compile (only a precompiled artifact): fails at once, no job needed.
        const bool hasSource = !shaderAsset->sourceCode.empty() || !shaderAsset->bytecode.empty();
        if (!AsyncShaderCompilesEnabled() || !hasSource)
            return m_variantCache.GetOrCreate(*shaderAsset, target, key);

        if (const ShaderHandle cached = m_variantCache.Find(key); cached.IsValid())
            return cached;
        if (m_failedAsyncVariants.count(key) != 0u)
            return ShaderHandle::Invalid();
        if (FindAsyncCompile(key, false))
        {
            if (outPending)
                *outPending = true;
            return ShaderHandle::Invalid();
        }

        // On-disk cache hit: cheap, take it right away (same as the synchronous path).
        assets::CompiledShaderArtifact artifact;
        if (ShaderCompiler::LoadCachedVariant(*shaderAsset, target, key.flags, artifact))
        {
            const ShaderHandle handle = m_variantCache.Store(*shaderAsset, key, artifact);
            if (!handle.IsValid())
                m_failedAsyncVariants.insert(key);
            return handle;
        }

        QueueAsyncCompile(*shaderAsset, target, key, false, 0ull);
        if (outPending)
            *outPending = true;
        return ShaderHandle::Invalid();
    }

    bool ShaderRuntime::AsyncShaderCompilesEnabled() const noexcept
    {
        // One worker stays free for the frame jobs: RenderFrameOrchestrator waits
        // on two of them, and a compile can occupy a worker for many seconds.
        return m_asyncCompileJobs && m_asyncCompileJobs->WorkerCount() >= 2u;
    }

    uint64_t ShaderRuntime::ShaderSourceHash(const assets::ShaderAsset& asset) noexcept
    {
        return HashBytes(asset.sourceCode.data(), asset.sourceCode.size()) ^
               (HashBytes(asset.bytecode.data(), asset.bytecode.size()) * 31ull);
    }

    const ShaderRuntime::AsyncCompileEntry* ShaderRuntime::FindAsyncCompile(const ShaderVariantKey& key,
                                                                            bool baseAsset) const noexcept
    {
        for (const AsyncCompileEntry& entry : m_asyncCompiles)
            if (entry.baseAsset == baseAsset && entry.key == key)
                return &entry;
        return nullptr;
    }

    bool ShaderRuntime::IsShaderCompilePending(ShaderHandle shaderAssetHandle) const noexcept
    {
        for (const AsyncCompileEntry& entry : m_asyncCompiles)
            if (entry.key.baseShader == shaderAssetHandle)
                return true;
        return false;
    }

    void ShaderRuntime::QueueAsyncCompile(const assets::ShaderAsset& asset,
                                          assets::ShaderTargetProfile target,
                                          const ShaderVariantKey& key,
                                          bool baseAsset,
                                          uint64_t sourceHash)
    {
        auto job = std::make_shared<AsyncCompileJob>();
        job->asset = asset;
        job->asset.compiledArtifacts.clear();
        job->target = target;
        job->flags = baseAsset ? ShaderVariantFlag::None : key.flags;

        AsyncCompileEntry entry{};
        entry.key = key;
        entry.baseAsset = baseAsset;
        entry.sourceHash = sourceHash;
        entry.shaderName = ShaderDisplayName(asset);
        entry.defines = JoinDefines(job->flags);
        entry.job = std::move(job);
        m_asyncCompiles.push_back(std::move(entry));
        ++m_asyncBatchTotal;
        Debug::LogVerbose("ShaderRuntime: compiling '%s' [%s] in the background",
                          m_asyncCompiles.back().shaderName.c_str(), m_asyncCompiles.back().defines.c_str());
        DispatchQueuedAsyncCompiles();
    }

    void ShaderRuntime::DispatchQueuedAsyncCompiles()
    {
        if (!AsyncShaderCompilesEnabled())
            return;
        // Half the workers at most: the editor stays responsive and frame jobs
        // still find free workers.
        const uint32_t maxRunning = std::max(1u, m_asyncCompileJobs->WorkerCount() / 2u);
        uint32_t running = 0u;
        for (const AsyncCompileEntry& entry : m_asyncCompiles)
            if (entry.dispatched && !entry.job->done.load(std::memory_order_acquire))
                ++running;

        for (AsyncCompileEntry& entry : m_asyncCompiles)
        {
            if (running >= maxRunning)
                break;
            if (entry.dispatched)
                continue;
            entry.dispatched = true;
            ++running;
            std::shared_ptr<AsyncCompileJob> job = entry.job;
            const bool queued = m_asyncCompileJobs->Dispatch([job]() {
                // CPU work only: no device, registry or runtime access here.
                try
                {
                    job->ok = ShaderCompiler::CompileVariant(job->asset, job->target, job->flags,
                                                             job->artifact, &job->error);
                }
                catch (const std::exception& e)
                {
                    job->ok = false;
                    job->error = e.what();
                }
                catch (...)
                {
                    job->ok = false;
                    job->error = "unknown exception";
                }
                job->done.store(true, std::memory_order_release);
            }, jobs::JobPriority::Background);
            if (!queued)
            {
                job->ok = false;
                job->error = "job rejected: shutdown in progress";
                job->done.store(true, std::memory_order_release);
            }
        }
    }

    void ShaderRuntime::ApplyAsyncCompile(AsyncCompileEntry& entry)
    {
        AsyncCompileJob& job = *entry.job;
        auto* asset = m_assets ? m_assets->shaders.Get(entry.key.baseShader) : nullptr;
        if (!asset || !m_device)
            return;
        const assets::ShaderTargetProfile target = ShaderCompiler::ResolveTargetProfile(*m_device);
        if (target != job.target)
            return;

        if (entry.baseAsset)
        {
            const uint64_t sourceHash = ShaderSourceHash(*asset);
            if (sourceHash != entry.sourceHash)
                return; // source changed meanwhile: the next PrepareShaderAsset queues it again
            if (job.ok)
            {
                if (!FindCompiledArtifact(*asset))
                    asset->compiledArtifacts.push_back(std::move(job.artifact));
                return;
            }
            Debug::LogError("ShaderRuntime.cpp: failed to compile shader '%s' for %s: %s",
                asset->debugName.c_str(), ShaderCompiler::ToString(target), job.error.c_str());
            ShaderAssetStatus status{};
            status.assetHandle = entry.key.baseShader;
            status.stage = ToStageMask(asset->stage);
            status.target = target;
            status.failed = true;
            status.sourceHash = sourceHash;
            m_shaderAssets[entry.key.baseShader] = status;
            return;
        }

        if (!job.ok)
        {
            Debug::LogError("ShaderVariantCache: CompileVariant failed for '%s': %s",
                            asset->debugName.c_str(), job.error.c_str());
            m_failedAsyncVariants.insert(entry.key);
            return;
        }
        if (!m_variantCache.Store(*asset, entry.key, job.artifact).IsValid())
            m_failedAsyncVariants.insert(entry.key);
    }

    void ShaderRuntime::PumpAsyncShaderCompiles()
    {
        if (m_asyncCompiles.empty())
            return;
        if (!RequireRenderThread("PumpAsyncShaderCompiles"))
            return;

        bool applied = false;
        for (auto it = m_asyncCompiles.begin(); it != m_asyncCompiles.end();)
        {
            if (!it->dispatched || !it->job->done.load(std::memory_order_acquire))
            {
                ++it;
                continue;
            }
            ApplyAsyncCompile(*it);
            ++m_asyncBatchCompleted;
            applied = true;
            it = m_asyncCompiles.erase(it);
        }
        if (applied)
            ++m_asyncCompileRevision;

        DispatchQueuedAsyncCompiles();
        if (m_asyncCompiles.empty())
        {
            m_asyncBatchCompleted = 0u;
            m_asyncBatchTotal = 0u;
        }
    }

    ShaderCompileProgress ShaderRuntime::GetShaderCompileProgress() const
    {
        ShaderCompileProgress progress{};
        progress.completed = m_asyncBatchCompleted;
        progress.total = m_asyncBatchTotal;
        for (const AsyncCompileEntry& entry : m_asyncCompiles)
        {
            if (!entry.dispatched)
                continue;
            if (progress.running++ == 0u)
            {
                progress.shaderName = entry.shaderName;
                progress.defines = entry.defines;
            }
        }
        if (progress.running == 0u && !m_asyncCompiles.empty())
        {
            progress.shaderName = m_asyncCompiles.front().shaderName;
            progress.defines = m_asyncCompiles.front().defines;
        }
        return progress;
    }

    bool ShaderRuntime::CollectShaderRequests(const IShaderMaterialSource& materials,
        std::vector<ShaderHandle>& outRequests) const
    {
        outRequests.clear();
        outRequests.reserve(materials.DescCount() * 2u);

        for (uint32_t i = 0; i < materials.DescCount(); ++i)
        {
            const MaterialHandle material = MaterialHandle::Make(i, 1u);
            const MaterialRuntimeDesc* runtime = materials.GetRuntimeDesc(material);
            if (!runtime)
                continue;
            if (runtime->vertexShader.IsValid())
                outRequests.push_back(runtime->vertexShader);
            if (runtime->fragmentShader.IsValid())
                outRequests.push_back(runtime->fragmentShader);
        }

        std::sort(outRequests.begin(), outRequests.end(), [](const ShaderHandle& a, const ShaderHandle& b) {
            return a.value < b.value;
            });
        outRequests.erase(std::unique(outRequests.begin(), outRequests.end(), [](const ShaderHandle& a, const ShaderHandle& b) {
            return a == b;
            }), outRequests.end());
        return true;
    }

    bool ShaderRuntime::CollectMaterialRequests(const IShaderMaterialSource& materials,
        std::vector<MaterialHandle>& outRequests) const
    {
        outRequests.clear();

        outRequests.reserve(materials.DescCount());

        for (uint32_t i = 0; i < materials.DescCount(); ++i)
        {
            const MaterialHandle material = MaterialHandle::Make(i, 1u);
            if (materials.GetDesc(material))
                outRequests.push_back(material);
        }

        std::sort(outRequests.begin(), outRequests.end(), [](const MaterialHandle& a, const MaterialHandle& b) {
            return a.value < b.value;
            });
        outRequests.erase(std::unique(outRequests.begin(), outRequests.end(), [](const MaterialHandle& a, const MaterialHandle& b) {
            return a == b;
            }), outRequests.end());
        return true;
    }

    ShaderHandle ShaderRuntime::PrepareShaderAsset(ShaderHandle shaderAssetHandle)
    {
        if (!RequireRenderThread("PrepareShaderAsset"))
            return ShaderHandle::Invalid();
        if (!m_device || !shaderAssetHandle.IsValid())
            return ShaderHandle::Invalid();
        if (!m_assets)
            return shaderAssetHandle;

        if (auto it = m_shaderAssets.find(shaderAssetHandle); it != m_shaderAssets.end() && it->second.gpuHandle.IsValid())
            return it->second.gpuHandle;

        const assets::ShaderAsset* shaderAsset = m_assets->shaders.Get(shaderAssetHandle);
        if (!shaderAsset)
            return shaderAssetHandle;

        const ShaderStageMask stageMask = ToStageMask(shaderAsset->stage);
        ShaderHandle gpuHandle = ShaderHandle::Invalid();
        bool fromBytecode = false;
        bool fromCompiledArtifact = false;
        uint64_t compiledHash = 0ull;
        const auto target = ShaderCompiler::ResolveTargetProfile(*m_device);
        const uint64_t sourceHash = ShaderSourceHash(*shaderAsset);

        // Schon einmal mit derselben Quelle gescheitert: nicht erneut versuchen
        // (sonst Kompilierversuch und Fehler-Log in jedem Frame).
        if (auto it = m_shaderAssets.find(shaderAssetHandle);
            it != m_shaderAssets.end() && it->second.failed &&
            it->second.sourceHash == sourceHash && it->second.target == target)
        {
            return ShaderHandle::Invalid();
        }

        auto* mutableShaderAsset = m_assets->shaders.Get(shaderAssetHandle);
        if (mutableShaderAsset)
        {
            const bool needsCompiledTarget = (target == assets::ShaderTargetProfile::Vulkan_SPIRV)
                || (target == assets::ShaderTargetProfile::DirectX11_SM5)
                || (target == assets::ShaderTargetProfile::DirectX12_SM6);

            const bool hasSource = !mutableShaderAsset->sourceCode.empty() || !mutableShaderAsset->bytecode.empty();
            if (needsCompiledTarget && !FindCompiledArtifact(*mutableShaderAsset) && AsyncShaderCompilesEnabled() &&
                hasSource)
            {
                const ShaderVariantKey baseKey{ shaderAssetHandle, ShaderPassType::Main, ShaderVariantFlag::None };
                if (FindAsyncCompile(baseKey, true))
                    return ShaderHandle::Invalid();
                assets::CompiledShaderArtifact cachedArtifact{};
                if (ShaderCompiler::LoadCachedVariant(*mutableShaderAsset, target, ShaderVariantFlag::None, cachedArtifact))
                {
                    mutableShaderAsset->compiledArtifacts.push_back(std::move(cachedArtifact));
                }
                else
                {
                    QueueAsyncCompile(*mutableShaderAsset, target, baseKey, true, sourceHash);
                    return ShaderHandle::Invalid();
                }
            }
            else if (needsCompiledTarget && !FindCompiledArtifact(*mutableShaderAsset))
            {
                assets::CompiledShaderArtifact compiledArtifact{};
                std::string compileError;
                if (!ShaderCompiler::CompileForTarget(*mutableShaderAsset, target, compiledArtifact, &compileError))
                {
                    Debug::LogError("ShaderRuntime.cpp: failed to compile shader '%s' for %s: %s",
                        mutableShaderAsset->debugName.c_str(),
                        ShaderCompiler::ToString(target),
                        compileError.c_str());
                }
                else
                {
                    mutableShaderAsset->compiledArtifacts.push_back(std::move(compiledArtifact));
                }
            }
        }

        shaderAsset = m_assets->shaders.Get(shaderAssetHandle);
        if (const auto* compiled = FindCompiledArtifact(*shaderAsset))
        {
            fromCompiledArtifact = true;
            compiledHash = compiled->sourceHash;
            if (!compiled->bytecode.empty())
            {
                gpuHandle = m_device->CreateShaderFromBytecode(compiled->bytecode.data(), compiled->bytecode.size(), stageMask, compiled->debugName.empty() ? shaderAsset->debugName : compiled->debugName);
                fromBytecode = true;
            }
            else
            {
                if (target == assets::ShaderTargetProfile::Vulkan_SPIRV)
                {
                    Debug::LogError("ShaderRuntime.cpp: Vulkan requires SPIR-V bytecode for '%s'",
                        (compiled->debugName.empty() ? shaderAsset->debugName : compiled->debugName).c_str());
                }
                else
                {
                    gpuHandle = m_device->CreateShaderFromSource(compiled->sourceText, stageMask, compiled->entryPoint, compiled->debugName.empty() ? shaderAsset->debugName : compiled->debugName);
                }
            }
        }
        else if (!shaderAsset->bytecode.empty())
        {
            gpuHandle = m_device->CreateShaderFromBytecode(shaderAsset->bytecode.data(), shaderAsset->bytecode.size(), stageMask, shaderAsset->debugName);
            fromBytecode = true;
            compiledHash = HashBytes(shaderAsset->bytecode.data(), shaderAsset->bytecode.size());
        }
        else
        {
            if (target == assets::ShaderTargetProfile::Vulkan_SPIRV)
            {
                Debug::LogError("ShaderRuntime.cpp: no SPIR-V bytecode available for shader '%s'", shaderAsset->debugName.c_str());
            }
            else
            {
                gpuHandle = m_device->CreateShaderFromSource(shaderAsset->sourceCode, stageMask, shaderAsset->entryPoint, shaderAsset->debugName);
                compiledHash = HashBytes(shaderAsset->sourceCode.data(), shaderAsset->sourceCode.size());
            }
        }

        ShaderAssetStatus status{};
        status.assetHandle = shaderAssetHandle;
        status.gpuHandle = gpuHandle;
        status.stage = stageMask;
        status.target = target;
        if (const auto* compiled = FindCompiledArtifact(*shaderAsset))
            status.contract = compiled->contract;
        status.compiledHash = compiledHash;
        status.loaded = gpuHandle.IsValid();
        status.fromBytecode = fromBytecode;
        status.fromCompiledArtifact = fromCompiledArtifact;
        status.failed = !gpuHandle.IsValid();
        status.sourceHash = sourceHash;
        m_shaderAssets[shaderAssetHandle] = status;
        if (status.failed)
        {
            Debug::LogError("ShaderRuntime: shader '%s' ist fuer %s nicht verfuegbar - Materialien damit "
                            "werden nicht gezeichnet, bis der Shader korrigiert ist",
                            shaderAsset->debugName.c_str(), ShaderCompiler::ToString(target));
        }
        return gpuHandle;
    }

    bool ShaderRuntime::CommitShaderRequests(const std::vector<ShaderHandle>& requests)
    {
        if (!RequireRenderThread("CommitShaderRequests"))
            return false;
        // Kein AssetRegistry → keine Asset-Kompilierung nötig.
        // Handles werden als direkte GPU-Handles behandelt.
        if (!m_assets)
            return true;

        PumpAsyncShaderCompiles();

        // Ein Shader, der nicht kompiliert, darf den Frame nicht abbrechen: Er ist
        // geloggt, seine Materialien werden in CommitMaterialRequests uebersprungen,
        // der Rest der Szene wird gezeichnet.
        for (ShaderHandle handle : requests)
            (void)PrepareShaderAsset(handle);
        return true;
    }

    bool ShaderRuntime::PrepareAllShaderAssets()
    {
        if (!m_assets) return true;
        std::vector<ShaderHandle> requests;
        m_assets->shaders.ForEach([&](ShaderHandle handle, assets::ShaderAsset&) {
            requests.push_back(handle);
            });
        std::sort(requests.begin(), requests.end(), [](const ShaderHandle& a, const ShaderHandle& b) { return a.value < b.value; });
        requests.erase(std::unique(requests.begin(), requests.end(), [](const ShaderHandle& a, const ShaderHandle& b) { return a == b; }), requests.end());
        return CommitShaderRequests(requests);
    }
    bool ShaderRuntime::ParallelCompileAllShaderAssets(jobs::JobSystem& jobSystem)
    {
        if (!m_device || !m_assets)
            return true;

        const auto target = ShaderCompiler::ResolveTargetProfile(*m_device);
        const bool needsCompilation =
            target == assets::ShaderTargetProfile::Vulkan_SPIRV       ||
            target == assets::ShaderTargetProfile::DirectX11_SM5      ||
            target == assets::ShaderTargetProfile::DirectX12_SM6;

        // Plattformen ohne separaten Compile-Schritt (z.B. Null-Backend): direkt hochladen.
        if (!needsCompilation || !jobSystem.IsParallel())
            return PrepareAllShaderAssets();

        // Phase 1: Shader sammeln, die noch kein CompileArtifact haben.
        struct CompileWork
        {
            ShaderHandle              handle;
            const assets::ShaderAsset* asset;
        };
        std::vector<CompileWork> work;
        m_assets->shaders.ForEach([&](ShaderHandle handle, assets::ShaderAsset& asset) {
            if (auto it = m_shaderAssets.find(handle);
                it != m_shaderAssets.end() && it->second.gpuHandle.IsValid())
                return;
            if (FindCompiledArtifact(asset))
                return;
            work.push_back({handle, &asset});
        });

        if (work.empty())
            return PrepareAllShaderAssets();

        // Phase 2: Parallel kompilieren. Jeder Thread schreibt ausschließlich in
        // sein eigenes results[i]-Slot — kein geteilter Schreibzugriff.
        struct CompileResult
        {
            ShaderHandle                   handle;
            assets::CompiledShaderArtifact artifact;
            bool                           ok = false;
        };
        std::vector<CompileResult> results(work.size());

        jobSystem.ParallelFor(work.size(),
            [&](size_t begin, size_t end) {
                for (size_t i = begin; i < end; ++i)
                {
                    results[i].handle = work[i].handle;
                    std::string error;
                    results[i].ok = ShaderCompiler::CompileForTarget(
                        *work[i].asset, target, results[i].artifact, &error);
                    if (!results[i].ok)
                        Debug::LogError(
                            "ShaderRuntime.cpp: parallel compile failed for '%s': %s",
                            work[i].asset->debugName.c_str(), error.c_str());
                }
            }, 1u);

        // Phase 3: Artefakte zurückschreiben (Render-Thread, seriell).
        for (auto& r : results)
        {
            if (r.ok)
            {
                if (auto* asset = m_assets->shaders.Get(r.handle))
                    asset->compiledArtifacts.push_back(std::move(r.artifact));
            }
        }

        // Phase 4: GPU-Upload — alle Artifacts sind jetzt vorhanden, PrepareShaderAsset
        // findet sie via FindCompiledArtifact und lädt nur noch hoch.
        return PrepareAllShaderAssets();
    }

    const ShaderAssetStatus* ShaderRuntime::GetShaderStatus(ShaderHandle shaderAssetHandle) const noexcept
    {
        if (auto it = m_shaderAssets.find(shaderAssetHandle); it != m_shaderAssets.end())
            return &it->second;
        return nullptr;
    }

} // namespace engine::renderer