# Windows SDK distribution

The first SDK supplies Windows x64 Release editors and static libraries built
with MSVC v143, the dynamic Release CRT (/MD), C++20 and IPO disabled. Project
builds require Visual Studio 2022 or Build Tools with the C++/Windows SDK tools,
CMake and Ninja. Consumers need no engine checkout or dependency downloads.
Vulkan headers/import library and DXC are included; a Vulkan-capable driver is
still required to run the Vulkan backend.

## Package

From a source checkout:

```powershell
tools/package_sdk.cmd
```

The script discovers Visual Studio with `vswhere`, captures its environment,
configures a Release build, builds `krom_sdk`, installs into a new directory and
creates a ZIP under `out/sdk/`. It refuses to overwrite an existing package
directory. Use `-ArtifactDirectory <new-directory>` to publish the same build
again. The script prints the resulting directory and ZIP paths.

For a producer build without downloading dependencies, supply existing sources:

```powershell
tools/package_sdk.cmd -JoltSource D:/dependencies/Jolt -MiniaudioSource D:/dependencies/miniaudio
```

When both paths are supplied, FetchContent is fully disconnected. Source paths
are used only by the producer build and are not exported into the SDK.

The package contains:

- `bin/`: three editors, resources, shader tools, Release CRT and D3D compiler;
- `include/`: public engine/addon headers and dependency headers;
- `lib/Release/`: engine archives, Jolt and the Vulkan loader import library;
- `cmake/`: relocatable `KromConfig.cmake`, imported targets and shared helpers;
- `sources/`: the small Script DLL/runtime entrypoints needed by project builds;
- `templates/project/`: the central project CMake entrypoint;
- `krom-sdk.json`: platform/compiler contract and SDK build ID;
- `LICENSE` and `bin/licenses/`: engine and third-party notices.

Miniaudio's implementation is compiled into `engine_audio`; it is not a separate
download or consumer library. Jolt is an exported target with its original
public compile definitions. The SDK build rejects `/GL` and `/LTCG`, enforces
/MD and disables IPO for installed libraries. Only Release configurations are
installed. The build identity includes engine sources, headers, CMake settings
and compiler version; consumers cannot substitute a different SDK build ID.

## Use

Extract the package anywhere, start an editor from `bin/`, create/open a project
and use Play, Testbuild or Export. Pick Release for Testbuild/Export. Generated
bindings and build outputs belong to the project; settings and no-project caches
belong to the user's directory. The installation can remain read-only.

Manual project build example, in a VS x64 developer environment:

```powershell
cmake -S <SDK>/templates/project -B <project>/.krom/manual -G Ninja `
  -DCMAKE_BUILD_TYPE=Release -DKrom_DIR=<SDK>/cmake -DKROM_PROJECT_ROOT=<project>
