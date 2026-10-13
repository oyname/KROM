#pragma once
// =============================================================================
// KROM Engine — renderer/VertexLayoutContract.hpp
// Deklariert den Vertex-Layout-Vertrag zwischen Shader-Preset und Mesh-Geometrie.
// =============================================================================
#include "renderer/RendererTypes.hpp"
#include <string>
#include <vector>

namespace engine::renderer {

// Beschreibt eine einzelne Vertex-Anforderung (Semantic + kanonisches Format).
struct VertexAttributeRequirement
{
    VertexSemantic semantic;
    Format         format;
};

// Vertrag zwischen einem Shader-Preset und der Mesh-Geometrie.
//   required: muss im Mesh vorhanden sein — fehlt einer → Fehler
//   optional: wird eingeschlossen wenn vorhanden, sonst übersprungen
struct VertexLayoutContract
{
    std::vector<VertexAttributeRequirement> required;
    std::vector<VertexAttributeRequirement> optional;
};

// Bitmaske über VertexSemantic-Werte (Bit N = Semantic mit Enum-Wert N).
using VertexSemanticMask = uint32_t;

inline constexpr VertexSemanticMask SemanticBit(VertexSemantic s) noexcept
{
    return 1u << static_cast<uint32_t>(s);
}

// Baut ein VertexLayout aus einem Vertrag und der vorhandenen Semantic-Maske.
// Attribute werden immer in der kanonischen Reihenfolge (= KMesh-Interleaving) emittiert.
// Rückgabe: leeres VertexLayout (.attributes.empty()) bei fehlendem Required-Semantic.
[[nodiscard]] VertexLayout ResolveVertexLayout(const VertexLayoutContract& contract,
                                               VertexSemanticMask          available,
                                               std::string*                outError = nullptr);

// ---------------------------------------------------------------------------
// Preset-Fabriken für die eingebauten Shader-Familien
// ---------------------------------------------------------------------------
namespace VertexContracts {

// Position + Normal required; Tangent, TexCoord0, Color0 optional.
[[nodiscard]] VertexLayoutContract StaticLit()  noexcept;

// Position, Normal, Tangent, TexCoord0 required; Color0 optional.
[[nodiscard]] VertexLayoutContract PbrLit()     noexcept;

// Position, Normal, Tangent, TexCoord0, BoneWeight, BoneIndex required; Color0 optional.
[[nodiscard]] VertexLayoutContract SkinnedLit() noexcept;

// Position required; BoneWeight + BoneIndex optional (Skinned-Shadow).
[[nodiscard]] VertexLayoutContract Shadow()     noexcept;

} // namespace VertexContracts
} // namespace engine::renderer
