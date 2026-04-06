// =============================================================================
// KROM Engine - src/rendergraph/ResourceAliaser.cpp
// Greedy Interval-Packing für transiente Ressource-Aliasing.
// =============================================================================
#include "rendergraph/ResourceAliaser.hpp"
#include "core/Debug.hpp"
#include <algorithm>
#include <cassert>

namespace engine::rendergraph {

// ---------------------------------------------------------------------------
// AliasingPlan helpers
// ---------------------------------------------------------------------------

bool AliasingPlan::IsAliased(RGResourceID id) const noexcept
{
    for (const auto& g : groups)
        for (RGResourceID r : g.resources)
            if (r == id) return true;
    return false;
}

int AliasingPlan::GetGroupIndex(RGResourceID id) const noexcept
{
    for (int gi = 0; gi < static_cast<int>(groups.size()); ++gi)
        for (RGResourceID r : groups[gi].resources)
            if (r == id) return gi;
    return -1;
}

// ---------------------------------------------------------------------------
// ResourceAliaser
// ---------------------------------------------------------------------------

int ResourceAliaser::SortedIndex(RGPassID passId,
                                   const std::vector<RGPassID>& sorted) noexcept
{
    for (int i = 0; i < static_cast<int>(sorted.size()); ++i)
        if (sorted[i] == passId) return i;
    return -1;
}

bool ResourceAliaser::CanAlias(const RGResourceDesc& a,
                                 const RGResourceDesc& b,
                                 const std::vector<RGPassID>& sorted) noexcept
{
    // Nur transiente Ressourcen können aliasiert werden
    if (a.lifetime != RGResourceLifetime::Transient) return false;
    if (b.lifetime != RGResourceLifetime::Transient) return false;

    // Ohne Lifecycle-Info kein Aliasing
    if (a.firstUsePass == RG_INVALID_PASS || a.lastUsePass == RG_INVALID_PASS) return false;
    if (b.firstUsePass == RG_INVALID_PASS || b.lastUsePass == RG_INVALID_PASS) return false;

    // Konvertiere Pass-IDs zu sortierter Reihenfolge (topologischer Index)
    const int aFirst = SortedIndex(a.firstUsePass, sorted);
    const int aLast  = SortedIndex(a.lastUsePass,  sorted);
    const int bFirst = SortedIndex(b.firstUsePass, sorted);
    const int bLast  = SortedIndex(b.lastUsePass,  sorted);

    if (aFirst < 0 || aLast < 0 || bFirst < 0 || bLast < 0) return false;

    // Disjunkte Intervalle: [aFirst, aLast] ∩ [bFirst, bLast] = ∅
    // Korrekte Bedingung: aLast < bFirst ODER bLast < aFirst.
    // Der RenderGraph plant Transitions selbst via beginTransitions/endTransitions,
    // daher kein manueller Sicherheitsabstand nötig.
    // Beispiel: A lebt in [0,1], B in [2,3] → aLast(1) < bFirst(2) → Aliasing OK.
    return (aLast < bFirst) || (bLast < aFirst);
}

AliasingPlan ResourceAliaser::Analyze(
    const RenderGraph& rg,
    const std::function<size_t(const RGResourceDesc&)>& sizeEstimator) const
{
    assert(rg.IsValid() && "ResourceAliaser::Analyze: graph must be compiled");

    AliasingPlan plan;
    const auto& resources    = rg.GetResources();
    const auto& sortedPasses = rg.GetSortedPasses();

    // Nur transiente Ressourcen mit gültigem Lifecycle betrachten
    std::vector<RGResourceID> candidates;
    for (const auto& r : resources)
    {
        if (r.lifetime != RGResourceLifetime::Transient) continue;
        if (r.firstUsePass == RG_INVALID_PASS) continue;
        candidates.push_back(r.id);
    }

    if (candidates.empty())
    {
        Debug::LogVerbose("ResourceAliaser.cpp: no transient resources to alias");
        return plan;
    }

    // Greedy Interval-Packing:
    // Sortiere Kandidaten nach firstUsePass (topologischer Index)
    std::sort(candidates.begin(), candidates.end(),
        [&](RGResourceID a, RGResourceID b) {
            return SortedIndex(resources[a].firstUsePass, sortedPasses)
                 < SortedIndex(resources[b].firstUsePass, sortedPasses);
        });

    // Weise jede Ressource einer bestehenden Gruppe zu oder erstelle neue
    std::vector<bool> assigned(resources.size(), false);

    for (RGResourceID candidateId : candidates)
    {
        if (assigned[candidateId]) continue;
        const RGResourceDesc& desc = resources[candidateId];

        // Suche Gruppe in der candidate mit allen Mitgliedern aliasieren kann
        bool foundGroup = false;
        for (auto& group : plan.groups)
        {
            bool allCompatible = true;
            for (RGResourceID memberId : group.resources)
            {
                if (!CanAlias(desc, resources[memberId], sortedPasses))
                { allCompatible = false; break; }
            }

            if (allCompatible)
            {
                group.resources.push_back(candidateId);
                const size_t sz  = sizeEstimator(desc);
                const size_t aln = 256u;
                group.requiredBytes     = std::max(group.requiredBytes, sz);
                group.requiredAlignment = std::max(group.requiredAlignment, aln);
                assigned[candidateId]   = true;
                foundGroup = true;
                ++plan.aliasingCount;
                break;
            }
        }

        if (!foundGroup)
        {
            // Neue Gruppe mit dieser Ressource als Seed
            AliasGroup g;
            g.resources.push_back(candidateId);
            g.requiredBytes     = sizeEstimator(desc);
            g.requiredAlignment = 256u;
            plan.groups.push_back(std::move(g));
            assigned[candidateId] = true;
        }
    }

    // Gruppen mit nur einer Ressource → nonAliased
    std::vector<AliasGroup> multiGroups;
    for (auto& g : plan.groups)
    {
        if (g.resources.size() == 1u)
            plan.nonAliased.push_back(g.resources[0]);
        else
            multiGroups.push_back(std::move(g));
    }
    plan.groups = std::move(multiGroups);

    Debug::Log("ResourceAliaser.cpp: Analyze - %zu candidates, "
        "%zu alias groups, %zu non-aliased, %u saved allocations",
        candidates.size(), plan.groups.size(),
        plan.nonAliased.size(), plan.aliasingCount);

    for (size_t gi = 0; gi < plan.groups.size(); ++gi)
    {
        std::string names;
        for (RGResourceID id : plan.groups[gi].resources)
            names += resources[id].debugName + " ";
        Debug::Log("ResourceAliaser.cpp:   Group %zu (%zu bytes): %s",
            gi, plan.groups[gi].requiredBytes, names.c_str());
    }

    return plan;
}

} // namespace engine::rendergraph
