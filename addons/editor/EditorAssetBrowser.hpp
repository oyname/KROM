#pragma once

#include "core/Types.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace engine::renderer::addons::editor {

struct EditorFrameContext;

// Persistenter Zustand des Asset-Browsers.
// Wird zusammen mit EditorState als App-Member gehalten.
struct AssetBrowserState
{
    std::filesystem::path currentDir;

    // Inhalt von currentDir, einmal eingelesen statt in jedem Bild mehrfach.
    // Neu gelesen bei Ordnerwechsel, wenn sich der Ordner aendert (Datei
    // angelegt/geloescht/umbenannt) und spaetestens jede Sekunde (geaenderte
    // Dateien). listedStamps: Aenderungszeit je Datei (generischer Pfad).
    std::filesystem::path                       listedDir;
    std::filesystem::file_time_type             listedDirStamp{};
    std::chrono::steady_clock::time_point       listedAt{};
    std::vector<std::filesystem::directory_entry> listedEntries;
    std::unordered_map<std::string, uint64_t>   listedStamps;
    bool                  initialized = false;
    std::string           lastClickedMaterialPath;
    double                lastClickedMaterialTime = -1.0;

    // Inline-Rename: Doppelklick auf ein Asset → Name wird direkt editierbar.
    std::filesystem::path renamingPath;         // leer = kein Rename aktiv
    char                  renamingBuffer[256] = {};
    bool                  renamingNeedsFocus  = false;

    struct TextureThumbnail
    {
        TextureHandle gpuTexture = TextureHandle::Invalid();
        void*         imguiId    = nullptr;
        uint64_t      fileStamp  = 0u;
    };
    std::unordered_map<std::string, TextureThumbnail> textureThumbnails;

    enum class PreviewKind : uint8_t
    {
        Material = 0,
        Model,
        Prefab
    };

    enum class PreviewStatus : uint8_t
    {
        Empty = 0,
        Queued,
        Rendering,
        Ready,
        Failed
    };

    struct AssetPreviewThumbnail
    {
        PreviewKind        kind = PreviewKind::Material;
        PreviewStatus      status = PreviewStatus::Empty;
        RenderTargetHandle renderTarget = RenderTargetHandle::Invalid();
        TextureHandle      gpuTexture = TextureHandle::Invalid();
        void*              imguiId = nullptr;
        uint64_t           fileStamp = 0u;
        uint32_t           schemaVersion = 0u;
        // Aus dem Platten-Cache geladen (.krom/thumbnails), bleibt bis zum Ende bestehen.
        TextureHandle      diskTexture = TextureHandle::Invalid();
    };
    std::unordered_map<std::string, AssetPreviewThumbnail> assetPreviewThumbnails;

    // Wenn gesetzt: Asset-Browser navigiert in dieses Verzeichnis und
    // markiert die Datei beim nächsten Draw-Aufruf.
    std::filesystem::path highlightPath;

    // Ordner der auf Löschen-Bestätigung wartet
    std::filesystem::path deleteDirPending;

    // C++ Script-Asset Erstellung
    bool                  createScriptDialogOpen = false;
    std::filesystem::path createScriptDirectory;
    char                  createScriptNameBuffer[128] = {};

    // Icons: geladen aus engineEditorDir beim ersten Draw-Aufruf
    struct Icons
    {
        TextureHandle folder = TextureHandle::Invalid();
        void*         folderImguiId = nullptr;
        bool          loaded = false;
    } icons;

    // Ordner-Baum: welche Ordner sind aufgeklappt
    std::unordered_set<std::string> expandedDirs;

    // Ob der Ordner-Baum links angezeigt wird
    bool showTree = true;
};

void DrawAssetBrowser(EditorFrameContext& ctx, AssetBrowserState& browser);
void ImportExternalFilesToAssetBrowser(EditorFrameContext& ctx,
                                       AssetBrowserState& browser,
                                       const std::vector<std::filesystem::path>& files);

// Lädt alle Texturen im Asset-Root rekursiv vor (GPU-Upload + Thumbnail-Cache).
// Aufruf nach Projekt-Laden damit der erste Klick auf "Textures" sofort reagiert.
void PreloadAllTextures(EditorFrameContext& ctx, AssetBrowserState& browser);

} // namespace engine::renderer::addons::editor
