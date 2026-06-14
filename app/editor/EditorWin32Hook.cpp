// <windows.h> muss als allererste Sache kommen, ohne Praekompilierte Header davor.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

// imgui_impl_win32.h setzt HWND voraus, daher nach <windows.h>
#ifdef KROM_EDITOR_HAS_IMGUI
#ifdef KROM_IMGUI_HAS_WIN32
#include "imgui_impl_win32.h"
// ImGui versteckt WndProcHandler hinter #if 0 um <windows.h>-Abhaengigkeit im Header zu vermeiden.
// Forward-Deklaration laut ImGui-Dokumentation manuell hier eintragen:
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
#endif
#endif

#include "app/editor/EditorWin32Hook.hpp"

namespace engine::app {

#if defined(KROM_EDITOR_HAS_IMGUI) && defined(KROM_IMGUI_HAS_WIN32)

namespace {
    static WNDPROC g_origWndProc = nullptr;

    static LRESULT CALLBACK HookWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp))
            return TRUE;
        return CallWindowProcW(g_origWndProc, hwnd, msg, wp, lp);
    }
}

void InstallImGuiWin32Hook(void* nativeHandle)
{
    HWND hwnd = static_cast<HWND>(nativeHandle);
    g_origWndProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(HookWndProc)));
}

void RemoveImGuiWin32Hook(void* nativeHandle)
{
    if (!g_origWndProc)
        return;
    HWND hwnd = static_cast<HWND>(nativeHandle);
    SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_origWndProc));
    g_origWndProc = nullptr;
}

#else

void InstallImGuiWin32Hook(void*) {}
void RemoveImGuiWin32Hook (void*) {}

#endif

} // namespace engine::app
