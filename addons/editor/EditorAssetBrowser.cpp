#include "addons/editor/EditorAssetBrowser.hpp"
#include "core/PathUtf8.hpp"
#include "addons/editor/EditorFileNaming.hpp"
#include "addons/editor/EditorLocalization.hpp"
#include "addons/editor/EditorFeature.hpp"
#include "addons/editor/EditorMaterialLibrary.hpp"
#include "addons/editor/EditorAnimationWindow.hpp"
#include "addons/editor/EditorModelClips.hpp"
#include "addons/editor/EditorAnimationControllers.hpp"
#include "addons/editor/EditorControllerGraph.hpp"
#include "addons/editor/EditorCommands.hpp"
#include "addons/editor/EditorThumbnailCache.hpp"
#include "addons/editor/EditorScriptAssets.hpp"
#include "addons/editor/EditorTheme.hpp"
#include "addons/editor/EditorIcons.hpp"

#ifdef KROM_EDITOR_HAS_IMGUI

#include "addons/lit/LitMaterial.hpp"
#include "addons/pbr/PbrMasterMaterial.hpp"
#include "addons/pbr/PbrInstanceBuilder.hpp"
#include "assets/MeshTangents.hpp"
#include "addons/gltf/GltfImporter.hpp"
#include "addons/prefab/Prefab.hpp"
#include "addons/prefab/PrefabInstanceComponent.hpp"
#include "EditorPrefabWindow.hpp"
#include "addons/mesh_renderer/MeshRendererComponents.hpp"
#include "addons/mesh_renderer/MeshRendererSerialization.hpp"
#include "addons/mesh_renderer/MeshSceneQueries.hpp"
#include "addons/probes/ProbeComponents.hpp"
#include "addons/probes/ProbeData.hpp"
#include "assets/AssetPipeline.hpp"
#include "assets/VertexLayoutBridge.hpp"
#include "collision/SceneQueries.hpp"
#include "ecs/World.hpp"
#include "ecs/Components.hpp"
#include "core/Logger.hpp"
#include "serialization/SceneSerializer.hpp"
#include "renderer/CompiledMaterialDesc.hpp"
#include "renderer/MaterialParameterLayout.hpp"
#include "renderer/VertexLayoutContract.hpp"

#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <set>
#include <fstream>
#include <iterator>
#include <functional>
#include <initializer_list>
#include <string>
#include <string_view>
#include <unordered_map>
#include <cstring>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

namespace engine::renderer::addons::editor {
namespace detail {
// EditorUIProject.cpp (declared with a default argument in EditorUIInternal.hpp).
bool SaveEntityAsPrefab(EditorFrameContext& ctx, EntityID entity, const std::filesystem::path& targetDir);
// EditorUI.cpp (see EditorUIInternal.hpp).
void KeepNextWindowInMainViewport(const char* windowName);
} // namespace detail

namespace {

// Hierarchy entity dropped on the asset browser: save it as a prefab in targetDir.
bool AcceptEntityToPrefabPayload(EditorFrameContext& ctx, const std::filesystem::path& targetDir)
{
    const ImGuiPayload* p = ImGui::AcceptDragDropPayload("KROM_EDITOR_ENTITY");
    if (!p || p->DataSize != sizeof(EntityID))
        return false;
    const EntityID entity = *static_cast<const EntityID*>(p->Data);
    return detail::SaveEntityAsPrefab(ctx, entity, targetDir);
}

// Vorschaubild im Platten-Cache (.krom/thumbnails) beim Loeschen/Umbenennen mitfuehren.
std::string ThumbnailRelPath(const EditorFrameContext& ctx, const std::filesystem::path& asset)
{
    if (ctx.currentProjectRoot.empty())
        return {};
    std::error_code ec;
    const std::filesystem::path rel = std::filesystem::relative(
        asset.lexically_normal(), PathFromUtf8(ctx.currentProjectRoot) / "Assets", ec);
    return (ec || rel.empty()) ? std::string{} : PathToGenericUtf8(rel);
}

void ForgetAssetThumbnail(const EditorFrameContext& ctx, const std::filesystem::path& asset)
{
    if (const std::string rel = ThumbnailRelPath(ctx, asset); !rel.empty())
        RemoveCachedThumbnail(PathFromUtf8(ctx.currentProjectRoot), rel);
}

void MoveAssetThumbnail(const EditorFrameContext& ctx, const std::filesystem::path& from,
                        const std::filesystem::path& to)
{
    const std::string oldRel = ThumbnailRelPath(ctx, from);
    const std::string newRel = ThumbnailRelPath(ctx, to);
    if (!oldRel.empty() && !newRel.empty())
        RenameCachedThumbnail(PathFromUtf8(ctx.currentProjectRoot), oldRel, newRel);
}


// Bereich, in dem die Szene sichtbar ist: freier Mittelbereich des Dockspace.
static void ComputeSceneViewRect(const EditorFrameContext& ctx, ImVec2& pos, ImVec2& size)
{
    if (ctx.sceneViewRectValid)
    {
        pos = {ctx.sceneViewX, ctx.sceneViewY};
        size = {ctx.sceneViewWidth, ctx.sceneViewHeight};
        return;
    }
    pos = {ctx.renderViewX, ctx.renderViewY};
    size = {ctx.renderViewWidth, ctx.renderViewHeight};
}

// Hauptpanels sind andockbar; Position und Groesse bestimmt der Dockspace.
static constexpr ImGuiWindowFlags kAlignedPanelFlags = ImGuiWindowFlags_None;

static void PushSolidEditorPanelStyle()
{
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 1.f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, theme::kWindowBg);
    ImGui::PushStyleColor(ImGuiCol_ChildBg,  theme::kWindowBg);
}

static void PopSolidEditorPanelStyle()
{
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();
}

struct SolidEditorPanelStyleScope
{
    SolidEditorPanelStyleScope()
    {
        PushSolidEditorPanelStyle();
    }

    ~SolidEditorPanelStyleScope()
    {
        PopSolidEditorPanelStyle();
    }

    SolidEditorPanelStyleScope(const SolidEditorPanelStyleScope&) = delete;
    SolidEditorPanelStyleScope& operator=(const SolidEditorPanelStyleScope&) = delete;
};

float EditorPickMaxDistance(const EditorCameraState& cam) noexcept
{
    return std::max(cam.farPlane, 100000.f);
}

bool IsSupportedModel(const std::filesystem::path& p)
{
    const auto ext = PathToUtf8(p.extension());
    return ext == ".gltf" || ext == ".glb";
}

bool IsEditorInternalAsset(const std::filesystem::path& p)
{
    const std::string file = PathToUtf8(p.filename());
    return file == "camera.glb" || file == "matcam.mat";
}

bool IsSupportedTexture(const std::filesystem::path& p)
{
    const auto ext = PathToUtf8(p.extension());
    return ext == ".png"  || ext == ".jpg"  || ext == ".jpeg"
        || ext == ".dds"  || ext == ".ktx"  || ext == ".ktx2"
        || ext == ".bmp" || ext == ".hdr";
}

bool IsSupportedAudio(const std::filesystem::path& p)
{
    std::string ext = PathToUtf8(p.extension());
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".wav" || ext == ".mp3" || ext == ".ogg" || ext == ".flac";
}

bool IsSupportedMaterial(const std::filesystem::path& p)
{
    return p.extension() == ".mat";
}

bool IsSupportedPrefab(const std::filesystem::path& p)
{
    return p.extension() == ".prefab";
}

static bool IsBakedProbeFile(const std::filesystem::path& p)
{
    return p.extension() == ".kprobe";
}

bool IsSupportedShader(const std::filesystem::path& p)
{
    const auto ext = PathToUtf8(p.extension());
    return ext == ".hlsl" || ext == ".glsl"
        || ext == ".vert" || ext == ".frag"
        || ext == ".vs"   || ext == ".ps"
        || ext == ".comp";
}

std::string QuoteShellArg(const std::string& value)
{
    std::string out = "\"";
    for (const char ch : value)
    {
        if (ch == '"')
            out += "\\\"";
        else
            out += ch;
    }
    out += "\"";
    return out;
}

std::string QuoteExecutableForShell(const std::string& value)
{
    const bool looksLikePlainCommand =
        value.find(' ') == std::string::npos &&
        value.find('\t') == std::string::npos &&
        value.find('/') == std::string::npos &&
        value.find('\\') == std::string::npos &&
        value.find(':') == std::string::npos;
    return looksLikePlainCommand ? value : QuoteShellArg(value);
}

void ReplaceAll(std::string& value, const std::string& from, const std::string& to)
{
    if (from.empty())
        return;
    size_t pos = 0u;
    while ((pos = value.find(from, pos)) != std::string::npos)
    {
        value.replace(pos, from.size(), to);
        pos += to.size();
    }
}

bool IsPlainCommand(const std::string& value)
{
    return value.find(' ') == std::string::npos &&
        value.find('\t') == std::string::npos &&
        value.find('/') == std::string::npos &&
        value.find('\\') == std::string::npos &&
        value.find(':') == std::string::npos;
}

// Shell-Kommando mit UTF-8-Pfaden ausfuehren (std::system nutzt unter Windows die ANSI-Codepage).
int RunShellCommand(const std::string& command)
{
#if defined(_WIN32)
    return _wsystem(engine::WideFromUtf8(command).c_str());
#else
    return std::system(command.c_str());
#endif
}

// Datei mit dem Standardprogramm des Systems oeffnen (z. B. Bilder im Bildprogramm).
void OpenWithDefaultApp(EditorFrameContext& ctx, const std::filesystem::path& path)
{
    if (path.empty() || !std::filesystem::exists(path))
        return;
    const std::string filePath = PathToUtf8(path);
#if defined(_WIN32)
    const std::string command = "cmd /c start \"\" " + QuoteShellArg(filePath);
#elif defined(__APPLE__)
    const std::string command = "open " + QuoteShellArg(filePath);
#else
    const std::string command = "xdg-open " + QuoteShellArg(filePath);
#endif
    const int result = RunShellCommand(command);
    if (result != 0)
        ctx.lastFileMessage = Translate("assets.external_failed") + filePath;
}

void OpenAssetInExternalEditor(EditorFrameContext& ctx, const std::filesystem::path& path, int line = 0)
{
    if (path.empty() || !std::filesystem::exists(path))
        return;

    const std::string filePath = PathToUtf8(path);
    const std::string executable = ctx.state.codeEditorExecutableBuffer.data();
    if (!executable.empty())
    {
        std::string arguments = ctx.state.codeEditorArgumentsBuffer.data();
        if (arguments.empty())
            arguments = "\"{file}\"";
        const bool hasLineToken = arguments.find("{line}") != std::string::npos;
        const std::filesystem::path workspacePath = ctx.currentProjectRoot.empty()
            ? std::filesystem::current_path()
            : PathFromUtf8(ctx.currentProjectRoot);
        ReplaceAll(arguments, "\"{workspace}\"", QuoteShellArg(PathToUtf8(workspacePath)));
        ReplaceAll(arguments, "{workspace}", QuoteShellArg(PathToUtf8(workspacePath)));
        ReplaceAll(arguments, "{line}", std::to_string(std::max(line, 1)));
        if (line > 0 && IsPlainCommand(executable) &&
            (executable == "code" || executable == "code.cmd") && !hasLineToken)
        {
            arguments = "-g " + QuoteShellArg(filePath + ":" + std::to_string(line));
        }
        else
        {
            ReplaceAll(arguments, "\"{file}\"", QuoteShellArg(filePath));
            ReplaceAll(arguments, "{file}", QuoteShellArg(filePath));
        }
        if (arguments.find(filePath) == std::string::npos)
            arguments += " " + QuoteShellArg(filePath);

        const std::string command = QuoteExecutableForShell(executable) + " " + arguments;
        const int result = RunShellCommand(command);
        if (result != 0)
            ctx.lastFileMessage = Translate("assets.code_editor_failed") + executable;
        return;
    }

#ifdef _WIN32
    if (IsSupportedShader(path))
    {
        // Shader extensions may have no registered text editor on Windows.
        const std::wstring argument = L"\"" + path.native() + L"\"";
        const auto result = ShellExecuteW(nullptr, L"open", L"notepad.exe", argument.c_str(), nullptr, SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(result) <= 32)
            ctx.lastFileMessage = std::string(Translate("assets.code_editor_failed")) + "notepad.exe";
        return;
    }
#endif

    OpenWithDefaultApp(ctx, path);
}

bool TryParseBuildLocation(const EditorFrameContext& ctx,
                           const std::string& message,
                           std::filesystem::path& outPath,
                           int& outLine)
{
    const size_t close = message.find("):");
    if (close == std::string::npos)
        return false;
    const size_t open = message.rfind('(', close);
    if (open == std::string::npos || open + 1u >= close)
        return false;

    for (size_t i = open + 1u; i < close; ++i)
        if (!std::isdigit(static_cast<unsigned char>(message[i])))
            return false;

    size_t start = 0u;
    const size_t driveSlash = message.find(":\\");
    const size_t driveForward = message.find(":/");
    const size_t drive = std::min(
        driveSlash == std::string::npos ? message.size() : driveSlash,
        driveForward == std::string::npos ? message.size() : driveForward);
    if (drive < message.size() && drive > 0u)
        start = drive - 1u;
    else if (message.rfind("[Build] ", 0) == 0)
        start = 8u;

    std::string file = message.substr(start, open - start);
    while (!file.empty() && std::isspace(static_cast<unsigned char>(file.front())))
        file.erase(file.begin());
    while (!file.empty() && std::isspace(static_cast<unsigned char>(file.back())))
        file.pop_back();

    if (file.empty())
        return false;

    std::filesystem::path path = PathFromUtf8(file);
    if (path.is_relative() && !ctx.currentProjectRoot.empty())
        path = PathFromUtf8(ctx.currentProjectRoot) / path;
    if (!std::filesystem::exists(path))
        return false;

    outPath = path;
    outLine = std::atoi(message.substr(open + 1u, close - open - 1u).c_str());
    return outLine > 0;
}

bool IsSupportedCppScriptSource(const std::filesystem::path& p)
{
    const auto ext = PathToUtf8(p.extension());
    return ext == ".hpp" || ext == ".h" || ext == ".cpp" || ext == ".cxx";
}

bool IsSceneFile(const std::filesystem::path& p)
{
    return p.extension() == ".json";
}

// Prüft ob das aktuelle Verzeichnis der Scenes-Ordner ist.
bool IsInScenesFolder(const AssetBrowserState& browser)
{
    return browser.currentDir.filename() == "Scenes";
}

std::filesystem::path ResolveEngineShaderPath(const EditorFrameContext& ctx, const char* filename)
{
    const std::filesystem::path path = PathFromUtf8(filename);
    if (path.is_absolute() || ctx.engineAssetRoot.empty())
        return path;
    return (PathFromUtf8(ctx.engineAssetRoot) / path).lexically_normal();
}

std::filesystem::path MakeUniqueDestinationPath(const std::filesystem::path& directory,
                                                const std::filesystem::path& source)
{
    return MakeUniqueFilesystemPath(directory, PathToUtf8(source.stem()), PathToUtf8(source.extension()));
}

bool CopyExternalFile(const std::filesystem::path& source,
                      const std::filesystem::path& destination,
                      std::string& outError)
{
    std::error_code ec;
    std::filesystem::create_directories(destination.parent_path(), ec);
    if (ec)
    {
        outError = ec.message();
        return false;
    }

    std::filesystem::copy_file(source, destination, std::filesystem::copy_options::overwrite_existing, ec);
    if (!ec)
        return true;

    std::ifstream in(source, std::ios::binary);
    std::ofstream out(destination, std::ios::binary | std::ios::trunc);
    if (!in || !out)
    {
        outError = ec.message();
        return false;
    }
    out << in.rdbuf();
    if (!out.good())
    {
        outError = "write failed";
        return false;
    }
    return true;
}

std::string ToRelativeAssetPath(const EditorFrameContext& ctx, const std::filesystem::path& path)
{
    if (path.empty())
        return {};

    const std::filesystem::path assetRoot = ctx.assetPipeline
        ? ctx.assetPipeline->GetAssetRoot().lexically_normal()
        : (std::filesystem::current_path() / "assets").lexically_normal();
    const std::filesystem::path absolutePath = path.is_absolute()
        ? path.lexically_normal()
        : (assetRoot / path).lexically_normal();
    std::error_code ec;
    const std::filesystem::path relative = std::filesystem::relative(absolutePath, assetRoot, ec);
    return (ec || relative.empty()) ? PathToGenericUtf8(absolutePath) : PathToGenericUtf8(relative);
}

std::filesystem::path ToAbsoluteAssetBrowserPath(const EditorFrameContext& ctx,
                                                 const char* rawPath,
                                                 bool isRelative)
{
    if (!rawPath || rawPath[0] == '\0')
        return {};
    std::filesystem::path src = PathFromUtf8(rawPath);
    if (isRelative && ctx.assetPipeline)
        src = ctx.assetPipeline->GetAssetRoot() / src;
    return src.lexically_normal();
}

bool MoveAssetBrowserPath(EditorFrameContext& ctx,
                          AssetBrowserState& browser,
                          const std::filesystem::path& source,
                          const std::filesystem::path& targetDir)
{
    if (source.empty() || targetDir.empty())
        return false;

    std::error_code ec;
    const std::filesystem::path src = source.lexically_normal();
    const std::filesystem::path dst = (targetDir / src.filename()).lexically_normal();
    if (!std::filesystem::exists(src, ec) || !std::filesystem::is_directory(targetDir, ec))
        return false;
    if (std::filesystem::equivalent(src, dst, ec) && !ec)
        return false;
    if (std::filesystem::exists(dst, ec))
    {
        ctx.lastFileMessage = Translate("assets.move_exists");
        return false;
    }
    if (std::filesystem::is_directory(src, ec))
    {
        const std::filesystem::path rel = std::filesystem::relative(targetDir, src, ec);
        const std::string relText = PathToGenericUtf8(rel);
        if (!ec && !rel.empty() && relText.rfind("..", 0u) != 0u)
        {
            ctx.lastFileMessage = Translate("assets.move_self");
            return false;
        }
    }

    const bool movingDirectory = std::filesystem::is_directory(src, ec);
    std::filesystem::rename(src, dst, ec);
    if (!ec && !movingDirectory)
        MoveAssetThumbnail(ctx, src, dst);
    if (ec)
    {
        ctx.lastFileMessage = Translate("assets.move_failed") + ec.message();
        engine::Debug::LogError("Asset-Browser: Verschieben fehlgeschlagen: %s", ec.message().c_str());
        return false;
    }

    browser.currentDir = targetDir;
    browser.expandedDirs.insert(PathToUtf8(targetDir));
    return true;
}

bool AcceptAssetMovePayload(EditorFrameContext& ctx,
                            AssetBrowserState& browser,
                            const std::filesystem::path& targetDir)
{
    if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("KROM_MOVE_FILE"))
    {
        return MoveAssetBrowserPath(
            ctx, browser,
            ToAbsoluteAssetBrowserPath(ctx, static_cast<const char*>(p->Data), false),
            targetDir);
    }
    if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("KROM_MOVE_DIR"))
    {
        return MoveAssetBrowserPath(
            ctx, browser,
            ToAbsoluteAssetBrowserPath(ctx, static_cast<const char*>(p->Data), false),
            targetDir);
    }
    if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("KROM_ASSET_MATERIAL"))
    {
        return MoveAssetBrowserPath(
            ctx, browser,
            ToAbsoluteAssetBrowserPath(ctx, static_cast<const char*>(p->Data), true),
            targetDir);
    }
    if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("KROM_ASSET_TEXTURE"))
    {
        return MoveAssetBrowserPath(
            ctx, browser,
            ToAbsoluteAssetBrowserPath(ctx, static_cast<const char*>(p->Data), true),
            targetDir);
    }
    return AcceptEntityToPrefabPayload(ctx, targetDir);
}

