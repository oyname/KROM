#pragma once
#include <string>

namespace engine::platform::dialog {

// Beschreibt einen Datei-Filter fuer den Oeffnen-Dialog.
// label:   Anzeigename, z.B. "KROM Projektdatei (krom-project.json)"
// pattern: Glob-Muster,  z.B. "*.json"   (Win32-Format; auf anderen Plattformen spaeter angepasst)
struct FileFilter
{
    const char* label;
    const char* pattern;
};

// Zeigt einen nativen "Datei oeffnen"-Dialog.
//
//   title          - Dialog-Titel; nullptr = OS-Standard
//   filters        - Array von FileFilter-Eintraegen; nullptr = alle Dateien
//   filterCount    - Anzahl der Filter-Eintraege
//   defaultFilename- Vorauswahl im Dateinamenfeld; nullptr = kein Vorschlag
//   parentHandle   - Plattformspezifischer Parent-Handle:
//                      Win32   → HWND (void*-Cast)
//                      Linux   → reserviert (ignoriert)
//                      macOS   → reserviert (ignoriert)
//
// Gibt den absoluten Pfad der gewaehlten Datei zurueck,
// oder einen leeren String wenn der Dialog abgebrochen wurde oder
// keine native Implementierung verfuegbar ist.
std::string BrowseForFile(const char*       title,
                           const FileFilter* filters,
                           int               filterCount,
                           const char*       defaultFilename = nullptr,
                           void*             parentHandle    = nullptr,
                           const char*       initialFolder   = nullptr);

// Zeigt einen nativen "Ordner auswaehlen"-Dialog.
//
//   title        - Dialog-Titel; nullptr = OS-Standard
//   parentHandle - s.o.
//
// Gibt den absoluten Ordnerpfad zurueck, oder "" bei Abbruch / nicht verfuegbar.
std::string BrowseForFolder(const char* title,
                             void*       parentHandle = nullptr);

// Gibt true zurueck wenn native Datei-Dialoge auf dieser Plattform unterstuetzt werden.
// Kann vom Editor genutzt werden, um den "..."-Browse-Button anzuzeigen oder zu verstecken.
bool IsAvailable() noexcept;

} // namespace engine::platform::dialog
