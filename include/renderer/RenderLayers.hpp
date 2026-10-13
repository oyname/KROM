#pragma once
#include <cstdint>

namespace engine::renderer {

// ============================================================================
// Render-Layer Bitmask-Konstanten
// Jedes Objekt traegt ein layerMask-Bitfeld (MeshComponent::layerMask).
// Jede Kamera traegt ein visibilityLayerMask-Bitfeld (RenderView).
// Ein Objekt ist sichtbar wenn: (objekt.layerMask & kamera.visibilityLayerMask) != 0
// ============================================================================

/// Standard-Szenenobjekte (Meshes, Lights, usw.)
inline constexpr uint32_t LAYER_DEFAULT      = 0x00000001u;

/// Editor-Gizmos (immer sichtbar, Depth-Buffer wird ignoriert) — Transform-Pfeile, Licht-Icons usw.
inline constexpr uint32_t LAYER_EDITOR_GIZMO = 0x00000002u;

/// Editor-Gizmos mit normalem Depth-Test (werden von Szenengeometrie verdeckt) — z.B. Kamera-Modell.
inline constexpr uint32_t LAYER_EDITOR_GIZMO_DEPTH = 0x00000100u;

/// Transparente Objekte (z.B. Glas, Partikel).
inline constexpr uint32_t LAYER_TRANSPARENT  = 0x00000004u;

/// UI-Elemente in der Spielwelt (z.B. World-Space-Canvas).
inline constexpr uint32_t LAYER_UI           = 0x00000008u;

/// Benutzerdefinierte Layer (frei verwendbar per Projekt).
inline constexpr uint32_t LAYER_USER_0       = 0x00000010u;
inline constexpr uint32_t LAYER_USER_1       = 0x00000020u;
inline constexpr uint32_t LAYER_USER_2       = 0x00000040u;
inline constexpr uint32_t LAYER_USER_3       = 0x00000080u;

/// Editor-intern: Materialvorschau-Kugel. Nie fuer normale Szenenobjekte verwenden.
inline constexpr uint32_t LAYER_EDITOR_MATERIAL_PREVIEW = 0x80000000u;

/// Editor-intern: Asset-Browser-Thumbnails. Nie fuer normale Szenenobjekte verwenden.
inline constexpr uint32_t LAYER_EDITOR_ASSET_THUMBNAIL = 0x40000000u;

/// Editor-intern: Prefab-Editor-Vorschau. Nie fuer normale Szenenobjekte verwenden.
inline constexpr uint32_t LAYER_EDITOR_PREFAB_PREVIEW  = 0x20000000u;

/// Alle Layer sichtbar (Editor-Kamera Standard)
inline constexpr uint32_t LAYER_ALL          = 0xFFFFFFFFu;

/// Kein Layer
inline constexpr uint32_t LAYER_NONE         = 0x00000000u;

/// Anzeigenamen pro Bit (Index 0..31).
/// Leerer String bedeutet: kein benannter Layer (Bit reserviert).
inline constexpr const char* LAYER_NAMES[32] = {
    "Default",     // bit 0
    "EditorGizmo", // bit 1
    "Transparent", // bit 2
    "UI",          // bit 3
    "User0",       // bit 4
    "User1",       // bit 5
    "User2",       // bit 6
    "User3",       // bit 7
    "", "", "", "", "", "", "", "",  // bits 8–15
    "", "", "", "", "", "", "", "",  // bits 16–23
    "", "", "", "", "", "", "", ""   // bits 24–31
};

} // namespace engine::renderer
