#pragma once
// =============================================================================
// KROM Engine - rendergraph/RenderGraph.hpp
// RenderGraph: Deklaration. Implementierung: src/rendergraph/RenderGraph.cpp
// =============================================================================
#include "renderer/IDevice.hpp"
#include "rendergraph/CompiledFrame.hpp"
#include "core/Debug.hpp"
#include <vector>
#include <string>
#include <functional>
#include <unordered_map>

namespace engine::rendergraph {

using namespace renderer;

// ---------------------------------------------------------------------------
// Resource-Handles
// ---------------------------------------------------------------------------
using RGResourceID = uint32_t;
using RGPassID     = uint32_t;
static constexpr RGResourceID RG_INVALID_RESOURCE = UINT32_MAX;
static constexpr RGPassID     RG_INVALID_PASS      = UINT32_MAX;

enum class RGResourceLifetime : uint8_t { Imported, Transient };
enum class RGResourceKind     : uint8_t
{
    Unknown, Backbuffer, ColorTexture, RenderTarget,
    DepthStencil, Buffer, StorageBuffer, ShadowMap, HistoryBuffer,
};

struct RGResourceDesc
{
    RGResourceID       id            = RG_INVALID_RESOURCE;
    std::string        debugName;
    RGResourceLifetime lifetime      = RGResourceLifetime::Transient;
    RGResourceKind     kind          = RGResourceKind::Unknown;
    bool               externalOutput = false;

    TextureHandle      texture        = TextureHandle::Invalid();
    RenderTargetHandle renderTarget   = RenderTargetHandle::Invalid();
    BufferHandle       buffer         = BufferHandle::Invalid();

    uint32_t           width          = 0u;
    uint32_t           height         = 0u;
    Format             format         = Format::Unknown;

    RGPassID    producerPass          = RG_INVALID_PASS;
    RGPassID    firstUsePass          = RG_INVALID_PASS;
    RGPassID    lastUsePass           = RG_INVALID_PASS;
    ResourceState plannedInitialState = ResourceState::Common;
    ResourceState plannedFinalState   = ResourceState::Common;

    // -------------------------------------------------------------------------
    // Resource Versioning
    // Jeder Write-Zugriff erhöht writeGeneration.
    // Ein Read der auf writeGeneration=0 zeigt bevor ein Producer geschrieben hat
    // wird vom Validator als Stale-Read gewertet (Fehler).
    // -------------------------------------------------------------------------
    uint32_t writeGeneration = 0u;

    // -------------------------------------------------------------------------
    // History / Ping-Pong
    // historyPeer: optionale Ressource die in der nächsten Frame-Instanz
    //              die "current"-Rolle übernimmt (Ping-Pong-Swap).
    // Wird von ImportHistoryResource() gesetzt.
    // -------------------------------------------------------------------------
    RGResourceID historyPeer = RG_INVALID_RESOURCE;
    bool         isHistoryCurrent = false;  // true = aktuelle Frame-Seite
    bool         isHistoryPrev    = false;  // true = vorherige Frame-Seite
};

// ---------------------------------------------------------------------------
// Zugriffs-Deklaration
// ---------------------------------------------------------------------------
enum class RGAccessMode : uint8_t { Read, Write, ReadWrite };
enum class RGAccessType : uint8_t
{
    RenderTarget, DepthWrite, DepthRead, ShaderResource,
    UnorderedAccess, CopySource, CopyDest, Present,
    HistoryRead, HistoryWrite,
};

ResourceState AccessTypeToResourceState(RGAccessType type) noexcept;

struct RGResourceAccess
{
    RGResourceID resource = RG_INVALID_RESOURCE;
    RGAccessType type     = RGAccessType::ShaderResource;
    RGAccessMode mode     = RGAccessMode::Read;

    [[nodiscard]] bool IsRead()  const noexcept { return mode == RGAccessMode::Read  || mode == RGAccessMode::ReadWrite; }
    [[nodiscard]] bool IsWrite() const noexcept { return mode == RGAccessMode::Write || mode == RGAccessMode::ReadWrite; }
};

struct RGPlannedTransition
{
    RGResourceID  resource = RG_INVALID_RESOURCE;
    ResourceState before   = ResourceState::Unknown;
    ResourceState after    = ResourceState::Unknown;
};

// ---------------------------------------------------------------------------
// Ausführungskontext
// ---------------------------------------------------------------------------
struct RGExecContext
{
    IDevice*      device    = nullptr;
    ICommandList* cmd       = nullptr;

    // Wird von Execute(CompiledFrame) gesetzt - zeigt auf frame.resources
    const std::vector<CompiledResourceSnapshot>* resources = nullptr;