struct EditorImportShaders
{
    ShaderHandle vertex;
    ShaderHandle fragment;
    ShaderHandle shadow;
};

TextureHandle LoadRuntimeTexture(EditorFrameContext& ctx, const std::filesystem::path& path,
                                 float* aspect = nullptr, uint32_t* width = nullptr, uint32_t* height = nullptr)
{
    if (!ctx.assetPipeline)
        return TextureHandle::Invalid();

    const TextureHandle assetTexture = ctx.assetPipeline->LoadTexture(PathToUtf8(path));
    if (!assetTexture.IsValid())
        return TextureHandle::Invalid();

    if (const auto* texture = ctx.registry.textures.Get(assetTexture); texture && texture->height > 0u)
    {
        if (aspect) *aspect = static_cast<float>(texture->width) / static_cast<float>(texture->height);
        if (width) *width = texture->width;
        if (height) *height = texture->height;
    }

    ctx.assetPipeline->UploadPendingGpuAssets();

    const TextureHandle gpuTexture = ctx.assetPipeline->GetGpuTexture(assetTexture);
    if (!gpuTexture.IsValid())
    {
        engine::Debug::LogError("Asset-Browser: GPU-Upload der Textur fehlgeschlagen: %s",
                                PathToUtf8(path).c_str());
        return TextureHandle::Invalid();
    }

    return gpuTexture;
}

EditorImportShaders LoadEditorImportShaders(EditorFrameContext& ctx, bool skinned)
{
    EditorImportShaders shaders{};
    if (!ctx.assetPipeline)
        return shaders;

    const bool opengl = ctx.shaderTarget == assets::ShaderTargetProfile::OpenGL_GLSL450;
    const char* vsPath = opengl
        ? "lit.opengl.vs.glsl"
        : (skinned ? "skinned_lit.vs.hlsl" : "lit.vs.hlsl");
    const char* fsPath = opengl ? "lit.opengl.fs.glsl" : "lit.ps.hlsl";
    const char* shadowPath = opengl ? "shadow.opengl.vs.glsl"
                           : (skinned ? "skinned_shadow.vs.hlsl" : "shadow.vs.hlsl");

    shaders.vertex = ctx.assetPipeline->LoadShader(
        PathToUtf8(ResolveEngineShaderPath(ctx, vsPath)),
        assets::ShaderStage::Vertex);
    shaders.fragment = ctx.assetPipeline->LoadShader(
        PathToUtf8(ResolveEngineShaderPath(ctx, fsPath)),
        assets::ShaderStage::Fragment);
    shaders.shadow = ctx.assetPipeline->LoadShader(
        PathToUtf8(ResolveEngineShaderPath(ctx, shadowPath)),
        assets::ShaderStage::Vertex);
    return shaders;
}


// Ergebnis der Laufzeit-Material-Erstellung: Handle + optionaler PBR-Master + Template-Typ.
struct RuntimeMaterialResult
{
    MaterialHandle                          handle;
    std::shared_ptr<pbr::PbrMasterMaterial> pbrMaster;
    EditorMaterialTemplate                  materialTemplate = EditorMaterialTemplate::LegacyLit;
};

bool MeshUsesMultipleMaterialSlots(const assets::MeshAsset& mesh)
{
    std::vector<uint32_t> materialIndices;
    materialIndices.reserve(mesh.submeshes.size());

    for (const assets::SubMeshData& submesh : mesh.submeshes)
    {
        if (std::find(materialIndices.begin(), materialIndices.end(), submesh.materialIndex) !=
            materialIndices.end())
            continue;

        materialIndices.push_back(submesh.materialIndex);
        if (materialIndices.size() > 1u)
            return true;
    }

    return false;
}

uint32_t RequiredMaterialSlotCount(const assets::MeshAsset& mesh)
{
    uint32_t count = static_cast<uint32_t>(mesh.materialHandles.size());
    for (const assets::SubMeshData& submesh : mesh.submeshes)
        count = std::max(count, submesh.materialIndex + 1u);
    return count;
}

// Versucht PBR (mit Tangenten-Generierung), fällt auf LegacyLit zurück.
RuntimeMaterialResult CreateRuntimeMaterialForMesh(
    EditorFrameContext&           ctx,
    assets::MeshAsset&            mesh,   // non-const: Tangenten können ergänzt werden
    const char*                   name,
    bool                          skinned,
    const assets::MaterialAsset*  srcMat = nullptr)
{
    if (mesh.submeshes.empty())
        return {};

    if (ctx.assetPipeline)
    {
        bool tangentsOk = true;
        for (auto& sub : mesh.submeshes)
        {
            if (assets::HasValidTangents(sub)) continue;
            if (!assets::EnsureTangents(sub))  { tangentsOk = false; break; }
            sub.rawInterleavedBytes.clear();
            sub.rawVertexStride  = 0u;
            mesh.gpuStatus.dirty    = true;
            mesh.gpuStatus.uploaded = false;
        }

        if (tangentsOk)
        {
            std::string pbrErr;
            const renderer::VertexLayout pbrLayout = assets::ResolveVertexLayout(
                skinned ? renderer::VertexContracts::SkinnedLit() : renderer::VertexContracts::PbrLit(),
                mesh.submeshes[0], &pbrErr);

            if (!pbrLayout.attributes.empty())
            {
                const bool opengl = ctx.shaderTarget == assets::ShaderTargetProfile::OpenGL_GLSL450;
                const bool forwardPlus = ctx.forwardPlusActive && !opengl;
                const char* pbrFsPath = opengl ? "pbr_lit.opengl.fs.glsl" : (forwardPlus ? "pbr_forward_plus.ps.hlsl" : "pbr_lit.ps.hlsl");
                ShaderHandle vs = ctx.assetPipeline->LoadShader(
                    PathToUtf8(ResolveEngineShaderPath(ctx, opengl ? "pbr_lit.opengl.vs.glsl"
                                                       : (skinned ? "skinned_pbr_lit.vs.hlsl" : "pbr_lit.vs.hlsl"))),
                    assets::ShaderStage::Vertex);
                ShaderHandle fs = ctx.assetPipeline->LoadShader(
                    PathToUtf8(ResolveEngineShaderPath(ctx, pbrFsPath)),
                    assets::ShaderStage::Fragment);
                ShaderHandle shadow = ctx.assetPipeline->LoadShader(
                    PathToUtf8(ResolveEngineShaderPath(ctx, opengl ? "shadow.opengl.vs.glsl"
                                                       : (skinned ? "skinned_shadow_pbr.vs.hlsl" : "shadow.vs.hlsl"))),
                    assets::ShaderStage::Vertex);

                if (vs.IsValid() && fs.IsValid() && shadow.IsValid())
                {
                    renderer::pbr::PbrMasterMaterial::Config cfg{};
                    cfg.vs             = vs;
                    cfg.fs             = fs;
                    cfg.shadow         = shadow;
                    cfg.vertexLayout   = pbrLayout;
                    cfg.cullMode       = (srcMat && srcMat->doubleSided)
                                            ? MaterialCullMode::None
                                            : MaterialCullMode::Back;
                    cfg.castShadows    = true;
                    cfg.receiveShadows = true;

                    renderer::pbr::PbrMasterMaterial masterVal =
                        renderer::pbr::PbrMasterMaterial::Create(ctx.materials, cfg);

                    if (masterVal.IsValid())
                    {
                        constexpr math::Vec4 kDefaultColor{0.6f, 0.6f, 0.6f, 1.f};
                        const math::Vec4 baseColor = srcMat ? srcMat->baseColorFactor : kDefaultColor;
                        const float roughness      = srcMat ? srcMat->roughnessFactor : 0.6f;
                        const float metallic       = srcMat ? srcMat->metallicFactor  : 0.0f;

                        auto builder = masterVal.CreateInstance(name)
                            .BaseColor(baseColor)
                            .Roughness(roughness)
                            .Metallic(metallic)
                            .Occlusion(srcMat ? srcMat->occlusionStrength : 1.f)
                            .IBL(false);

                        if (srcMat)
                        {
                            if (!srcMat->baseColorTexture.path.empty())
                            {
                                if (const TextureHandle tex = LoadRuntimeTexture(ctx, PathFromUtf8(srcMat->baseColorTexture.path)); tex.IsValid())
                                    builder.BaseColor(tex);
                            }
                            if (!srcMat->metallicRoughnessTexture.path.empty())
                            {
                                if (const TextureHandle tex = LoadRuntimeTexture(ctx, PathFromUtf8(srcMat->metallicRoughnessTexture.path)); tex.IsValid())
                                {
                                    builder.Roughness(tex, pbr::MaterialChannel::G);
                                    builder.Metallic(tex, pbr::MaterialChannel::B);
                                    builder.Occlusion(tex, pbr::MaterialChannel::R, srcMat->occlusionStrength);
                                }
                            }
                            if (!srcMat->normalTexture.path.empty())
                            {
                                if (const TextureHandle tex = LoadRuntimeTexture(ctx, PathFromUtf8(srcMat->normalTexture.path)); tex.IsValid())
                                    builder.Normal(tex, srcMat->normalScale);
                            }
                            if (!srcMat->emissiveTexture.path.empty())
                            {
                                if (const TextureHandle tex = LoadRuntimeTexture(ctx, PathFromUtf8(srcMat->emissiveTexture.path)); tex.IsValid())
                                    builder.Emissive(tex);
                            }
                        }

                        MaterialHandle h = builder.Build();

                        if (h.IsValid())
                        {
                            ctx.materials.SetVec2(h, "uvScale",  math::Vec2{1.f, 1.f});
                            ctx.materials.SetVec2(h, "uvOffset", math::Vec2{0.f, 0.f});
                            auto master = std::make_shared<renderer::pbr::PbrMasterMaterial>(
                                std::move(masterVal));
                            return { h, std::move(master), EditorMaterialTemplate::PbrLit };
                        }
                    }
                }
            }
        }
    }

    // --- LegacyLit-Fallback ---
    const bool hasLegacyNormalMap = srcMat && !srcMat->normalTexture.path.empty();
    std::string layoutError;
    const renderer::VertexLayout layout = assets::ResolveVertexLayout(
        skinned ? renderer::VertexContracts::SkinnedLit()
                : hasLegacyNormalMap ? renderer::VertexContracts::PbrLit()
                                     : renderer::VertexContracts::StaticLit(),
        mesh.submeshes[0],
        &layoutError);
    if (layout.attributes.empty())
    {
        engine::Debug::LogError("Asset-Browser: Vertex-Layout fehlgeschlagen fuer '%s': %s",
                                name, layoutError.c_str());
        return {};
    }

    const EditorImportShaders shaders = LoadEditorImportShaders(ctx, skinned);
    if (!shaders.vertex.IsValid() || !shaders.fragment.IsValid() || !shaders.shadow.IsValid())
    {
        engine::Debug::LogError("Asset-Browser: Import-Shader konnten nicht geladen werden");
        return {};
    }

    constexpr math::Vec4 kDefaultColor{0.6f, 0.6f, 0.6f, 1.f};
    renderer::lit::LitMaterialCreateInfo info{};
    info.name               = std::string("EditorImport_") + name;
    info.vertexShader       = shaders.vertex;
    info.fragmentShader     = shaders.fragment;
    info.shadowShader       = shaders.shadow;
    info.vertexLayout       = layout;
    info.baseColorFactor    = srcMat ? srcMat->baseColorFactor : kDefaultColor;
    info.roughnessFactor    = srcMat ? srcMat->roughnessFactor : 0.6f;
    info.normalStrength     = srcMat ? srcMat->normalScale : 1.0f;
    info.specularStrength   = 0.3f;
    info.enableBaseColorMap = true;
    info.enableNormalMap    = hasLegacyNormalMap;
    info.castShadows        = true;
    info.doubleSided        = srcMat ? srcMat->doubleSided : true;

    const MaterialHandle handle = renderer::lit::LitMaterial::Register(ctx.materials, info);
    if (handle.IsValid() && srcMat)
    {
        if (!srcMat->baseColorTexture.path.empty())
        {
            if (const TextureHandle tex = LoadRuntimeTexture(ctx, PathFromUtf8(srcMat->baseColorTexture.path)); tex.IsValid())
                ctx.materials.SetTexture(handle, "albedo", tex);
        }
        if (!srcMat->emissiveTexture.path.empty())
        {
            if (const TextureHandle tex = LoadRuntimeTexture(ctx, PathFromUtf8(srcMat->emissiveTexture.path)); tex.IsValid())
                ctx.materials.SetTexture(handle, "emissive", tex);
        }
        if (!srcMat->normalTexture.path.empty())
        {
            if (const TextureHandle tex = LoadRuntimeTexture(ctx, PathFromUtf8(srcMat->normalTexture.path)); tex.IsValid())
                ctx.materials.SetTexture(handle, "normalMap", tex);
        }
        ctx.materials.MarkDirty(handle);
    }

    return { handle, nullptr, EditorMaterialTemplate::LegacyLit };
}

void AssignRuntimeMaterials(
    EditorFrameContext&                                     ctx,
    engine::addons::prefab::PrefabAsset&                   prefab,
    std::unordered_map<uint32_t, std::vector<RuntimeMaterialResult>>& outResults)
{
    for (engine::addons::prefab::PrefabEntityRecord& record : prefab.records)
    {
        if (!record.mesh.IsValid())
            continue;

        auto cached = outResults.find(record.mesh.value);
        if (cached != outResults.end())
        {
            record.material =
                cached->second.size() == 1u && cached->second[0].handle.IsValid()
                    ? cached->second[0].handle
                    : MaterialHandle::Invalid();
            continue;
        }

        assets::MeshAsset* mesh = ctx.registry.meshes.Get(record.mesh);
        if (!mesh)
            continue;

        std::vector<MaterialHandle> sourceMaterialHandles = mesh->materialHandles;
        const uint32_t requiredSlotCount = RequiredMaterialSlotCount(*mesh);
        sourceMaterialHandles.resize(requiredSlotCount, MaterialHandle::Invalid());

        std::vector<RuntimeMaterialResult> results(requiredSlotCount);
        std::vector<MaterialHandle> runtimeMaterialHandles(requiredSlotCount, MaterialHandle::Invalid());

        for (uint32_t slot = 0u; slot < requiredSlotCount; ++slot)
        {
            const assets::MaterialAsset* srcMat = sourceMaterialHandles[slot].IsValid()
                ? ctx.registry.materials.Get(sourceMaterialHandles[slot])
                : nullptr;

            const std::string materialName =
                (record.name.empty() ? mesh->debugName : record.name) +
                "_mat" + std::to_string(slot);

            RuntimeMaterialResult result = CreateRuntimeMaterialForMesh(
                ctx, *mesh,
                materialName.c_str(),
                record.skeleton.IsValid(),
                srcMat);

            if (result.handle.IsValid())
                runtimeMaterialHandles[slot] = result.handle;
            results[slot] = std::move(result);
        }

        mesh->materialHandles = std::move(runtimeMaterialHandles);
        record.material =
            !MeshUsesMultipleMaterialSlots(*mesh) && !mesh->materialHandles.empty() &&
            mesh->materialHandles[0].IsValid()
                ? mesh->materialHandles[0]
                : MaterialHandle::Invalid();

        outResults.emplace(record.mesh.value, std::move(results));
    }
}

// Sammelt alle Entities mit MeshComponent die Nachfahren von 'root' sind (inkl. root selbst).
void CollectMeshEntities(ecs::World& world, EntityID root, std::vector<EntityID>& out)
{
    world.ForEachAlive([&](EntityID id)
    {
        if (!world.Get<MeshComponent>(id))
            return;

        // Pruefen ob id == root oder ein Nachfahre von root ist
        EntityID cur = id;
        while (cur.IsValid())
        {
            if (cur == root) { out.push_back(id); return; }
            const auto* pc = world.Get<ParentComponent>(cur);
            cur = pc ? pc->parent : NULL_ENTITY;
        }
    });
}

void ApplyTextureToMeshEntity(EditorFrameContext& ctx,
                              EntityID entity,
                              TextureHandle tex,
                              const std::string& paramName,
                              const std::filesystem::path& path,
                              const std::string& baseName)
{
    auto* meshComp = ctx.world.Get<MeshComponent>(entity);
    if (!meshComp || !meshComp->mesh.IsValid())
        return;
    assets::MeshAsset* mesh = ctx.registry.meshes.Get(meshComp->mesh);
    if (!mesh || mesh->submeshes.empty())
        return;

    MaterialComponent* materialComponent = ctx.world.Get<MaterialComponent>(entity);
    MaterialHandle matHandle = MaterialHandle::Invalid();
    if (materialComponent)
        matHandle = materialComponent->material;
    if (!matHandle.IsValid() && !mesh->materialHandles.empty())
        matHandle = mesh->materialHandles[0];

    if (!matHandle.IsValid())
    {
        matHandle = CreateRuntimeMaterialForMesh(ctx, *mesh, baseName.c_str(), /*skinned=*/false).handle;
        if (!matHandle.IsValid())
        {
            engine::Debug::LogError("Asset-Browser: Material konnte nicht erstellt werden fuer Textur-Zuweisung");
            return;
        }
        if (materialComponent)
            materialComponent->material = matHandle;
        else
            materialComponent = &ctx.world.Add<MaterialComponent>(entity, matHandle);
        for (MaterialHandle& h : mesh->materialHandles)
            h = matHandle;
    }

    // Sicherheitscheck: hat das Material diesen Textur-Parameter ueberhaupt?
    // LegacyLit mit enableBaseColorMap=false hat keine Texture2D-Slots —
    // SetTexture wuerde dann ins Leere schreiben ohne sichtbaren Effekt.
    if (const CompiledMaterialDesc* compiled = ctx.materials.GetCompiledDesc(matHandle))
    {
        bool hasSlot = false;
        for (uint32_t i = 0u; i < compiled->parameterLayout.slotCount; ++i)
        {
            const MaterialParameterSlot& slot = compiled->parameterLayout.slots[i];
            if (slot.IsValid()
                && slot.type == MaterialParameterType::Texture2D
                && std::string_view(slot.Name()) == paramName)
            {
                hasSlot = true;
                break;
            }
        }
        if (!hasSlot)
        {
            const MaterialDesc* desc = ctx.materials.GetDesc(matHandle);
            const char* matName = (desc && !desc->name.empty()) ? desc->name.c_str() : baseName.c_str();
            engine::Debug::LogWarning(
                "Asset-Browser: Material '%s' hat keinen Textur-Parameter '%s' "
                "(z.B. LegacyLit ohne Textur-Support) — Zuweisung uebersprungen.",
                matName, paramName.c_str());
            return;
        }
    }

    ctx.materials.SetTexture(matHandle, paramName.c_str(), tex);
    ctx.materials.MarkDirty(matHandle);

    const std::string texturePath = PathToGenericUtf8(path.lexically_normal());
    if (!materialComponent)
    {
        MaterialComponent component{};
        component.material = matHandle;
        materialComponent = &ctx.world.Add<MaterialComponent>(entity, component);
    }
    else if (!materialComponent->material.IsValid())
    {
        materialComponent->material = matHandle;
    }

    if (materialComponent)
    {
        if (paramName == "albedo" || paramName == "baseColor" || paramName == "Base Color")
            materialComponent->baseColorTexturePath = texturePath;
    }

}

