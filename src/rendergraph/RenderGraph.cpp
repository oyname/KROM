// =============================================================================
// KROM Engine - src/rendergraph/RenderGraph.cpp
// RenderGraph: Kompilierung, Topologie, State-Planung, Execute.
// =============================================================================
#include "rendergraph/RenderGraph.hpp"
#include "core/Debug.hpp"
#include <algorithm>
#include <cassert>

// BufferHandle liegt in engine::, nicht engine::renderer:: - Alias zur Lesbarkeit
using engine::BufferHandle;

namespace engine::rendergraph {

// ---------------------------------------------------------------------------
// Free-Funktionen
// ---------------------------------------------------------------------------

ResourceState AccessTypeToResourceState(RGAccessType type) noexcept
{
    switch (type) {
    case RGAccessType::RenderTarget:    return ResourceState::RenderTarget;
    case RGAccessType::DepthWrite:      return ResourceState::DepthWrite;
    case RGAccessType::DepthRead:       return ResourceState::DepthRead;
    case RGAccessType::ShaderResource:  return ResourceState::ShaderRead;
    case RGAccessType::UnorderedAccess: return ResourceState::UnorderedAccess;
    case RGAccessType::CopySource:      return ResourceState::CopySource;
    case RGAccessType::CopyDest:        return ResourceState::CopyDest;
    case RGAccessType::Present:         return ResourceState::Present;
    case RGAccessType::HistoryRead:     return ResourceState::ShaderRead;
    case RGAccessType::HistoryWrite:    return ResourceState::RenderTarget;
    default:                            return ResourceState::Unknown;
    }
}

TextureHandle RGExecContext::GetTexture(RGResourceID id) const noexcept
{
    if (!resources || id >= resources->size()) return TextureHandle::Invalid();
    return (*resources)[id].texture;
}

RenderTargetHandle RGExecContext::GetRenderTarget(RGResourceID id) const noexcept
{
    if (!resources || id >= resources->size()) return RenderTargetHandle::Invalid();
    return (*resources)[id].renderTarget;
}

BufferHandle RGExecContext::GetBuffer(RGResourceID id) const noexcept
{
    if (!resources || id >= resources->size()) return BufferHandle::Invalid();
    return (*resources)[id].buffer;
}

// ---------------------------------------------------------------------------
// RGPassBuilder
// ---------------------------------------------------------------------------

RGPassBuilder& RGPassBuilder::Execute(std::function<void(const RGExecContext&)> fn)
{
    m_pass.executeFn = std::move(fn);
    return *this;
}

RGPassBuilder& RGPassBuilder::SetEnabled(bool enabled)
{
    m_pass.enabled = enabled;
    return *this;
}

// ---------------------------------------------------------------------------
// RenderGraph - öffentliche Schnittstelle
// ---------------------------------------------------------------------------

void RenderGraph::Reset()
{
    m_resources.clear();
    m_passes.clear();
    m_sortedPasses.clear();
    m_valid = false;
    m_topologyKey = 0ull;
}

RGResourceID RenderGraph::ImportRenderTarget(RenderTargetHandle rt,
                                              TextureHandle tex,
                                              const char* name,
                                              uint32_t w, uint32_t h,
                                              renderer::Format fmt,
                                              bool externalOutput)
{
    return AddResource(name, RGResourceLifetime::Imported,
                       RGResourceKind::RenderTarget,
                       tex, rt, BufferHandle::Invalid(),
                       w, h, fmt, externalOutput);
}

RGResourceID RenderGraph::ImportTexture(TextureHandle tex, const char* name,
                                         uint32_t w, uint32_t h,
                                         renderer::Format fmt)
{
    return AddResource(name, RGResourceLifetime::Imported,
                       RGResourceKind::ColorTexture,
                       tex, RenderTargetHandle::Invalid(),
                       BufferHandle::Invalid(),
                       w, h, fmt, false);
}

RGResourceID RenderGraph::ImportBackbuffer(RenderTargetHandle rt,
                                            TextureHandle tex,
                                            uint32_t w, uint32_t h)
{
    return AddResource("Backbuffer", RGResourceLifetime::Imported,
                       RGResourceKind::Backbuffer,
                       tex, rt, BufferHandle::Invalid(),
                       w, h, renderer::Format::Unknown, true);
}

RGResourceID RenderGraph::CreateTransientRenderTarget(const char* name,
                                                       uint32_t w, uint32_t h,
                                                       renderer::Format fmt,
                                                       RGResourceKind kind)
{
    return AddResource(name, RGResourceLifetime::Transient, kind,
                       TextureHandle::Invalid(),
                       RenderTargetHandle::Invalid(),
                       BufferHandle::Invalid(),
                       w, h, fmt, false);
}

RGPassBuilder RenderGraph::AddPass(const char* name, RGPassType type)
{
    RGPass pass;
    pass.id        = static_cast<RGPassID>(m_passes.size());
    pass.debugName = name ? name : "";
    pass.type      = type;
    m_passes.push_back(std::move(pass));
    return RGPassBuilder(m_passes.back());
}

void RenderGraph::SetTransientRenderTarget(RGResourceID id,
                                            RenderTargetHandle rt,
                                            TextureHandle tex)
{
    if (id < m_resources.size())
    {
        m_resources[id].renderTarget = rt;
        m_resources[id].texture      = tex;
    }
}

void RenderGraph::SetResourceBinding(RGResourceID id,
                                     RenderTargetHandle rt,
                                     TextureHandle tex,
                                     BufferHandle buf)
{
    if (id < m_resources.size())
    {
        m_resources[id].renderTarget = rt;
        m_resources[id].texture = tex;
        m_resources[id].buffer = buf;
    }
}

void RenderGraph::ClearTransientResourceBindings()
{
    for (RGResourceDesc& res : m_resources)
    {
        if (res.lifetime != RGResourceLifetime::Transient)
            continue;
        res.renderTarget = RenderTargetHandle::Invalid();
        res.texture = TextureHandle::Invalid();
        res.buffer = BufferHandle::Invalid();
    }
}

bool RenderGraph::Compile()
{
    m_valid = false;
    m_sortedPasses.clear();

    if (m_passes.empty()) return false;

    BuildDependencies();

    if (!TopologicalSort())
    {
        Debug::LogError("RenderGraph.cpp: Compile - Zyklus im Graph erkannt!");
        return false;
    }

    // Dead Pass Elimination: Passes die zu keiner Sink-Ressource beitragen
    // werden deaktiviert (enabled = false). Senken = externalOutput-Ressourcen.
    CullDeadPasses();

    // Nochmals sortieren damit deaktivierte Passes rausfallen
    m_sortedPasses.clear();
    if (!TopologicalSort())
    {
        Debug::LogError("RenderGraph.cpp: Compile - Zyklus nach CullDeadPasses!");
        return false;
    }

    ComputeLifetimes();
    PlanResourceStates();

    if (!Validate()) return false;

    m_valid = true;
    m_topologyKey = ComputeTopologyKey(m_passes, m_sortedPasses);
    Debug::Log("RenderGraph.cpp: Compile - %zu passes active, %zu resources, key=0x%llx",
                m_sortedPasses.size(), m_resources.size(),
                static_cast<unsigned long long>(m_topologyKey));
    return true;
}

void RenderGraph::Execute(const RGExecContext& ctx) const
{
    if (!m_valid)
    {
        Debug::LogError("RenderGraph::Execute - Graph nicht kompiliert");
        return;
    }
    // Interne Materialisierung für die alte API (kein externer CompiledFrame)
    CompiledFrame frame;
    const_cast<RenderGraph*>(this)->MaterializeFrame(frame);
    frame.valid = true;
    Execute(frame, ctx);
}

void RenderGraph::PrintGraph() const
{
    Debug::Log("RenderGraph.cpp: === RenderGraph ===");
    Debug::Log("  Resources (%zu):", m_resources.size());
    for (const auto& r : m_resources)
        Debug::Log("    [%u] %s (%s)", r.id, r.debugName.c_str(),
            r.lifetime == RGResourceLifetime::Imported ? "imported" : "transient");

    Debug::Log("  Passes (%zu), sorted:", m_sortedPasses.size());
    for (RGPassID pid : m_sortedPasses)
    {
        const RGPass& p = m_passes[pid];
        Debug::Log("    [%u] %s (%s)", p.id, p.debugName.c_str(),
            p.type == RGPassType::Graphics ? "Graphics" : "Compute");
    }
}

// ---------------------------------------------------------------------------
// Private Implementierungen
// ---------------------------------------------------------------------------

RGResourceID RenderGraph::AddResource(const char* name,
                                       RGResourceLifetime lifetime,
                                       RGResourceKind kind,
                                       TextureHandle tex,
                                       RenderTargetHandle rt,
                                       BufferHandle buf,
                                       uint32_t w, uint32_t h,
                                       renderer::Format fmt,
                                       bool externalOutput)
{
    const RGResourceID id = static_cast<RGResourceID>(m_resources.size());
    RGResourceDesc r;
    r.id             = id;
    r.debugName      = name ? name : "";
    r.lifetime       = lifetime;
    r.kind           = kind;
    r.texture        = tex;
    r.renderTarget   = rt;
    r.buffer         = buf;
    r.width          = w;
    r.height         = h;
    r.format         = fmt;
    r.externalOutput = externalOutput;
    m_resources.push_back(r);
    return id;
}

void RenderGraph::BuildDependencies()
{
    for (auto& pass : m_passes)
        pass.dependencies.clear();
    for (auto& res : m_resources)
        res.producerPass = RG_INVALID_PASS;

    auto addDependency = [](RGPass& pass, RGPassID dep)
    {
        if (dep == RG_INVALID_PASS || dep == pass.id)
            return;
        for (RGPassID existing : pass.dependencies)
            if (existing == dep)
                return;
        pass.dependencies.push_back(dep);
    };

    for (size_t resIndex = 0; resIndex < m_resources.size(); ++resIndex)
    {
        std::vector<RGPassID> priorReaders;
        RGPassID lastWriter = RG_INVALID_PASS;

        for (auto& pass : m_passes)
        {
            bool reads = false;
            bool writes = false;
            for (const auto& acc : pass.accesses)
            {
                if (acc.resource != resIndex)
                    continue;
                reads |= acc.IsRead();
                writes |= acc.IsWrite() || acc.type == RGAccessType::Present || acc.type == RGAccessType::HistoryWrite;
            }
            if (!reads && !writes)
                continue;

            if (reads)
            {
                if (lastWriter != RG_INVALID_PASS)
                {
                    addDependency(pass, lastWriter);
                }
                else if (m_resources[resIndex].lifetime == RGResourceLifetime::Transient)
                {
                    for (size_t futurePassIndex = pass.id + 1u; futurePassIndex < m_passes.size(); ++futurePassIndex)
                    {
                        bool futureWrites = false;
                        for (const auto& futureAcc : m_passes[futurePassIndex].accesses)
                        {
                            if (futureAcc.resource != resIndex)
                                continue;
                            futureWrites |= futureAcc.IsWrite() || futureAcc.type == RGAccessType::Present || futureAcc.type == RGAccessType::HistoryWrite;
                        }
                        if (futureWrites)
                        {
                            addDependency(pass, static_cast<RGPassID>(futurePassIndex));
                            break;
                        }
                    }
                }
            }

            if (writes)
            {
                if (lastWriter != RG_INVALID_PASS)
                    addDependency(pass, lastWriter);
                for (RGPassID reader : priorReaders)
                    addDependency(pass, reader);
                lastWriter = pass.id;
                priorReaders.clear();
                if (m_resources[resIndex].producerPass == RG_INVALID_PASS)
                    m_resources[resIndex].producerPass = pass.id;
            }

            if (reads)
                priorReaders.push_back(pass.id);
        }
    }
}

void RenderGraph::ComputeLifetimes()
{
    for (const auto& pass : m_passes)
    {
        if (!pass.enabled) continue;
        for (const auto& acc : pass.accesses)
        {
            RGResourceDesc& r = m_resources[acc.resource];
            if (r.firstUsePass == RG_INVALID_PASS) r.firstUsePass = pass.id;
            r.lastUsePass = pass.id;
        }
    }
}

void RenderGraph::PlanResourceStates()
{
    for (auto& pass : m_passes)
    {
        pass.beginTransitions.clear();
        pass.endTransitions.clear();
    }

    // Iteriert in topologisch sortierter Ausführungsreihenfolge (nicht Einfüge-Reihenfolge).
    // Damit sind die Transitions korrekt - jeder Pass sieht den State den der Vorgänger
    // hinterlassen hat.
    for (auto& res : m_resources)
    {
        renderer::ResourceState cur = renderer::ResourceState::Common;

        for (RGPassID pid : m_sortedPasses)
        {
            auto& pass = m_passes[pid];
            if (!pass.enabled) continue;

            for (const auto& acc : pass.accesses)
            {
                if (acc.resource != res.id) continue;
                const renderer::ResourceState req = AccessTypeToResourceState(acc.type);
                if (req != cur && req != renderer::ResourceState::Unknown)
                {
                    pass.beginTransitions.push_back({ res.id, cur, req });
                    cur = req;
                }
            }
        }
        res.plannedFinalState = cur;

        // Externe Outputs am Ende des letzten aktiven Passes in Ziel-State überführen
        if (res.externalOutput && !m_sortedPasses.empty())
        {
            const renderer::ResourceState dst =
                (res.kind == RGResourceKind::Backbuffer)
                    ? renderer::ResourceState::Present
                    : renderer::ResourceState::Common;
            if (cur != dst)
                m_passes[m_sortedPasses.back()].endTransitions.push_back({ res.id, cur, dst });
        }
    }
}

void RenderGraph::CullDeadPasses()
{
    const size_t nPasses    = m_passes.size();
    const size_t nResources = m_resources.size();
    if (nPasses == 0u) return;

    // Hilfsfunktion: ist ein Zugriff ein effektiver "Schreiber" (produziert Inhalt)?
    // Present gilt als Schreiber weil er den Backbuffer für die Anzeige verbraucht.
    auto isEffectiveWrite = [](const RGResourceAccess& acc) noexcept {
        return acc.IsWrite()
            || acc.type == RGAccessType::Present
            || acc.type == RGAccessType::HistoryWrite;
    };

    // Schritt 1: Senken = Ressourcen mit externalOutput=true
    std::vector<bool> isSink(nResources, false);
    for (const auto& r : m_resources)
        if (r.externalOutput) isSink[r.id] = true;

    // Schritt 2: Rückwärts-Reachability - wer schreibt (transitiv) in eine Senke?
    std::vector<bool> liveRes (nResources, false);
    std::vector<bool> livePass(nPasses,    false);

    // Seed: Senk-Ressourcen sind lebendig
    for (size_t i = 0; i < nResources; ++i)
        if (isSink[i]) liveRes[i] = true;

    // Fixed-Point in reverse topologischer Reihenfolge
    bool changed = true;
    int  iters   = 0;
    while (changed)
    {
        changed = false;
        ++iters;
        for (int pi = static_cast<int>(m_sortedPasses.size()) - 1; pi >= 0; --pi)
        {
            const RGPassID pid  = m_sortedPasses[static_cast<size_t>(pi)];
            auto& pass          = m_passes[pid];
            if (!pass.enabled) continue;

            // Pass ist lebendig wenn er effektiv in eine lebendige Ressource schreibt
            if (!livePass[pid])
            {
                for (const auto& acc : pass.accesses)
                {
                    if (isEffectiveWrite(acc) && liveRes[acc.resource])
                    {
                        livePass[pid] = true;
                        changed = true;
                        break;
                    }
                }
            }

            // Wenn lebendig: alle gelesenen Ressourcen auch lebendig machen
            if (livePass[pid])
            {
                for (const auto& acc : pass.accesses)
                {
                    if (!liveRes[acc.resource])
                    { liveRes[acc.resource] = true; changed = true; }
                }
            }
        }
    }

    Debug::LogVerbose("RenderGraph.cpp: CullDeadPasses - %d fixed-point iterations", iters);

    // Schritt 3: tote Passes deaktivieren
    uint32_t culledCount = 0u;
    for (auto& pass : m_passes)
    {
        if (pass.enabled && !livePass[pass.id])
        {
            pass.enabled = false;
            ++culledCount;
            Debug::Log("RenderGraph.cpp: CullDeadPasses - culled '%s'",
                pass.debugName.c_str());
        }
    }
    if (culledCount == 0u)
        Debug::LogVerbose("RenderGraph.cpp: CullDeadPasses - all passes alive");
    else
        Debug::Log("RenderGraph.cpp: CullDeadPasses - %u passes culled", culledCount);
}

bool RenderGraph::TopologicalSort()
{
    const size_t n = m_passes.size();
    std::vector<int> inDegree(n, 0);

    for (const auto& pass : m_passes)
        for (RGPassID dep : pass.dependencies)
            if (dep < n) ++inDegree[pass.id];

    std::vector<RGPassID> queue;
    queue.reserve(n);
    for (uint32_t i = 0; i < static_cast<uint32_t>(n); ++i)
        if (inDegree[i] == 0 && m_passes[i].enabled)
            queue.push_back(i);

    m_sortedPasses.clear();
    m_sortedPasses.reserve(n);

    while (!queue.empty())
    {
        std::sort(queue.begin(), queue.end()); // stabile Reihenfolge
        const RGPassID cur = queue.front();
        queue.erase(queue.begin());
        m_sortedPasses.push_back(cur);

        for (auto& pass : m_passes)
        {
            if (!pass.enabled) continue;
            for (RGPassID dep : pass.dependencies)
            {
                if (dep != cur) continue;
                if (--inDegree[pass.id] == 0)
                    queue.push_back(pass.id);
            }
        }
    }

    // Zykluserkennung: alle enabled Passes müssen in der Sortierung auftauchen
    size_t enabledCount = 0u;
    for (const auto& p : m_passes) if (p.enabled) ++enabledCount;
    return m_sortedPasses.size() == enabledCount;
}

bool RenderGraph::Validate() const
{
    bool ok = true;
    for (const auto& pass : m_passes)
    {
        if (!pass.enabled) continue;
        if (pass.accesses.empty())
        {
            Debug::LogError("RenderGraph.cpp: Validate - Pass '%s' hat keine Resource-Zugriffe", pass.debugName.c_str());
            ok = false;
            continue;
        }

        std::unordered_map<RGResourceID, RGAccessMode> perPassUsage;
        for (const auto& acc : pass.accesses)
        {
            if (acc.resource >= m_resources.size())
            {
                Debug::LogError("RenderGraph.cpp: Validate - Pass '%s' referenziert ungueltige Ressource %u",
                                pass.debugName.c_str(), acc.resource);
                ok = false;
                continue;
            }

            const RGResourceDesc& r = m_resources[acc.resource];
            auto it = perPassUsage.find(acc.resource);
            if (it != perPassUsage.end())
            {
                const bool writeConflict = (it->second != RGAccessMode::Read) || (acc.mode != RGAccessMode::Read);
                if (writeConflict)
                {
                    Debug::LogError("RenderGraph.cpp: Validate - Pass '%s' hat mehrfachen schreibenden Zugriff auf '%s'",
                                    pass.debugName.c_str(), r.debugName.c_str());
                    ok = false;
                }
            }
            else
            {
                perPassUsage.emplace(acc.resource, acc.mode);
            }

            if (acc.IsRead() && r.lifetime == RGResourceLifetime::Transient && r.producerPass == RG_INVALID_PASS)
            {
                Debug::LogError("RenderGraph.cpp: Validate - transiente Ressource '%s' gelesen von Pass '%s' ohne Produzenten",
                                r.debugName.c_str(), pass.debugName.c_str());
                ok = false;
            }

            const bool needsPhysicalHandle =
                r.kind == RGResourceKind::Backbuffer ||
                r.kind == RGResourceKind::ColorTexture ||
                r.kind == RGResourceKind::RenderTarget ||
                r.kind == RGResourceKind::DepthStencil ||
                r.kind == RGResourceKind::ShadowMap ||
                r.kind == RGResourceKind::HistoryBuffer;
            if (needsPhysicalHandle && !r.texture.IsValid() && !r.renderTarget.IsValid())
            {
                Debug::LogError("RenderGraph.cpp: Validate - Ressource '%s' ist nicht materialisiert (kein Texture/RT-Handle)",
                                r.debugName.c_str());
                ok = false;
            }
        }
    }
    return ok;
}


uint64_t RenderGraph::ComputeTopologyKey(const std::vector<RGPass>& passes,
                                        const std::vector<RGPassID>& sorted) noexcept
{
    uint64_t h = 14695981039346656037ull;
    auto mix = [&](uint64_t v) { h ^= v; h *= 1099511628211ull; };

    mix(static_cast<uint64_t>(sorted.size()));
    for (RGPassID pid : sorted)
    {
        if (pid >= passes.size())
            continue;

        const RGPass& p = passes[pid];
        if (!p.enabled)
            continue;

        mix(static_cast<uint64_t>(p.type));
        mix(static_cast<uint64_t>(p.accesses.size()));
        for (const RGResourceAccess& a : p.accesses)
        {
            mix(static_cast<uint64_t>(a.resource));
            mix(static_cast<uint64_t>(a.type));
            mix(static_cast<uint64_t>(a.mode));
        }
    }
    return h;
}
void RenderGraph::ApplyTransitions(const RGExecContext& ctx,
                                    const std::vector<RGPlannedTransition>& transitions) const
{
    if (!ctx.cmd || transitions.empty()) return;
    for (const auto& t : transitions)
    {
        const RGResourceDesc& res = m_resources[t.resource];
        if (res.texture.IsValid())
            ctx.cmd->TransitionResource(res.texture, t.before, t.after);
        else if (res.renderTarget.IsValid())
            ctx.cmd->TransitionRenderTarget(res.renderTarget, t.before, t.after);
        else if (res.buffer.IsValid())
            ctx.cmd->TransitionResource(res.buffer, t.before, t.after);
    }
}

} // namespace engine::rendergraph

