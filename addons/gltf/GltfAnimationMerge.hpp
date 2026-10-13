#pragma once
// =============================================================================
// KROM Engine - addons/gltf/GltfAnimationMerge.hpp
// Hilfsfunktionen für den GltfImporter — keine cgltf-Abhängigkeit.
//
// Das ursprüngliche TRS-Track-Merge (BoneTracks → merged Keyframes) entfällt:
// Seit der AnimationClip-Neugestaltung trägt jeder Channel eine eigene
// Zeitreihe (T, R oder S getrennt). Der GltfImporter erstellt Channels direkt
// aus den cgltf-Accessors — kein Union-Zeitgitter mehr nötig.
//
// Diese Datei verbleibt als Forward-Header falls andere Addons das Include
// referenzieren, enthält aber nur noch die Namespace-Öffnung.
// =============================================================================
#include "assets/AnimationClip.hpp"

namespace engine::addons::gltf {
// Keine Inhalte mehr erforderlich.
// GltfImporter baut AnimationChannel-Objekte direkt ohne Merge-Schritt.
} // namespace engine::addons::gltf