void AssignTexture(EditorFrameContext& ctx, const std::filesystem::path& path)
{
    if (!ctx.assetPipeline)
        return;

    // Prioritaet 1: das im Materialeditor geoeffnete Material-Asset.
    // AssignTextureToSelectedMaterial nutzt nur den Dateipfad, nicht den Handle —
    // daher KEIN frühes Return wenn LoadRuntimeTexture noch keinen gültigen GPU-
    // Handle liefert (Textur wird asynchron geladen). Beim ersten Doppelklick wird
    // der Pfad sofort im Asset gesetzt; UpdateCachedTexture lädt die Textur im
    // nächsten Frame nach. Ohne diese Änderung musste der Nutzer zweimal doppel-
    // klicken: erster Doppelklick schlug fehl weil tex.IsValid() = false.
    FlushPendingMaterialOpen(ctx);
    if (ctx.state.materialWindowOpen && !ctx.state.selectedMaterialAssetPath.empty())
    {
        // Textur schon jetzt laden damit GPU-Upload angestossen wird.
        // Fehler werden still ignoriert — der Pfad wird trotzdem gespeichert
        // und beim nächsten PreviewMaterialAssetOnBoundEntities nachgeladen.
        LoadRuntimeTexture(ctx, path);
        AssignTextureToSelectedMaterial(ctx, TextureHandle::Invalid(),
                                        ctx.state.activeTextureParamName,
                                        PathToUtf8(path.lexically_normal()));
        return;
    }

    // Prioritaet 2: Entity-basiert — hier wird der Handle benötigt.
    const TextureHandle tex = LoadRuntimeTexture(ctx, path);
    if (!tex.IsValid())
    {
        engine::Debug::LogError("Asset-Browser: Textur konnte nicht geladen werden: %s", PathToUtf8(path).c_str());
        return;
    }

    if (ctx.state.selectedEntity.IsValid() && ctx.world.IsAlive(ctx.state.selectedEntity))
    {
        std::vector<EntityID> targets;
        CollectMeshEntities(ctx.world, ctx.state.selectedEntity, targets);
        const std::string baseName = PathToUtf8(path.stem());
        bool skippedAssetMaterial = false;
        for (EntityID target : targets)
        {
            // A material asset is shared and only changes in the material editor.
            const auto* material = ctx.world.Get<MaterialComponent>(target);
            bool usesAsset = material && !material->materialAssetPath.empty();
            if (material)
                for (const auto& slot : material->slotOverrides)
                    usesAsset = usesAsset || !slot.materialAssetPath.empty();
            if (usesAsset)
            {
                skippedAssetMaterial = true;
                continue;
            }
            ApplyTextureToMeshEntity(ctx, target, tex, ctx.state.activeTextureParamName, path, baseName);
        }
        if (skippedAssetMaterial)
            engine::Debug::LogWarning("Asset browser: open the material in the material editor to change its textures.");
        return;
    }

    engine::Debug::LogWarning(
        "Asset-Browser: Bitte zuerst ein Material-Asset oder eine Mesh-Entity auswaehlen, bevor eine Textur zugewiesen wird.");
}

collision::Ray BuildEditorMouseRay(const EditorFrameContext& ctx, const ImVec2& mousePos)
{
    const ImGuiIO& io = ImGui::GetIO();
    const EditorCameraState& cam = ctx.state.editorCamera;
    // Bildbereich der Szene (Viewport-Panel), ohne Panel das ganze Fenster.
    const bool hasView = ctx.renderViewWidth > 1.f && ctx.renderViewHeight > 1.f;
    // Without a view the whole editor window; its origin is a desktop position
    // once multi-viewports are enabled.
    const ImVec2 windowOrigin = ImGui::GetCurrentContext() ? ImGui::GetMainViewport()->Pos : ImVec2(0.f, 0.f);
    const float originX = hasView ? ctx.renderViewX : windowOrigin.x;
    const float originY = hasView ? ctx.renderViewY : windowOrigin.y;
    const float displayW = hasView ? ctx.renderViewWidth : std::max(io.DisplaySize.x, 1.f);
    const float displayH = hasView ? ctx.renderViewHeight : std::max(io.DisplaySize.y, 1.f);
    const float x = std::clamp(mousePos.x - originX, 0.f, displayW);
    const float y = std::clamp(mousePos.y - originY, 0.f, displayH);
    const float ndcX = (x / displayW) * 2.f - 1.f;
    const float ndcY = 1.f - (y / displayH) * 2.f;
    const float aspect = displayW / displayH;
    const float tanHalfFov = std::tan((cam.fovDeg * math::DEG_TO_RAD) * 0.5f);
    const math::Quat rot = math::Quat::FromEulerDeg(cam.pitchDeg, cam.yawDeg, 0.f);
    const math::Vec3 rayDir = rot.Rotate({
        ndcX * aspect * tanHalfFov,
        ndcY * tanHalfFov,
        -1.f
    }).Normalized();
    return { cam.position, rayDir };
}

math::Vec3 AssetDropPosition(EditorFrameContext& ctx, const ImVec2& mousePos)
{
    const collision::Ray ray = BuildEditorMouseRay(ctx, mousePos);
    const float kMaxDist = EditorPickMaxDistance(ctx.state.editorCamera);

    collision::RaycastHit hit{};
    if (mesh_renderer::MeshSceneQueries::RaycastTriangles(ctx.world, ctx.registry, ray, kMaxDist, hit))
        return ray.origin + ray.direction * hit.distance;

    collision::SceneQueries queries;
    queries.Build(ctx.world);
    if (queries.Raycast(ray, kMaxDist, hit))
        return ray.origin + ray.direction * hit.distance;

    if (std::abs(ray.direction.y) > 1e-4f)
    {
        const float t = -ray.origin.y / ray.direction.y;
        if (t > 0.f && t < kMaxDist)
            return ray.origin + ray.direction * t;
    }

    return ray.origin + ray.direction * 5.f;
}

// Registry keys and prefab references of an imported model use the path relative to
// the project's Assets folder (as scene loading does), so saved prefabs stay portable.
void UseAssetRelativeBundlePaths(const EditorFrameContext& ctx,
                                 engine::assets::ImportedAssetBundle& bundle,
                                 const std::filesystem::path& path)
{
    if (!ctx.assetPipeline)
        return;
    const std::string relative = ctx.assetPipeline->ToAssetRelative(path);
    if (relative.empty() || !IsPortableAssetPath(relative))
        return;
    for (auto& mesh : bundle.meshes)          mesh.path = relative;
    for (auto& skeleton : bundle.skeletons)   skeleton.path = relative;
    for (auto& animation : bundle.animations) animation.path = relative;
}

// Doppelklick auf ein Modell: Modell-Ansicht (wie Prefab-Modus, nur anschauen).
void OpenModelView(EditorFrameContext& ctx, const std::filesystem::path& path)
{
    engine::addons::gltf::GltfImporter importer;
    engine::assets::ImportedAssetBundle bundle = importer.Import(PathToUtf8(path));
    if (!bundle.Ok())
    {
        ctx.lastFileMessage = Translate("assets.model_load_failed") + bundle.error;
        return;
    }
    UseAssetRelativeBundlePaths(ctx, bundle, path);
    engine::addons::prefab::PrefabBuildOptions buildOptions;
    buildOptions.name                = PathToUtf8(path.stem());
    buildOptions.createSyntheticRoot = true;
    buildOptions.playFirstAnimation  = true;
    buildOptions.loopAnimations      = true;
    engine::addons::prefab::PrefabAsset prefab =
        engine::addons::prefab::BuildPrefabFromImportedBundle(std::move(bundle), ctx.registry, buildOptions);
    if (prefab.Empty())
    {
        ctx.lastFileMessage = Translate("assets.model_empty") + PathToUtf8(path.filename());
        return;
    }
    std::unordered_map<uint32_t, std::vector<RuntimeMaterialResult>> runtimeResults;
    AssignRuntimeMaterials(ctx, prefab, runtimeResults);
    EnterModelView(ctx, path, prefab);
}

void SpawnModel(EditorFrameContext& ctx,
                const std::filesystem::path& path,
                const math::Vec3* worldPosition = nullptr)
{
    if (!ctx.assetPipeline)
    {
        engine::Debug::LogError("Asset-Browser: kein AssetPipeline-Kontext fuer Model-Spawn");
        return;
    }

    engine::addons::gltf::GltfImporter importer;
    engine::assets::ImportedAssetBundle bundle = importer.Import(PathToUtf8(path));
    if (!bundle.Ok())
    {
        engine::Debug::LogError("Asset-Browser: Import fehlgeschlagen: %s", bundle.error.c_str());
        return;
    }
    UseAssetRelativeBundlePaths(ctx, bundle, path);

    engine::addons::prefab::PrefabBuildOptions buildOptions;
    buildOptions.name                = PathToUtf8(path.stem());
    buildOptions.createSyntheticRoot = true;
    buildOptions.playFirstAnimation  = true;
    buildOptions.loopAnimations      = true;

    engine::addons::prefab::PrefabAsset prefab =
        engine::addons::prefab::BuildPrefabFromImportedBundle(
            std::move(bundle), ctx.registry, buildOptions);

    if (prefab.Empty())
    {
        engine::Debug::LogError("Asset-Browser: Prefab leer nach Import: %s", PathToUtf8(path).c_str());
        return;
    }

    std::unordered_map<uint32_t, std::vector<RuntimeMaterialResult>> runtimeResults;
    AssignRuntimeMaterials(ctx, prefab, runtimeResults);

    engine::addons::prefab::PrefabInstantiateOptions instantiateOptions{};
    instantiateOptions.scriptRegistry = ctx.scriptRegistry;
    const engine::addons::prefab::PrefabInstance instance =
        engine::addons::prefab::InstantiatePrefab(ctx.world, prefab, instantiateOptions);

    if (instance.IsValid())
    {
        ctx.state.selectedEntity = instance.root;

        if (worldPosition)
        {
            if (auto* t = ctx.world.Get<TransformComponent>(instance.root))
            {
                t->localPosition = *worldPosition;
                t->dirty = true;
                ++t->localVersion;
            }
            return;
        }

        // Kombinierte AABB aller Meshes unter dem Root berechnen
        math::Vec3 combinedMin{ 1e30f,  1e30f,  1e30f};
        math::Vec3 combinedMax{-1e30f, -1e30f, -1e30f};
        bool hasBounds = false;

        std::vector<EntityID> meshEntities;
        CollectMeshEntities(ctx.world, instance.root, meshEntities);
        for (EntityID meshEnt : meshEntities)
        {
            const auto* mc = ctx.world.Get<MeshComponent>(meshEnt);
            if (!mc) continue;
            const assets::MeshAsset* mesh = ctx.registry.meshes.Get(mc->mesh);
            if (!mesh) continue;

            math::Vec3 meshMin, meshMax;
            mesh->ComputeBounds(meshMin, meshMax);

            combinedMin.x = std::min(combinedMin.x, meshMin.x);
            combinedMin.y = std::min(combinedMin.y, meshMin.y);
            combinedMin.z = std::min(combinedMin.z, meshMin.z);
            combinedMax.x = std::max(combinedMax.x, meshMax.x);
            combinedMax.y = std::max(combinedMax.y, meshMax.y);
            combinedMax.z = std::max(combinedMax.z, meshMax.z);
            hasBounds = true;
        }

        // Spawn-Distanz: Bounding-Sphere-Radius durch tan(30°) (passt fuer 60° FOV), 1.5x Marge
        float spawnDist = 5.f;
        if (hasBounds)
        {
            const float ex     = (combinedMax.x - combinedMin.x) * 0.5f;
            const float ey     = (combinedMax.y - combinedMin.y) * 0.5f;
            const float ez     = (combinedMax.z - combinedMin.z) * 0.5f;
            const float radius = std::sqrt(ex * ex + ey * ey + ez * ez);
            spawnDist = std::max(radius / 0.577f * 1.5f, 0.5f);
        }

        const EditorCameraState& cam     = ctx.state.editorCamera;
        const math::Quat         rot     = math::Quat::FromEulerDeg(cam.pitchDeg, cam.yawDeg, 0.f);
        const math::Vec3         forward = rot.Rotate({0.f, 0.f, -1.f});

        if (auto* t = ctx.world.Get<TransformComponent>(instance.root))
        {
            t->localPosition = cam.position + forward * spawnDist;
            t->dirty         = true;
        }
    }
}

void SpawnPrefab(EditorFrameContext& ctx,
                 const std::filesystem::path& path,
                 const math::Vec3* worldPosition = nullptr)
{
    engine::addons::prefab::PrefabAsset prefab;
    std::string error;
    if (!engine::addons::prefab::LoadPrefabFromFile(path, ctx.registry, prefab, &error))
    {
        engine::Debug::LogError("Asset-Browser: Prefab konnte nicht geladen werden: %s",
                                error.empty() ? PathToUtf8(path).c_str() : error.c_str());
        return;
    }

    engine::addons::prefab::PrefabInstantiateOptions instantiateOptions{};
    instantiateOptions.scriptRegistry = ctx.scriptRegistry;
    const engine::addons::prefab::PrefabInstance instance =
        engine::addons::prefab::InstantiatePrefab(ctx.world, prefab, instantiateOptions);
    if (!instance.IsValid())
    {
        engine::Debug::LogError("Asset-Browser: Prefab konnte nicht instanziiert werden: %s",
                                PathToUtf8(path).c_str());
        return;
    }

    if (ctx.assetPipeline)
    {
        engine::addons::mesh_renderer::ResolveMeshAssetBindings(
            *ctx.assetPipeline, ctx.registry, ctx.world);
        ResolveMaterialAssetBindings(ctx);
    }

    for (size_t i = 0; i < prefab.records.size() && i < instance.entities.size(); ++i)
    {
        const auto& record = prefab.records[i];
        if (record.baseColorTexturePath.empty())
            continue;

        // Nur als Fallback anwenden, wenn noch kein gültiges GPU-Material-Handle vorhanden.
        // ResolveMaterialAssetBindings hat ggf. bereits ein .kmat aufgelöst — das darf nicht
        // durch ein neu erstelltes LegacyLit-Material überschrieben werden.
        const EntityID entity = instance.entities[i];
        if (const auto* mc = ctx.world.Get<MaterialComponent>(entity))
            if (mc->material.IsValid())
                continue;

        const TextureHandle texture = LoadRuntimeTexture(ctx, PathFromUtf8(record.baseColorTexturePath));
        if (!texture.IsValid())
            continue;
        ApplyTextureToMeshEntity(ctx,
                                 entity,
                                 texture,
                                 "albedo",
                                 PathFromUtf8(record.baseColorTexturePath),
                                 record.name.empty() ? prefab.name : record.name);
    }

    if (worldPosition)
    {
        if (auto* transform = ctx.world.Get<TransformComponent>(instance.root))
        {
            transform->localPosition = *worldPosition;
            transform->dirty = true;
            ++transform->localVersion;
        }
    }

    // PrefabInstanceComponent: Live-Link zur Quell-Datei
    {
        std::string relPath = PathToGenericUtf8(path);
        if (!ctx.currentProjectRoot.empty())
        {
            const std::filesystem::path assetsRoot =
                PathFromUtf8(ctx.currentProjectRoot) / "Assets";
            std::error_code ec;
            auto rel = std::filesystem::relative(path, assetsRoot, ec);
            if (!ec) relPath = PathToGenericUtf8(rel);
        }
        engine::addons::prefab::LinkPrefabInstance(ctx.world, prefab, instance, relPath);
    }

    ctx.state.selectedEntity = instance.root;
    ctx.lastFileMessage = Translate("prefab.loaded_prefix") + PathToUtf8(path.filename());
}

std::filesystem::path ResolveAssetPayloadPath(const EditorFrameContext& ctx, const char* payloadPath)
{
    if (!payloadPath || payloadPath[0] == '\0')
        return {};

    std::filesystem::path path = PathFromUtf8(payloadPath);
    if (path.is_absolute())
        return path;
    if (!ctx.currentProjectRoot.empty())
        return PathFromUtf8(ctx.currentProjectRoot) / "Assets" / path;
    return path;
}

void DrawViewportAssetDropTarget(EditorFrameContext& ctx)
{
    const ImGuiPayload* activePayload = ImGui::GetDragDropPayload();
    if (!activePayload)
        return;

    const std::string_view type(activePayload->DataType);
    const bool acceptsPrefab = type == "KROM_ASSET_PREFAB";
    const bool acceptsModel = type == "KROM_MOVE_FILE";
    if (!acceptsPrefab && !acceptsModel)
        return;

    ImGuiIO& io = ImGui::GetIO();
    ImVec2 renderPos{};
    ImVec2 renderSize{};
    ComputeSceneViewRect(ctx, renderPos, renderSize);
    ImGui::SetNextWindowPos(renderPos);
    ImGui::SetNextWindowSize(renderSize);
    ImGui::SetNextWindowBgAlpha(0.f);

    constexpr ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav |
    ImGuiWindowFlags_NoBackground;
    // Ohne NoBringToFrontOnFocus: sonst laege die Ablageflaeche hinter dem Viewport-Panel.

    detail::KeepNextWindowInMainViewport("##ViewportAssetDropTarget");
    ImGui::Begin("##ViewportAssetDropTarget", nullptr, flags);
    ImGui::InvisibleButton("##ViewportAssetDropArea", renderSize);

    if (acceptsPrefab && ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("KROM_ASSET_PREFAB"))
        {
            const auto* rawPath = static_cast<const char*>(payload->Data);
            const std::filesystem::path path = ResolveAssetPayloadPath(ctx, rawPath);
            const math::Vec3 position = AssetDropPosition(ctx, io.MousePos);
            SpawnPrefab(ctx, path, &position);
        }

        ImGui::EndDragDropTarget();
    }

    if (const auto path = AcceptModelAssetDrop(ctx); !path.empty())
    {
        const math::Vec3 position = AssetDropPosition(ctx, io.MousePos);
        SpawnModel(ctx, path, &position);
    }

    ImGui::End();
}