cmake --build <project>/.krom/manual --target krom_scripts_<runtimeTarget>
cmake --build <project>/.krom/manual --target krom_runtime_<runtimeTarget>_dx11
```

The template uses the same project helpers as the engine source build. The
editor's automatic cache additionally separates a 16-digit SDK ID prefix, platform and full MSVC compiler version. The full SDK ID remains checked in the CMake cache and module ABI; a moved SDK resets its project CMake cache.
The two SDK entrypoints are copied into that cache during configuration to keep
MSVC object paths short. Runtime redistributables are copied through a generated
CMake script, avoiding UTF-8 batch files interpreted in the Windows OEM codepage.
Source-build timestamp/header checks remain in source mode. SDK modules instead
must match the immutable build ID and the explicit ABI policy.

## Platform boundaries

| Concern | Boundary / disposition |
| --- | --- |
| Build process and VS environment | `BuildProcess` receives executable, argument vector, cwd and environment. Only MSVC bootstrap invokes a shell. |
| Runtime launch | `LaunchProcess` launches directly without waiting; no `cmd /c start`. |
| Dynamic libraries | `platform/DynamicModule` owns load, symbol lookup and unload. Win32 and POSIX loader implementations exist; the full script host remains Windows-supported. |
| PDB/session inspection | Windows-specific hot-reload implementation; not a portable ABI promise. |
| Binary names | CMake suffixes and generated target paths, shared by source and SDK builds. |
| Resources/user directories | `EnginePaths`; package resources are read-only, generated files are project/user-local. |
| File dialogs | Existing `NativeFileDialog` interface, Win32 implementation and non-Windows stubs. |
| Graphics/system redistributables | Stay in their backend/package rules. Windows SDK includes applicable CRT/D3D/Vulkan files. |

Code inventory against archive 2026-11-06, point 4:

| Current location | Windows dependency | SDK decision |
| --- | --- | --- |
| `app/editor/BuildTools.cpp`, `BuildProcess.cpp` | vswhere, VsDevCmd, CreateProcess, handles/job objects | Tool discovery stays separate from the program/arguments/cwd process interface. CMake/Ninja run directly. A non-Windows process implementation remains future work. |
| `app/editor/ScriptModuleHost.cpp`, `src/platform/DynamicModule.cpp` | DLL loading, PDB/session inspection | Library loading moved behind the platform interface; Windows PDB handling remains in the Windows host. CMake supplies target paths and binary suffixes. |
| `app/editor/EnginePaths.cpp` | Executable location, known folders | Central path resolver retains OS-specific directory discovery. Package paths and writable paths remain distinct. |
| `src/platform/NativeFileDialog_Win32.cpp` | COM and native file/folder dialogs | Existing interface retained; non-Windows implementation still required. |
| `src/platform/win32/*`, `app/editor/EditorWin32Hook.cpp` | HWND, messages, input, ImGui Win32 hook | Remain in existing platform/window integration. GLFW is the later alternative. |
| `addons/dx11/*`, `addons/opengl/OpenGLWin32Loader.*`, editor Vulkan surface setup | D3D/COM, WGL, Win32 Vulkan surface | Remain backend/window-specific; no new SDK-wide graphics abstraction. |
| `src/renderer/ShaderToolPaths.cpp`, `ShaderCompilerShared.cpp`, `ShaderCompilerD3D.cpp` | Executable/tool discovery, wide file APIs, D3D compiler | Existing renderer helper boundaries retained; tools and redistributables bundled. |
| `addons/editor/EditorAssetBrowser.cpp` | ShellExecute / shell opening of external editors and folders | Deferred to a future platform shell/open interface; independent of SDK compilation and loading. |
| `src/app/KromWrapper.cpp`, `app/KromEditorApp.cpp`, `include/core/PathUtf8.hpp` | User/font directories, Win32 integration, UTF-16 file APIs | Keep current guarded implementations; directory/font portability belongs to later OS support. |
| `addons/script/ScriptRegistry.hpp`, `ScriptModuleApi.hpp` | Export attributes, compiler/CRT ABI | Platform-guarded exports retained; SDK policy explicitly limited to Windows x64 MSVC Release. |

Linux/macOS SDKs, their compiler/standard-library ABI policies, PIC/rpath and
macOS signing/notarization require separate implementations and real system tests.

## Acceptance tests

Build `krom_sdk_abi_tests`, `krom_engine_paths_tests`, `krom_build_tools_tests` and
the editor loading tests. Source-mode external Script/Play regression is
`krom_editor_loading_tests_dx11 --external-script-reload <project.json>`.

For the complete SDK flow, build `krom_editor_loading_tests_dx11` in the **same
Release SDK producer build**, install into a validation copy and run:

```powershell
./tests/test_sdk_package.ps1 -SdkDirectory <validation-copy> `
  -SmokeExecutable <sdk-build>/bin/krom_editor_loading_tests_dx11.exe `
  -HideSources -ReadOnly
```

The script requires a fresh copy inside this workspace with inherited permissions. It checks exported CMake paths,
hides engine source/resources, denies installation writes, and restores both
sources and permissions in `finally`. The probe creates a Unicode-path project,
builds/loads/executes a Script DLL, builds and runs a runtime, exports and warms
up the exported game. It also checks unsupported configuration errors and that
installation files remain unchanged. This is a local isolation test, not a
substitute for a clean offline VM with a separately installed supported toolset.

Clean-machine acceptance: Windows x64 VM with VS/Build Tools C++, no engine
checkout, network disconnected, package moved/read-only, empty caches. Create a
project in a Unicode/space path, add a Script, Play, Testbuild, Export and run the
export. Repeat with a supported newer MSVC toolset and verify Debug/wrong-build
modules are rejected. Record the VM/toolset versions and results.
