#include "platform/NativeFileDialog.hpp"

// Wird nur auf Windows kompiliert (CMakeLists.txt waehlt diese Datei per $<IF:$<BOOL:${WIN32}>,...>).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <shobjidl.h>   // IFileOpenDialog, IFileDialog, CLSID_FileOpenDialog
#include <combaseapi.h> // CoCreateInstance, CoInitializeEx, CoUninitialize, CoTaskMemFree
#include <windows.h>    // WideCharToMultiByte, MultiByteToWideChar, HWND, LPWSTR

#include <string>
#include <vector>

namespace engine::platform::dialog {

namespace {

// Initialisiert COM fuer diesen Thread und gibt zurueck ob wir selbst initialisiert haben
// (noetig um CoUninitialize nur dann aufzurufen wenn wir es besitzen).
// S_OK   → wir haben initialisiert → beim Ende CoUninitialize aufrufen
// S_FALSE→ war schon initialisiert → NICHT CoUninitialize aufrufen
bool ComInit()
{
    const HRESULT hr = CoInitializeEx(nullptr,
                                      COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    return hr == S_OK;
}

std::string WideToUtf8(LPCWSTR wide)
{
    if (!wide || wide[0] == L'\0')
        return {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, wide, -1,
                                        nullptr, 0, nullptr, nullptr);
    if (len <= 0)
        return {};
    std::string result(static_cast<size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1,
                        result.data(), len, nullptr, nullptr);
    return result;
}

std::wstring Utf8ToWide(const char* utf8)
{
    if (!utf8 || utf8[0] == '\0')
        return {};
    const int len = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    if (len <= 0)
        return {};
    std::wstring result(static_cast<size_t>(len - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, result.data(), len);
    return result;
}

} // namespace

// ---------------------------------------------------------------------------

std::string BrowseForFile(const char*       title,
                           const FileFilter* filters,
                           int               filterCount,
                           const char*       defaultFilename,
                           void*             parentHandle,
                           const char*       initialFolder)
{
    const bool ownCom = ComInit();
    std::string result;

    IFileOpenDialog* dlg = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                                   CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))))
    {
        // Filter aufbauen
        std::vector<std::wstring>       labelBuf;
        std::vector<std::wstring>       patternBuf;
        std::vector<COMDLG_FILTERSPEC>  specs;

        if (filters && filterCount > 0)
        {
            labelBuf.reserve(static_cast<size_t>(filterCount));
            patternBuf.reserve(static_cast<size_t>(filterCount));
            specs.reserve(static_cast<size_t>(filterCount));

            for (int i = 0; i < filterCount; ++i)
            {
                labelBuf.push_back(Utf8ToWide(filters[i].label));
                patternBuf.push_back(Utf8ToWide(filters[i].pattern));
                COMDLG_FILTERSPEC spec{};
                spec.pszName = labelBuf.back().c_str();
                spec.pszSpec = patternBuf.back().c_str();
                specs.push_back(spec);
            }
            dlg->SetFileTypes(static_cast<UINT>(specs.size()), specs.data());
            dlg->SetFileTypeIndex(1u);
        }

        if (title)
            dlg->SetTitle(Utf8ToWide(title).c_str());

        IShellItem* initialFolderItem = nullptr;
        if (initialFolder && initialFolder[0] != '\0')
        {
            const std::wstring wideInitialFolder = Utf8ToWide(initialFolder);
            if (SUCCEEDED(SHCreateItemFromParsingName(
                    wideInitialFolder.c_str(), nullptr, IID_PPV_ARGS(&initialFolderItem))))
            {
                dlg->SetDefaultFolder(initialFolderItem);
                dlg->SetFolder(initialFolderItem);
            }
        }

        if (defaultFilename)
            dlg->SetFileName(Utf8ToWide(defaultFilename).c_str());

        DWORD opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_NOCHANGEDIR | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST);

        const HWND hwnd = parentHandle ? static_cast<HWND>(parentHandle) : nullptr;
        if (SUCCEEDED(dlg->Show(hwnd)))
        {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dlg->GetResult(&item)))
            {
                LPWSTR widePath = nullptr;
                item->GetDisplayName(SIGDN_FILESYSPATH, &widePath);
                result = WideToUtf8(widePath);
                CoTaskMemFree(widePath);
                item->Release();
            }
        }
        if (initialFolderItem)
            initialFolderItem->Release();
        dlg->Release();
    }

    if (ownCom)
        CoUninitialize();
    return result;
}

// ---------------------------------------------------------------------------

std::string BrowseForFolder(const char* title, void* parentHandle)
{
    const bool ownCom = ComInit();
    std::string result;

    IFileOpenDialog* dlg = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                                   CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))))
    {
        DWORD opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_NOCHANGEDIR | FOS_PATHMUSTEXIST);

        if (title)
            dlg->SetTitle(Utf8ToWide(title).c_str());

        const HWND hwnd = parentHandle ? static_cast<HWND>(parentHandle) : nullptr;
        if (SUCCEEDED(dlg->Show(hwnd)))
        {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dlg->GetResult(&item)))
            {
                LPWSTR widePath = nullptr;
                item->GetDisplayName(SIGDN_FILESYSPATH, &widePath);
                result = WideToUtf8(widePath);
                CoTaskMemFree(widePath);
                item->Release();
            }
        }
        dlg->Release();
    }

    if (ownCom)
        CoUninitialize();
    return result;
}

// ---------------------------------------------------------------------------

bool IsAvailable() noexcept
{
    return true;
}

} // namespace engine::platform::dialog
