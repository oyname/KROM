// =============================================================================
// KROM Engine — src/renderer/VertexLayoutContract.cpp
// =============================================================================
#include "renderer/VertexLayoutContract.hpp"
#include "core/Debug.hpp"

namespace engine::renderer {

namespace {

struct CanonicalEntry
{
    VertexSemantic semantic;
    Format         format;
    uint32_t       sizeBytes;
};

// Kanonische Attribut-Reihenfolge — muss identisch mit KMeshSerializer::BuildLayout() sein,
// damit der resolvedLayout exakt die Byte-Offsets des interleaved .kmesh-Puffers beschreibt.
static constexpr CanonicalEntry kCanonical[] = {
    { VertexSemantic::Position,   Format::RGB32_FLOAT,  12u },
    { VertexSemantic::Normal,     Format::RGB32_FLOAT,  12u },
    { VertexSemantic::Tangent,    Format::RGBA32_FLOAT, 16u },
    { VertexSemantic::TexCoord0,  Format::RG32_FLOAT,    8u },
    { VertexSemantic::Color0,     Format::RGBA32_FLOAT, 16u },
    { VertexSemantic::BoneWeight, Format::RGBA32_FLOAT, 16u },
    { VertexSemantic::BoneIndex,  Format::RGBA32_UINT,  16u },
};

const char* SemanticName(VertexSemantic s) noexcept
{
    switch (s)
    {
    case VertexSemantic::Position:   return "Position";
    case VertexSemantic::Normal:     return "Normal";
    case VertexSemantic::Tangent:    return "Tangent";
    case VertexSemantic::Bitangent:  return "Bitangent";
    case VertexSemantic::TexCoord0:  return "TexCoord0";
    case VertexSemantic::TexCoord1:  return "TexCoord1";
    case VertexSemantic::Color0:     return "Color0";
    case VertexSemantic::BoneWeight: return "BoneWeight";
    case VertexSemantic::BoneIndex:  return "BoneIndex";
    default:                         return "Unknown";
    }
}

const CanonicalEntry* FindCanonical(VertexSemantic semantic) noexcept
{
    for (const auto& canon : kCanonical)
    {
        if (canon.semantic == semantic)
            return &canon;
    }
    return nullptr;
}

bool ValidateRequirement(const VertexAttributeRequirement& requirement, std::string* outError)
{
    const CanonicalEntry* canon = FindCanonical(requirement.semantic);
    if (!canon)
    {
        const char* name = SemanticName(requirement.semantic);
        Debug::LogError("ResolveVertexLayout: semantic '%s' has no canonical layout entry", name);
        if (outError)
            *outError = std::string("Semantic has no canonical layout entry: ") + name;
        return false;
    }

    if (canon->format != requirement.format)
    {
        const char* name = SemanticName(requirement.semantic);
        Debug::LogError("ResolveVertexLayout: semantic '%s' requests a non-canonical format", name);
        if (outError)
            *outError = std::string("Semantic requests non-canonical format: ") + name;
        return false;
    }

    return true;
}

} // anonymous namespace

VertexLayout ResolveVertexLayout(const VertexLayoutContract& contract,
                                  VertexSemanticMask          available,
                                  std::string*                outError)
{
    VertexSemanticMask includeMask = 0u;

    for (const auto& req : contract.required)
    {
        if (!ValidateRequirement(req, outError))
            return {};

        const VertexSemanticMask bit = SemanticBit(req.semantic);
        if (!(available & bit))
        {
            const char* name = SemanticName(req.semantic);
            Debug::LogError("ResolveVertexLayout: required semantic '%s' missing", name);
            if (outError)
                *outError = std::string("Required semantic missing: ") + name;
            return {};
        }
        includeMask |= bit;
    }

    for (const auto& opt : contract.optional)
    {
        if (!ValidateRequirement(opt, outError))
            return {};

        const VertexSemanticMask bit = SemanticBit(opt.semantic);
        if (available & bit)
            includeMask |= bit;
    }

    VertexLayout layout;
    uint32_t offset = 0u;

    for (const auto& canon : kCanonical)
    {
        const VertexSemanticMask bit = SemanticBit(canon.semantic);
        if (!(available & bit))
            continue;

        if (includeMask & bit)
            layout.attributes.push_back({ canon.semantic, canon.format, 0u, offset });

        offset += canon.sizeBytes;
    }

    if (!layout.attributes.empty())
        layout.bindings.push_back({ 0u, offset });

    return layout;
}

// ---------------------------------------------------------------------------

namespace VertexContracts {

VertexLayoutContract StaticLit() noexcept
{
    return {
        .required = {
            { VertexSemantic::Position,  Format::RGB32_FLOAT  },
            { VertexSemantic::Normal,    Format::RGB32_FLOAT  },
        },
        .optional = {
            { VertexSemantic::Tangent,   Format::RGBA32_FLOAT },
            { VertexSemantic::TexCoord0, Format::RG32_FLOAT   },
            { VertexSemantic::Color0,    Format::RGBA32_FLOAT },
        },
    };
}

VertexLayoutContract PbrLit() noexcept
{
    return {
        .required = {
            { VertexSemantic::Position,  Format::RGB32_FLOAT  },
            { VertexSemantic::Normal,    Format::RGB32_FLOAT  },
            { VertexSemantic::Tangent,   Format::RGBA32_FLOAT },
            { VertexSemantic::TexCoord0, Format::RG32_FLOAT   },
        },
        .optional = {
            { VertexSemantic::Color0,    Format::RGBA32_FLOAT },
        },
    };
}

VertexLayoutContract SkinnedLit() noexcept
{
    return {
        .required = {
            { VertexSemantic::Position,   Format::RGB32_FLOAT  },
            { VertexSemantic::Normal,     Format::RGB32_FLOAT  },
            { VertexSemantic::Tangent,    Format::RGBA32_FLOAT },
            { VertexSemantic::TexCoord0,  Format::RG32_FLOAT   },
            { VertexSemantic::BoneWeight, Format::RGBA32_FLOAT },
            { VertexSemantic::BoneIndex,  Format::RGBA32_UINT  },
        },
        .optional = {
            { VertexSemantic::Color0,     Format::RGBA32_FLOAT },
        },
    };
}

VertexLayoutContract Shadow() noexcept
{
    return {
        .required = {
            { VertexSemantic::Position,   Format::RGB32_FLOAT  },
        },
        .optional = {
            { VertexSemantic::BoneWeight, Format::RGBA32_FLOAT },
            { VertexSemantic::BoneIndex,  Format::RGBA32_UINT  },
        },
    };
}

} // namespace VertexContracts
} // namespace engine::renderer
