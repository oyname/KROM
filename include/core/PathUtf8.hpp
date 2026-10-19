#pragma once
// =============================================================================
// KROM Engine - core/PathUtf8.hpp
// Einheitliche Kodierung von Dateipfaden.
//
// Engine-Konvention: Pfade in std::string / const char* sind IMMER UTF-8
// (Szenen-JSON, Materialien, Projektdateien, IFilesystem-API).
//
// Unter MSVC interpretieren fs::path(std::string), path.string() und
// generic_string() die Bytes dagegen als ANSI-Codepage. Deshalb wird an jeder
// Grenze zwischen std::string und std::filesystem::path ausschließlich über
// diese Helfer konvertiert (char8_t / u8string, unabhängig von der Codepage).
// =============================================================================

#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>

namespace engine {

// UTF-8-String -> Pfad. Ungültiges UTF-8 (Altbestand mit ANSI-Bytes, z. B.
// "einheitsw\xFCrfel") wird wie früher über die native Codepage gelesen,
// statt eine Exception zu werfen.
[[nodiscard]] inline std::filesystem::path PathFromUtf8(std::string_view utf8)
{
    try
    {
        return std::filesystem::path(
            std::u8string_view(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
    }
    catch (...)
    {
        try { return std::filesystem::path(std::string(utf8)); }
        catch (...) { return {}; }
    }
}

[[nodiscard]] inline std::filesystem::path PathFromUtf8(const char* utf8)
{
    return PathFromUtf8(std::string_view(utf8 ? utf8 : ""));
}

[[nodiscard]] inline std::filesystem::path PathFromUtf8(const std::string& utf8)
{
    return PathFromUtf8(std::string_view(utf8));
}

// Pfad -> UTF-8-String (native Trenner).
[[nodiscard]] inline std::string PathToUtf8(const std::filesystem::path& path)
{
    try
    {
        const std::u8string u8 = path.u8string();
        return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
    }
    catch (...) { return {}; }
}

// Pfad -> UTF-8-String mit '/' als Trenner (für gespeicherte Asset-Referenzen).
[[nodiscard]] inline std::string PathToGenericUtf8(const std::filesystem::path& path)
{
    try
    {
        const std::u8string u8 = path.generic_u8string();
        return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
    }
    catch (...) { return {}; }
}

#if defined(_WIN32)
// UTF-8 -> UTF-16 für Wide-Win32/CRT-APIs (_wsystem, _wpopen, ...).
[[nodiscard]] inline std::wstring WideFromUtf8(std::string_view utf8)
{
    return PathFromUtf8(utf8).native();
}
#endif

// fopen-Ersatz für UTF-8-Pfade (unter Windows über _wfopen).
[[nodiscard]] inline std::FILE* OpenFileUtf8(const std::filesystem::path& path, const char* mode)
{
#if defined(_WIN32)
    std::wstring wideMode;
    for (const char* m = mode ? mode : "rb"; *m; ++m)
        wideMode.push_back(static_cast<wchar_t>(*m));
    return _wfopen(path.c_str(), wideMode.c_str());
#else
    return std::fopen(path.c_str(), mode ? mode : "rb");
#endif
}

[[nodiscard]] inline std::FILE* OpenFileUtf8(const char* utf8Path, const char* mode)
{
    return OpenFileUtf8(PathFromUtf8(utf8Path), mode);
}

[[nodiscard]] inline std::FILE* OpenFileUtf8(const std::string& utf8Path, const char* mode)
{
    return OpenFileUtf8(PathFromUtf8(utf8Path), mode);
}

} // namespace engine