// =============================================================================
// KROM Engine - RenderGraph: CompiledFrame-Erweiterungen
// =============================================================================

namespace engine::rendergraph {

// ---------------------------------------------------------------------------
// Compile(CompiledFrame&)
// ---------------------------------------------------------------------------

bool RenderGraph::Compile(CompiledFrame& outFrame)
{
    outFrame.Reset();
    if (!Compile())
    {
        outFrame.errorMessage = "Compile(CompiledFrame) - interne Kompilierung fehlgeschlagen";
        return false;
    }
    return ResolveCompiledFrame(outFrame);
}

bool RenderGraph::ResolveCompiledFrame(CompiledFrame& outFrame) const
{
    outFrame.Reset();
    if (!m_valid)
    {
        outFrame.errorMessage = "ResolveCompiledFrame - graph not compiled";
        return false;
    }

    MaterializeFrame(outFrame);

    // Barrier-Optimierung: No-Ops eliminieren, Folge-Transitions mergen
    outFrame.barrierStats = OptimizeBarriers(outFrame);

    // Resource Versioning: Stale-Reads und fehlende Writes prüfen
    outFrame.versioningWarnings = CheckResourceVersioning();
    for (const auto& w : outFrame.versioningWarnings)
        Debug::LogWarning("RenderGraph Versioning: %s", w.c_str());

    outFrame.valid = true;
    Debug::Log("RenderGraph::ResolveCompiledFrame - %zu passes, key=0x%llx",
               outFrame.passes.size(),
               static_cast<unsigned long long>(outFrame.topologyKey));
    return true;
}

// ---------------------------------------------------------------------------
// MaterializeFrame
// ---------------------------------------------------------------------------

void RenderGraph::MaterializeFrame(CompiledFrame& outFrame) const
{
    outFrame.resources.clear();
    outFrame.resources.reserve(m_resources.size());
    for (const RGResourceDesc& r : m_resources)
    {
        CompiledResourceSnapshot snap;
        snap.id           = r.id;
        snap.debugName    = r.debugName;
        snap.texture      = r.texture;
        snap.renderTarget = r.renderTarget;
        snap.buffer       = r.buffer;
        snap.initialState = r.plannedInitialState;
        snap.finalState   = r.plannedFinalState;
        snap.width        = r.width;
        snap.height       = r.height;
        snap.format       = r.format;
        outFrame.resources.push_back(std::move(snap));
    }

    outFrame.passes.clear();
    outFrame.passes.reserve(m_sortedPasses.size());
    for (RGPassID pid : m_sortedPasses)
    {
        if (pid >= m_passes.size()) continue;
        const RGPass& pass = m_passes[pid];
        if (!pass.enabled) continue;

        CompiledPassEntry entry;
        entry.passIndex = pid;
        entry.debugName = pass.debugName;

        auto materialize = [&](const std::vector<RGPlannedTransition>& src,
                                std::vector<CompiledTransition>& dst)
        {
            dst.reserve(src.size());
            for (const RGPlannedTransition& t : src)
            {
                if (t.resource >= outFrame.resources.size()) continue;
                const CompiledResourceSnapshot& res = outFrame.resources[t.resource];
                CompiledTransition ct;
                ct.texture      = res.texture;
                ct.renderTarget = res.renderTarget;
                ct.buffer       = res.buffer;
                ct.before       = t.before;
                ct.after        = t.after;
                ct.debugName    = res.debugName.c_str();
                dst.push_back(ct);
            }
        };
        materialize(pass.beginTransitions, entry.beginTransitions);
        materialize(pass.endTransitions,   entry.endTransitions);
        outFrame.passes.push_back(std::move(entry));
    }

    // FNV-1a Topologie-Schlüssel
    uint64_t h = 14695981039346656037ull;
    auto mix = [&](uint64_t v) { h ^= v; h *= 1099511628211ull; };
    mix(static_cast<uint64_t>(m_sortedPasses.size()));
    for (RGPassID pid : m_sortedPasses)
    {
        if (pid >= m_passes.size()) continue;
        const RGPass& p = m_passes[pid];
        mix(static_cast<uint64_t>(p.type));
        for (const RGResourceAccess& a : p.accesses)
        {
            mix(static_cast<uint64_t>(a.resource));
            mix(static_cast<uint64_t>(a.type));
            mix(static_cast<uint64_t>(a.mode));
        }
    }
    outFrame.topologyKey = m_topologyKey != 0ull ? m_topologyKey : h;
}

// ---------------------------------------------------------------------------
// Execute(CompiledFrame) - reine Übersetzung, keine Topologie-Logik
// ---------------------------------------------------------------------------

void RenderGraph::Execute(const CompiledFrame& frame, const RGExecContext& ctx) const
{
    if (!frame.IsValid())
    {
        Debug::LogError("RenderGraph::Execute(CompiledFrame) - Frame ungueltig");
        return;
    }
    RGExecContext localCtx = ctx;
    localCtx.resources     = &frame.resources;

    for (const CompiledPassEntry& entry : frame.passes)
    {
        if (entry.passIndex >= m_passes.size()) continue;
        const RGPass& pass = m_passes[entry.passIndex];
        if (!pass.enabled || !pass.executeFn) continue;

        ApplyCompiledTransitions(localCtx, entry.beginTransitions);
        pass.executeFn(localCtx);
        ApplyCompiledTransitions(localCtx, entry.endTransitions);
    }
}

void RenderGraph::ApplyCompiledTransitions(const RGExecContext& ctx,
                                            const std::vector<CompiledTransition>& transitions) const
{
    if (!ctx.cmd || transitions.empty()) return;
    for (const CompiledTransition& t : transitions)
    {
        if      (t.texture.IsValid())      ctx.cmd->TransitionResource(t.texture, t.before, t.after);
        else if (t.renderTarget.IsValid()) ctx.cmd->TransitionRenderTarget(t.renderTarget, t.before, t.after);
        else if (t.buffer.IsValid())       ctx.cmd->TransitionResource(t.buffer, t.before, t.after);
    }
}

} // namespace engine::rendergraph

