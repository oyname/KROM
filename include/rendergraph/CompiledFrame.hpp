#pragma once
// =============================================================================
// KROM Engine - rendergraph/CompiledFrame.hpp
//
// CompiledFrame ist das Ergebnis von RenderGraph::Compile().
// Vollständig backend-neutrale, cacheable Ausführungsrepräsentation.
//
// Nach Compile():
//   - passes ist topologisch sortiert, tote Passes eliminiert
//   - resources enthält Snapshots aller Ressourcen mit aufgelösten Handles
//   - alle Transitions sind materialisiert (keine Ressource-Index-Lookups mehr)
//   - topologyKey erlaubt Cache-Invalidierung ohne erneutes Compile()
//
// Execute(frame, ctx) iteriert frame.passes ohne eigene Topologie-Logik.
// =============================================================================
#include "renderer/RendererTypes.hpp"
#include <vector>
#include <string>
#include <cstdint>

namespace engine::rendergraph {

using namespace renderer;

// ---------------------------------------------------------------------------
// CompiledTransition - vollständig aufgelöste Ressource-Zustandsänderung
// ---------------------------------------------------------------------------
struct CompiledTransition
{
    TextureHandle      texture      = TextureHandle::Invalid();
    RenderTargetHandle renderTarget = RenderTargetHandle::Invalid();
    BufferHandle       buffer       = BufferHandle::Invalid();
    ResourceState      before       = ResourceState::Unknown;
    ResourceState      after        = ResourceState::Unknown;
    const char*        debugName    = "";
};

// ---------------------------------------------------------------------------
// CompiledResourceSnapshot - Ressourcen-Metadaten zum Compile()-Zeitpunkt
// Wird in RGExecContext::resources eingehängt
// ---------------------------------------------------------------------------
struct CompiledResourceSnapshot
{
    uint32_t           id           = UINT32_MAX;
    std::string        debugName;
    TextureHandle      texture      = TextureHandle::Invalid();
    RenderTargetHandle renderTarget = RenderTargetHandle::Invalid();
    BufferHandle       buffer       = BufferHandle::Invalid();
    ResourceState      initialState = ResourceState::Unknown;
    ResourceState      finalState   = ResourceState::Unknown;
    uint32_t           width        = 0u;
    uint32_t           height       = 0u;
    Format             format       = Format::Unknown;
};

// ---------------------------------------------------------------------------
// CompiledPassEntry - ein Ausführungsschritt in topologischer Reihenfolge
// ---------------------------------------------------------------------------
enum class FrameQueueModel : uint8_t
{
    SingleGraphicsQueueV1 = 0,
    MultiQueue,
};

struct CompiledPassEntry
{
    uint32_t    passIndex = UINT32_MAX;  // Index zurück in RenderGraph::m_passes
    QueueType   declaredQueue = QueueType::Graphics;
    QueueType   executionQueue = QueueType::Graphics;
    uint32_t    submissionId = 0u;
    std::string debugName;
    std::vector<CompiledTransition> beginTransitions;
    std::vector<CompiledTransition> endTransitions;
};

struct CompiledInterQueueDependency
{
    uint32_t producerPassIndex = UINT32_MAX;
    QueueType producerQueue = QueueType::Graphics;
    uint32_t consumerPassIndex = UINT32_MAX;
    QueueType consumerQueue = QueueType::Graphics;
    QueueSyncPrimitive primitive = QueueSyncPrimitive::None;
    QueueDependencyScope scope = QueueDependencyScope::None;
    QueueOwnershipTransferPoint ownershipTransferPoint = QueueOwnershipTransferPoint::None;
};

// ---------------------------------------------------------------------------
// BarrierStats - Ergebnis der Barrier-Optimierung
// ---------------------------------------------------------------------------
struct BarrierStats
{
    uint32_t totalTransitions     = 0u;  // vor Optimierung
    uint32_t redundantEliminated  = 0u;  // No-Ops (before == after)
    uint32_t mergedTransitions    = 0u;  // zusammengeführte Folge-Transitions
    uint32_t finalTransitions     = 0u;  // nach Optimierung
};

// ---------------------------------------------------------------------------
// CompiledFrame
// ---------------------------------------------------------------------------
struct CompiledFrame
{
    std::vector<CompiledPassEntry>        passes;
    std::vector<CompiledResourceSnapshot> resources;
    bool            valid        = false;
    std::string     errorMessage;
    uint64_t        topologyKey  = 0ull;
    FrameQueueModel queueModel   = FrameQueueModel::SingleGraphicsQueueV1;
    uint32_t        normalizedNonGraphicsPassCount = 0u;
    std::vector<CompiledInterQueueDependency> interQueueDependencies;

    // Barrier-Optimierungsergebnis (nach Compile())
    BarrierStats barrierStats;

    // Versioning-Warnungen (Stale-Reads, fehlende Writes)
    std::vector<std::string> versioningWarnings;

    void Reset()
    {
        passes.clear();
        resources.clear();
        valid        = false;
        errorMessage.clear();
        topologyKey  = 0ull;
        queueModel   = FrameQueueModel::SingleGraphicsQueueV1;
        normalizedNonGraphicsPassCount = 0u;
        interQueueDependencies.clear();
        barrierStats = {};
        versioningWarnings.clear();
    }

    [[nodiscard]] bool IsValid() const noexcept { return valid; }
};

} // namespace engine::rendergraph
