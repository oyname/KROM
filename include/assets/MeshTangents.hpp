#pragma once

#include "assets/AssetRegistry.hpp"

namespace engine::assets {

struct TangentGenerationOptions
{
    bool overwriteExistingTangents = false;
    bool requireNormals = true;
    bool requireTexcoords = true;
};

// Erzeugt industrietaugliche Tangenten als float4 pro Vertex:
// xyz = Tangent, w = Handedness/Bitangent-Sign.
// Bitangent wird nicht persistent gespeichert und später im Shader aus
// cross(N, T) * sign rekonstruiert.
//
// Rückgabe:
//   true  -> Tangentdaten wurden erzeugt oder waren bereits gültig vorhanden.
//   false -> Erzeugung nicht möglich (z.B. fehlende Positionen/Normalen/UVs).
bool GenerateTangents(SubMeshData& subMesh,
                      const TangentGenerationOptions& options = {}) noexcept;

// Stellt sicher, dass Tangenten vorhanden sind. Erzeugt sie nur, wenn der
// Tangent-Channel fehlt oder semantisch veraltet ist.
bool EnsureTangents(SubMeshData& subMesh,
                    const TangentGenerationOptions& options = {}) noexcept;

bool HasValidTangents(const SubMeshData& subMesh) noexcept;

} // namespace engine::assets
