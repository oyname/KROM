#pragma once
#include "platform/IWindow.hpp"

namespace engine::app {

// Schaltet ImGui_ImplWin32_WndProcHandler vor den originalen WndProc des Fensters.
// Nicht-Win32-Handles werden anhand des Backend-Tags abgewiesen.
void InstallImGuiWin32Hook(platform::NativeWindowHandle windowHandle);
void RemoveImGuiWin32Hook(platform::NativeWindowHandle windowHandle);

} // namespace engine::app