// =============================================================================
// History Resources - Ping-Pong
// =============================================================================

namespace engine::rendergraph {

RenderGraph::HistoryPair RenderGraph::ImportHistoryPair(
    RenderTargetHandle curRT,  TextureHandle curTex,
    RenderTargetHandle prevRT, TextureHandle prevTex,
    const char* name,
    uint32_t w, uint32_t h, renderer::Format fmt)
{
    const std::string curName  = std::string(name ? name : "") + "_Current";
    const std::string prevName = std::string(name ? name : "") + "_Prev";

    const RGResourceID cur = AddResource(curName.c_str(),
        RGResourceLifetime::Imported, RGResourceKind::HistoryBuffer,
        curTex, curRT, BufferHandle::Invalid(), w, h, fmt, false);

    const RGResourceID prev = AddResource(prevName.c_str(),
        RGResourceLifetime::Imported, RGResourceKind::HistoryBuffer,
        prevTex, prevRT, BufferHandle::Invalid(), w, h, fmt, false);

    m_resources[cur].historyPeer      = prev;
    m_resources[cur].isHistoryCurrent = true;
    m_resources[prev].historyPeer     = cur;
    m_resources[prev].isHistoryPrev   = true;

    return { cur, prev };
}

void RenderGraph::SwapHistoryResources(RGResourceID current, RGResourceID prev)
{
    if (current >= m_resources.size() || prev >= m_resources.size()) return;

    RGResourceDesc& c = m_resources[current];
    RGResourceDesc& p = m_resources[prev];

    // GPU-Handles tauschen
    std::swap(c.texture,      p.texture);
    std::swap(c.renderTarget, p.renderTarget);
    std::swap(c.buffer,       p.buffer);

    // isHistoryCurrent/Prev bleiben unverändert - die IDs bezeichnen
    // semantische Rollen, nicht physische Ressourcen
}

// =============================================================================
// Barrier-Optimierung
// =============================================================================

BarrierStats RenderGraph::OptimizeBarriers(CompiledFrame& frame)
{
    BarrierStats stats;

    for (CompiledPassEntry& entry : frame.passes)
    {
        auto optimize = [&](std::vector<CompiledTransition>& transitions)
        {
            stats.totalTransitions += static_cast<uint32_t>(transitions.size());

            // Schritt 1: No-Op-Transitions entfernen (before == after)
            auto it = std::remove_if(transitions.begin(), transitions.end(),
                [&](const CompiledTransition& t) -> bool {
                    if (t.before == t.after) {
                        ++stats.redundantEliminated;
                        return true;
                    }
                    return false;
                });
            transitions.erase(it, transitions.end());

            // Schritt 2: Aufeinanderfolgende Transitions zur gleichen Ressource mergen.
            // Wenn Ressource A: Common→RT, dann A: RT→ShaderRead
            // → merge zu A: Common→ShaderRead
            bool merged = true;
            while (merged)
            {
                merged = false;
                for (size_t i = 0; i + 1 < transitions.size(); ++i)
                {
                    CompiledTransition& a = transitions[i];
                    CompiledTransition& b = transitions[i + 1];

                    // Gleiche Ressource? (Vergleich über Handle-Werte)
                    const bool sameTexture = a.texture.IsValid() && b.texture.IsValid()
                        && a.texture.value == b.texture.value;
                    const bool sameRT = a.renderTarget.IsValid() && b.renderTarget.IsValid()
                        && a.renderTarget.value == b.renderTarget.value;
                    const bool sameBuf = a.buffer.IsValid() && b.buffer.IsValid()
                        && a.buffer.value == b.buffer.value;

                    if ((sameTexture || sameRT || sameBuf) && a.after == b.before)
                    {
                        // Merge: a.before → b.after, b entfernen
                        a.after = b.after;
                        transitions.erase(transitions.begin() + static_cast<std::ptrdiff_t>(i + 1));
                        ++stats.mergedTransitions;
                        merged = true;
                        break;
                    }
                }
            }
        };

        optimize(entry.beginTransitions);
        optimize(entry.endTransitions);
    }

    // Gesamtzahl nach Optimierung
    for (const CompiledPassEntry& entry : frame.passes)
    {
        stats.finalTransitions += static_cast<uint32_t>(entry.beginTransitions.size());
        stats.finalTransitions += static_cast<uint32_t>(entry.endTransitions.size());
    }

    return stats;
}

// =============================================================================
// Resource Versioning - Stale-Read-Erkennung
// =============================================================================

std::vector<std::string> RenderGraph::CheckResourceVersioning() const
{
    std::vector<std::string> warnings;

    // Simuliere Write-Generationen entlang der Execution-Order
    std::vector<uint32_t> writeGen(m_resources.size(), 0u);

    // Importierte Ressourcen starten bei Generation 1
    for (const RGResourceDesc& r : m_resources)
        if (r.lifetime == RGResourceLifetime::Imported)
            writeGen[r.id] = 1u;

    for (RGPassID pid : m_sortedPasses)
    {
        if (pid >= m_passes.size()) continue;
        const RGPass& pass = m_passes[pid];
        if (!pass.enabled) continue;

        for (const RGResourceAccess& acc : pass.accesses)
        {
            if (acc.resource >= m_resources.size()) continue;
            const RGResourceDesc& res = m_resources[acc.resource];

            if (acc.IsRead())
            {
                // Stale-Read: Ressource wurde noch nie geschrieben
                if (writeGen[acc.resource] == 0u &&
                    res.lifetime != RGResourceLifetime::Imported &&
                    res.kind     != RGResourceKind::HistoryBuffer)
                {
                    warnings.push_back(
                        "Stale-Read: Pass '" + pass.debugName +
                        "' liest '" + res.debugName +
                        "' (writeGeneration=0, kein Producer)");
                }
            }

            if (acc.IsWrite())
            {
                ++writeGen[acc.resource];
            }
        }
    }

    // History-Ressourcen ohne Write im aktuellen Frame - Warnung wenn _Current nie beschrieben
    for (const RGResourceDesc& r : m_resources)
    {
        if (r.isHistoryCurrent && writeGen[r.id] <= 1u)
        {
            warnings.push_back(
                "History-Ressource '" + r.debugName +
                "' wurde in diesem Frame nie beschrieben (kein HistoryWrite-Pass)");
        }
    }

    return warnings;
}

} // namespace engine::rendergraph
