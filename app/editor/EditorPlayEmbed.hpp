#pragma once
// =============================================================================
// KROM Engine - app/editor/EditorPlayEmbed.hpp
// Play im Game-Reiter: startet die Projekt-Runtime als eigenen Prozess, dessen
// Fenster als Kindfenster im Editorfenster liegt (--parent-hwnd), und haelt
// es auf der Flaeche des Game-Reiters. Nur Win32; sonst liefert Launch false.
// =============================================================================
#include "platform/IWindow.hpp"
#include <cstdint>
#include <filesystem>

namespace engine::app {

class EmbeddedPlayProcess
{
public:
    EmbeddedPlayProcess() = default;
    ~EmbeddedPlayProcess();
    EmbeddedPlayProcess(const EmbeddedPlayProcess&) = delete;
    EmbeddedPlayProcess& operator=(const EmbeddedPlayProcess&) = delete;

    // Startet die Runtime mit dem Projekt; ein laufendes Spiel wird vorher beendet.
    [[nodiscard]] bool Launch(const std::filesystem::path& exePath,
                              const std::filesystem::path& projectFile,
                              platform::NativeWindowHandle parent,
                              uint32_t width, uint32_t height);

    // Pro Frame: Fenster auf das Rechteck (Client-Koordinaten des Editors) legen
    // oder ausblenden; erkennt das Ende des Prozesses.
    void Update(bool visible, int x, int y, int width, int height);

    // Bittet das Spiel um Beenden (WM_CLOSE); haengt es, beendet Update es nach
    // kurzer Frist hart.
    void Stop();
    // Beendet das Spiel sofort (Editor schliesst).
    void Terminate();

    [[nodiscard]] bool IsRunning() const noexcept { return m_process != nullptr; }

private:
    void Reset();

    void* m_process = nullptr;      // HANDLE
    unsigned long m_processId = 0u;
    void* m_parent = nullptr;       // HWND des Editors
    void* m_window = nullptr;       // HWND der Runtime (Kind von m_parent)
    bool m_shown = false;
    int m_x = 0, m_y = 0, m_width = 0, m_height = 0;
    uint64_t m_stopRequestedMs = 0u;
};

} // namespace engine::app
