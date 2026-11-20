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

## Current capability

This step does not yet ship an SDK. Script builds/loading, editor Play, runtime
test builds and export are blocked in SDK mode with a translated message, before
build preparation or export staging. Editing and saving project content remains
available. Install rules, imported CMake targets, project generation, SDK ABI
policy and the packaging script are separate follow-up steps. Compiler/ABI
checks in source mode remain unchanged.

## Verification

- `krom_engine_paths_tests`: source-mode parity, both manifest locations and
  precedence, malformed/unreadable manifests, relocation, foreign CWD, flat/bin
  layouts, Unicode paths/escapes and separate writable destinations.
- `krom_editor_loading_tests --sdk-build-guards`: run a **copy** of this test
  executable beside a minimal manifest and empty assets/shaders/editor directories;
  verifies editor build/reload guards
  without initializing graphics or invoking tools.
- `krom_editor_loading_tests --sdk-invalid-manifest`: run a copy beside an invalid
  manifest; verifies startup fails before rendering initialization.
- `krom_editor_loading_tests[_{dx11,opengl}] --ini-path`: initializes a hidden
  editor and checks that ImGui saves to the user directory. Redirect
  `LOCALAPPDATA` to a temporary Unicode path for an isolated check. This also
  works with an SDK copy containing assets, shaders and editor resources.
- Build all three editor backends to verify shared path and ImGui changes.

A complete read-only-installation test and a clean offline VM test remain part
of the later SDK packaging acceptance test.