static void StartRename(AssetBrowserState& browser, const std::filesystem::path& path);
static bool DrawRenameInputIfActive(EditorFrameContext& ctx, AssetBrowserState& browser,
                                    const std::filesystem::path& path);
static void DrawAssetCreateMenu(EditorFrameContext& ctx,
                                AssetBrowserState& browser,
                                const std::filesystem::path& directory);

uint64_t FileStamp(const std::filesystem::path& path)
{
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(path, ec);
    if (ec)
        return 0u;
    return static_cast<uint64_t>(t.time_since_epoch().count());
}

// Inhalt des aktuellen Ordners (siehe AssetBrowserState::listedEntries).
const std::vector<std::filesystem::directory_entry>& CurrentDirEntries(AssetBrowserState& browser)
{
    constexpr auto kRefreshInterval = std::chrono::seconds(1);
    const auto now = std::chrono::steady_clock::now();
    std::error_code ec;
    const auto dirStamp = std::filesystem::last_write_time(browser.currentDir, ec);
    if (browser.listedDir == browser.currentDir && dirStamp == browser.listedDirStamp &&
        now < browser.listedAt + kRefreshInterval)
    {
        return browser.listedEntries;
    }

    browser.listedDir = browser.currentDir;
    browser.listedDirStamp = dirStamp;
    browser.listedAt = now;
    browser.listedEntries.clear();
    browser.listedStamps.clear();
    for (const auto& entry : std::filesystem::directory_iterator(browser.currentDir, ec))
    {
        browser.listedEntries.push_back(entry);
        std::error_code stampEc;
        const auto stamp = entry.last_write_time(stampEc);
        if (!stampEc)
        {
            browser.listedStamps[PathToGenericUtf8(entry.path().lexically_normal())] =
                static_cast<uint64_t>(stamp.time_since_epoch().count());
        }
    }
    return browser.listedEntries;
}

// Aenderungszeit aus dem Ordner-Cache, sonst direkt von der Platte.
uint64_t CachedFileStamp(const AssetBrowserState& browser, const std::filesystem::path& path)
{
    const auto it = browser.listedStamps.find(PathToGenericUtf8(path.lexically_normal()));
    return it != browser.listedStamps.end() ? it->second : FileStamp(path);
}

const AssetBrowserState::TextureThumbnail* ResolveTextureThumbnail(
    EditorFrameContext& ctx,
    AssetBrowserState& browser,
    const std::filesystem::path& path)
{
    if (!ctx.assetPipeline || !ctx.editorTextureId)
        return nullptr;

    const std::string key = PathToGenericUtf8(path.lexically_normal());
    const uint64_t stamp = CachedFileStamp(browser, path);
    const auto [entry, inserted] = browser.textureThumbnails.try_emplace(key);
    auto& thumb = entry->second;

    if (inserted || thumb.fileStamp != stamp)
    {
        thumb = {};
        thumb.fileStamp = stamp;
        thumb.gpuTexture = LoadRuntimeTexture(ctx, path, &thumb.aspect, &thumb.width, &thumb.height);
    }
    if (!thumb.imguiId && thumb.gpuTexture.IsValid())
        thumb.imguiId = ctx.editorTextureId(thumb.gpuTexture);

    return thumb.imguiId ? &thumb : nullptr;
}

AssetBrowserState::AssetPreviewThumbnail* ResolveAssetPreviewThumbnail(
    AssetBrowserState& browser,
    const std::filesystem::path& path,
    AssetBrowserState::PreviewKind kind)
{
    constexpr uint32_t kAssetPreviewThumbnailSchemaVersion = 3u;
    const std::string key = PathToGenericUtf8(path.lexically_normal());
    const uint64_t stamp = CachedFileStamp(browser, path);
    auto& thumb = browser.assetPreviewThumbnails[key];
    if (thumb.fileStamp != stamp ||
        thumb.kind != kind ||
        thumb.schemaVersion != kAssetPreviewThumbnailSchemaVersion)
    {
        thumb.kind = kind;
        thumb.status = AssetBrowserState::PreviewStatus::Queued;
        thumb.fileStamp = stamp;
        thumb.schemaVersion = kAssetPreviewThumbnailSchemaVersion;
        thumb.imguiId = nullptr;
    }
    if (thumb.status == AssetBrowserState::PreviewStatus::Empty)
    {
        thumb.kind = kind;
        thumb.status = AssetBrowserState::PreviewStatus::Queued;
        thumb.fileStamp = stamp;
        thumb.schemaVersion = kAssetPreviewThumbnailSchemaVersion;
    }
    return &thumb;
}

} // namespace

std::filesystem::path AcceptModelAssetDrop(const EditorFrameContext& ctx)
{
    const ImGuiPayload* active = ImGui::GetDragDropPayload();
    if (!active || !active->IsDataType("KROM_MOVE_FILE"))
        return {};

    const auto path = ResolveAssetPayloadPath(ctx, static_cast<const char*>(active->Data));
    if (!IsSupportedModel(path) || !ImGui::BeginDragDropTarget())
        return {};

    const bool dropped = ImGui::AcceptDragDropPayload("KROM_MOVE_FILE") != nullptr;
    ImGui::EndDragDropTarget();
    return dropped ? path : std::filesystem::path{};
}

bool IsParticleEffectPrefabJson(const std::string& json)
{
    std::string error;
    const serialization::JsonValue document = serialization::JsonParser::Parse(json, error);
    const serialization::JsonValue* records = document.IsObject() ? document.Get("records") : nullptr;
    if (!records || !records->IsArray() || records->arrayVal.empty())
        return false;
    const serialization::JsonValue* rootIndex = document.Get("rootIndex");
    const size_t index = rootIndex ? rootIndex->AsUint() : 0u;
    if (index >= records->arrayVal.size())
        return false;

    const auto hasEmitter = [](const serialization::JsonValue& record) {
        const serialization::JsonValue* emitter = record.Get("particleEmitter");
        return emitter && emitter->IsObject();
    };
    const auto hasContent = [](const serialization::JsonValue& record) {
        const serialization::JsonValue* mesh = record.Get("meshAssetPath");
        const serialization::JsonValue* nested = record.Get("nestedPrefabJson");
        return (mesh && mesh->IsString() && !mesh->AsString().empty()) ||
               (nested && nested->IsString() && !nested->AsString().empty());
    };

    // Die Wurzel bestimmt den Typ: ein Emitter an ihr ist ein Effekt, ein Mesh
    // ein normales Prefab. Eine leere Wurzel (Gruppe) ist ein Effekt, wenn
    // darunter nur Emitter haengen und kein Mesh und kein eingebettetes Prefab.
    const serialization::JsonValue& root = records->arrayVal[index];
    if (!root.IsObject())
        return false;
    if (hasEmitter(root))
        return true;
    if (hasContent(root))
        return false;
    bool anyEmitter = false;
    for (const serialization::JsonValue& record : records->arrayVal)
    {
        if (!record.IsObject())
            continue;
        if (hasContent(record))
            return false;
        anyEmitter |= hasEmitter(record);
    }
    return anyEmitter;
}

namespace {

bool DrawAssetPreviewTile(EditorFrameContext& ctx,
                          AssetBrowserState& browser,
                          const std::filesystem::path& path,
                          AssetBrowserState::PreviewKind kind,
                          bool selected,
                          const char* fallbackText,
                          const char* iconGlyph = nullptr)
{
    (void)ctx;
    constexpr float kThumbSize = 72.f;
    constexpr float kLabelWidth = 104.f;
    constexpr float kTileHeight = 100.f;

    // Assets shown as a plain symbol do not need a rendered thumbnail.
    AssetBrowserState::AssetPreviewThumbnail* thumb =
        iconGlyph ? nullptr : ResolveAssetPreviewThumbnail(browser, path, kind);

    ImGui::InvisibleButton("##asset_preview", ImVec2(kLabelWidth, kTileHeight));
    const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    const ImVec2 itemMin = ImGui::GetItemRectMin();
    const ImVec2 previewMin{itemMin.x + (kLabelWidth - kThumbSize) * 0.5f, itemMin.y};
    const ImVec2 previewMax{previewMin.x + kThumbSize, previewMin.y + kThumbSize};
    ImDrawList* drawList = ImGui::GetWindowDrawList();

    if (selected)
        drawList->AddRectFilled(previewMin, previewMax, IM_COL32(120, 165, 210, 70));

    if (thumb && thumb->imguiId && thumb->status == AssetBrowserState::PreviewStatus::Ready)
    {
        drawList->AddImage(reinterpret_cast<ImTextureID>(thumb->imguiId),
                           previewMin,
                           previewMax,
                           ImVec2(0.f, 0.f),
                           ImVec2(1.f, 1.f));
    }
    else
    {
        drawList->AddRectFilled(previewMin, previewMax, IM_COL32(48, 52, 56, 255));
        if (iconGlyph)
        {
            DrawIcon(drawList, iconGlyph, ImVec2(previewMin.x + kThumbSize * 0.5f, previewMin.y + kThumbSize * 0.5f), 38.f,
                     ImGui::GetColorU32(ImGuiCol_Text));
            fallbackText = "";
        }
        const ImVec2 textSize = ImGui::CalcTextSize(fallbackText);
        drawList->AddText(ImVec2(previewMin.x + (kThumbSize - textSize.x) * 0.5f,
                                 previewMin.y + (kThumbSize - textSize.y) * 0.5f),
                          ImGui::GetColorU32(ImGuiCol_TextDisabled),
                          fallbackText);
    }

    drawList->AddRect(previewMin, previewMax, ImGui::GetColorU32(ImGuiCol_Border));
    if (selected)
        drawList->AddRect(previewMin, previewMax, IM_COL32(160, 200, 245, 255), 0.f, 0, 2.f);

    // Typ-Symbol unten links im Bild: Modell, Prefab oder Partikel-Effekt
    // (Prefab mit Emitter). Material braucht keines, die Kugel ist eindeutig.
    if (thumb && kind == AssetBrowserState::PreviewKind::Prefab && thumb->contentStamp != thumb->fileStamp)
    {
        thumb->contentStamp = thumb->fileStamp;
        std::ifstream file(path, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        thumb->hasParticles = IsParticleEffectPrefabJson(text);
    }
    const char* badgeIcon = nullptr;
    const char* badgeTip = nullptr;
    ImU32 badgeColor = 0u;
    if (kind == AssetBrowserState::PreviewKind::Model)
    {
        badgeIcon = ICON_LC_BOX;
        badgeTip = "assets.kind_model";
        badgeColor = IM_COL32(70, 130, 210, 235);
    }
    else if (kind == AssetBrowserState::PreviewKind::Prefab && thumb && thumb->hasParticles)
    {
        badgeIcon = ICON_LC_SPARKLES;
        badgeTip = "assets.kind_particles";
        badgeColor = IM_COL32(220, 125, 40, 235);
    }
    else if (kind == AssetBrowserState::PreviewKind::Prefab)
    {
        badgeIcon = ICON_LC_PACKAGE;
        badgeTip = "assets.kind_prefab";
        badgeColor = IM_COL32(60, 165, 120, 235);
    }
    if (badgeIcon)
    {
        const ImVec2 iconSize = ImGui::CalcTextSize(badgeIcon);
        const float side = std::max(iconSize.x, iconSize.y) + 6.f;
        const ImVec2 badgeMin(previewMin.x + 3.f, previewMax.y - side - 3.f);
        const ImVec2 badgeMax(badgeMin.x + side, badgeMin.y + side);
        drawList->AddRectFilled(badgeMin, badgeMax, badgeColor, 4.f);
        drawList->AddText(ImVec2(badgeMin.x + (side - iconSize.x) * 0.5f, badgeMin.y + (side - iconSize.y) * 0.5f),
                          IM_COL32(255, 255, 255, 255), badgeIcon);
        if (ImGui::IsItemHovered() && ImGui::IsMouseHoveringRect(badgeMin, badgeMax))
            ImGui::SetTooltip("%s", Translate(badgeTip));
    }

    std::string label = PathToUtf8(path.stem());
    const ImVec2 labelSize = ImGui::CalcTextSize(label.c_str());
    if (labelSize.x > kLabelWidth)
    {
        while (!label.empty() && ImGui::CalcTextSize((label + "...").c_str()).x > kLabelWidth)
            label.pop_back();
        label += "...";
    }
    const ImVec2 finalLabelSize = ImGui::CalcTextSize(label.c_str());
    drawList->AddText(ImVec2(itemMin.x + (kLabelWidth - finalLabelSize.x) * 0.5f,
                             previewMax.y + 3.f),
                      ImGui::GetColorU32(ImGuiCol_Text),
                      label.c_str());
    return clicked;
}

// Models whose clip tiles are shown in the asset browser (relative paths).
std::set<std::string>& ModelClipsExpanded()
{
    static std::set<std::string> expanded;
    return expanded;
}

ImVec2& ModelClipBadgeMin() { static ImVec2 value; return value; }
ImVec2& ModelClipBadgeMax() { static ImVec2 value; return value; }

// Clip count in the top right corner of the model tile drawn last; true when clicked.
bool DrawModelClipBadge(size_t count, bool expanded)
{
    constexpr float kThumbSize = 72.f;
    constexpr float kLabelWidth = 104.f;
    const ImVec2 itemMin = ImGui::GetItemRectMin();
    const bool tileHovered = ImGui::IsItemHovered();
    const std::string text = std::string(expanded ? "< " : "") + std::to_string(count) + " " + ICON_LC_VIDEO;
    const ImVec2 textSize = ImGui::CalcTextSize(text.c_str());
    const float right = itemMin.x + (kLabelWidth + kThumbSize) * 0.5f - 3.f;
    ModelClipBadgeMin() = ImVec2(right - textSize.x - 8.f, itemMin.y + 3.f);
    ModelClipBadgeMax() = ImVec2(right, itemMin.y + 3.f + textSize.y + 4.f);
    const bool badgeHovered = tileHovered && ImGui::IsMouseHoveringRect(ModelClipBadgeMin(), ModelClipBadgeMax());
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(ModelClipBadgeMin(), ModelClipBadgeMax(),
                            badgeHovered ? IM_COL32(150, 95, 210, 255) : IM_COL32(120, 75, 180, 235), 4.f);
    drawList->AddText(ImVec2(ModelClipBadgeMin().x + 4.f, ModelClipBadgeMin().y + 2.f), IM_COL32(255, 255, 255, 255), text.c_str());
    if (badgeHovered)
        ImGui::SetTooltip("%s", Translate(expanded ? "assets.hide_clips" : "assets.show_clips"));
    return badgeHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
}

void DrawTextureAssetTile(EditorFrameContext& ctx,
                          AssetBrowserState& browser,
                          const std::filesystem::path& path)
{
    constexpr float kThumbSize = 72.f;
    constexpr float kTileWidth = 104.f;

    ImGui::PushID(PathToUtf8(path).c_str());

    if (DrawRenameInputIfActive(ctx, browser, path))
    {
        ImGui::PopID();
        return;
    }

    ImGui::BeginGroup();

    const auto* thumb = ResolveTextureThumbnail(ctx, browser, path);

    // ImGui::Image() has no item ID. Calling BeginPopupContextItem() or
    // BeginDragDropSource() directly after Image() can assert with "id != 0".
    // Use a real invisible button as the interactive item and draw the preview
    // into its rectangle. This keeps every texture tile on a stable, non-zero ID.
    ImGui::InvisibleButton("##texture_preview", ImVec2(kThumbSize, kThumbSize));
    const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);

    const ImVec2 previewMin = ImGui::GetItemRectMin();
    const ImVec2 previewMax = ImGui::GetItemRectMax();
    ImDrawList* drawList = ImGui::GetWindowDrawList();

    // Hervorheben wenn dieser Tile vom Inspector angefragt wurde
    std::error_code ecEq;
    const bool isHighlighted = !browser.highlightPath.empty() &&
        std::filesystem::equivalent(path, browser.highlightPath, ecEq) && !ecEq;
    if (isHighlighted)
    {
        drawList->AddRectFilled(previewMin, previewMax, IM_COL32(200, 160, 20, 60));
        browser.highlightPath.clear(); // einmalig anzeigen, dann zurücksetzen
    }

    if (thumb && thumb->imguiId)
    {
        drawList->AddImage(
            reinterpret_cast<ImTextureID>(thumb->imguiId),
            previewMin,
            previewMax,
            ImVec2(0.f, 0.f),
            ImVec2(1.f, 1.f));
    }
    else
    {
        drawList->AddRect(previewMin, previewMax, ImGui::GetColorU32(ImGuiCol_Border));
        const char* fallbackText = "TEX";
        const ImVec2 textSize = ImGui::CalcTextSize(fallbackText);
        const ImVec2 textPos(
            previewMin.x + (kThumbSize - textSize.x) * 0.5f,
            previewMin.y + (kThumbSize - textSize.y) * 0.5f);
        drawList->AddText(textPos, ImGui::GetColorU32(ImGuiCol_TextDisabled), fallbackText);
    }

    // Goldener Rahmen wenn hervorgehoben
    if (isHighlighted)
        drawList->AddRect(previewMin, previewMax, IM_COL32(220, 180, 30, 255), 0.f, 0, 2.f);

    // Doppelklick: im Bildprogramm des Systems oeffnen (Zuweisen per Drag & Drop).
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        OpenWithDefaultApp(ctx, path);

    if (ImGui::BeginDragDropSource())
    {
        const std::string rel = ToRelativeAssetPath(ctx, path);
        ImGui::SetDragDropPayload("KROM_ASSET_TEXTURE", rel.c_str(), rel.size() + 1u);
        ImGui::TextUnformatted(PathToUtf8(path.filename()).c_str());
        ImGui::EndDragDropSource();
    }

    if (ImGui::BeginPopupContextItem())
    {
        DrawAssetCreateMenu(ctx, browser, browser.currentDir);
        ImGui::Separator();
        if (ImGui::MenuItem(TranslateLabel("assets.assign_texture", "Textur zuweisen").c_str()))
            AssignTexture(ctx, path);
        ImGui::Separator();
        if (ImGui::MenuItem(TranslateLabel("common.rename", "Umbenennen").c_str()))
            StartRename(browser, path);
        ImGui::Separator();
        ImGui::PushStyleColor(ImGuiCol_Text, theme::kDanger);
        if (ImGui::MenuItem(TranslateLabel("common.delete", "Loeschen").c_str()))
        {
            std::error_code ec;
            ForgetAssetThumbnail(ctx, path);
            std::filesystem::remove(path, ec);
        }
        ImGui::PopStyleColor();
        ImGui::EndPopup();
    }

    const std::string name = PathToUtf8(path.stem());
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + kTileWidth);
    ImGui::TextUnformatted(name.c_str());
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();

