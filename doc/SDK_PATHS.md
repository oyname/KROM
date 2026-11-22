# SDK paths: first migration step

`app/editor/EnginePaths.{hpp,cpp}` resolves editor resources independently of the
open project. Only this implementation reads the compiled engine source/build
paths. Without an SDK manifest, resource, project, build, scene and shader-cache
paths retain their previous values, including the file dialog's `CWD/projects`
fallback. ImGui layout settings now use the existing user preference directory
in both modes (`%LOCALAPPDATA%/KROM/imgui.ini` on Windows).

## Manifest and layout

The editor checks `krom-sdk.json` beside the actual running executable, then in
its immediate parent directory. The first existing candidate wins. An unreadable,
malformed or unsupported manifest stops startup with its path in the diagnostic;
it never selects the other candidate or falls back to the source tree. The CWD
and more distant ancestors are not searched.

The minimal v1 manifest is:

```json
{"schemaVersion": 1}
```

Defaults support both a flat package and an executable in `bin/`:

| Purpose | Default |
| --- | --- |
| Example assets | `<exe directory>/assets/` |
| Engine shaders | `<exe directory>/shaders/` |
| Editor resources, fonts and gizmos | `<exe directory>/editor/` |
| SDK CMake files | `<manifest directory>/cmake/` |
| Project template | `<manifest directory>/templates/project/` |
| New projects | Windows Documents known folder + `KromProjects/` |
| Shader cache without a project | `<user preference directory>/cache/shader_artifacts/` |
| IBL cache | `<user preference directory>/cache/ibl/` |
| Scenes without a project | `<user preference directory>/scenes/` |

The resource/CMake/template directories can be overridden with the flat string
fields `assetsDir`, `shadersDir`, `editorDir`, `sdkCmakeDir` and
`projectTemplateDir`. Values are relative to the **manifest directory**, including
when the executable is in `bin/`. Absolute paths, empty values and paths escaping
the SDK root are rejected. UTF-8 and JSON Unicode escapes are supported. Unknown
or duplicate fields are rejected in schema v1.

For example, a manifest at the package root can explicitly state its layout:

```json
{
  "schemaVersion": 1,
  "assetsDir": "bin/assets",
  "shadersDir": "bin/shaders",
  "editorDir": "bin/editor",
  "sdkCmakeDir": "cmake",
  "projectTemplateDir": "templates/project"
}
```

Path resolution only reads the manifest and checks resource directories; it creates
no directories. All three resource directories must exist; missing resources fail
clearly rather than using development-tree or CWD fallbacks. Source and engine build
paths are empty in SDK mode. With an open project, the existing project asset,
scene and shader-cache paths are preserved.

## SDK v2

A complete Release SDK uses `schemaVersion: 2` and additionally records
`sdkBuildId` (64 hex digits), `system` (`Windows`), `architecture` (`x64`),
`compilerFamily` (`MSVC`), `compilerVersion` (`_MSC_FULL_VER`, as a string),
`standardLibrary` (`msvc`), `toolset`, `runtime` (`MD`) and `configuration`
(`Release`). The manifest must match the editor binary. CMake package and project
template must exist. V1 remains a path-only preview with builds disabled.

V2 builds Script DLLs and runtimes against installed libraries with
`find_package(Krom CONFIG REQUIRED)`. Build outputs are isolated under
`<project>/.krom/b/<hash>/`, where `<hash>` is 12 hex digits of FNV-1a over SDK build
ID, platform (`win64`), full compiler version and `Release`. `krom-build-info.txt` in
that folder lists these four values. The folder name is short on purpose: cl.exe is
not long-path aware and CMake's compiler probes add ~92 characters below it. The
editor refuses a build when the folder plus a 95-character reserve exceeds 250
characters and already warns when such a project is created or opened. No engine
source build is involved. CMake writes actual target paths into
`target-paths/Release/`; the editor reads these instead of guessing DLL/EXE names.

Script loading verifies API version 7, SDK build ID, compiler family,
architecture, pointer size, CRT, iterator-debug level and debug mode. The SDK
allows the same or a newer compiler in the explicitly supported MSVC v143 range
19.30 through 19.49. Source builds retain exact compiler matching. SDK Debug and
RelWithDebInfo builds are rejected before preparing output.

See [SDK_DISTRIBUTION.md](SDK_DISTRIBUTION.md) for packaging and acceptance tests.
