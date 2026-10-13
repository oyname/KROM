#pragma once

// MaterialDomain enum ist in MaterialTypes.hpp definiert (zirkuläre-Include-frei).
// Diese Datei enthält DomainConstraints und ValidatePolicyForDomain.
#include "renderer/MaterialTypes.hpp"
#include "core/Debug.hpp"

namespace engine::renderer {

// Constraints, die für eine Domain gelten.
// forbidden: States, die ein Material in dieser Domain NICHT setzen darf.
// required:  States, die ein Material in dieser Domain IMMER haben muss.
// defaults:  Vorschlagswerte, wenn das Material keinen eigenen Wert hat.
struct DomainConstraints
{
    struct DepthConstraints
    {
        bool testForbidden  = false;
        bool writeForbidden = false;
        bool testRequired   = false;
        bool writeRequired  = false;
        bool testDefault    = true;
        bool writeDefault   = true;
    };

    struct CullConstraints
    {
        bool noneForbidden = false;  // Cull::None darf nicht verwendet werden
        bool noneRequired  = false;  // Muss Cull::None sein
    };

    DepthConstraints depth{};
    CullConstraints  cull{};
    bool             castShadowsAllowed = true;
};

[[nodiscard]] inline const DomainConstraints& GetDomainConstraints(MaterialDomain domain) noexcept
{
    static const DomainConstraints kMesh = []() {
        DomainConstraints c{};
        c.depth.testDefault  = true;
        c.depth.writeDefault = true;
        return c;
    }();

    static const DomainConstraints kPostprocess = []() {
        DomainConstraints c{};
        c.depth.testForbidden  = true;
        c.depth.writeForbidden = true;
        c.cull.noneRequired    = true;
        c.castShadowsAllowed   = false;
        return c;
    }();

    static const DomainConstraints kUI = []() {
        DomainConstraints c{};
        c.depth.testForbidden  = true;
        c.depth.writeForbidden = true;
        c.cull.noneRequired    = true;
        c.castShadowsAllowed   = false;
        return c;
    }();

    static const DomainConstraints kShadow = []() {
        DomainConstraints c{};
        c.depth.testRequired  = true;
        c.depth.writeRequired = true;
        c.castShadowsAllowed  = false;  // Domain ist Shadow — kein rekursiver Cast
        return c;
    }();

    static const DomainConstraints kDepthOnly = []() {
        DomainConstraints c{};
        c.depth.testRequired  = true;
        c.depth.writeRequired = true;
        c.castShadowsAllowed  = false;
        return c;
    }();

    static const DomainConstraints kDebug = []() {
        DomainConstraints c{};
        c.castShadowsAllowed = false;
        return c;
    }();

    switch (domain)
    {
    case MaterialDomain::Mesh:        return kMesh;
    case MaterialDomain::Postprocess: return kPostprocess;
    case MaterialDomain::UI:          return kUI;
    case MaterialDomain::Shadow:      return kShadow;
    case MaterialDomain::DepthOnly:   return kDepthOnly;
    case MaterialDomain::Debug:       return kDebug;
    default:                          return kMesh;
    }
}

// Gibt true zurück, wenn keine Verletzung gefunden wurde.
// materialName darf nullptr sein (wird dann als "<unknown>" ausgegeben).
[[nodiscard]] inline bool ValidatePolicyForDomain(const MaterialRenderPolicy& policy,
                                                   MaterialDomain domain,
                                                   const char* materialName) noexcept
{
    const DomainConstraints& c = GetDomainConstraints(domain);
    const char* name = materialName ? materialName : "<unknown>";
    bool valid = true;

    if (c.depth.testForbidden && policy.depth.test)
    {
        Debug::LogWarning("[MaterialDomain] '%s': depth.test=true ist in dieser Domain verboten", name);
        valid = false;
    }
    if (c.depth.writeForbidden && policy.depth.write)
    {
        Debug::LogWarning("[MaterialDomain] '%s': depth.write=true ist in dieser Domain verboten", name);
        valid = false;
    }
    if (c.depth.testRequired && !policy.depth.test)
    {
        Debug::LogWarning("[MaterialDomain] '%s': depth.test=false ist in dieser Domain verboten (muss true sein)", name);
        valid = false;
    }
    if (c.depth.writeRequired && !policy.depth.write)
    {
        Debug::LogWarning("[MaterialDomain] '%s': depth.write=false ist in dieser Domain verboten (muss true sein)", name);
        valid = false;
    }
    if (c.cull.noneRequired && policy.cull.mode != MaterialCullMode::None && !policy.cull.doubleSided)
    {
        Debug::LogWarning("[MaterialDomain] '%s': cullMode muss None sein in dieser Domain", name);
        valid = false;
    }
    if (!c.castShadowsAllowed && policy.castShadows)
    {
        Debug::LogWarning("[MaterialDomain] '%s': castShadows=true ist in dieser Domain nicht erlaubt", name);
        valid = false;
    }

    return valid;
}

} // namespace engine::renderer
