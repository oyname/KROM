#pragma once
// =============================================================================
// KROM Engine - assets/AssetBase.hpp
// Gemeinsame CPU-seitige Asset-Basisdaten ohne Registry-Abhaengigkeit.
// =============================================================================
#include <cstdint>
#include <string>

namespace engine::assets {

// Zustand eines Assets im Ladeprozess
enum class AssetState : uint8_t
{
    Unloaded  = 0,
    Loading   = 1,
    Loaded    = 2,
    Failed    = 3,
    Evicted   = 4, // War geladen, wurde entfernt (Hot-Reload pending)
};

// Gemeinsame Basisklasse aller CPU-seitigen Assets
struct AssetBase
{
    std::string  path;
    std::string  debugName;
    AssetState   state = AssetState::Unloaded;
    uint64_t     lastModifiedTimestamp = 0ull; // fuer Hot-Reload
};

} // namespace engine::assets
