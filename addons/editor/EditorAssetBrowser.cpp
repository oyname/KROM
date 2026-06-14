#include "addons/editor/EditorAssetBrowser.hpp"
#include "addons/editor/EditorFileNaming.hpp"
#include "addons/editor/EditorFeature.hpp"
#include "addons/editor/EditorMaterialLibrary.hpp"
#include "addons/editor/EditorScriptAssets.hpp"

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
#include "assets/AssetPipeline.hpp"
#include "assets/VertexLayoutBridge.hpp"
#include "collision/SceneQueries.hpp"
#include "ecs/World.hpp"
#include "ecs/Components.hpp"
#include "core/Logger.hpp"
#include "renderer/CompiledMaterialDesc.hpp"
#include "renderer/MaterialParameterLayout.hpp"
#include "renderer/VertexLayoutContract.hpp"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <cstring>

namespace engine::renderer::addons::editor {
namespace {

float EditorPickMaxDistance(const EditorCameraState& cam) noexcept
{
    return std::max(cam.farPlane, 100000.f);
}

bool IsSupportedModel(const std::filesystem::path& p)
{
    const auto ext = p.extension().string();
    return ext == ".gltf" || ext == ".glb";
}

bool IsEditorInternalAsset(const std::filesystem::path& p)
{
    const std::string file = p.filename().string();
    return file == "camera.glb" || file == "matcam.mat";
}

bool IsSupportedTexture(const std::filesystem::path& p)
{
    const auto ext = p.extension().string();
    return ext == ".png"  || ext == ".jpg"  || ext == ".jpeg"
        || ext == ".dds"  || ext == ".ktx"  || ext == ".ktx2"
        || ext == ".bmp" || ext == ".hdr";
}

bool IsSupportedMaterial(const std::filesystem::path& p)
{
    return p.extension() == ".mat";
}

bool IsSupportedPrefab(const std::filesystem::path& p)
{
    return p.extension() == ".prefab";
}

bool IsSupportedShader(const std::filesystem::path& p)
{
    const auto ext = p.extension().string();
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

void OpenAssetInExternalEditor(EditorFrameContext& ctx, const std::filesystem::path& path, int line = 0)
{
    if (path.empty() || !std::filesystem::exists(path))
        return;

    const std::string filePath = path.string();
    const std::string executable = ctx.state.codeEditorExecutableBuffer.data();
    if (!executable.empty())
    {
        std::string arguments = ctx.state.codeEditorArgumentsBuffer.data();
        if (arguments.empty())
            arguments = "\"{file}\"";
        const bool hasLineToken = arguments.find("{line}") != std::string::npos;
        const std::filesystem::path workspacePath = ctx.currentProjectRoot.empty()
            ? std::filesystem::current_path()
            : std::filesystem::path(ctx.currentProjectRoot);
        ReplaceAll(arguments, "\"{workspace}\"", QuoteShellArg(workspacePath.string()));
        ReplaceAll(arguments, "{workspace}", QuoteShellArg(workspacePath.string()));
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
        const int result = std::system(command.c_str());
        if (result != 0)
            ctx.lastFileMessage = "Code Editor konnte nicht gestartet werden: " + executable;
        return;
    }

#if defined(_WIN32)
    const std::string command = "cmd /c start \"\" " + QuoteShellArg(filePath);
#elif defined(__APPLE__)
    const std::string command = "open " + QuoteShellArg(filePath);
#else
    const std::string command = "xdg-open " + QuoteShellArg(filePath);
#endif
    const int result = std::system(command.c_str());
    if (result != 0)
        ctx.lastFileMessage = "Externes Oeffnen fehlgeschlagen: " + path.string();
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

    std::filesystem::path path(file);
    if (path.is_relative() && !ctx.currentProjectRoot.empty())
        path = std::filesystem::path(ctx.currentProjectRoot) / path;
    if (!std::filesystem::exists(path))
        return false;

    outPath = path;
    outLine = std::atoi(message.substr(open + 1u, close - open - 1u).c_str());
    return outLine > 0;
}

bool IsSupportedCppScriptSource(const std::filesystem::path& p)
{
    const auto ext = p.extension().string();
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
    const std::filesystem::path path(filename ? filename : "");
    if (path.is_absolute() || ctx.engineAssetRoot.empty())
        return path;
    return (std::filesystem::path(ctx.engineAssetRoot) / path).lexically_normal();
}

std::filesystem::path MakeUniqueDestinationPath(const std::filesystem::path& directory,
                                                const std::filesystem::path& source)
{
    return MakeUniqueFilesystemPath(directory, source.stem().string(), source.extension().string());
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
    return (ec || relative.empty()) ? absolutePath.generic_string() : relative.generic_string();
}

std::filesystem::path ToAbsoluteAssetBrowserPath(const EditorFrameContext& ctx,
                                                 const char* rawPath,
                                                 bool isRelative)
{
    if (!rawPath || rawPath[0] == '\0')
        return {};
    std::filesystem::path src(rawPath);
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
        ctx.lastFileMessage = "Verschieben fehlgeschlagen: Ziel existiert bereits.";
        return false;
    }
    if (std::filesystem::is_directory(src, ec))
    {
        const std::filesystem::path rel = std::filesystem::relative(targetDir, src, ec);
        const std::string relText = rel.generic_string();
        if (!ec && !rel.empty() && relText.rfind("..", 0u) != 0u)
        {
            ctx.lastFileMessage = "Verschieben fehlgeschlagen: Ordner kann nicht in sich selbst verschoben werden.";
            return false;
        }
    }

    std::filesystem::rename(src, dst, ec);
    if (ec)
    {
        ctx.lastFileMessage = "Verschieben fehlgeschlagen: " + ec.message();
        engine::Debug::LogError("Asset-Browser: Verschieben fehlgeschlagen: %s", ec.message().c_str());
        return false;
    }

    browser.currentDir = targetDir;
    browser.expandedDirs.insert(targetDir.string());
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
    return false;
}

struct EditorImportShaders
{
    ShaderHandle vertex;
    ShaderHandle fragment;
    ShaderHandle shadow;
};

TextureHandle LoadRuntimeTexture(EditorFrameContext& ctx, const std::filesystem::path& path)
{
    if (!ctx.assetPipeline)
        return TextureHandle::Invalid();

    const TextureHandle assetTexture = ctx.assetPipeline->LoadTexture(path.string());
    if (!assetTexture.IsValid())
        return TextureHandle::Invalid();

    ctx.assetPipeline->UploadPendingGpuAssets();

    const TextureHandle gpuTexture = ctx.assetPipeline->GetGpuTexture(assetTexture);
    if (!gpuTexture.IsValid())
    {
        engine::Debug::LogError("Asset-Browser: GPU-Upload der Textur fehlgeschlagen: %s",
                                path.string().c_str());
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
    const char* shadowPath = opengl ? "shadow.opengl.vs.glsl" : "shadow.vs.hlsl";

    shaders.vertex = ctx.assetPipeline->LoadShader(
        ResolveEngineShaderPath(ctx, vsPath).string(),
        assets::ShaderStage::Vertex);
    shaders.fragment = ctx.assetPipeline->LoadShader(
        ResolveEngineShaderPath(ctx, fsPath).string(),
        assets::ShaderStage::Fragment);
    shaders.shadow = ctx.assetPipeline->LoadShader(
        ResolveEngineShaderPath(ctx, shadowPath).string(),
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

    // --- PBR-Versuch (nur für statische Meshes) ---
    if (!skinned && ctx.assetPipeline)
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
                renderer::VertexContracts::PbrLit(), mesh.submeshes[0], &pbrErr);

            if (!pbrLayout.attributes.empty())
            {
                const bool opengl = ctx.shaderTarget == assets::ShaderTargetProfile::OpenGL_GLSL450;
                ShaderHandle vs = ctx.assetPipeline->LoadShader(
                    ResolveEngineShaderPath(ctx, opengl ? "pbr_lit.opengl.vs.glsl" : "pbr_lit.vs.hlsl").string(),
                    assets::ShaderStage::Vertex);
                ShaderHandle fs = ctx.assetPipeline->LoadShader(
                    ResolveEngineShaderPath(ctx, opengl ? "pbr_lit.opengl.fs.glsl" : "pbr_lit.ps.hlsl").string(),
                    assets::ShaderStage::Fragment);
                ShaderHandle shadow = ctx.assetPipeline->LoadShader(
                    ResolveEngineShaderPath(ctx, opengl ? "shadow.opengl.vs.glsl" : "shadow.vs.hlsl").string(),
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
                                if (const TextureHandle tex = LoadRuntimeTexture(ctx, srcMat->baseColorTexture.path); tex.IsValid())
                                    builder.BaseColor(tex);
                            }
                            if (!srcMat->metallicRoughnessTexture.path.empty())
                            {
                                if (const TextureHandle tex = LoadRuntimeTexture(ctx, srcMat->metallicRoughnessTexture.path); tex.IsValid())
                                {
                                    builder.Roughness(tex, pbr::MaterialChannel::G);
                                    builder.Metallic(tex, pbr::MaterialChannel::B);
                                    builder.Occlusion(tex, pbr::MaterialChannel::R, srcMat->occlusionStrength);
                                }
                            }
                            if (!srcMat->normalTexture.path.empty())
                            {
                                if (const TextureHandle tex = LoadRuntimeTexture(ctx, srcMat->normalTexture.path); tex.IsValid())
                                    builder.Normal(tex, srcMat->normalScale);
                            }
                            if (!srcMat->emissiveTexture.path.empty())
                            {
                                if (const TextureHandle tex = LoadRuntimeTexture(ctx, srcMat->emissiveTexture.path); tex.IsValid())
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
    std::string layoutError;
    const renderer::VertexLayout layout = assets::ResolveVertexLayout(
        skinned ? renderer::VertexContracts::SkinnedLit()
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
    info.specularStrength   = 0.3f;
    info.enableBaseColorMap = true;
    info.castShadows        = true;
    info.doubleSided        = srcMat ? srcMat->doubleSided : true;

    const MaterialHandle handle = renderer::lit::LitMaterial::Register(ctx.materials, info);
    if (handle.IsValid() && srcMat)
    {
        if (!srcMat->baseColorTexture.path.empty())
        {
            if (const TextureHandle tex = LoadRuntimeTexture(ctx, srcMat->baseColorTexture.path); tex.IsValid())
                ctx.materials.SetTexture(handle, "albedo", tex);
        }
        if (!srcMat->emissiveTexture.path.empty())
        {
            if (const TextureHandle tex = LoadRuntimeTexture(ctx, srcMat->emissiveTexture.path); tex.IsValid())
                ctx.materials.SetTexture(handle, "emissive", tex);
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

    const std::string texturePath = path.lexically_normal().generic_string();
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

    // Prioritaet 1: aktives Material-Asset (Inspector oder Materialeditor).
    // AssignTextureToSelectedMaterial nutzt nur den Dateipfad, nicht den Handle —
    // daher KEIN frühes Return wenn LoadRuntimeTexture noch keinen gültigen GPU-
    // Handle liefert (Textur wird asynchron geladen). Beim ersten Doppelklick wird
    // der Pfad sofort im Asset gesetzt; UpdateCachedTexture lädt die Textur im
    // nächsten Frame nach. Ohne diese Änderung musste der Nutzer zweimal doppel-
    // klicken: erster Doppelklick schlug fehl weil tex.IsValid() = false.
    FlushPendingMaterialOpen(ctx);
    if (!ctx.state.selectedMaterialAssetPath.empty())
    {
        // Textur schon jetzt laden damit GPU-Upload angestossen wird.
        // Fehler werden still ignoriert — der Pfad wird trotzdem gespeichert
        // und beim nächsten PreviewMaterialAssetOnBoundEntities nachgeladen.
        LoadRuntimeTexture(ctx, path);
        AssignTextureToSelectedMaterial(ctx, TextureHandle::Invalid(),
                                        ctx.state.activeTextureParamName,
                                        path.lexically_normal().string());
        return;
    }

    // Prioritaet 2: Entity-basiert — hier wird der Handle benötigt.
    const TextureHandle tex = LoadRuntimeTexture(ctx, path);
    if (!tex.IsValid())
    {
        engine::Debug::LogError("Asset-Browser: Textur konnte nicht geladen werden: %s", path.string().c_str());
        return;
    }

    if (ctx.state.selectedEntity.IsValid() && ctx.world.IsAlive(ctx.state.selectedEntity))
    {
        std::vector<EntityID> targets;
        CollectMeshEntities(ctx.world, ctx.state.selectedEntity, targets);
        const std::string baseName = path.stem().string();
        for (EntityID target : targets)
            ApplyTextureToMeshEntity(ctx, target, tex, ctx.state.activeTextureParamName, path, baseName);
        return;
    }

    engine::Debug::LogWarning(
        "Asset-Browser: Bitte zuerst ein Material-Asset oder eine Mesh-Entity auswaehlen, bevor eine Textur zugewiesen wird.");
}

collision::Ray BuildEditorMouseRay(const EditorFrameContext& ctx, const ImVec2& mousePos)
{
    const ImGuiIO& io = ImGui::GetIO();
    const EditorCameraState& cam = ctx.state.editorCamera;
    const float displayW = std::max(io.DisplaySize.x, 1.f);
    const float displayH = std::max(io.DisplaySize.y, 1.f);
    const float ndcX = (mousePos.x / displayW) * 2.f - 1.f;
    const float ndcY = 1.f - (mousePos.y / displayH) * 2.f;
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
    engine::assets::ImportedAssetBundle bundle = importer.Import(path.string());
    if (!bundle.Ok())
    {
        engine::Debug::LogError("Asset-Browser: Import fehlgeschlagen: %s", bundle.error.c_str());
        return;
    }

    engine::addons::prefab::PrefabBuildOptions buildOptions;
    buildOptions.name                = path.stem().string();
    buildOptions.createSyntheticRoot = true;
    buildOptions.playFirstAnimation  = true;
    buildOptions.loopAnimations      = true;

    engine::addons::prefab::PrefabAsset prefab =
        engine::addons::prefab::BuildPrefabFromImportedBundle(
            std::move(bundle), ctx.registry, buildOptions);

    if (prefab.Empty())
    {
        engine::Debug::LogError("Asset-Browser: Prefab leer nach Import: %s", path.string().c_str());
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
                                error.empty() ? path.string().c_str() : error.c_str());
        return;
    }

    engine::addons::prefab::PrefabInstantiateOptions instantiateOptions{};
    instantiateOptions.scriptRegistry = ctx.scriptRegistry;
    const engine::addons::prefab::PrefabInstance instance =
        engine::addons::prefab::InstantiatePrefab(ctx.world, prefab, instantiateOptions);
    if (!instance.IsValid())
    {
        engine::Debug::LogError("Asset-Browser: Prefab konnte nicht instanziiert werden: %s",
                                path.string().c_str());
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

        const TextureHandle texture = LoadRuntimeTexture(ctx, record.baseColorTexturePath);
        if (!texture.IsValid())
            continue;
        ApplyTextureToMeshEntity(ctx,
                                 entity,
                                 texture,
                                 "albedo",
                                 record.baseColorTexturePath,
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
        std::string relPath = path.generic_string();
        if (!ctx.currentProjectRoot.empty())
        {
            const std::filesystem::path assetsRoot =
                std::filesystem::path(ctx.currentProjectRoot) / "Assets";
            std::error_code ec;
            auto rel = std::filesystem::relative(path, assetsRoot, ec);
            if (!ec) relPath = rel.generic_string();
        }
        ctx.world.Add<PrefabInstanceComponent>(instance.root,
            PrefabInstanceComponent{ relPath, /*overrideTransform=*/true });
    }

    ctx.state.selectedEntity = instance.root;
    ctx.lastFileMessage = "Prefab geladen: " + path.filename().string();
}

std::filesystem::path ResolveAssetPayloadPath(const EditorFrameContext& ctx, const char* payloadPath)
{
    if (!payloadPath || payloadPath[0] == '\0')
        return {};

    std::filesystem::path path(payloadPath);
    if (path.is_absolute())
        return path;
    if (!ctx.currentProjectRoot.empty())
        return std::filesystem::path(ctx.currentProjectRoot) / "Assets" / path;
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
    ImGui::SetNextWindowPos(ImVec2(0.f, 0.f));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::SetNextWindowBgAlpha(0.f);

    constexpr ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoBackground;

    ImGui::Begin("##ViewportAssetDropTarget", nullptr, flags);
    ImGui::InvisibleButton("##ViewportAssetDropArea", io.DisplaySize);

    if (ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("KROM_ASSET_PREFAB"))
        {
            const auto* rawPath = static_cast<const char*>(payload->Data);
            const std::filesystem::path path = ResolveAssetPayloadPath(ctx, rawPath);
            const math::Vec3 position = AssetDropPosition(ctx, io.MousePos);
            SpawnPrefab(ctx, path, &position);
        }

        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("KROM_MOVE_FILE"))
        {
            const auto* rawPath = static_cast<const char*>(payload->Data);
            const std::filesystem::path path = ResolveAssetPayloadPath(ctx, rawPath);
            if (IsSupportedModel(path))
            {
                const math::Vec3 position = AssetDropPosition(ctx, io.MousePos);
                SpawnModel(ctx, path, &position);
            }
        }

        ImGui::EndDragDropTarget();
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

AssetBrowserState::TextureThumbnail* ResolveTextureThumbnail(
    EditorFrameContext& ctx,
    AssetBrowserState& browser,
    const std::filesystem::path& path)
{
    if (!ctx.assetPipeline || !ctx.editorTextureId)
        return nullptr;

    const std::string key = path.lexically_normal().generic_string();
    const uint64_t stamp = FileStamp(path);
    auto& thumb = browser.textureThumbnails[key];

    if (!thumb.gpuTexture.IsValid() || !thumb.imguiId || thumb.fileStamp != stamp)
    {
        thumb.gpuTexture = LoadRuntimeTexture(ctx, path);
        thumb.imguiId = thumb.gpuTexture.IsValid()
            ? ctx.editorTextureId(thumb.gpuTexture)
            : nullptr;
        thumb.fileStamp = stamp;
    }

    return thumb.imguiId ? &thumb : nullptr;
}

AssetBrowserState::AssetPreviewThumbnail* ResolveAssetPreviewThumbnail(
    AssetBrowserState& browser,
    const std::filesystem::path& path,
    AssetBrowserState::PreviewKind kind)
{
    constexpr uint32_t kAssetPreviewThumbnailSchemaVersion = 3u;
    const std::string key = path.lexically_normal().generic_string();
    const uint64_t stamp = FileStamp(path);
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

bool DrawAssetPreviewTile(EditorFrameContext& ctx,
                          AssetBrowserState& browser,
                          const std::filesystem::path& path,
                          AssetBrowserState::PreviewKind kind,
                          bool selected,
                          const char* fallbackText)
{
    (void)ctx;
    constexpr float kThumbSize = 72.f;
    constexpr float kLabelWidth = 104.f;
    constexpr float kTileHeight = 100.f;

    AssetBrowserState::AssetPreviewThumbnail* thumb =
        ResolveAssetPreviewThumbnail(browser, path, kind);

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
        const ImVec2 textSize = ImGui::CalcTextSize(fallbackText);
        drawList->AddText(ImVec2(previewMin.x + (kThumbSize - textSize.x) * 0.5f,
                                 previewMin.y + (kThumbSize - textSize.y) * 0.5f),
                          ImGui::GetColorU32(ImGuiCol_TextDisabled),
                          fallbackText);
    }

    drawList->AddRect(previewMin, previewMax, ImGui::GetColorU32(ImGuiCol_Border));
    if (selected)
        drawList->AddRect(previewMin, previewMax, IM_COL32(160, 200, 245, 255), 0.f, 0, 2.f);

    std::string label = path.stem().string();
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

void DrawTextureAssetTile(EditorFrameContext& ctx,
                          AssetBrowserState& browser,
                          const std::filesystem::path& path)
{
    constexpr float kThumbSize = 72.f;
    constexpr float kTileWidth = 104.f;

    ImGui::PushID(path.string().c_str());

    if (DrawRenameInputIfActive(ctx, browser, path))
    {
        ImGui::PopID();
        return;
    }

    ImGui::BeginGroup();

    AssetBrowserState::TextureThumbnail* thumb = ResolveTextureThumbnail(ctx, browser, path);

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

    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        AssignTexture(ctx, path);

    if (ImGui::BeginDragDropSource())
    {
        const std::string rel = ToRelativeAssetPath(ctx, path);
        ImGui::SetDragDropPayload("KROM_ASSET_TEXTURE", rel.c_str(), rel.size() + 1u);
        ImGui::TextUnformatted(path.filename().string().c_str());
        ImGui::EndDragDropSource();
    }

    if (ImGui::BeginPopupContextItem())
    {
        DrawAssetCreateMenu(ctx, browser, browser.currentDir);
        ImGui::Separator();
        if (ImGui::MenuItem("Textur zuweisen"))
            AssignTexture(ctx, path);
        ImGui::Separator();
        if (ImGui::MenuItem("Umbenennen"))
            StartRename(browser, path);
        ImGui::Separator();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.4f, 0.4f, 1.f));
        if (ImGui::MenuItem("Loeschen"))
        {
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }
        ImGui::PopStyleColor();
        ImGui::EndPopup();
    }

    const std::string name = path.stem().string();
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
        ? path.filename().string()
        : path.stem().string();
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
    browser.expandedDirs.insert(parentDir.string());
    StartRename(browser, newDir);
    return true;
}

static void DrawAssetCreateMenu(EditorFrameContext& ctx,
                                AssetBrowserState& browser,
                                const std::filesystem::path& directory)
{
    ImGui::TextDisabled("Erstellen in: %s", directory.filename().string().c_str());
    ImGui::Separator();
    if (ImGui::MenuItem("Material"))
        CreateMaterialAsset(ctx, directory);
    const bool canCreateScript = !ctx.currentProjectRoot.empty();
    if (!canCreateScript) ImGui::BeginDisabled();
    if (ImGui::MenuItem("C++ Script..."))
    {
        browser.createScriptDialogOpen = true;
        browser.createScriptDirectory = directory;
        std::memset(browser.createScriptNameBuffer, 0, sizeof(browser.createScriptNameBuffer));
        std::strncpy(browser.createScriptNameBuffer, "NewScript", sizeof(browser.createScriptNameBuffer) - 1);
    }
    if (!canCreateScript) ImGui::EndDisabled();
    if (ImGui::MenuItem("Szene..."))
    {
        ctx.state.newSceneNameBuffer.fill('\0');
        ctx.state.newSceneTargetDir  = directory.string();
        ctx.state.newSceneDialogOpen = true;
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Neuer Ordner"))
        CreateFolderInAssetBrowser(browser, directory);
}

static void CommitRename(EditorFrameContext& ctx, AssetBrowserState& browser)
{
    if (browser.renamingPath.empty()) return;

    const std::string oldStem(browser.renamingPath.stem().string());
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
                (newStem + browser.renamingPath.extension().string());

            if (std::filesystem::exists(newPath))
            {
                ctx.lastFileMessage = "Umbenennen fehlgeschlagen: '" + newStem + "' existiert bereits.";
            }
            else
            {
                std::error_code ec;
                std::filesystem::rename(browser.renamingPath, newPath, ec);
                if (ec)
                {
                    ctx.lastFileMessage = "Umbenennen fehlgeschlagen: " + ec.message();
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

static void LoadEditorIcons(EditorFrameContext& ctx, AssetBrowserState& browser)
{
    if (browser.icons.loaded || ctx.engineEditorDir.empty() || !ctx.assetPipeline)
        return;
    browser.icons.loaded = true;

    std::filesystem::path iconPath =
        std::filesystem::path(ctx.engineEditorDir) / "folder.png";
    if (!std::filesystem::exists(iconPath))
        iconPath = std::filesystem::path(ctx.engineEditorDir) / "file.png";
    if (!std::filesystem::exists(iconPath))
        return;

    browser.icons.folder = LoadRuntimeTexture(ctx, iconPath.string());
    if (browser.icons.folder.IsValid() && ctx.editorTextureId)
        browser.icons.folderImguiId = ctx.editorTextureId(browser.icons.folder);
}

// Zeichnet einen Ordner-Eintrag im Baum (rekursiv für Unterordner)
static void DrawFolderGlyph(AssetBrowserState& browser, const ImVec2& min, float size)
{
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImVec2 max(min.x + size, min.y + size);

    if (browser.icons.folderImguiId)
    {
        drawList->AddImage(reinterpret_cast<ImTextureID>(browser.icons.folderImguiId), min, max);
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

static bool DrawFolderSelectable(AssetBrowserState& browser,
                                 const std::filesystem::path& path,
                                 ImGuiSelectableFlags flags)
{
    const float iconSize = ImGui::GetTextLineHeight();
    const std::string label = std::string("   ") + path.filename().string();
    const bool selected = ImGui::Selectable(label.c_str(), false, flags);
    const ImVec2 rectMin = ImGui::GetItemRectMin();
    const ImVec2 rectMax = ImGui::GetItemRectMax();
    const ImVec2 iconMin(rectMin.x + 2.f,
                         rectMin.y + (rectMax.y - rectMin.y - iconSize) * 0.5f);
    DrawFolderGlyph(browser, iconMin, iconSize);
    return selected;
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
    const std::string key    = dir.string();
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
    if (browser.icons.folderImguiId)
    {
        ImGui::Image(reinterpret_cast<ImTextureID>(browser.icons.folderImguiId),
                     ImVec2(kIconSize, kIconSize));
        ImGui::SameLine();
    }

    const bool open = ImGui::TreeNodeEx(key.c_str(), flags, "%s",
                                        dir.filename().string().c_str());

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

        if (ImGui::MenuItem("Neuen Ordner erstellen"))
        {
            CreateFolderInAssetBrowser(browser, dir);
        }

        if (ImGui::MenuItem("Umbenennen"))
            StartRename(browser, dir);

        ImGui::Separator();

        // Ordner löschen (leer = sofort, nicht leer = mit Bestätigung)
        std::error_code ecEmpty;
        const bool isEmpty = std::filesystem::is_empty(dir, ecEmpty) && !ecEmpty;
        if (ImGui::MenuItem(isEmpty ? "Ordner loeschen" : "Ordner loeschen (mit Inhalt)..."))
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
            ImGui::SetTooltip("Loescht den Ordner und ALLEN Inhalt.\nEine Bestaetigung wird angefordert.");

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
        browser.expandedDirs.insert(browser.currentDir.string());
    }

    LoadEditorIcons(ctx, browser);

    // highlightPath: vom Material-Inspector angefordert → Verzeichnis öffnen.
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
                browser.expandedDirs.insert(p.string());
        }

        std::error_code ec;
        if (!std::filesystem::is_regular_file(highlight, ec))
            browser.highlightPath.clear();
        else
            browser.highlightPath = highlight;
    }

    ImGui::SetNextWindowPos(ImVec2(280.f, 540.f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(600.f, 220.f), ImGuiCond_FirstUseEver);
    ImGui::Begin("Assets##editor");

    // ── Tab-Leiste ────────────────────────────────────────────────────────────
    // Neue Tabs einfach hier mit ImGui::BeginTabItem(...) ergänzen.
    if (!ImGui::BeginTabBar("##AssetsTabs"))
    {
        ImGui::End();
        return;
    }

    const bool assetTabOpen   = ImGui::BeginTabItem("Assets");
    if (assetTabOpen) ImGui::EndTabItem();
    const bool consoleTabOpen = ImGui::BeginTabItem("Console");
    if (consoleTabOpen) ImGui::EndTabItem();
    ImGui::EndTabBar();

    // Console-Tab
    if (consoleTabOpen)
    {
        // Filter-Buttons
        static bool showInfo    = true;
        static bool showWarning = true;
        static bool showError   = true;
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.f, 2.f));
        ImGui::Checkbox("Info",    &showInfo);    ImGui::SameLine();
        ImGui::Checkbox("Warning", &showWarning); ImGui::SameLine();
        ImGui::Checkbox("Error",   &showError);   ImGui::SameLine();
        if (ImGui::SmallButton("Leeren"))
            ctx.state.consoleLogs.clear();
        ImGui::SameLine();
        ImGui::Checkbox("Auto-Scroll", &ctx.state.consoleAutoScroll);
        ImGui::PopStyleVar();
        ImGui::Separator();

        ImGui::BeginChild("##console_scroll", ImVec2(0.f, 0.f), false,
                          ImGuiWindowFlags_HorizontalScrollbar);
        for (const auto& entry : ctx.state.consoleLogs)
        {
            if (entry.level == engine::LogLevel::Info    && !showInfo)    continue;
            if (entry.level == engine::LogLevel::Warning && !showWarning) continue;
            if (entry.level == engine::LogLevel::Error   && !showError)   continue;
            if (entry.level == engine::LogLevel::Fatal   && !showError)   continue;

            ImVec4 col = ImVec4(0.85f, 0.85f, 0.85f, 1.f);
            if (entry.level == engine::LogLevel::Warning)
                col = ImVec4(1.f, 0.85f, 0.2f, 1.f);
            else if (entry.level >= engine::LogLevel::Error)
                col = ImVec4(1.f, 0.4f, 0.4f, 1.f);
            else if (entry.level == engine::LogLevel::Verbose)
                col = ImVec4(0.55f, 0.55f, 0.55f, 1.f);

            ImGui::PushStyleColor(ImGuiCol_Text, col);
            ImGui::TextUnformatted(entry.message.c_str());
            ImGui::PopStyleColor();

            std::filesystem::path diagnosticPath;
            int diagnosticLine = 0;
            if (TryParseBuildLocation(ctx, entry.message, diagnosticPath, diagnosticLine))
            {
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Doppelklick: %s:%d oeffnen",
                                      diagnosticPath.string().c_str(),
                                      diagnosticLine);
                if (ImGui::IsItemHovered() &&
                    ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                {
                    OpenAssetInExternalEditor(ctx, diagnosticPath, diagnosticLine);
                }
            }
        }
        if (ctx.state.consoleAutoScroll && ctx.state.consoleScrollToBottom)
        {
            ImGui::SetScrollHereY(1.f);
            ctx.state.consoleScrollToBottom = false;
        }
        ImGui::EndChild();
        ImGui::End();
        return;
    }

    if (!assetTabOpen)
    {
        ImGui::End();
        return;
    }

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
            const std::string seg = crumbs[i].filename().string().empty()
                ? crumbs[i].string() : crumbs[i].filename().string();
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
        ImGui::OpenPopup("Neue Szene##newscenedlg");
        ctx.state.newSceneDialogOpen = false;
    }
    if (ImGui::BeginPopupModal("Neue Szene##newscenedlg", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextUnformatted("Szenenname:");
        ImGui::SetNextItemWidth(260.f);
        const bool confirmed = ImGui::InputText(
            "##newscenename", ctx.state.newSceneNameBuffer.data(),
            ctx.state.newSceneNameBuffer.size(),
            ImGuiInputTextFlags_EnterReturnsTrue);
        const bool hasName = ctx.state.newSceneNameBuffer[0] != '\0';

        ImGui::Spacing();
        if (!hasName) ImGui::BeginDisabled();
        if ((ImGui::Button("Erstellen", ImVec2(120.f, 0.f)) || confirmed) && hasName)
        {
            const std::string name = ctx.state.newSceneNameBuffer.data();
            const std::string dir  = ctx.state.newSceneTargetDir;
            if (!dir.empty() && ctx.newEditorSceneInDir)
                ctx.newEditorSceneInDir(name, dir);
            else if (ctx.newEditorScene)
                ctx.newEditorScene(name);
            ImGui::CloseCurrentPopup();
        }
        if (!hasName) ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Abbrechen", ImVec2(120.f, 0.f)) ||
            ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // ── Bestätigungs-Dialog: nicht-leeren Ordner löschen ─────────────────────
    if (browser.createScriptDialogOpen)
    {
        ImGui::OpenPopup("C++ Script erstellen##createscriptdlg");
        browser.createScriptDialogOpen = false;
    }
    if (ImGui::BeginPopupModal("C++ Script erstellen##createscriptdlg",
                               nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextUnformatted("Klassenname:");
        ImGui::SetNextItemWidth(280.f);
        const bool confirmed = ImGui::InputText(
            "##newscriptname",
            browser.createScriptNameBuffer,
            sizeof(browser.createScriptNameBuffer),
            ImGuiInputTextFlags_EnterReturnsTrue);

        const std::string className = browser.createScriptNameBuffer;
        const bool validName = IsValidCppScriptClassName(className);
        if (!validName)
            ImGui::TextDisabled("Nur gueltige C++ Identifier, z.B. PlayerController.");

        ImGui::TextDisabled("Ziel: %s",
            browser.createScriptDirectory.empty()
                ? "(aktueller Ordner)"
                : browser.createScriptDirectory.string().c_str());

        ImGui::Spacing();
        if (!validName) ImGui::BeginDisabled();
        if ((ImGui::Button("Erstellen", ImVec2(120.f, 0.f)) || confirmed) && validName)
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
        if (ImGui::Button("Abbrechen", ImVec2(120.f, 0.f)) ||
            ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            ImGui::CloseCurrentPopup();

        ImGui::EndPopup();
    }

    if (!browser.deleteDirPending.empty())
    {
        ImGui::OpenPopup("OrdnerLoeschenBestaetigung##delbrowser");
    }
    if (ImGui::BeginPopupModal("OrdnerLoeschenBestaetigung##delbrowser",
                               nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextUnformatted("Ordner und gesamten Inhalt loeschen?");
        ImGui::TextColored(ImVec4(1.f, 0.7f, 0.3f, 1.f), "%s",
                           browser.deleteDirPending.filename().string().c_str());
        ImGui::TextDisabled("Dieser Schritt kann nicht rueckgaengig gemacht werden.");
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.7f, 0.1f, 0.1f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.9f, 0.2f, 0.2f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(1.0f, 0.3f, 0.3f, 1.f));
        if (ImGui::Button("Loeschen", ImVec2(120.f, 0.f)))
        {
            const std::filesystem::path dir = browser.deleteDirPending;
            browser.deleteDirPending.clear();
            std::error_code ecRm;
            std::filesystem::remove_all(dir, ecRm);
            if (!ecRm)
            {
                browser.currentDir = dir.parent_path();
                browser.expandedDirs.erase(dir.string());
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleColor(3);
        ImGui::SameLine();
        if (ImGui::Button("Abbrechen", ImVec2(120.f, 0.f)) ||
            ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        {
            browser.deleteDirPending.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // ── Alle Dateien im aktuellen Ordner ──────────────────────────────────────
    std::error_code ec;

    for (const auto& entry : std::filesystem::directory_iterator(browser.currentDir, ec))
    {
        if (!entry.is_directory(ec))
            continue;

        const std::filesystem::path p = entry.path();
        ImGui::PushID(p.string().c_str());

        if (!DrawRenameInputIfActive(ctx, browser, p))
        {
            if (DrawFolderSelectable(browser, p, ImGuiSelectableFlags_AllowDoubleClick) &&
                ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            {
                browser.currentDir = p;
                browser.expandedDirs.insert(p.parent_path().string());
            }

            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID))
            {
                const std::string absPath = p.string();
                ImGui::SetDragDropPayload("KROM_MOVE_DIR", absPath.c_str(), absPath.size() + 1u);
                ImGui::TextUnformatted(p.filename().string().c_str());
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
                if (ImGui::MenuItem("Oeffnen"))
                {
                    browser.currentDir = p;
                    browser.expandedDirs.insert(p.parent_path().string());
                }
                if (ImGui::MenuItem("Umbenennen"))
                    StartRename(browser, p);
                ImGui::Separator();
                std::error_code ecEmpty;
                const bool isEmpty = std::filesystem::is_empty(p, ecEmpty) && !ecEmpty;
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.4f, 0.4f, 1.f));
                if (ImGui::MenuItem(isEmpty ? "Loeschen" : "Loeschen (mit Inhalt)..."))
                {
                    if (isEmpty)
                    {
                        std::error_code ecRm;
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
    for (const auto& entry : std::filesystem::directory_iterator(browser.currentDir, ec))
    {
        const auto& p = entry.path();
        if (entry.is_directory(ec) || !IsSceneFile(p))
            continue;

        const std::string sceneName = p.stem().string();
        const bool isCurrent = (sceneName == ctx.currentEditorSceneName);
        const bool isLoaded  = !isCurrent &&
            std::find(ctx.loadedEditorScenes.begin(),
                      ctx.loadedEditorScenes.end(),
                      sceneName) != ctx.loadedEditorScenes.end();

        if (isCurrent)   ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.9f, 0.4f, 1.f));
        else if (isLoaded) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.85f, 0.6f, 1.f));

        ImGui::PushID(p.string().c_str());
        if (!DrawRenameInputIfActive(ctx, browser, p))
        {
            std::string label = "[SCENE] " + sceneName;
            if (isCurrent)     label += " *";
            else if (isLoaded) label += " +";

            if (ImGui::Selectable(label.c_str(), isCurrent || isLoaded,
                                  ImGuiSelectableFlags_AllowDoubleClick) &&
                ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !isCurrent &&
                ctx.switchEditorScene)
                ctx.switchEditorScene(sceneName, /*additive=*/false);

            // Drag-Source: Datei verschieben
            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID))
            {
                const std::string absPath = p.string();
                ImGui::SetDragDropPayload("KROM_MOVE_FILE", absPath.c_str(), absPath.size() + 1u);
                ImGui::TextUnformatted(p.filename().string().c_str());
                ImGui::EndDragDropSource();
            }

            if (ImGui::BeginPopupContextItem())
            {
                DrawAssetCreateMenu(ctx, browser, browser.currentDir);
                ImGui::Separator();
                if (ImGui::MenuItem("Laden", nullptr, false, !isCurrent) && ctx.switchEditorScene)
                    ctx.switchEditorScene(sceneName, false);
                if (ImGui::MenuItem("Additiv laden", nullptr, false, !isLoaded) && ctx.switchEditorScene)
                    ctx.switchEditorScene(sceneName, true);
                ImGui::Separator();
                if (ImGui::MenuItem("Umbenennen")) StartRename(browser, p);
                ImGui::Separator();
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.4f, 0.4f, 1.f));
                if (ImGui::MenuItem("Loschen"))
                {
                    if (ctx.deleteEditorSceneByPath)
                        ctx.deleteEditorSceneByPath(p.string());
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
    for (const auto& entry : std::filesystem::directory_iterator(browser.currentDir, ec))
    {
        const auto& p = entry.path();
        if (!entry.is_directory(ec) && IsSupportedMaterial(p) && !IsEditorInternalAsset(p))
        {
            const std::string materialPath = ToRelativeAssetPath(ctx, p);
            const bool isSelected = ctx.state.selectedMaterialAssetPath == materialPath;

            ImGui::PushID(p.string().c_str());

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
                    if (ImGui::MenuItem("Im Materialeditor oeffnen"))
                    {
                        ctx.state.selectedEntity = NULL_ENTITY;
                        QueueOpenMaterialAsset(ctx, p);
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem("Umbenennen"))
                        StartRename(browser, p);
                    ImGui::Separator();
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.4f, 0.4f, 1.f));
                    if (ImGui::MenuItem("Loeschen"))
                    {
                        std::error_code ec;
                        std::filesystem::remove(p, ec);
                    }
                    ImGui::PopStyleColor();
                    ImGui::EndPopup();
                }

                if (ImGui::BeginDragDropSource())
                {
                    ImGui::SetDragDropPayload("KROM_ASSET_MATERIAL", materialPath.c_str(), materialPath.size() + 1u);
                    ImGui::TextUnformatted(p.filename().string().c_str());
                    ImGui::EndDragDropSource();
                }

                materialTileRowUsed += assetTileWidth;
            }

            ImGui::PopID();
        }
    }
    // ── Shader-Dateien ────────────────────────────────────────────────────────
    for (const auto& entry : std::filesystem::directory_iterator(browser.currentDir, ec))
    {
        const auto& p = entry.path();
        if (!entry.is_directory(ec) && IsSupportedShader(p))
        {
            ImGui::PushID(p.string().c_str());

            if (!DrawRenameInputIfActive(ctx, browser, p))
            {
                // Shader-Typ im Label anzeigen
                const auto ext = p.extension().string();
                const char* tag = "[SHD]";
                if (ext == ".vs"   || p.stem().extension() == ".vs")   tag = "[VS ]";
                else if (ext == ".ps"   || p.stem().extension() == ".ps")   tag = "[PS ]";
                else if (ext == ".vert" || p.stem().extension() == ".vert") tag = "[VS ]";
                else if (ext == ".frag" || p.stem().extension() == ".frag") tag = "[FS ]";
                else if (ext == ".comp")                                     tag = "[CS ]";
                else if (p.filename().string().find(".vs.") != std::string::npos) tag = "[VS ]";
                else if (p.filename().string().find(".ps.") != std::string::npos) tag = "[PS ]";
                else if (p.filename().string().find(".fs.") != std::string::npos) tag = "[FS ]";

                const std::string label = std::string(tag) + " " + p.stem().string();
                ImGui::Selectable(label.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick);

                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", p.filename().string().c_str());

                if (ImGui::BeginPopupContextItem("##shader_asset_popup"))
                {
                    DrawAssetCreateMenu(ctx, browser, browser.currentDir);
                    ImGui::Separator();
                    if (ImGui::MenuItem("Umbenennen"))
                        StartRename(browser, p);
                    ImGui::Separator();
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.4f, 0.4f, 1.f));
                    if (ImGui::MenuItem("Loeschen"))
                    {
                        std::error_code delEc;
                        std::filesystem::remove(p, delEc);
                    }
                    ImGui::PopStyleColor();
                    ImGui::EndPopup();
                }

                // Drag-Drop: absoluter Pfad als KROM_MOVE_FILE
                // Der Material-Editor akzeptiert diesen Payload und prueft die Extension
                if (ImGui::BeginDragDropSource())
                {
                    const std::string absPath = p.string();
                    ImGui::SetDragDropPayload("KROM_MOVE_FILE", absPath.c_str(), absPath.size() + 1u);
                    ImGui::TextUnformatted(p.filename().string().c_str());
                    ImGui::EndDragDropSource();
                }
            }

            ImGui::PopID();
        }
    }

    // ── 3D-Modelle ────────────────────────────────────────────────────────────
    for (const auto& entry : std::filesystem::directory_iterator(browser.currentDir, ec))
    {
        const auto& p = entry.path();
        if (!entry.is_directory(ec) && IsSupportedCppScriptSource(p))
        {
            ImGui::PushID(p.string().c_str());

            if (!DrawRenameInputIfActive(ctx, browser, p))
            {
                const std::string label = std::string("[C++] ") + p.filename().string();
                ImGui::Selectable(label.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick);

                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", p.string().c_str());

                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                    OpenAssetInExternalEditor(ctx, p);

                if (ImGui::BeginPopupContextItem("##cpp_script_asset_popup"))
                {
                    DrawAssetCreateMenu(ctx, browser, browser.currentDir);
                    ImGui::Separator();
                    if (ImGui::MenuItem("Extern oeffnen"))
                        OpenAssetInExternalEditor(ctx, p);
                    ImGui::Separator();
                    if (ImGui::MenuItem("Script-Bindings neu generieren"))
                    {
                        const EditorScriptAssetResult result = RefreshCppScriptProject(ctx);
                        ctx.lastFileMessage = result.message;
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem("Umbenennen"))
                        StartRename(browser, p);
                    ImGui::Separator();
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.4f, 0.4f, 1.f));
                    if (ImGui::MenuItem("Loeschen"))
                    {
                        std::error_code delEc;
                        std::filesystem::remove(p, delEc);
                        if (delEc)
                        {
                            ctx.lastFileMessage = "Loeschen fehlgeschlagen: " + delEc.message();
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
                    const std::string absPath = p.string();
                    ImGui::SetDragDropPayload("KROM_MOVE_FILE", absPath.c_str(), absPath.size() + 1u);
                    ImGui::TextUnformatted(p.filename().string().c_str());
                    ImGui::EndDragDropSource();
                }
            }

            ImGui::PopID();
        }
    }

    float modelTileRowUsed = 0.f;
    const float modelTileAvail = std::max(ImGui::GetContentRegionAvail().x, assetTileWidth);
    for (const auto& entry : std::filesystem::directory_iterator(browser.currentDir, ec))
    {
        const auto& p = entry.path();
        if (!entry.is_directory(ec) && IsSupportedModel(p) && !IsEditorInternalAsset(p))
        {
            ImGui::PushID(p.string().c_str());

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
                if (clicked && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                    SpawnModel(ctx, p);

                if (ImGui::BeginDragDropSource())
                {
                    const std::string absPath = p.string();
                    ImGui::SetDragDropPayload("KROM_MOVE_FILE", absPath.c_str(), absPath.size() + 1u);
                    ImGui::TextUnformatted(p.filename().string().c_str());
                    ImGui::EndDragDropSource();
                }

                if (ImGui::BeginPopupContextItem("##model_asset_popup"))
                {
                    DrawAssetCreateMenu(ctx, browser, browser.currentDir);
                    ImGui::Separator();
                    if (ImGui::MenuItem("In Szene laden"))
                        SpawnModel(ctx, p);
                    ImGui::Separator();
                    if (ImGui::MenuItem("Umbenennen"))
                        StartRename(browser, p);
                    ImGui::Separator();
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.4f, 0.4f, 1.f));
                    if (ImGui::MenuItem("Loeschen"))
                    {
                        std::error_code ec;
                        std::filesystem::remove(p, ec);
                    }
                    ImGui::PopStyleColor();
                    ImGui::EndPopup();
                }

                modelTileRowUsed += assetTileWidth;
            }

            ImGui::PopID();
        }
    }

    float prefabTileRowUsed = 0.f;
    const float prefabTileAvail = std::max(ImGui::GetContentRegionAvail().x, assetTileWidth);
    for (const auto& entry : std::filesystem::directory_iterator(browser.currentDir, ec))
    {
        const auto& p = entry.path();
        if (!entry.is_directory(ec) && IsSupportedPrefab(p))
        {
            const std::string prefabPath = ToRelativeAssetPath(ctx, p);
            const bool isSelected = ctx.state.selectedPrefabAssetPath == prefabPath;
            ImGui::PushID(p.string().c_str());

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
                        engine::addons::editor::DrawPrefabEditorWindow(ctx, p);
                }

                if (ImGui::BeginDragDropSource())
                {
                    ImGui::SetDragDropPayload("KROM_ASSET_PREFAB",
                                              prefabPath.c_str(),
                                              prefabPath.size() + 1u);
                    ImGui::TextUnformatted(p.filename().string().c_str());
                    ImGui::EndDragDropSource();
                }

                if (ImGui::BeginPopupContextItem("##prefab_asset_popup"))
                {
                    DrawAssetCreateMenu(ctx, browser, browser.currentDir);
                    ImGui::Separator();
                    if (ImGui::MenuItem("Bearbeiten"))
                        engine::addons::editor::DrawPrefabEditorWindow(ctx, p);
                    if (ImGui::MenuItem("In Szene laden"))
                        SpawnPrefab(ctx, p);
                    ImGui::Separator();
                    if (ImGui::MenuItem("Umbenennen"))
                        StartRename(browser, p);
                    ImGui::Separator();
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.4f, 0.4f, 1.f));
                    if (ImGui::MenuItem("Loeschen"))
                    {
                        std::error_code delEc;
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
    for (const auto& entry : std::filesystem::directory_iterator(browser.currentDir, ec))
    {
        const auto& p = entry.path();
        if (!entry.is_directory(ec) && IsSupportedTexture(p))
        {
            if (!textureHeaderDrawn)
            {
                ImGui::SeparatorText("Texturen");
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

    ImGui::EndChild(); // ##content_panel
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
                                    file.string().c_str(), dest.string().c_str(), error.c_str());
    }

    if (importedCount > 0u)
    {
        browser.currentDir = targetDir;
        browser.initialized = true;
        ctx.lastFileMessage = "Importiert: " + std::to_string(importedCount) +
            " Datei(en)" + (skippedCount > 0u ? ", uebersprungen: " + std::to_string(skippedCount) : "");
    }
    else if (skippedCount > 0u)
    {
        ctx.lastFileMessage = "Keine unterstuetzten Dateien fuer den Asset-Import gefunden.";
    }
}

void PreloadAllTextures(EditorFrameContext& ctx, AssetBrowserState& browser)
{
    if (!ctx.assetPipeline || !ctx.editorTextureId)
        return;

    const std::filesystem::path assetRoot = ctx.assetPipeline->GetAssetRoot();
    if (assetRoot.empty() || !std::filesystem::exists(assetRoot))
        return;

    uint32_t count = 0u;
    std::error_code ec;
    for (const auto& entry :
         std::filesystem::recursive_directory_iterator(assetRoot, ec))
    {
        if (entry.is_directory(ec))
            continue;
        const auto& p = entry.path();
        if (!IsSupportedTexture(p))
            continue;

        // GPU-Textur laden (gecacht in assetPipeline)
        const TextureHandle tex = LoadRuntimeTexture(ctx, p.string());
        if (!tex.IsValid())
            continue;

        // ImGui-Thumbnail-ID vorberechnen und im Browser-Cache speichern
        void* imguiId = ctx.editorTextureId(tex);
        if (!imguiId)
            continue;

        const uint64_t stamp = [&]() -> uint64_t {
            std::error_code ec2;
            const auto t = std::filesystem::last_write_time(p, ec2);
            if (ec2) return 0u;
            return static_cast<uint64_t>(
                t.time_since_epoch().count());
        }();

        AssetBrowserState::TextureThumbnail thumb{};
        thumb.gpuTexture = tex;
        thumb.imguiId    = imguiId;
        thumb.fileStamp  = stamp;
        browser.textureThumbnails[p.string()] = thumb;
        ++count;
    }

    if (count > 0u)
        Debug::Log("AssetBrowser: %u Texturen vorgeladen.", count);
}

} // namespace engine::renderer::addons::editor

#else // KROM_EDITOR_HAS_IMGUI

namespace engine::renderer::addons::editor {
void DrawAssetBrowser(EditorFrameContext&, AssetBrowserState&) {}
void ImportExternalFilesToAssetBrowser(EditorFrameContext&, AssetBrowserState&, const std::vector<std::filesystem::path>&) {}
} // namespace engine::renderer::addons::editor

#endif // KROM_EDITOR_HAS_IMGUI
