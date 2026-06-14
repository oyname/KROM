#pragma once

namespace engine::app {

// Schaltet ImGui_ImplWin32_WndProcHandler vor den originalen WndProc des Fensters.
// nativeHandle muss ein HWND sein (void* damit dieser Header kein <windows.h> braucht).
void  InstallImGuiWin32Hook(void* nativeHandle);
void  RemoveImGuiWin32Hook (void* nativeHandle);

} // namespace engine::app
