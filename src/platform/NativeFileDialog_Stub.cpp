#include "platform/NativeFileDialog.hpp"

// Stub-Implementierung fuer Plattformen ohne nativen Dialog-Support (aktuell Linux, macOS).
// Gibt immer einen leeren String zurueck; IsAvailable() liefert false.
//
// Spaetere Erweiterung:
//   Linux  → GTK GtkFileChooserDialog  oder  popen("zenity --file-selection …")
//   macOS  → NSOpenPanel (Objective-C++, .mm-Datei) oder popen("osascript …")
//
// Dafuer einfach diese Datei durch eine plattformspezifische Implementierung ersetzen
// und den entsprechenden CMake-Zweig in CMakeLists.txt ausbauen.

namespace engine::platform::dialog {

std::string BrowseForFile(const char* /*title*/,
                           const FileFilter* /*filters*/,
                           int               /*filterCount*/,
                           const char*       /*defaultFilename*/,
                           void*             /*parentHandle*/,
                           const char*       /*initialFolder*/)
{
    return {};
}

std::string BrowseForFolder(const char* /*title*/, void* /*parentHandle*/)
{
    return {};
}

bool IsAvailable() noexcept
{
    return false;
}

} // namespace engine::platform::dialog