    ImGui::PopID();
}

// =============================================================================
// Inline-Rename Helpers
// =============================================================================

static void StartRename(AssetBrowserState& browser, const std::filesystem::path& path)
{
    browser.renamingPath = path;
    std::error_code ec;
    const std::string stem = std::filesystem::is_directory(path, ec)
        ? PathToUtf8(path.filename())
        : PathToUtf8(path.stem());
    std::memset(browser.renamingBuffer, 0, sizeof(browser.renamingBuffer));
    std::strncpy(browser.renamingBuffer, stem.c_str(), sizeof(browser.renamingBuffer) - 1);
    browser.renamingNeedsFocus = true;
}

static bool CreateFolderInAssetBrowser(AssetBrowserState& browser,
                                       const std::filesystem::path& parentDir)
{
    const std::filesystem::path newDir =
        MakeUniqueFilesystemPath(parentDir, "Neuer Ordner", "");

    std::error_code ec;
    std::filesystem::create_directory(newDir, ec);
    if (ec)
        return false;

    browser.currentDir = parentDir;
    browser.expandedDirs.insert(PathToUtf8(parentDir));
    StartRename(browser, newDir);
    return true;
}

static void DrawAssetCreateMenu(EditorFrameContext& ctx,
                                AssetBrowserState& browser,
                                const std::filesystem::path& directory)
{
    ImGui::TextDisabled(Translate("assets.create_in"), PathToUtf8(directory.filename()).c_str());
    ImGui::Separator();
    if (ImGui::MenuItem("Material"))
        CreateMaterialAsset(ctx, directory);
    if (ImGui::MenuItem(TranslateLabel("assets.new_controller", "Animation Controller").c_str()))
    {
        const std::string created = CreateAnimationControllerAsset(ctx, directory);
        if (!created.empty())
            StartRename(browser, ctx.assetPipeline->GetAssetRoot() / PathFromUtf8(created));
    }
    const bool canCreateScript = !ctx.currentProjectRoot.empty();
    if (!canCreateScript) ImGui::BeginDisabled();
    if (ImGui::MenuItem(TranslateLabel("assets.new_script", "C++ Script...").c_str()))
    {
        browser.createScriptDialogOpen = true;
        browser.createScriptDirectory = directory;
        std::memset(browser.createScriptNameBuffer, 0, sizeof(browser.createScriptNameBuffer));
        std::strncpy(browser.createScriptNameBuffer, "NewScript", sizeof(browser.createScriptNameBuffer) - 1);
    }
    if (!canCreateScript) ImGui::EndDisabled();
    if (ImGui::MenuItem(TranslateLabel("assets.new_scene", "Szene...").c_str()))
    {
        ctx.state.newSceneNameBuffer.fill('\0');
        ctx.state.newSceneTargetDir  = PathToUtf8(directory);
        ctx.state.newSceneDialogOpen = true;
    }
    ImGui::Separator();
    if (ImGui::MenuItem(TranslateLabel("assets.new_folder", "Neuer Ordner").c_str()))
        CreateFolderInAssetBrowser(browser, directory);
}

static void CommitRename(EditorFrameContext& ctx, AssetBrowserState& browser)
{
    if (browser.renamingPath.empty()) return;

    const std::string oldStem(PathToUtf8(browser.renamingPath.stem()));
    const std::string newStem(browser.renamingBuffer);

    if (!newStem.empty() && newStem != oldStem)
    {
        if (IsSceneFile(browser.renamingPath))
        {
            if (ctx.renameEditorScene)
                ctx.renameEditorScene(oldStem, newStem);
        }
        else
        {
            const std::filesystem::path newPath =
                browser.renamingPath.parent_path() /
                PathFromUtf8(newStem + PathToUtf8(browser.renamingPath.extension()));

            if (std::filesystem::exists(newPath))
            {
                ctx.lastFileMessage = TranslateWithValue("common.rename_exists", newStem);
            }
            else
            {
                std::error_code ec;
                std::filesystem::rename(browser.renamingPath, newPath, ec);
                if (!ec)
                    MoveAssetThumbnail(ctx, browser.renamingPath, newPath);
                if (ec)
                {
                    ctx.lastFileMessage = Translate("common.rename_failed_prefix") + ec.message();
                }
                else if (IsSupportedMaterial(browser.renamingPath))
                {
                    // Materialpfad in der Selektion aktualisieren
                    const std::string oldRel = ToRelativeAssetPath(ctx, browser.renamingPath);
                    const std::string newRel = ToRelativeAssetPath(ctx, newPath);
                    if (ctx.state.selectedMaterialAssetPath == oldRel)
                        ctx.state.selectedMaterialAssetPath = newRel;
                }
                else if (IsSupportedCppScriptSource(newPath))
                {
                    const EditorScriptAssetResult result = RefreshCppScriptProject(ctx);
                    ctx.lastFileMessage = result.message;
                }
            }
        }
    }

    browser.renamingPath.clear();
    browser.renamingNeedsFocus = false;
}

// Rendert das InputText-Feld wenn dieser Pfad gerade umbenannt wird.
// Gibt true zurück wenn das Feld gerendert wurde (→ normales Selectable überspringen).
static bool DrawRenameInputIfActive(EditorFrameContext& ctx, AssetBrowserState& browser,
                                    const std::filesystem::path& path)
{
    if (browser.renamingPath != path)
        return false;

    if (browser.renamingNeedsFocus)
    {
        ImGui::SetKeyboardFocusHere();
        browser.renamingNeedsFocus = false;
    }
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 4.f);
    const bool enter = ImGui::InputText(
        "##renameinline", browser.renamingBuffer, sizeof(browser.renamingBuffer),
        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);

    if (enter)
        CommitRename(ctx, browser);
    else if (ImGui::IsItemDeactivated()) // Escape oder Klick woanders → Abbrechen
        browser.renamingPath.clear();

    return true;
}

// Liest Breite/Hoehe aus dem IHDR-Chunk einer PNG-Datei; 1.0 bei Fehler
static float ReadPngAspect(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    unsigned char header[24] = {};
    if (!file.read(reinterpret_cast<char*>(header), sizeof(header)))
        return 1.f;
    auto readBE32 = [&](int offset)
    {
        return (uint32_t(header[offset]) << 24) | (uint32_t(header[offset + 1]) << 16) |
               (uint32_t(header[offset + 2]) << 8) | uint32_t(header[offset + 3]);
    };
    const uint32_t width = readBE32(16);
    const uint32_t height = readBE32(20);
    return (width > 0u && height > 0u) ? float(width) / float(height) : 1.f;
}

void LoadEditorIconsImpl(EditorFrameContext& ctx, AssetBrowserState& browser)
{
    if (browser.icons.loaded || ctx.engineEditorDir.empty() || !ctx.assetPipeline)
        return;
    browser.icons.loaded = true;

    const std::filesystem::path editorDir = PathFromUtf8(ctx.engineEditorDir);
    auto loadIcon = [&](AssetBrowserState::Icon& icon, std::initializer_list<const char*> names)
    {
        for (const char* name : names)
        {
            const std::filesystem::path iconPath = editorDir / name;
            if (!std::filesystem::exists(iconPath))
                continue;

            icon.texture = LoadRuntimeTexture(ctx, iconPath);
            if (icon.texture.IsValid() && ctx.editorTextureId)
                icon.imguiId = ctx.editorTextureId(icon.texture);
            icon.aspect = ReadPngAspect(iconPath);
            return;
        }
    };

    loadIcon(browser.icons.folder,         {"folder.png", "file.png"});
    loadIcon(browser.icons.scene,          {"scene.png"});
    loadIcon(browser.icons.cppSource,      {"cpp.png"});
    loadIcon(browser.icons.cppHeader,      {"hpp.png"});
    loadIcon(browser.icons.vertexShader,   {"vertexshader.png"});
    loadIcon(browser.icons.pixelShader,    {"pixelshader.png"});
    loadIcon(browser.icons.fragmentShader, {"fragmentshader.png"});
    loadIcon(browser.icons.sound,          {"sound.png"});
    loadIcon(browser.icons.reflectionProbe, {"probe.png"});
    loadIcon(browser.icons.entity,         {"entity.png"});
    loadIcon(browser.icons.mesh,           {"mesh.png"});
    loadIcon(browser.icons.lightDirectional, {"light_directional.png", "light.png"});
    loadIcon(browser.icons.lightPoint,       {"light_point.png", "light.png"});
    loadIcon(browser.icons.lightSpot,        {"light_spot.png", "light.png"});
    loadIcon(browser.icons.camera,           {"camera.png"});
    loadIcon(browser.icons.particleEmitter,  {"particle_emitter.png"});
    loadIcon(browser.icons.prefab,           {"prefab.png"});
    loadIcon(browser.icons.audioSource,      {"audio_source.png"});
}

// Zeichnet ein Icon-Bild zentriert und mit Seitenverhaeltnis in ein Quadrat
static void DrawIconImage(const AssetBrowserState::Icon& icon, const ImVec2& min, float size)
{
    ImVec2 imageSize(size, size);
    if (icon.aspect > 1.f)
        imageSize.y = size / icon.aspect;
    else
        imageSize.x = size * icon.aspect;
    const ImVec2 imageMin(min.x + (size - imageSize.x) * 0.5f,
                          min.y + (size - imageSize.y) * 0.5f);
    ImGui::GetWindowDrawList()->AddImage(reinterpret_cast<ImTextureID>(icon.imguiId),
                                         imageMin,
                                         ImVec2(imageMin.x + imageSize.x, imageMin.y + imageSize.y));
}

// Zeichnet einen Ordner-Eintrag im Baum (rekursiv für Unterordner)
static void DrawFolderGlyph(AssetBrowserState& browser, const ImVec2& min, float size)
{
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImVec2 max(min.x + size, min.y + size);

    if (browser.icons.folder.imguiId)
    {
        DrawIconImage(browser.icons.folder, min, size);
        return;
    }

    const float tabH = std::max(3.f, size * 0.28f);
    const float bodyY = min.y + size * 0.32f;
    const ImU32 tabCol = IM_COL32(238, 198, 98, 255);
    const ImU32 bodyCol = IM_COL32(223, 174, 67, 255);
    const ImU32 edgeCol = IM_COL32(128, 96, 42, 255);

    drawList->AddRectFilled(ImVec2(min.x + size * 0.08f, min.y + size * 0.16f),
                            ImVec2(min.x + size * 0.48f, min.y + size * 0.16f + tabH),
                            tabCol, 2.f);
    drawList->AddRectFilled(ImVec2(min.x + size * 0.04f, bodyY),
                            ImVec2(max.x - size * 0.04f, max.y - size * 0.10f),
                            bodyCol, 2.f);
    drawList->AddRect(ImVec2(min.x + size * 0.04f, bodyY),
                      ImVec2(max.x - size * 0.04f, max.y - size * 0.10f),
                      edgeCol, 2.f);
}

// Kachel-Zeilenumbruch im Inhaltsbereich: SameLine solange die Zeile Platz hat
static void BeginTileInRow(float& rowUsed, float rowAvail)
{
    constexpr float kTileStride = 112.f;
    if (rowUsed > 0.f && rowUsed + kTileStride <= rowAvail)
        ImGui::SameLine();
    else
        rowUsed = 0.f;
    rowUsed += kTileStride;
}

// Symbol-Kachel im Inhaltsbereich, gleiche Maße wie DrawAssetPreviewTile.
// Ohne Icon-Bild wird drawFallback (falls gesetzt) oder fallbackText gezeichnet.
static bool DrawIconTile(const AssetBrowserState::Icon& icon,
                         std::string label,
                         bool selected,
                         const char* fallbackText,
                         const std::function<void(const ImVec2&, float)>& drawFallback = {}, bool* activated = nullptr)
{
    constexpr float kThumbSize = 72.f;
    constexpr float kLabelWidth = 104.f;
    constexpr float kTileHeight = 100.f;
    constexpr float kInset = 6.f;

    const bool pressed = ImGui::InvisibleButton("##icon_tile", ImVec2(kLabelWidth, kTileHeight));
    if (activated) *activated = pressed;
    const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 itemMin = ImGui::GetItemRectMin();
    const ImVec2 previewMin{itemMin.x + (kLabelWidth - kThumbSize) * 0.5f, itemMin.y};
    const ImVec2 previewMax{previewMin.x + kThumbSize, previewMin.y + kThumbSize};
    ImDrawList* drawList = ImGui::GetWindowDrawList();

    if (selected)
        drawList->AddRectFilled(previewMin, previewMax, IM_COL32(120, 165, 210, 70), 3.f);
    else if (hovered)
        drawList->AddRectFilled(previewMin, previewMax, IM_COL32(120, 165, 210, 40), 3.f);

    const ImVec2 iconMin(previewMin.x + kInset, previewMin.y + kInset);
    const float iconSize = kThumbSize - 2.f * kInset;
    if (icon.imguiId)
    {
        DrawIconImage(icon, iconMin, iconSize);
    }
    else if (drawFallback)
    {
        drawFallback(iconMin, iconSize);
    }
    else
    {
        drawList->AddRectFilled(previewMin, previewMax, IM_COL32(48, 52, 56, 255));
        drawList->AddRect(previewMin, previewMax, ImGui::GetColorU32(ImGuiCol_Border));
        const ImVec2 textSize = ImGui::CalcTextSize(fallbackText);
        drawList->AddText(ImVec2(previewMin.x + (kThumbSize - textSize.x) * 0.5f,
                                 previewMin.y + (kThumbSize - textSize.y) * 0.5f),
                          ImGui::GetColorU32(ImGuiCol_TextDisabled),
                          fallbackText);
    }

    if (ImGui::CalcTextSize(label.c_str()).x > kLabelWidth)
    {
        while (!label.empty() && ImGui::CalcTextSize((label + "...").c_str()).x > kLabelWidth)
            label.pop_back();
        label += "...";
    }
    const ImVec2 labelSize = ImGui::CalcTextSize(label.c_str());
    drawList->AddText(ImVec2(itemMin.x + (kLabelWidth - labelSize.x) * 0.5f,
                             previewMax.y + 3.f),
                      ImGui::GetColorU32(ImGuiCol_Text),
                      label.c_str());
    if (selected)
        drawList->AddRect(previewMin, previewMax, ImGui::GetColorU32(ImGuiCol_HeaderActive), 3.f, 0, 2.f);
    return clicked;
}

static void DrawFolderTreeNode(EditorFrameContext& ctx,
                               AssetBrowserState& browser,
                               const std::filesystem::path& dir,
                               int depth)
{
    if (depth > 16) return;

    std::error_code ec;
    // Nur zeichnen wenn Verzeichnis existiert
    if (!std::filesystem::exists(dir, ec) || !std::filesystem::is_directory(dir, ec))
        return;

    // Unterordner sammeln
    std::vector<std::filesystem::path> subdirs;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec))
        if (entry.is_directory(ec))
            subdirs.push_back(entry.path());
    std::sort(subdirs.begin(), subdirs.end());

    const bool hasChildren   = !subdirs.empty();
    const bool isSelected    = (browser.currentDir == dir);
    const std::string key    = PathToUtf8(dir);
    const bool wasExpanded   = browser.expandedDirs.count(key) > 0;

    ImGuiTreeNodeFlags flags =
        ImGuiTreeNodeFlags_SpanAvailWidth |
        ImGuiTreeNodeFlags_OpenOnArrow;
    if (!hasChildren)
        flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (isSelected)
        flags |= ImGuiTreeNodeFlags_Selected;
    if (wasExpanded)
        flags |= ImGuiTreeNodeFlags_DefaultOpen;

    // Folder-Icon vor dem Label
    constexpr float kIconSize = 14.f;
    if (browser.icons.folder.imguiId)
    {
        ImGui::Image(reinterpret_cast<ImTextureID>(browser.icons.folder.imguiId),
                     ImVec2(kIconSize, kIconSize));
        ImGui::SameLine();
    }

    if (!browser.highlightPath.empty() && wasExpanded)
        ImGui::SetNextItemOpen(true);
    const bool open = ImGui::TreeNodeEx(key.c_str(), flags, "%s",
                                        PathToUtf8(dir.filename()).c_str());

    // Klick auf den Node → Ordner selektieren
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen())
        browser.currentDir = dir;

    // Drop-Target: Datei in diesen Ordner verschieben
    if (ImGui::BeginDragDropTarget())
    {
        AcceptAssetMovePayload(ctx, browser, dir);
        ImGui::EndDragDropTarget();
    }

    // Expand-State tracken
    if (open && hasChildren)
        browser.expandedDirs.insert(key);
    else if (!open && hasChildren)
        browser.expandedDirs.erase(key);

    // Rechtsklick → Ordner-Aktionen
    if (ImGui::BeginPopupContextItem(("##ftree_ctx_" + key).c_str()))
    {
        browser.currentDir = dir;

        if (ImGui::MenuItem(TranslateLabel("assets.create_folder", "Neuen Ordner erstellen").c_str()))
        {
            CreateFolderInAssetBrowser(browser, dir);
        }

        if (ImGui::MenuItem(TranslateLabel("common.rename", "Umbenennen").c_str()))
            StartRename(browser, dir);

        ImGui::Separator();

        // Ordner löschen (leer = sofort, nicht leer = mit Bestätigung)
        std::error_code ecEmpty;
        const bool isEmpty = std::filesystem::is_empty(dir, ecEmpty) && !ecEmpty;
        if (ImGui::MenuItem(isEmpty ? TranslateLabel("assets.delete_folder", "Ordner loeschen").c_str() : TranslateLabel("assets.delete_folder_contents", "Ordner loeschen (mit Inhalt)...").c_str()))
        {
            if (isEmpty)
            {
                std::error_code ecRm;
                std::filesystem::remove(dir, ecRm);
                if (!ecRm)
                {
                    browser.currentDir = dir.parent_path();
                    browser.expandedDirs.erase(key);
                }
            }
            else
            {
                // Bestätigungs-Dialog öffnen
                browser.deleteDirPending = dir;
            }
        }
        if (ImGui::IsItemHovered() && !isEmpty)
            ImGui::SetTooltip(Translate("assets.delete_folder_tip"));

        ImGui::EndPopup();
    }

    // Rename-Input wenn aktiv
    if (browser.renamingPath == dir)
        DrawRenameInputIfActive(ctx, browser, dir);

    if (open && hasChildren)
    {
        for (const auto& sub : subdirs)
            DrawFolderTreeNode(ctx, browser, sub, depth + 1);
        ImGui::TreePop();
    }
}

} // namespace

const AssetBrowserState::TextureThumbnail* GetAssetTextureThumbnail(EditorFrameContext& ctx,
                                                                   const std::string& assetPath)
{
    if (!ctx.assetPipeline || !ctx.state.assetBrowser || assetPath.empty())
        return nullptr;
    const auto path = (ctx.assetPipeline->GetAssetRoot() / PathFromUtf8(assetPath)).lexically_normal();
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec))
        return nullptr;
    return ResolveTextureThumbnail(ctx, *ctx.state.assetBrowser, path);
}

