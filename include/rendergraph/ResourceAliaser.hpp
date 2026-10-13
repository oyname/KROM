#pragma once
// =============================================================================
// KROM Engine - rendergraph/ResourceAliaser.hpp
// Ressource-Aliasing für transiente RenderGraph-Ressourcen.
//
// Kernidee: Transiente Ressourcen mit nicht-überlappenden Lifetimes können
// denselben physischen Speicher nutzen (Backend entscheidet über Allokation).
//
// Lifecycle einer Ressource im Graph:
//   firstUsePass  = Pass-Index in m_sortedPasses, bei dem die Ressource zuerst benutzt wird
//   lastUsePass   = letzter Pass der sie nutzt
//
// Zwei Ressourcen A und B können aliasieren wenn:
//   lastUsePass(A) < firstUsePass(B)  ODER  lastUsePass(B) < firstUsePass(A)
//
// AliasGroup: Menge von Ressourcen die denselben Speicher teilen können.
// Das Backend bekommt eine Liste von AliasGroups und entscheidet selbst
// über die konkrete physische Allokation (Heap-Aliasing, Placed Resources etc.)
//
// Deklaration. Implementierung: src/rendergraph/ResourceAliaser.cpp
// =============================================================================
#include "rendergraph/RenderGraph.hpp"
#include <vector>
#include <cstdint>

namespace engine::rendergraph {

// Aliasing-Kandidat: zwei Ressourcen die aliasieren können
struct AliasPair
{
    RGResourceID resourceA;
    RGResourceID resourceB;
};

// Gruppe von Ressourcen die alle untereinander aliasieren können
// (disjunkte Lifetimes innerhalb der Gruppe)
struct AliasGroup
{
    std::vector<RGResourceID> resources;

    // Maximale Größe in Bytes - Backend muss mindestens so viel allozieren
    size_t requiredBytes = 0u;

    // Maximales Alignment - Backend muss mindestens dieses Alignment einhalten
    size_t requiredAlignment = 0u;
};

struct AliasingPlan
{
    std::vector<AliasGroup>   groups;          // Gruppen von aliasierenden Ressourcen
    std::vector<RGResourceID> nonAliased;      // Ressourcen die NICHT aliasiert werden
    uint32_t                  aliasingCount = 0u; // Wie viele Allokationen eingespart

    // Gibt true zurück wenn Ressource id in einer Gruppe ist
    [[nodiscard]] bool IsAliased(RGResourceID id) const noexcept;
    [[nodiscard]] int  GetGroupIndex(RGResourceID id) const noexcept; // -1 wenn nicht aliasiert
};

class ResourceAliaser
{
public:
    // Analysiert den kompilierten RenderGraph und erstellt einen Aliasing-Plan.
    // Voraussetzung: rg.IsValid() == true (Compile() wurde aufgerufen).
    //
    // sizeEstimator: Funktion die für eine RGResourceDesc die Größe in Bytes schätzt.
    //   Im Engine-Core fehlt direkter Zugriff auf Backend-Größen, daher callback-basiert.
    [[nodiscard]] AliasingPlan Analyze(
        const RenderGraph& rg,
        const std::function<size_t(const RGResourceDesc&)>& sizeEstimator) const;

    // Prüft ob zwei Ressourcen aliasieren können (disjunkte Lifetimes)
    static bool CanAlias(const RGResourceDesc& a, const RGResourceDesc& b,
                          const std::vector<RGPassID>& sortedPasses) noexcept;

private:
    // Sortierte Pass-Index von firstUsePass/lastUsePass für eine Ressource
    static int SortedIndex(RGPassID passId, const std::vector<RGPassID>& sorted) noexcept;
};

} // namespace engine::rendergraph
