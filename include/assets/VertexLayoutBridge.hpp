#pragma once
// =============================================================================
// KROM Engine — assets/VertexLayoutBridge.hpp
// Brücke zwischen SubMeshData (assets-Layer) und VertexLayoutContract (renderer-Layer).
// Enthält nur inline-Hilfsfunktionen — keine eigene Übersetzungseinheit nötig.
// =============================================================================
#include "assets/AssetRegistry.hpp"
#include "renderer/VertexLayoutContract.hpp"

namespace engine::assets {

// Leitet aus SubMeshData eine VertexSemanticMask ab.
[[nodiscard]] inline renderer::VertexSemanticMask SubMeshSemanticMask(
    const SubMeshData& sm) noexcept
{
    using renderer::VertexSemantic;
    renderer::VertexSemanticMask mask = 0u;
    if (!sm.positions.empty())   mask |= renderer::SemanticBit(VertexSemantic::Position);
    if (!sm.normals.empty())     mask |= renderer::SemanticBit(VertexSemantic::Normal);
    if (!sm.tangents.empty())    mask |= renderer::SemanticBit(VertexSemantic::Tangent);
    if (!sm.uvs.empty())         mask |= renderer::SemanticBit(VertexSemantic::TexCoord0);
    if (!sm.colors.empty())      mask |= renderer::SemanticBit(VertexSemantic::Color0);
    if (!sm.boneWeights.empty() && !sm.boneIndices.empty())
    {
        mask |= renderer::SemanticBit(VertexSemantic::BoneWeight);
        mask |= renderer::SemanticBit(VertexSemantic::BoneIndex);
    }
    return mask;
}

// Löst ein konkretes VertexLayout aus einem Vertrag und einem SubMeshData auf.
// Gibt ein leeres VertexLayout zurück wenn ein Required-Semantic fehlt.
[[nodiscard]] inline renderer::VertexLayout ResolveVertexLayout(
    const renderer::VertexLayoutContract& contract,
    const SubMeshData&                    sm,
    std::string*                          outError = nullptr)
{
    return renderer::ResolveVertexLayout(contract, SubMeshSemanticMask(sm), outError);
}

} // namespace engine::assets