void LoadEditorIcons(EditorFrameContext& ctx, AssetBrowserState& browser)
{
    LoadEditorIconsImpl(ctx, browser);
}

void DrawEditorConsole(EditorFrameContext& ctx)
{
    const auto assetsLabel = TranslateLabel("panel.assets", "Assets##editor");
    const auto* settings = ImGui::FindWindowSettingsByID(ImHashStr(assetsLabel.c_str()));
    // Existing layouts have an Assets dock, but no separate Console yet.
    if (settings && settings->DockId && ImGui::DockBuilderGetNode(settings->DockId))
        ImGui::SetNextWindowDockID(settings->DockId, ImGuiCond_FirstUseEver);

    SolidEditorPanelStyleScope solidPanelStyle;
    const bool selectTab = ctx.state.consoleFocusRequest;
    ctx.state.consoleFocusRequest = false;
    if (selectTab && !ctx.playActive)
        ImGui::SetNextWindowFocus();
    const std::string consoleTitle = TranslateLabel("assets.console", "Console##editor");
    detail::KeepNextWindowInMainViewport(consoleTitle.c_str());
    const bool visible = ImGui::Begin(consoleTitle.c_str(), &ctx.consolePanelVisible,
        kAlignedPanelFlags | ImGuiWindowFlags_NoFocusOnAppearing);
    if (selectTab && ctx.playActive)
    {
        // Select the bottom tab without taking keyboard focus from the game.
        const auto* window = ImGui::GetCurrentWindowRead();
        if (window->DockNode && window->DockNode->TabBar)
            window->DockNode->TabBar->NextSelectedTabId = window->TabId;
    }
    if (!ctx.assetsPanelVisible)
        ctx.assetsPanelHeight = ImGui::GetWindowHeight();
    if (!visible)
    {
        ImGui::End();
        return;
    }

    // Filter-Buttons
    static bool showInfo    = true;
    static bool showWarning = true;
    static bool showError   = true;
    const auto isShown = [&](engine::LogLevel level) {
        if (level == engine::LogLevel::Info) return showInfo;
        if (level == engine::LogLevel::Warning) return showWarning;
        if (level >= engine::LogLevel::Error) return showError;
        return true;
    };
    const auto clearLabel = TranslateLabel("console.clear", "Leeren");
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.f, 2.f));
    const auto& style = ImGui::GetStyle();
    const float rowWidth = ImGui::GetContentRegionAvail().x;
    const float rowLeft = ImGui::GetCursorPosX();
    const float clearWidth = ImGui::CalcTextSize(clearLabel.c_str(), nullptr, true).x + 2.f * style.FramePadding.x;

    ImGui::Checkbox("Info", &showInfo);
    ImGui::SameLine();
    ImGui::Checkbox(TranslateLabel("console.warning", "Warning").c_str(), &showWarning);
    ImGui::SameLine();
    ImGui::Checkbox(TranslateLabel("console.error", "Error").c_str(), &showError);
    ImGui::SameLine();
    ImGui::Checkbox("Auto-Scroll", &ctx.state.consoleAutoScroll);

    if (clearWidth <= rowWidth)
    {
        const float actionX = rowLeft + rowWidth - clearWidth;
        const float filterEnd = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x + ImGui::GetScrollX();
        if (actionX >= filterEnd + style.ItemSpacing.x)
            ImGui::SameLine();
        ImGui::SetCursorPosX(actionX);
    }
    if (ImGui::Button(clearLabel.c_str()))
        ctx.state.consoleLogs.clear();
    ImGui::PopStyleVar();
    ImGui::Separator();

    constexpr const char* textLabel = "##ConsoleText";
    const ImGuiID textId = ImGui::GetID(textLabel);
    auto* textState = ImGui::GetInputTextState(textId);
    const bool wasActive = ImGui::GetActiveID() == textId;
    const bool openSource = ImGui::GetIO().KeyCtrl && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    static std::string text;
    static std::string previousText;
    text.swap(previousText);
    text.clear();
    for (const auto& entry : ctx.state.consoleLogs)
    {
        if (!isShown(entry.level))
            continue;
        text += entry.message;
        text += '\n';
    }
    // Appending logs preserves byte offsets. Filtering, clearing or dropping
    // old entries can move them, so discard a selection in that case.
    if (textState && !text.starts_with(previousText))
        textState->SetSelection(0, 0);

    ImGuiWindow* consoleWindow = ImGui::GetCurrentWindowRead();
    ImGui::InputTextMultiline(textLabel, text.data(), text.size() + 1, ImVec2(-1.f, -1.f),
                              ImGuiInputTextFlags_ReadOnly | ImGuiInputTextFlags_WordWrap);
    textState = ImGui::GetInputTextState(textId);

    if (ctx.state.consoleAutoScroll && ctx.state.consoleScrollToBottom &&
        (!textState || !ImGui::IsItemActive() || !textState->HasSelection()) &&
        !ImGui::IsMouseDragging(ImGuiMouseButton_Left) &&
        !consoleWindow->DC.ChildWindows.empty())
    {
        auto* textWindow = consoleWindow->DC.ChildWindows.back();
        if (textWindow->ChildId == textId)
        {
            ImGui::SetScrollY(textWindow, textWindow->DC.CursorMaxPos.y - textWindow->DC.CursorStartPos.y);
            ctx.state.consoleScrollToBottom = false;
        }
    }

    static std::string selectedText;
    if (ImGui::IsItemHovered() && textState)
    {
        if (openSource && ImGui::IsItemActive())
        {
            const auto caret = static_cast<size_t>(textState->GetCursorPos());
            size_t start = 0;
            for (const auto& entry : ctx.state.consoleLogs)
            {
                if (!isShown(entry.level))
                    continue;
                const size_t end = start + entry.message.size();
                if (caret <= end)
                {
                    std::filesystem::path path;
                    int line = 0;
                    if (TryParseBuildLocation(ctx, entry.message, path, line))
                        OpenAssetInExternalEditor(ctx, path, line);
                    break;
                }
                start = end + 1;
            }
        }
        if (wasActive && textState->HasSelection() && ImGui::IsMouseReleased(ImGuiMouseButton_Right))
        {
            const int start = textState->GetSelectionStart();
            const int end = textState->GetSelectionEnd();
            selectedText = text.substr(std::min(start, end), std::abs(end - start));
            ImGui::OpenPopup("##ConsoleTextActions");
        }
    }
    if (ImGui::BeginPopup("##ConsoleTextActions"))
    {
        if (ImGui::MenuItem(TranslateLabel("console.copy_selection", "CopyConsoleSelection").c_str(), "Ctrl+C"))
            ImGui::SetClipboardText(selectedText.c_str());
        ImGui::EndPopup();
    }
    ImGui::End();
}

