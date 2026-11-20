#include "app/editor/EnginePaths.hpp"
#include "core/PathUtf8.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace engine;
using namespace engine::app;
namespace fs = std::filesystem;

namespace {
int failures = 0;
void Check(bool ok, const char* description)
{
    std::cout << (ok ? "PASS: " : "FAIL: ") << description << '\n';
    if (!ok) ++failures;
}

struct ScratchDirectory
{
    fs::path path = fs::temp_directory_path() / PathFromUtf8("krom-paths-Größe space-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ScratchDirectory() { fs::create_directory(path); }
    ~ScratchDirectory()
    {
        std::error_code ec;
        if (path.parent_path() == fs::temp_directory_path()) fs::remove_all(path, ec);
    }
};

void Write(const fs::path& file, const std::string& text)
{
    fs::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary);
    out << text;
    out.close();
    if (!out) throw std::runtime_error("Cannot write fixture");
}
}

int main()
{
    ScratchDirectory scratch;
    const auto user = scratch.path / "user";
    const auto documents = scratch.path / "Documents";
    const auto sdk = scratch.path / "SDK";
    const auto executable = sdk / "bin" / "Editor.exe";
    const auto source = PathFromUtf8(__FILE__).parent_path().parent_path();
    auto resolve = [&] { return ResolveEnginePaths(executable, user, documents); };

    auto paths = resolve();
    Check(!paths.sdk && paths.error.empty() && paths.BuildsAvailable(), "no manifest preserves source mode");
    Check(paths.assetsDir == source / "assets" && paths.shadersDir == source / "shaders" &&
          paths.editorDir == source / "editor" && paths.projectsDir == source / "projects",
          "historical __FILE__ resource and project roots preserved");
    Check(paths.engineSourceDir == source && paths.engineBuildDir == PathFromUtf8(KROM_TEST_BUILD_DIR),
          "CMake source and build roots preserved");
    Check(paths.RuntimeBuildDir("Release") == paths.engineBuildDir.parent_path() / "runtime-Release" &&
          paths.RuntimeBuildDir("Debug") == paths.engineBuildDir.parent_path() / "runtime-Debug",
          "source build profiles preserve existing sibling folders");
    Check(paths.shaderCacheDir == source / "shader_artifacts" && paths.standaloneScenesDir == source / "editor",
          "source cache and legacy scene locations preserved");
    Check(paths.ProjectBrowseDir() == fs::current_path() / "projects", "source file dialog retains CWD fallback");

    Write(sdk / "krom-sdk.json", R"({"schemaVersion":1})");
    for (const auto* name : {"assets", "shaders", "editor"}) fs::create_directories(sdk / "bin" / name);
    paths = resolve();
    Check(paths.sdk && paths.error.empty() && !paths.BuildsAvailable(), "parent manifest selects SDK and blocks builds");
    Check(paths.sdkRoot == sdk && paths.assetsDir == sdk / "bin/assets" && paths.shadersDir == sdk / "bin/shaders" &&
          paths.editorDir == sdk / "bin/editor", "SDK defaults resolve from actual executable");
    Check(paths.engineSourceDir.empty() && paths.engineBuildDir.empty() && paths.RuntimeBuildDir("Release").empty(),
          "SDK never exposes development source or build directories");
    Check(paths.sdkCmakeDir == sdk / "cmake" && paths.projectTemplateDir == sdk / "templates/project",
          "SDK CMake and template paths are relative to manifest root");
    Check(paths.projectsDir == documents / "KromProjects" && paths.ProjectBrowseDir() == paths.projectsDir &&
          paths.shaderCacheDir == user / "cache/shader_artifacts" && paths.standaloneScenesDir == user / "scenes",
          "SDK writable paths stay outside installation, including Unicode user path");
    Check(!fs::exists(user) && !fs::exists(documents), "resolution performs no writes");

    const auto previousCwd = fs::current_path();
    fs::create_directories(scratch.path / "foreign-cwd");
    fs::current_path(scratch.path / "foreign-cwd");
    const auto foreign = resolve();
    fs::current_path(previousCwd);
    Check(foreign.assetsDir == paths.assetsDir && foreign.projectsDir == paths.projectsDir &&
          foreign.sdkRoot == paths.sdkRoot, "SDK detection and paths ignore foreign working directory");

    const auto flatRoot = scratch.path / "flat-sdk";
    Write(flatRoot / "krom-sdk.json", R"({"schemaVersion":1})");
    for (const auto* name : {"assets", "shaders", "editor"}) fs::create_directories(flatRoot / name);
    const auto flat = ResolveEnginePaths(flatRoot / "Editor.exe", user, documents);
    Check(flat.error.empty() && flat.sdkRoot == flatRoot && flat.editorDir == flatRoot / "editor" &&
          flat.shadersDir == flatRoot / "shaders", "flat SDK layout works with marker next to executable");
    fs::remove(flatRoot / "editor");
    const auto incomplete = ResolveEnginePaths(flatRoot / "Editor.exe", user, documents);
    Check(incomplete.sdk && !incomplete.error.empty() && incomplete.engineSourceDir.empty(),
          "missing SDK resources fail without development or CWD fallback");

    // A read-only marker is sufficient; resolution never rewrites it or creates
    // caches in its directory. Windows does not enforce read-only directories.
    fs::permissions(sdk / "krom-sdk.json", fs::perms::owner_read, fs::perm_options::replace);
    Check(resolve().error.empty(), "read-only manifest can be read");
    fs::permissions(sdk / "krom-sdk.json", fs::perms::owner_write, fs::perm_options::add);

    const auto moved = scratch.path / PathFromUtf8("Verschoben Größe");
    fs::rename(sdk, moved);
    const auto relocated = ResolveEnginePaths(moved / "bin/Editor.exe", user, documents);
    Check(relocated.sdkRoot == moved && relocated.assetsDir == moved / "bin/assets" && relocated.error.empty(),
          "relocated SDK resolves without previous installation or source tree");
    fs::rename(moved, sdk);

    Write(sdk / "bin/krom-sdk.json", R"({"schemaVersion":1,"assetsDir":"Inhalte/Gr\u00f6\u00dfe","shadersDir":"gpu","editorDir":"ui","sdkCmakeDir":"build-support","projectTemplateDir":"starter"})");
    for (const auto* name : {"Inhalte/Größe", "gpu", "ui"}) fs::create_directories(sdk / "bin" / PathFromUtf8(name));
    paths = resolve();
    Check(paths.sdkRoot == sdk / "bin" && paths.error.empty(), "manifest beside executable takes precedence");
    Check(paths.assetsDir == sdk / "bin" / PathFromUtf8("Inhalte/Größe") && paths.shadersDir == sdk / "bin/gpu" &&
          paths.editorDir == sdk / "bin/ui" && paths.sdkCmakeDir == sdk / "bin/build-support" &&
          paths.projectTemplateDir == sdk / "bin/starter", "manifest paths use its root and decode Unicode escapes");

    for (const auto& invalid : {
        "", "{}", "{", R"({"schemaVersion":1)", R"({"schemaVersion":1} garbage)",
        R"({"schemaVersion":2})", R"({"schemaVersion":1,})", R"({"schemaVersion":1,"schemaVersion":1})",
        R"({"schemaVersion":1,"assetsDir":"unterminated})", R"({"schemaVersion":1,"assetsDir":"../escape"})",
        R"({"schemaVersion":1,"assetsDir":"C:/source/assets"})", R"({"schemaVersion":1,"assetsDir":"/absolute"})",
        R"({"schemaVersion":1,"assetsDir":"bad\u0000path"})", R"({"schemaVersion":1,"unknown":"field"})",
        R"({"schemaVersion":1,"assetsDir":"\uD800"})"})
    {
        Write(sdk / "bin/krom-sdk.json", invalid);
        paths = resolve();
        Check(paths.sdk && !paths.error.empty() && !paths.BuildsAvailable() && paths.engineSourceDir.empty() &&
              paths.error.find(PathToUtf8(sdk / "bin" / "krom-sdk.json")) != std::string::npos &&
              paths.sdkRoot == sdk / "bin", "broken nearer manifest reports its path without parent/source fallback");
    }
    fs::remove(sdk / "bin/krom-sdk.json");
    fs::create_directory(sdk / "bin/krom-sdk.json");
    Check(!resolve().error.empty(), "directory masquerading as manifest is rejected");
    fs::remove(sdk / "bin/krom-sdk.json");
    Check(!ResolveEnginePaths(executable, {}, documents).error.empty(), "SDK missing user directory fails without CWD writes");
    Check(!ResolveEnginePaths({}, user, documents).error.empty(), "unknown executable cannot select development mode");

    const auto nestedExe = sdk / "nested/bin/Editor.exe";
    Check(!ResolveEnginePaths(nestedExe, user, documents).sdk, "manifest search stops after immediate parent");
    Check(CurrentExecutablePath().is_absolute(), "native executable detection returns absolute path");
    return failures ? 1 : 0;
}