    [[nodiscard]] TextureHandle      GetTexture(RGResourceID id)      const noexcept;
    [[nodiscard]] RenderTargetHandle GetRenderTarget(RGResourceID id) const noexcept;
    [[nodiscard]] BufferHandle       GetBuffer(RGResourceID id)       const noexcept;
};

// ---------------------------------------------------------------------------
// Pass
// ---------------------------------------------------------------------------
enum class RGPassType : uint8_t { Graphics, Compute, Transfer };

struct RGPass
{
    RGPassID     id         = RG_INVALID_PASS;
    std::string  debugName;
    RGPassType   type       = RGPassType::Graphics;
    bool         enabled    = true;

    std::vector<RGResourceAccess>    accesses;
    std::vector<RGPassID>            dependencies;
    std::vector<RGPlannedTransition> beginTransitions;
    std::vector<RGPlannedTransition> endTransitions;

    std::function<void(const RGExecContext&)> executeFn;

    void ReadRT(RGResourceID r)      { accesses.push_back({r, RGAccessType::RenderTarget,    RGAccessMode::Read});  }
    void WriteRT(RGResourceID r)     { accesses.push_back({r, RGAccessType::RenderTarget,    RGAccessMode::Write}); }
    void WriteDepth(RGResourceID r)  { accesses.push_back({r, RGAccessType::DepthWrite,      RGAccessMode::Write}); }
    void ReadDepth(RGResourceID r)   { accesses.push_back({r, RGAccessType::DepthRead,       RGAccessMode::Read});  }
    void ReadSRV(RGResourceID r)     { accesses.push_back({r, RGAccessType::ShaderResource,  RGAccessMode::Read});  }
    void WriteUAV(RGResourceID r)    { accesses.push_back({r, RGAccessType::UnorderedAccess, RGAccessMode::Write}); }
    void WritePresent(RGResourceID r){ accesses.push_back({r, RGAccessType::Present,         RGAccessMode::Read});  }
    void ReadHistory(RGResourceID r) { accesses.push_back({r, RGAccessType::HistoryRead,     RGAccessMode::Read});  }
    void WriteHistory(RGResourceID r){ accesses.push_back({r, RGAccessType::HistoryWrite,    RGAccessMode::Write}); }
};

// ---------------------------------------------------------------------------
// RGPassBuilder - Fluent-API für Pass-Konfiguration
// ---------------------------------------------------------------------------
class RGPassBuilder
{
public:
    explicit RGPassBuilder(RGPass& pass) : m_pass(pass) {}

    RGPassBuilder& ReadRenderTarget(RGResourceID r)  { m_pass.ReadRT(r);      return *this; }
    RGPassBuilder& WriteRenderTarget(RGResourceID r) { m_pass.WriteRT(r);     return *this; }
    RGPassBuilder& WriteDepthStencil(RGResourceID r) { m_pass.WriteDepth(r);  return *this; }
    RGPassBuilder& ReadDepthStencil(RGResourceID r)  { m_pass.ReadDepth(r);   return *this; }
    RGPassBuilder& ReadTexture(RGResourceID r)       { m_pass.ReadSRV(r);     return *this; }
    RGPassBuilder& WriteStorageBuffer(RGResourceID r){ m_pass.WriteUAV(r);    return *this; }
    RGPassBuilder& Present(RGResourceID r)           { m_pass.WritePresent(r);return *this; }
    RGPassBuilder& ReadHistoryBuffer(RGResourceID r) { m_pass.ReadHistory(r); return *this; }
    RGPassBuilder& WriteHistoryBuffer(RGResourceID r){ m_pass.WriteHistory(r);return *this; }

    RGPassBuilder& Execute(std::function<void(const RGExecContext&)> fn);
    RGPassBuilder& SetEnabled(bool enabled);

private:
    RGPass& m_pass;
};

// ---------------------------------------------------------------------------
// RenderGraph
// ---------------------------------------------------------------------------
class RenderGraph
{
public:
    RenderGraph()  = default;
    ~RenderGraph() = default;

    void Reset();

    // --- Ressourcen registrieren ---
    RGResourceID ImportRenderTarget(RenderTargetHandle rt, TextureHandle tex,
                                     const char* name,
                                     uint32_t w = 0u, uint32_t h = 0u,
                                     Format fmt = Format::Unknown,
                                     bool externalOutput = false);
    RGResourceID ImportTexture(TextureHandle tex, const char* name,
                                uint32_t w = 0u, uint32_t h = 0u,
                                Format fmt = Format::Unknown);
    RGResourceID ImportBackbuffer(RenderTargetHandle rt, TextureHandle tex,
                                   uint32_t w, uint32_t h);
    RGResourceID CreateTransientRenderTarget(const char* name,
                                             uint32_t w, uint32_t h,
                                             Format fmt,
                                             RGResourceKind kind = RGResourceKind::RenderTarget);

