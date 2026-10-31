#pragma once
// =============================================================================
// KROM Engine - addons/editor/EditorThumbnailCache.hpp
// Vorschaubilder des Asset-Browsers auf der Platte (wie Unitys Library-Ordner):
// <Projekt>/.krom/thumbnails/<Hash des Asset-Pfads>.kthumb
//
// Pro Asset genau eine Datei (Name nur aus dem Pfad), darin Asset-Pfad,
// Aenderungszeit des Assets und die Pixel (RGBA8, oben nach unten). Ein
// geaendertes Asset ueberschreibt sein Bild; Loeschen/Umbenennen im Editor
// nehmen es mit. CleanupThumbnailCache entfernt beim Start Bilder ohne Asset
// und haelt den Ordner unter einer Obergrenze (zuletzt benutzte bleiben).
// =============================================================================
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace engine::renderer::addons::editor {

inline constexpr uint64_t kThumbnailCacheMaxBytes = 200ull * 1024ull * 1024ull;

[[nodiscard]] std::filesystem::path ThumbnailCacheDir(const std::filesystem::path& projectRoot);
// assetRelPath: Pfad relativ zu <Projekt>/Assets (UTF-8, mit '/').
[[nodiscard]] std::filesystem::path ThumbnailCacheFile(const std::filesystem::path& projectRoot,
                                                       const std::string& assetRelPath);

bool SaveCachedThumbnail(const std::filesystem::path& projectRoot, const std::string& assetRelPath,
                         uint64_t assetStamp, uint32_t width, uint32_t height,
                         const std::vector<uint8_t>& rgba);
// false, wenn kein Bild da ist oder es zu einem anderen Stand des Assets gehoert.
// Ein Treffer markiert das Bild als zuletzt benutzt.
bool LoadCachedThumbnail(const std::filesystem::path& projectRoot, const std::string& assetRelPath,
                         uint64_t assetStamp, uint32_t& outWidth, uint32_t& outHeight,
                         std::vector<uint8_t>& outRgba);
void RemoveCachedThumbnail(const std::filesystem::path& projectRoot, const std::string& assetRelPath);
void RenameCachedThumbnail(const std::filesystem::path& projectRoot, const std::string& oldRelPath,
                           const std::string& newRelPath);
// Entfernt Bilder, deren Asset fehlt, und danach die am laengsten nicht
// benutzten, bis der Ordner unter maxBytes liegt. Liefert die Anzahl.
size_t CleanupThumbnailCache(const std::filesystem::path& projectRoot,
                             uint64_t maxBytes = kThumbnailCacheMaxBytes);

// Verkleinert RGBA8 um einen ganzzahligen Faktor (Mittelwert je Block).
[[nodiscard]] std::vector<uint8_t> DownscaleRgba(const std::vector<uint8_t>& rgba, uint32_t width,
                                                 uint32_t height, uint32_t factor);

} // namespace engine::renderer::addons::editor