void DrawAssetBrowser(EditorFrameContext& ctx, AssetBrowserState& browser)
{
    DrawViewportAssetDropTarget(ctx);

    if (!browser.initialized)
    {
        browser.currentDir = ctx.assetPipeline
            ? ctx.assetPipeline->GetAssetRoot()
            : (std::filesystem::current_path() / "assets");
        if (!std::filesystem::exists(browser.currentDir))
            browser.currentDir = std::filesystem::current_path();
        browser.initialized = true;
        // Root-Ordner standardmäßig aufklappen
        browser.expandedDirs.insert(PathToUtf8(browser.currentDir));
    }

    LoadEditorIcons(ctx, browser);

    // Vom Inspector angefordert: Verzeichnis oeffnen und Datei anzeigen.
    if (!browser.highlightPath.empty())
    {
        std::filesystem::path highlight = browser.highlightPath;
        if (highlight.is_relative() && ctx.assetPipeline)
            highlight = ctx.assetPipeline->GetAssetRoot() / highlight;

        const std::filesystem::path dir = std::filesystem::is_directory(highlight)
            ? highlight
            : highlight.parent_path();
        if (std::filesystem::exists(dir))
        {
            browser.currentDir = dir;
            // Alle übergeordneten Ordner aufklappen
            for (auto p = dir; p != p.root_path(); p = p.parent_path())
                browser.expandedDirs.insert(PathToUtf8(p));
        }

        std::error_code ec;
        if (!std::filesystem::is_regular_file(highlight, ec))
            browser.highlightPath.clear();
        else
        {
            browser.highlightPath = highlight;
            if (IsSupportedCppScriptSource(highlight))
            {
                browser.selectedScriptPath = highlight;
                browser.selectedFolder.clear();
            }
        }
    }

    // Die Anfrage gilt bis zum ersten sichtbaren Assets-Frame. Ein Tab-Wechsel
    // kann einen Frame brauchen. Bisher loeschte nur eine Textur-Kachel den Pfad - bei
    // Scripts oder Materialien sprang currentDir dauerhaft zurück. Eine während
    // dieses Frames neu gesetzte Anfrage (z. B. „C++ Script erstellen“) bleibt.
    struct ConsumeHighlightAtFrameEnd
    {
        AssetBrowserState& browser;
        std::filesystem::path consumed;
        ~ConsumeHighlightAtFrameEnd()
        {
            if (!consumed.empty() && browser.highlightPath == consumed)
                browser.highlightPath.clear();
        }
    } consumeHighlight{ browser, {} };

    SolidEditorPanelStyleScope solidPanelStyle;
    if (!browser.highlightPath.empty())
        ImGui::SetNextWindowFocus();
    const std::string assetsTitle = TranslateLabel("panel.assets", "Assets##editor");
    detail::KeepNextWindowInMainViewport(assetsTitle.c_str());
    const bool visible = ImGui::Begin(assetsTitle.c_str(), &ctx.assetsPanelVisible, kAlignedPanelFlags);
    ctx.assetsPanelHeight = ImGui::GetWindowHeight();
    if (!visible)
    {
        ImGui::End();
        return;
    }
    consumeHighlight.consumed = browser.highlightPath;

    // ── Breadcrumb-Navigation oben ────────────────────────────────────────────
    {
        const std::filesystem::path assetRoot = ctx.assetPipeline
            ? ctx.assetPipeline->GetAssetRoot()
            : browser.currentDir;

        // Pfad-Segmente von assetRoot bis currentDir
        std::vector<std::filesystem::path> crumbs;
        for (auto p = browser.currentDir; ; p = p.parent_path())
        {
            crumbs.push_back(p);
            std::error_code ec;
            if (std::filesystem::equivalent(p, assetRoot, ec) || ec || p == p.root_path())
                break;
        }
        std::reverse(crumbs.begin(), crumbs.end());

        for (size_t i = 0; i < crumbs.size(); ++i)
        {
            if (i > 0) { ImGui::SameLine(); ImGui::TextDisabled("/"); ImGui::SameLine(); }
            const std::string seg = PathToUtf8(crumbs[i].filename()).empty()
                ? PathToUtf8(crumbs[i]) : PathToUtf8(crumbs[i].filename());
            if (i + 1 == crumbs.size())
                ImGui::TextUnformatted(seg.c_str()); // aktuell — nicht klickbar
            else if (ImGui::SmallButton(seg.c_str()))
                browser.currentDir = crumbs[i];
        }
        ImGui::SameLine();
        ImGui::TextDisabled("  -> %s", ctx.state.activeTextureParamName.c_str());
    }
    ImGui::Separator();

    // ── Zwei-Spalten-Layout: Baum links, Inhalt rechts ───────────────────────
    const float treeWidth = 160.f;
    const float spacing   = ImGui::GetStyle().ItemSpacing.x;

    // Linke Spalte: Ordner-Baum
    ImGui::BeginChild("##tree_panel", ImVec2(treeWidth, 0.f),
                      ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
    {
        const std::filesystem::path assetRoot = ctx.assetPipeline
            ? ctx.assetPipeline->GetAssetRoot()
            : browser.currentDir;
        DrawFolderTreeNode(ctx, browser, assetRoot, 0);
    }
    ImGui::EndChild();

    ImGui::SameLine(0.f, spacing);

    // Rechte Spalte: Dateiinhalt
    ImGui::BeginChild("##content_panel", ImVec2(0.f, 0.f), ImGuiChildFlags_None);

    if (ImGui::BeginPopupContextWindow("AssetBrowserContext##editor", ImGuiPopupFlags_MouseButtonRight))
    {
        DrawAssetCreateMenu(ctx, browser, browser.currentDir);
        ImGui::EndPopup();
    }

    // Neue-Szene-Dialog (modal)
    if (ctx.state.newSceneDialogOpen)
    {
        ImGui::OpenPopup(TranslateLabel("assets.scene_title", "Neue Szene##newscenedlg").c_str());
        ctx.state.newSceneDialogOpen = false;
    }
    if (ImGui::BeginPopupModal(TranslateLabel("assets.scene_title", "Neue Szene##newscenedlg").c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextUnformatted(Translate("assets.scene_name"));
        ImGui::SetNextItemWidth(260.f);
        const bool confirmed = ImGui::InputText(
            "##newscenename", ctx.state.newSceneNameBuffer.data(),
            ctx.state.newSceneNameBuffer.size(),
            ImGuiInputTextFlags_EnterReturnsTrue);
        const bool hasName = ctx.state.newSceneNameBuffer[0] != '\0';

        ImGui::Spacing();
        if (!hasName) ImGui::BeginDisabled();
        if ((ImGui::Button(TranslateLabel("common.create", "Erstellen").c_str(), ImVec2(120.f, 0.f)) || confirmed) && hasName)
        {
            const std::string name = ctx.state.newSceneNameBuffer.data();
            const std::string dir  = ctx.state.newSceneTargetDir;
            if (!dir.empty() && ctx.newEditorSceneInDir)
                ctx.newEditorSceneInDir(name, dir); // only writes the file, the scene stays
            else if (ctx.newEditorScene)
                RunAfterSceneSaveCheck(ctx, name, [&ctx, name] { ctx.newEditorScene(name); });
            ImGui::CloseCurrentPopup();
        }
        if (!hasName) ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button(TranslateLabel("common.cancel", "Abbrechen").c_str(), ImVec2(120.f, 0.f)) ||
            ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // ── Bestätigungs-Dialog: nicht-leeren Ordner löschen ─────────────────────
    if (browser.createScriptDialogOpen)
    {
        ImGui::OpenPopup(TranslateLabel("assets.script_title", "C++ Script erstellen##createscriptdlg").c_str());
        browser.createScriptDialogOpen = false;
    }
    if (ImGui::BeginPopupModal(TranslateLabel("assets.script_title", "C++ Script erstellen##createscriptdlg").c_str(),
                               nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextUnformatted(Translate("assets.class_name"));
        ImGui::SetNextItemWidth(280.f);
        const bool confirmed = ImGui::InputText(
            "##newscriptname",
            browser.createScriptNameBuffer,
            sizeof(browser.createScriptNameBuffer),
            ImGuiInputTextFlags_EnterReturnsTrue);

        const std::string className = browser.createScriptNameBuffer;
        const bool validName = IsValidCppScriptClassName(className);
        if (!validName)
            ImGui::TextDisabled(Translate("assets.cpp_identifier"));

        ImGui::TextDisabled(Translate("assets.destination"),
            browser.createScriptDirectory.empty()
                ? "(aktueller Ordner)"
                : PathToUtf8(browser.createScriptDirectory).c_str());

        ImGui::Spacing();
        if (!validName) ImGui::BeginDisabled();
        if ((ImGui::Button(TranslateLabel("common.create", "Erstellen").c_str(), ImVec2(120.f, 0.f)) || confirmed) && validName)
        {
            const EditorScriptAssetResult result =
                CreateCppScriptAsset(ctx, browser.createScriptDirectory, className);
            ctx.lastFileMessage = result.message;
            if (result.ok)
            {
                browser.highlightPath = result.primaryPath;
                ImGui::CloseCurrentPopup();
            }
        }
        if (!validName) ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button(TranslateLabel("common.cancel", "Abbrechen").c_str(), ImVec2(120.f, 0.f)) ||
            ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            ImGui::CloseCurrentPopup();

        ImGui::EndPopup();
    }

    if (!browser.deleteDirPending.empty())
    {
        ImGui::OpenPopup(TranslateLabel("assets.delete_folder_title", "OrdnerLoeschenBestaetigung##delbrowser").c_str());
    }
    if (ImGui::BeginPopupModal(TranslateLabel("assets.delete_folder_title", "OrdnerLoeschenBestaetigung##delbrowser").c_str(),
                               nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextUnformatted(Translate("assets.delete_folder_prompt"));
        ImGui::TextColored(theme::kWarning, "%s",
                           PathToUtf8(browser.deleteDirPending.filename()).c_str());
        ImGui::TextDisabled(Translate("assets.delete_permanent"));
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Button,        theme::kDanger);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::Hex(0xE2707A));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  theme::Hex(0xB8424D));
        if (ImGui::Button(TranslateLabel("common.delete", "Loeschen").c_str(), ImVec2(120.f, 0.f)))
        {
            const std::filesystem::path dir = browser.deleteDirPending;
            browser.deleteDirPending.clear();
            std::error_code ecRm;
            std::filesystem::remove_all(dir, ecRm);
            if (!ecRm)
            {
                browser.currentDir = dir.parent_path();
                browser.expandedDirs.erase(PathToUtf8(dir));
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleColor(3);
        ImGui::SameLine();
        if (ImGui::Button(TranslateLabel("common.cancel", "Abbrechen").c_str(), ImVec2(120.f, 0.f)) ||
            ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        {
            browser.deleteDirPending.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // ── Alle Dateien im aktuellen Ordner ──────────────────────────────────────
    std::error_code ec;

    const float tileRowAvail = ImGui::GetContentRegionAvail().x;
    float folderTileRowUsed = 0.f;
    for (const auto& entry : CurrentDirEntries(browser))
    {
        if (!entry.is_directory(ec))
            continue;

        const std::filesystem::path p = entry.path();
        ImGui::PushID(PathToUtf8(p).c_str());

        if (!DrawRenameInputIfActive(ctx, browser, p))
        {
            BeginTileInRow(folderTileRowUsed, tileRowAvail);
            bool openFolder = false;
            DrawIconTile(
                browser.icons.folder, PathToUtf8(p.filename()), browser.selectedFolder == p, "DIR",
                [&browser](const ImVec2& min, float size) { DrawFolderGlyph(browser, min, size); }, &openFolder);
            const ImVec2 dragDelta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
            if (openFolder && dragDelta.x == 0.f && dragDelta.y == 0.f)
            {
                const double now = ImGui::GetTime();
                const bool doubleClick = browser.selectedFolder == p && browser.lastFolderClickTime >= 0.0 &&
                    now - browser.lastFolderClickTime <= ImGui::GetIO().MouseDoubleClickTime;
                browser.selectedFolder = p;
                browser.lastFolderClickTime = now;
                if (doubleClick)
                {
                    browser.currentDir = p;
                    browser.expandedDirs.insert(PathToUtf8(p.parent_path()));
                    browser.selectedFolder.clear();
                    browser.lastFolderClickTime = -1.0;
                }
            }

            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID))
            {
                const std::string absPath = PathToUtf8(p);
                ImGui::SetDragDropPayload("KROM_MOVE_DIR", absPath.c_str(), absPath.size() + 1u);
                ImGui::TextUnformatted(PathToUtf8(p.filename()).c_str());
                ImGui::EndDragDropSource();
            }

            if (ImGui::BeginDragDropTarget())
            {
                AcceptAssetMovePayload(ctx, browser, p);
                ImGui::EndDragDropTarget();
            }

            if (ImGui::BeginPopupContextItem())
            {
                DrawAssetCreateMenu(ctx, browser, browser.currentDir);
                ImGui::Separator();
                if (ImGui::MenuItem(TranslateLabel("common.open", "Oeffnen").c_str()))
                {
                    browser.currentDir = p;
                    browser.expandedDirs.insert(PathToUtf8(p.parent_path()));
                }
                if (ImGui::MenuItem(TranslateLabel("common.rename", "Umbenennen").c_str()))
                    StartRename(browser, p);
                ImGui::Separator();
                std::error_code ecEmpty;
                const bool isEmpty = std::filesystem::is_empty(p, ecEmpty) && !ecEmpty;
                ImGui::PushStyleColor(ImGuiCol_Text, theme::kDanger);
                if (ImGui::MenuItem(TranslateLabel(isEmpty ? "common.delete" : "assets.delete_contents",
                                                  isEmpty ? "Loeschen" : "Loeschen (mit Inhalt)...").c_str()))
                {
                    if (isEmpty)
                    {
                        std::error_code ecRm;
                        ForgetAssetThumbnail(ctx, p);
                        std::filesystem::remove(p, ecRm);
                    }
                    else
                    {
                        browser.deleteDirPending = p;
                    }
                }
                ImGui::PopStyleColor();
                ImGui::EndPopup();
            }
        }

        ImGui::PopID();
    }

    // Szenen (.json)
    float sceneTileRowUsed = 0.f;
    for (const auto& entry : CurrentDirEntries(browser))
    {
        const auto& p = entry.path();
        if (entry.is_directory(ec) || !IsSceneFile(p))
            continue;

        const std::string sceneName = PathToUtf8(p.stem());
        const bool isCurrent = (sceneName == ctx.currentEditorSceneName);
        const bool isLoaded  = !isCurrent &&
            std::find(ctx.loadedEditorScenes.begin(),
                      ctx.loadedEditorScenes.end(),
                      sceneName) != ctx.loadedEditorScenes.end();

        if (isCurrent)   ImGui::PushStyleColor(ImGuiCol_Text, theme::kSuccess);
        else if (isLoaded) ImGui::PushStyleColor(ImGuiCol_Text, theme::Hex(0x3BBF7A, 0.7f));

        ImGui::PushID(PathToUtf8(p).c_str());
        if (!DrawRenameInputIfActive(ctx, browser, p))
        {
            std::string label = sceneName;
            if (isCurrent)     label += " *";
            else if (isLoaded) label += " +";

            BeginTileInRow(sceneTileRowUsed, tileRowAvail);
            if (DrawIconTile(browser.icons.scene, label, isCurrent || isLoaded, "SCENE") &&
                ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !isCurrent &&
                ctx.switchEditorScene)
                RunAfterSceneSaveCheck(ctx, sceneName, [&ctx, sceneName] { ctx.switchEditorScene(sceneName, /*additive=*/false); });

            // Drag-Source: Datei verschieben
            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID))
            {
                const std::string absPath = PathToUtf8(p);
                ImGui::SetDragDropPayload("KROM_MOVE_FILE", absPath.c_str(), absPath.size() + 1u);
                ImGui::TextUnformatted(PathToUtf8(p.filename()).c_str());
                ImGui::EndDragDropSource();
            }

            if (ImGui::BeginPopupContextItem())
            {
                DrawAssetCreateMenu(ctx, browser, browser.currentDir);
                ImGui::Separator();
                if (ImGui::MenuItem(TranslateLabel("common.load", "Laden").c_str(), nullptr, false, !isCurrent) && ctx.switchEditorScene)
                    RunAfterSceneSaveCheck(ctx, sceneName, [&ctx, sceneName] { ctx.switchEditorScene(sceneName, false); });
                if (ImGui::MenuItem(TranslateLabel("menu.additive", "Additiv laden").c_str(), nullptr, false, !isLoaded) && ctx.switchEditorScene)
                    ctx.switchEditorScene(sceneName, true);
                ImGui::Separator();
                if (ImGui::MenuItem(TranslateLabel("common.rename", "Umbenennen").c_str())) StartRename(browser, p);
                ImGui::Separator();
                ImGui::PushStyleColor(ImGuiCol_Text, theme::kDanger);
                if (ImGui::MenuItem(TranslateLabel("common.delete", "Loschen").c_str()))
                {
                    if (ctx.deleteEditorSceneByPath)
                        ctx.deleteEditorSceneByPath(PathToUtf8(p));
                    else if (ctx.deleteEditorScene)
                        ctx.deleteEditorScene(sceneName);
                }
                ImGui::PopStyleColor();
                ImGui::EndPopup();
            }
        }
        ImGui::PopID();
        if (isCurrent || isLoaded) ImGui::PopStyleColor();
    }

    float materialTileRowUsed = 0.f;
    const float assetTileWidth = 112.f;
    const float assetTileAvail = std::max(ImGui::GetContentRegionAvail().x, assetTileWidth);
    for (const auto& entry : CurrentDirEntries(browser))
    {
        const auto& p = entry.path();
        if (!entry.is_directory(ec) && IsSupportedMaterial(p) && !IsEditorInternalAsset(p))
        {
            const std::string materialPath = ToRelativeAssetPath(ctx, p);
            const bool isSelected = ctx.state.selectedMaterialAssetPath == materialPath;

            ImGui::PushID(PathToUtf8(p).c_str());

            if (!DrawRenameInputIfActive(ctx, browser, p))
            {
                if (materialTileRowUsed > 0.f && materialTileRowUsed + assetTileWidth <= assetTileAvail)
                    ImGui::SameLine();
                else
                    materialTileRowUsed = 0.f;

                const bool clicked = DrawAssetPreviewTile(ctx,
                                                          browser,
                                                          p,
                                                          AssetBrowserState::PreviewKind::Material,
                                                          isSelected,
                                                          "MAT");
                if (clicked)
                {
                    ctx.state.selectedPrefabAssetPath.clear();
                    SelectMaterialAsset(ctx, p);
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                    {
                        ctx.state.selectedEntity = NULL_ENTITY;
                        QueueOpenMaterialAsset(ctx, p);
                    }
                }

                if (ImGui::BeginPopupContextItem("##material_asset_popup"))
                {
                    DrawAssetCreateMenu(ctx, browser, browser.currentDir);
                    ImGui::Separator();
                    if (ImGui::MenuItem(TranslateLabel("assets.open_material", "Im Materialeditor oeffnen").c_str()))
                    {
                        ctx.state.selectedEntity = NULL_ENTITY;
                        QueueOpenMaterialAsset(ctx, p);
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem(TranslateLabel("common.rename", "Umbenennen").c_str()))
                        StartRename(browser, p);
                    ImGui::Separator();
                    ImGui::PushStyleColor(ImGuiCol_Text, theme::kDanger);
                    if (ImGui::MenuItem(TranslateLabel("common.delete", "Loeschen").c_str()))
                    {
                        std::error_code ec;
                        ForgetAssetThumbnail(ctx, p);
                        std::filesystem::remove(p, ec);
                    }
                    ImGui::PopStyleColor();
                    ImGui::EndPopup();
                }

                if (ImGui::BeginDragDropSource())
                {
                    ImGui::SetDragDropPayload("KROM_ASSET_MATERIAL", materialPath.c_str(), materialPath.size() + 1u);
                    ImGui::TextUnformatted(PathToUtf8(p.filename()).c_str());
                    ImGui::EndDragDropSource();
                }

                materialTileRowUsed += assetTileWidth;
            }

            ImGui::PopID();
        }
    }
    // ── Animationen ───────────────────────────────────────────────────────────
    float animationTileRowUsed = 0.f;
    for (const auto& entry : CurrentDirEntries(browser))
    {
        const auto& p = entry.path();
        if (entry.is_directory(ec) || p.extension() != ".anim")
            continue;
        const std::string animationPath = ToRelativeAssetPath(ctx, p);
        ImGui::PushID(PathToUtf8(p).c_str());
        if (!DrawRenameInputIfActive(ctx, browser, p))
        {
            if (animationTileRowUsed > 0.f && animationTileRowUsed + assetTileWidth <= assetTileAvail)
                ImGui::SameLine();
            else
                animationTileRowUsed = 0.f;
            if (DrawAssetPreviewTile(ctx, browser, p, AssetBrowserState::PreviewKind::Material, false, "ANIM", ICON_LC_VIDEO) &&
                ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                OpenAnimationFile(ctx, animationPath);
            if (ImGui::BeginPopupContextItem("##animation_asset_popup"))
            {
                DrawAssetCreateMenu(ctx, browser, browser.currentDir);
                ImGui::Separator();
                if (ImGui::MenuItem(TranslateLabel("common.open", "Open").c_str()))
                    OpenAnimationFile(ctx, animationPath);
                if (ImGui::MenuItem(TranslateLabel("common.rename", "Umbenennen").c_str()))
                    StartRename(browser, p);
                ImGui::Separator();
                ImGui::PushStyleColor(ImGuiCol_Text, theme::kDanger);
                if (ImGui::MenuItem(TranslateLabel("common.delete", "Loeschen").c_str()))
                {
                    std::error_code removeError;
                    std::filesystem::remove(p, removeError);
                }
                ImGui::PopStyleColor();
                ImGui::EndPopup();
            }
            if (ImGui::BeginDragDropSource())
            {
                ImGui::SetDragDropPayload("KROM_ASSET_ANIMATION", animationPath.c_str(), animationPath.size() + 1u);
                ImGui::TextUnformatted(PathToUtf8(p.filename()).c_str());
                ImGui::EndDragDropSource();
            }
            animationTileRowUsed += assetTileWidth;
        }
        ImGui::PopID();
    }

    // ── Animation Controller (*.animctrl) ─────────────────────────────────────
    float controllerTileRowUsed = 0.f;
    for (const auto& entry : CurrentDirEntries(browser))
    {
        const auto& p = entry.path();
        if (entry.is_directory(ec) || p.extension() != engine::addons::animation::kAnimationControllerExtension)
            continue;
        const std::string controllerPath = ToRelativeAssetPath(ctx, p);
        ImGui::PushID(PathToUtf8(p).c_str());
        if (!DrawRenameInputIfActive(ctx, browser, p))
        {
            if (controllerTileRowUsed > 0.f && controllerTileRowUsed + assetTileWidth <= assetTileAvail)
                ImGui::SameLine();
            else
                controllerTileRowUsed = 0.f;
            if (DrawAssetPreviewTile(ctx, browser, p, AssetBrowserState::PreviewKind::Material, false, "CTRL", ICON_LC_LIST_TREE) &&
                ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                OpenControllerGraph(ctx, controllerPath);
            if (ImGui::IsItemHovered())
            {
                const auto& controller = GetEditorAnimationController(ctx, controllerPath);
                ImGui::SetTooltip("%s\n%s: %zu  %s: %zu", controllerPath.c_str(),
                                  Translate("animation.states"), controller.asset.states.size(),
                                  Translate("animation.parameters"), controller.asset.parameters.size());
            }
            if (ImGui::BeginPopupContextItem("##controller_asset_popup"))
            {
                DrawAssetCreateMenu(ctx, browser, browser.currentDir);
                ImGui::Separator();
                if (ImGui::MenuItem(TranslateLabel("common.open", "Open").c_str()))
                    OpenControllerGraph(ctx, controllerPath);
                if (ImGui::MenuItem(TranslateLabel("common.rename", "Umbenennen").c_str()))
                    StartRename(browser, p);
                ImGui::Separator();
                ImGui::PushStyleColor(ImGuiCol_Text, theme::kDanger);
                if (ImGui::MenuItem(TranslateLabel("common.delete", "Loeschen").c_str()))
                {
                    std::error_code removeError;
                    std::filesystem::remove(p, removeError);
                }
                ImGui::PopStyleColor();
                ImGui::EndPopup();
            }
            if (ImGui::BeginDragDropSource())
            {
                ImGui::SetDragDropPayload("KROM_ASSET_ANIMCTRL", controllerPath.c_str(), controllerPath.size() + 1u);
                ImGui::TextUnformatted(PathToUtf8(p.filename()).c_str());
                ImGui::EndDragDropSource();
            }
            controllerTileRowUsed += assetTileWidth;
        }
        ImGui::PopID();
    }

    // ── Shader-Dateien ────────────────────────────────────────────────────────
    float shaderTileRowUsed = 0.f;
    for (const auto& entry : CurrentDirEntries(browser))
    {
        const auto& p = entry.path();
        if (!entry.is_directory(ec) && IsSupportedShader(p))
        {
            ImGui::PushID(PathToUtf8(p).c_str());

            if (!DrawRenameInputIfActive(ctx, browser, p))
            {
                // Shader-Typ bestimmt das Symbol
                const auto ext = PathToUtf8(p.extension());
                const AssetBrowserState::Icon noIcon{};
                const AssetBrowserState::Icon* icon = &noIcon;
                const char* tag = "SHD";
                if (ext == ".vs"   || p.stem().extension() == ".vs")        { icon = &browser.icons.vertexShader;   tag = "VS"; }
                else if (ext == ".ps"   || p.stem().extension() == ".ps")   { icon = &browser.icons.pixelShader;    tag = "PS"; }
                else if (ext == ".vert" || p.stem().extension() == ".vert") { icon = &browser.icons.vertexShader;   tag = "VS"; }
                else if (ext == ".frag" || p.stem().extension() == ".frag") { icon = &browser.icons.fragmentShader; tag = "FS"; }
                else if (ext == ".comp")                                     { tag = "CS"; }
                else if (PathToUtf8(p.filename()).find(".vs.") != std::string::npos) { icon = &browser.icons.vertexShader;   tag = "VS"; }
                else if (PathToUtf8(p.filename()).find(".ps.") != std::string::npos) { icon = &browser.icons.pixelShader;    tag = "PS"; }
                else if (PathToUtf8(p.filename()).find(".fs.") != std::string::npos) { icon = &browser.icons.fragmentShader; tag = "FS"; }

                BeginTileInRow(shaderTileRowUsed, tileRowAvail);
                if (DrawIconTile(*icon, PathToUtf8(p.stem()), false, tag) &&
                    ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                    OpenAssetInExternalEditor(ctx, p);

                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", PathToUtf8(p.filename()).c_str());

                if (ImGui::BeginPopupContextItem("##shader_asset_popup"))
                {
                    DrawAssetCreateMenu(ctx, browser, browser.currentDir);
                    ImGui::Separator();
                    if (ImGui::MenuItem(TranslateLabel("assets.open_external", "Extern oeffnen").c_str()))
                        OpenAssetInExternalEditor(ctx, p);
                    ImGui::Separator();
                    if (ImGui::MenuItem(TranslateLabel("common.rename", "Umbenennen").c_str()))
                        StartRename(browser, p);
                    ImGui::Separator();
                    ImGui::PushStyleColor(ImGuiCol_Text, theme::kDanger);
                    if (ImGui::MenuItem(TranslateLabel("common.delete", "Loeschen").c_str()))
                    {
                        std::error_code delEc;
                        ForgetAssetThumbnail(ctx, p);
                        std::filesystem::remove(p, delEc);
                    }
                    ImGui::PopStyleColor();
                    ImGui::EndPopup();
                }

                // Drag-Drop: absoluter Pfad als KROM_MOVE_FILE
                // Der Material-Editor akzeptiert diesen Payload und prueft die Extension
                if (ImGui::BeginDragDropSource())
                {
                    const std::string absPath = PathToUtf8(p);
                    ImGui::SetDragDropPayload("KROM_MOVE_FILE", absPath.c_str(), absPath.size() + 1u);
                    ImGui::TextUnformatted(PathToUtf8(p.filename()).c_str());
                    ImGui::EndDragDropSource();
                }
            }

            ImGui::PopID();
        }
    }

    // ── Audio-Dateien ─────────────────────────────────────────────────────────
    float audioTileRowUsed = 0.f;
    for (const auto& entry : CurrentDirEntries(browser))
    {
        const auto& p = entry.path();
        if (entry.is_directory(ec) || !IsSupportedAudio(p))
            continue;
        ImGui::PushID(PathToUtf8(p).c_str());
        if (!DrawRenameInputIfActive(ctx, browser, p))
        {
            BeginTileInRow(audioTileRowUsed, tileRowAvail);
            DrawIconTile(browser.icons.sound, PathToUtf8(p.filename()), false, "SND");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(Translate("assets.audio_drop"), PathToUtf8(p.filename()).c_str());

            if (ImGui::BeginPopupContextItem("##audio_asset_popup"))
            {
                DrawAssetCreateMenu(ctx, browser, browser.currentDir);
                ImGui::Separator();
                if (ImGui::MenuItem(TranslateLabel("common.rename", "Umbenennen").c_str()))
                    StartRename(browser, p);
                ImGui::EndPopup();
            }

            // Drag-Drop: Pfad relativ zum Asset-Root (wie bei Texturen)
            if (ImGui::BeginDragDropSource())
            {
                const std::string rel = ToRelativeAssetPath(ctx, p);
                ImGui::SetDragDropPayload("KROM_ASSET_AUDIO", rel.c_str(), rel.size() + 1u);
                ImGui::TextUnformatted(PathToUtf8(p.filename()).c_str());
                ImGui::EndDragDropSource();
            }
        }
        ImGui::PopID();
    }

    // ── Baked reflection probes (.kprobe) ─────────────────────────────────────
    // Written by the probe bake under Baked/<scene>/<probeId>.kprobe. The file
    // name is the probe id, so there is no rename; deleting drops the bake.
    // The tooltip names the object whose probe uses the file; double click
    // selects it. Unused files are deleted when the scene is saved.
    float probeTileRowUsed = 0.f;
    for (const auto& entry : CurrentDirEntries(browser))
    {
        const auto& p = entry.path();
        if (entry.is_directory(ec) || !IsBakedProbeFile(p))
            continue;
        ImGui::PushID(PathToUtf8(p).c_str());
        BeginTileInRow(probeTileRowUsed, tileRowAvail);
        DrawIconTile(browser, p, browser.icons.reflectionProbe, PathToUtf8(p.filename()), "PROBE");
        const bool tileHovered = ImGui::IsItemHovered();
        EntityID owner = NULL_ENTITY;
        if (tileHovered)
        {
            if (const uint64_t probeId = engine::addons::probes::ProbeIdFromFileName(PathToUtf8(p.filename())))
            {
                ctx.world.View<ReflectionProbeComponent>([&](EntityID id, const ReflectionProbeComponent& probe) {
                    if (probe.probeId == probeId && !owner.IsValid())
                        owner = id;
                });
            }
            std::string ownerText;
            if (owner.IsValid())
            {
                const auto* name = ctx.world.Get<NameComponent>(owner);
                ownerText = std::string(Translate("probe.file_owner")) +
                    (name && !name->name.empty() ? name->name : "Entity " + std::to_string(owner.Index()));
            }
            else
                ownerText = Translate("probe.file_no_owner");
            ImGui::SetTooltip("%s\n%s", ownerText.c_str(), ToRelativeAssetPath(ctx, p).c_str());
        }
        if (tileHovered && owner.IsValid() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        {
            ClearMultiSelection(ctx);
            ctx.state.selectedEntity = owner;
            ctx.state.selectionSource = SelectionSource::Outliner;
            ctx.state.hierarchyRevealEntity = owner;
        }

        if (ImGui::BeginPopupContextItem("##probe_asset_popup"))
        {
            DrawAssetCreateMenu(ctx, browser, browser.currentDir);
            ImGui::Separator();
            ImGui::PushStyleColor(ImGuiCol_Text, theme::kDanger);
            if (ImGui::MenuItem(TranslateLabel("common.delete", "Loeschen").c_str()))
            {
                std::error_code delEc;
                std::filesystem::remove(p, delEc);
                if (delEc)
                    ctx.lastFileMessage = Translate("common.delete_failed_prefix") + delEc.message();
            }
            ImGui::PopStyleColor();
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }

    // ── C++-Scripts ───────────────────────────────────────────────────────────
    float cppTileRowUsed = 0.f;
    for (const auto& entry : CurrentDirEntries(browser))
    {
        const auto& p = entry.path();
        if (!entry.is_directory(ec) && IsSupportedCppScriptSource(p))
        {
            ImGui::PushID(PathToUtf8(p).c_str());

            if (!DrawRenameInputIfActive(ctx, browser, p))
            {
                BeginTileInRow(cppTileRowUsed, tileRowAvail);
                const auto ext = PathToUtf8(p.extension());
                const bool isHeader = ext == ".hpp" || ext == ".h";
                if (DrawIconTile(isHeader ? browser.icons.cppHeader : browser.icons.cppSource,
                                 PathToUtf8(p.filename()), browser.selectedScriptPath == p,
                                 isHeader ? "HPP" : "CPP"))
                {
                    browser.selectedScriptPath = p;
                    browser.selectedFolder.clear();
                }
                if (browser.highlightPath == p)
                    ImGui::SetScrollHereY(0.5f);

                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", PathToUtf8(p).c_str());

                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                    OpenAssetInExternalEditor(ctx, p);

                if (ImGui::BeginPopupContextItem("##cpp_script_asset_popup"))
                {
                    DrawAssetCreateMenu(ctx, browser, browser.currentDir);
                    ImGui::Separator();
                    if (ImGui::MenuItem(TranslateLabel("assets.open_external", "Extern oeffnen").c_str()))
                        OpenAssetInExternalEditor(ctx, p);
                    ImGui::Separator();
                    if (ImGui::MenuItem(TranslateLabel("assets.regenerate_bindings", "Script-Bindings neu generieren").c_str()))
                    {
                        const EditorScriptAssetResult result = RefreshCppScriptProject(ctx);
                        ctx.lastFileMessage = result.message;
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem(TranslateLabel("common.rename", "Umbenennen").c_str()))
                        StartRename(browser, p);
                    ImGui::Separator();
                    ImGui::PushStyleColor(ImGuiCol_Text, theme::kDanger);
                    if (ImGui::MenuItem(TranslateLabel("common.delete", "Loeschen").c_str()))
                    {
                        std::error_code delEc;
                        ForgetAssetThumbnail(ctx, p);
                        std::filesystem::remove(p, delEc);
                        if (delEc)
                        {
                            ctx.lastFileMessage = Translate("common.delete_failed_prefix") + delEc.message();
                        }
                        else
                        {
                            const EditorScriptAssetResult result = RefreshCppScriptProject(ctx);
                            ctx.lastFileMessage = result.message;
                        }
                    }
                    ImGui::PopStyleColor();
                    ImGui::EndPopup();
                }

                if (ImGui::BeginDragDropSource())
                {
                    const std::string absPath = PathToUtf8(p);
                    ImGui::SetDragDropPayload("KROM_MOVE_FILE", absPath.c_str(), absPath.size() + 1u);
                    ImGui::TextUnformatted(PathToUtf8(p.filename()).c_str());
                    ImGui::EndDragDropSource();
                }
            }

            ImGui::PopID();
        }
    }

    // ── 3D-Modelle ────────────────────────────────────────────────────────────
    float modelTileRowUsed = 0.f;
    const float modelTileAvail = std::max(ImGui::GetContentRegionAvail().x, assetTileWidth);
    for (const auto& entry : CurrentDirEntries(browser))
    {
        const auto& p = entry.path();
        if (!entry.is_directory(ec) && IsSupportedModel(p) && !IsEditorInternalAsset(p))
        {
            ImGui::PushID(PathToUtf8(p).c_str());

            if (!DrawRenameInputIfActive(ctx, browser, p))
            {
                if (modelTileRowUsed > 0.f && modelTileRowUsed + assetTileWidth <= modelTileAvail)
                    ImGui::SameLine();
                else
                    modelTileRowUsed = 0.f;

                const bool clicked = DrawAssetPreviewTile(ctx,
                                                          browser,
                                                          p,
                                                          AssetBrowserState::PreviewKind::Model,
                                                          false,
                                                          "3D");
                // Animations of the model (Blender actions): a badge with their count
                // folds the clip tiles in and out after the model.
                const std::string modelPath = ToRelativeAssetPath(ctx, p);
                const ModelClipList* modelClips = nullptr;
                bool clipsShown = false;
                if (ctx.assetPipeline)
                {
                    const ModelClipList& list = GetModelClips(ctx.assetPipeline->GetAssetRoot(), modelPath);
                    if (!list.settings.clips.empty())
                    {
                        modelClips = &list;
                        clipsShown = ModelClipsExpanded().count(modelPath) != 0u;
                        if (DrawModelClipBadge(list.settings.clips.size(), clipsShown))
                        {
                            if (clipsShown)
                                ModelClipsExpanded().erase(modelPath);
                            else
                                ModelClipsExpanded().insert(modelPath);
                            clipsShown = !clipsShown;
                        }
                    }
                }
                if (clicked && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) &&
                    !ImGui::IsMouseHoveringRect(ModelClipBadgeMin(), ModelClipBadgeMax()))
                    OpenModelView(ctx, p);

                if (ImGui::BeginDragDropSource())
                {
                    const std::string absPath = PathToUtf8(p);
                    ImGui::SetDragDropPayload("KROM_MOVE_FILE", absPath.c_str(), absPath.size() + 1u);
                    ImGui::TextUnformatted(PathToUtf8(p.filename()).c_str());
                    ImGui::EndDragDropSource();
                }

                if (ImGui::BeginPopupContextItem("##model_asset_popup"))
                {
                    DrawAssetCreateMenu(ctx, browser, browser.currentDir);
                    ImGui::Separator();
                    if (ImGui::MenuItem(TranslateLabel("assets.load_into_scene", "In Szene laden").c_str()))
                        SpawnModel(ctx, p);
                    if (modelClips &&
                        ImGui::MenuItem(TranslateLabel(clipsShown ? "assets.hide_clips" : "assets.show_clips",
                                                       clipsShown ? "Hide Clips" : "Show Clips").c_str()))
                    {
                        if (clipsShown)
                            ModelClipsExpanded().erase(modelPath);
                        else
                            ModelClipsExpanded().insert(modelPath);
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem(TranslateLabel("common.rename", "Umbenennen").c_str()))
                        StartRename(browser, p);
                    ImGui::Separator();
                    ImGui::PushStyleColor(ImGuiCol_Text, theme::kDanger);
                    if (ImGui::MenuItem(TranslateLabel("common.delete", "Loeschen").c_str()))
                    {
                        std::error_code ec;
                        ForgetAssetThumbnail(ctx, p);
                        std::filesystem::remove(p, ec);
                    }
                    ImGui::PopStyleColor();
                    ImGui::EndPopup();
                }

                modelTileRowUsed += assetTileWidth;

                if (modelClips && clipsShown)
                    for (const animation::ModelClipEntry& clip : modelClips->settings.clips)
                    {
                        ImGui::PushID(clip.id.c_str());
                        if (modelTileRowUsed > 0.f && modelTileRowUsed + assetTileWidth <= modelTileAvail)
                            ImGui::SameLine();
                        else
                            modelTileRowUsed = 0.f;
                        std::string label = clip.name;
                        std::replace(label.begin(), label.end(), '/', '_');
                        std::replace(label.begin(), label.end(), '\\', '_');
                        const std::string clipPath = animation::ModelClipAssetPath(modelPath, clip.id);
                        const bool sourceMissing = !modelClips->SourceExists(clip.source);
                        if (sourceMissing)
                            ImGui::PushStyleColor(ImGuiCol_Text, theme::kDanger);
                        const bool clipClicked = DrawAssetPreviewTile(ctx, browser, p.parent_path() / PathFromUtf8(label + ".clip"),
                                                                      AssetBrowserState::PreviewKind::Material, false, "CLIP", ICON_LC_VIDEO);
                        if (sourceMissing)
                            ImGui::PopStyleColor();
                        if (ImGui::IsItemHovered())
                        {
                            std::string tip = PathToUtf8(p.filename()) + " / " + clip.source;
                            if (clip.hasRange)
                                tip += "  [" + std::to_string(clip.startFrame) + " - " + std::to_string(clip.endFrame) + "]";
                            if (sourceMissing)
                                tip += std::string("\n") + Translate("animation.source_missing");
                            ImGui::SetTooltip("%s", tip.c_str());
                        }
                        if (clipClicked && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                            OpenModelClip(ctx, clipPath);
                        if (ImGui::BeginDragDropSource())
                        {
                            // The id leaves the browser: it must be in the settings file.
                            (void)PersistModelClips(ctx.assetPipeline->GetAssetRoot(), modelPath);
                            ImGui::SetDragDropPayload("KROM_ASSET_ANIMATION", clipPath.c_str(), clipPath.size() + 1u);
                            ImGui::TextUnformatted(clip.name.c_str());
                            ImGui::EndDragDropSource();
                        }
                        if (ImGui::BeginPopupContextItem("##model_clip_popup"))
                        {
                            if (ImGui::MenuItem(TranslateLabel("common.open", "Open").c_str()))
                                OpenModelClip(ctx, clipPath);
                            ImGui::EndPopup();
                        }
                        modelTileRowUsed += assetTileWidth;
                        ImGui::PopID();
                    }
            }

            ImGui::PopID();
        }
    }

    float prefabTileRowUsed = 0.f;
    const float prefabTileAvail = std::max(ImGui::GetContentRegionAvail().x, assetTileWidth);
    for (const auto& entry : CurrentDirEntries(browser))
    {
        const auto& p = entry.path();
        if (!entry.is_directory(ec) && IsSupportedPrefab(p))
        {
            const std::string prefabPath = ToRelativeAssetPath(ctx, p);
            const bool isSelected = ctx.state.selectedPrefabAssetPath == prefabPath;
            ImGui::PushID(PathToUtf8(p).c_str());

            if (!DrawRenameInputIfActive(ctx, browser, p))
            {
                if (prefabTileRowUsed > 0.f && prefabTileRowUsed + assetTileWidth <= prefabTileAvail)
                    ImGui::SameLine();
                else
                    prefabTileRowUsed = 0.f;

                const bool clicked = DrawAssetPreviewTile(ctx,
                                                          browser,
                                                          p,
                                                          AssetBrowserState::PreviewKind::Prefab,
                                                          isSelected,
                                                          "PFB");
                if (clicked)
                {
                    ctx.state.selectedPrefabAssetPath = prefabPath;
                    ctx.state.selectedMaterialAssetPath.clear();
                    ctx.state.selectedEntity = NULL_ENTITY;
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                        EnterPrefabMode(ctx, p);
                }

                if (ImGui::BeginDragDropSource())
                {
                    ImGui::SetDragDropPayload("KROM_ASSET_PREFAB",
                                              prefabPath.c_str(),
                                              prefabPath.size() + 1u);
                    ImGui::TextUnformatted(PathToUtf8(p.filename()).c_str());
                    ImGui::EndDragDropSource();
                }

                if (ImGui::BeginPopupContextItem("##prefab_asset_popup"))
                {
                    DrawAssetCreateMenu(ctx, browser, browser.currentDir);
                    ImGui::Separator();
                    if (ImGui::MenuItem(TranslateLabel("menu.edit", "Bearbeiten").c_str()))
                        EnterPrefabMode(ctx, p);
                    if (ImGui::MenuItem(TranslateLabel("assets.load_into_scene", "In Szene laden").c_str()))
                        SpawnPrefab(ctx, p);
                    ImGui::Separator();
                    if (ImGui::MenuItem(TranslateLabel("common.rename", "Umbenennen").c_str()))
                        StartRename(browser, p);
                    ImGui::Separator();
                    ImGui::PushStyleColor(ImGuiCol_Text, theme::kDanger);
                    if (ImGui::MenuItem(TranslateLabel("common.delete", "Loeschen").c_str()))
                    {
                        std::error_code delEc;
                        ForgetAssetThumbnail(ctx, p);
                        std::filesystem::remove(p, delEc);
                    }
                    ImGui::PopStyleColor();
                    ImGui::EndPopup();
                }

                prefabTileRowUsed += assetTileWidth;
            }

            ImGui::PopID();
        }
    }

    bool textureHeaderDrawn = false;
    float textureRowUsed = 0.f;
    const float textureTileWidth = 112.f;
    const float textureAvail = std::max(ImGui::GetContentRegionAvail().x, textureTileWidth);
    for (const auto& entry : CurrentDirEntries(browser))
    {
        const auto& p = entry.path();
        if (!entry.is_directory(ec) && IsSupportedTexture(p))
        {
            if (!textureHeaderDrawn)
            {
                ImGui::SeparatorText(Translate("assets.textures"));
                textureHeaderDrawn = true;
            }

            if (textureRowUsed > 0.f && textureRowUsed + textureTileWidth <= textureAvail)
                ImGui::SameLine();
            else
                textureRowUsed = 0.f;

            DrawTextureAssetTile(ctx, browser, p);
            textureRowUsed += textureTileWidth;
        }
    }

    // Whole content area: a hierarchy entity dropped here becomes a prefab in the
    // current folder. Folder tiles and the folder tree are smaller targets and win.
    if (const ImGuiPayload* active = ImGui::GetDragDropPayload();
        active && active->IsDataType("KROM_EDITOR_ENTITY"))
    {
        ImGuiWindow* contentWindow = ImGui::GetCurrentWindow();
        if (ImGui::BeginDragDropTargetCustom(contentWindow->InnerRect,
                                             ImGui::GetID("##EntityToPrefabDrop")))
        {
            AcceptEntityToPrefabPayload(ctx, browser.currentDir);
            ImGui::EndDragDropTarget();
        }
    }

    ImGui::EndChild(); // ##content_panel
    ctx.assetsPanelHeight = ImGui::GetWindowHeight();
    ImGui::End();
}

void ImportExternalFilesToAssetBrowser(EditorFrameContext& ctx,
                                       AssetBrowserState& browser,
                                       const std::vector<std::filesystem::path>& files)
{
    if (files.empty())
        return;

    const std::filesystem::path targetDir = browser.initialized
        ? browser.currentDir
        : (ctx.assetPipeline ? ctx.assetPipeline->GetAssetRoot() : std::filesystem::current_path());

    uint32_t importedCount = 0u;
    uint32_t skippedCount = 0u;
    for (const auto& file : files)
    {
        if (file.empty() || !std::filesystem::exists(file) || std::filesystem::is_directory(file))
        {
            ++skippedCount;
            continue;
        }

        if (!IsSupportedModel(file) && !IsSupportedTexture(file) &&
            !IsSupportedMaterial(file) && !IsSupportedShader(file))
        {
            ++skippedCount;
            continue;
        }

        const std::filesystem::path dest = MakeUniqueDestinationPath(targetDir, file);
        std::string error;
        if (CopyExternalFile(file, dest, error))
            ++importedCount;
        else
            engine::Debug::LogError("Asset-Browser: Datei-Import fehlgeschlagen '%s' -> '%s': %s",
                                    PathToUtf8(file).c_str(), PathToUtf8(dest).c_str(), error.c_str());
    }

    if (importedCount > 0u)
    {
        browser.currentDir = targetDir;
        browser.initialized = true;
        ctx.lastFileMessage = TranslateWithValue("assets.imported_count", std::to_string(importedCount)) +
            (skippedCount > 0u ? TranslateWithValue("assets.skipped_count", std::to_string(skippedCount)) : "");
    }
    else if (skippedCount > 0u)
    {
        ctx.lastFileMessage = Translate("assets.import_empty");
    }
}

std::vector<std::filesystem::path> ListProjectTextures(const EditorFrameContext& ctx)
{
    std::vector<std::filesystem::path> paths;
    if (!ctx.assetPipeline || !ctx.editorTextureId)
        return paths;

    std::error_code ec;
    const auto& root = ctx.assetPipeline->GetAssetRoot();
    if (root.empty())
        return paths;
    const auto flags = std::filesystem::directory_options::skip_permission_denied;
    for (std::filesystem::recursive_directory_iterator it(root, flags, ec), end; !ec && it != end; it.increment(ec))
        if (!it->is_directory(ec) && !ec && IsSupportedTexture(it->path()))
            paths.push_back(it->path());
    return paths;
}

bool PreloadEditorTexture(EditorFrameContext& ctx, AssetBrowserState& browser, const std::filesystem::path& path)
{
    return ResolveTextureThumbnail(ctx, browser, path) != nullptr;
}

void PreloadAllTextures(EditorFrameContext& ctx, AssetBrowserState& browser)
{
    uint32_t count = 0u;
    for (const auto& path : ListProjectTextures(ctx))
        if (PreloadEditorTexture(ctx, browser, path))
            ++count;
    if (count > 0u)
        Debug::LogVerbose("AssetBrowser: %u Texturen vorgeladen.", count);
}

} // namespace engine::renderer::addons::editor

#else // KROM_EDITOR_HAS_IMGUI

namespace engine::renderer::addons::editor {
void DrawAssetBrowser(EditorFrameContext&, AssetBrowserState&) {}
void DrawEditorConsole(EditorFrameContext&) {}
void ImportExternalFilesToAssetBrowser(EditorFrameContext&, AssetBrowserState&, const std::vector<std::filesystem::path>&) {}
} // namespace engine::renderer::addons::editor

#endif // KROM_EDITOR_HAS_IMGUI