    // -------------------------------------------------------------------------
    // History Resources - Ping-Pong zwischen Frames
    //
    // Registriert ein Ressourcen-Paar: (current, prev).
    //   current: wird in diesem Frame beschrieben (HistoryWrite)
    //   prev:    enthält das Ergebnis des letzten Frames (HistoryRead)
    //
    // Am Frame-Ende: SwapHistoryResources(current, prev) aufrufen um die
    // Handles für den nächsten Frame zu tauschen.
    //
    // Beispiel (TAA, SSR, GTAO-Akkumulation):
    //   auto [cur, prev] = rg.ImportHistoryPair(curRT, curTex, prevRT, prevTex,
    //                                            "GTAO", 1280, 720, Format::R8_UNORM);
    //   rg.AddPass("GTAO").WriteHistoryBuffer(cur).ReadHistoryBuffer(prev)...
    // -------------------------------------------------------------------------
    struct HistoryPair { RGResourceID current; RGResourceID prev; };

    HistoryPair ImportHistoryPair(
        RenderTargetHandle curRT,  TextureHandle curTex,
        RenderTargetHandle prevRT, TextureHandle prevTex,
        const char* name,
        uint32_t w, uint32_t h, Format fmt);

    // Tauscht die GPU-Handles zweier History-Ressourcen.
    // Muss nach Execute() und vor dem nächsten Compile() aufgerufen werden.
    void SwapHistoryResources(RGResourceID current, RGResourceID prev);

    // --- Passes ---
    RGPassBuilder AddPass(const char* name, RGPassType type = RGPassType::Graphics);

    // --- Kompilierung und Ausführung ---

    // Alte API (für Tests und bestehenden Code)
    bool Compile();
    void Execute(const RGExecContext& ctx) const;

    // Neue API - Compile produziert einen expliziten Frame-Plan.
    // Execute iteriert den CompiledFrame ohne eigene Topologie-Logik.
    // topologyKey erlaubt Compile() zu überspringen wenn Topologie konstant ist.
    bool Compile(CompiledFrame& outFrame);
    void Execute(const CompiledFrame& frame, const RGExecContext& ctx) const;

    // Barrier-Optimierung - public damit Tests direkt synthetische Frames testen können
    static BarrierStats OptimizeBarriers(CompiledFrame& frame);

    // --- Transiente Ressource physisch belegen ---
    void SetTransientRenderTarget(RGResourceID id, RenderTargetHandle rt, TextureHandle tex);

    // --- Diagnose ---
    void PrintGraph() const;
    [[nodiscard]] bool IsValid() const noexcept { return m_valid; }
    [[nodiscard]] const std::vector<RGPass>&         GetPasses()       const { return m_passes; }
    [[nodiscard]] const std::vector<RGResourceDesc>& GetResources()    const { return m_resources; }
    [[nodiscard]] const std::vector<RGPassID>&        GetSortedPasses() const { return m_sortedPasses; }

private:
    std::vector<RGResourceDesc> m_resources;
    std::vector<RGPass>         m_passes;
    std::vector<RGPassID>       m_sortedPasses;
    bool                        m_valid = false;

    RGResourceID AddResource(const char* name,
                              RGResourceLifetime lifetime,
                              RGResourceKind kind,
                              TextureHandle tex,
                              RenderTargetHandle rt,
                              BufferHandle buf,
                              uint32_t w, uint32_t h,
                              Format fmt,
                              bool externalOutput);

    void BuildDependencies();
    void ComputeLifetimes();
    void PlanResourceStates();
    void CullDeadPasses();
    bool TopologicalSort();
    bool Validate() const;

    void MaterializeFrame(CompiledFrame& outFrame) const;

    // Resource Versioning
    std::vector<std::string> CheckResourceVersioning() const;

    static uint64_t ComputeTopologyKey(const std::vector<RGPass>& passes,
                                        const std::vector<RGPassID>& sorted) noexcept;
    void ApplyCompiledTransitions(const RGExecContext& ctx,
                                   const std::vector<CompiledTransition>& transitions) const;
    void ApplyTransitions(const RGExecContext& ctx,
                           const std::vector<RGPlannedTransition>& transitions) const;
};

} // namespace engine::rendergraph
