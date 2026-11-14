#include "addons/editor/EditorCard.hpp"
#include "addons/editor/EditorMaterialLibrary.hpp"
#include "addons/editor/EditorMaterialGraph.hpp"
#include "addons/editor/EditorUIInternal.hpp"
#include "core/PathUtf8.hpp"
#include "addons/editor/EditorValueClipboard.hpp"

#include "addons/editor/EditorFileNaming.hpp"
#include "addons/editor/EditorAssetBrowser.hpp"
#include "addons/editor/EditorFeature.hpp"
#include "addons/editor/EditorPreferences.hpp"

#ifdef KROM_EDITOR_HAS_IMGUI

#include "addons/lit/LitMaterial.hpp"
#include "addons/mesh_renderer/MeshRendererComponents.hpp"
#include "renderer/MaterialGraphBinding.hpp"
#include "addons/pbr/PbrInstanceBuilder.hpp"
#include "addons/pbr/PbrMasterMaterial.hpp"
#include "addons/pbr/PbrSlot.hpp"
#include "addons/unlit/UnlitMaterial.hpp"
#include "assets/AssetPipeline.hpp"
#include "assets/MaterialProperties.hpp"
#include "assets/MeshTangents.hpp"
#include "assets/VertexLayoutBridge.hpp"
#include "core/Logger.hpp"
#include "ecs/Components.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include "platform/NativeFileDialog.hpp"
#include "renderer/ShaderBindingModel.hpp"
#include "renderer/ShaderConstantBufferReflection.hpp"
#include "renderer/VertexLayoutContract.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace engine::renderer::addons::editor {

// Vorwaertsdeklaration (oeffentlich) — wird von Funktionen in der anonymen Namespace
// genutzt; eigentliche Definition steht nach dem Ende der anonymen Namespace.
bool SaveSelectedMaterialAsset(EditorFrameContext& ctx);

namespace {

using assets::TextureLoadRole;

const char* MaterialTemplateName(EditorMaterialTemplate materialTemplate) noexcept
{
    switch (materialTemplate)
    {
    case EditorMaterialTemplate::PbrLit:    return "PBR Lit";
    case EditorMaterialTemplate::Unlit:     return "Unlit";
    case EditorMaterialTemplate::LegacyLit: return "Legacy Lit";
    case EditorMaterialTemplate::Custom:    return "Custom";
    default:                                return "Material";
    }
}

const char* MaterialTemplateToken(EditorMaterialTemplate materialTemplate) noexcept
{
    switch (materialTemplate)
    {
    case EditorMaterialTemplate::PbrLit:    return "pbr-lit";
    case EditorMaterialTemplate::Unlit:     return "unlit";
    case EditorMaterialTemplate::LegacyLit: return "legacy-lit";
    case EditorMaterialTemplate::Custom:    return "custom";
    default:                                return "pbr-lit";
    }
}

EditorMaterialTemplate ParseMaterialTemplate(const std::string& value) noexcept
{
    if (value == "unlit")       return EditorMaterialTemplate::Unlit;
    if (value == "legacy-lit")  return EditorMaterialTemplate::LegacyLit;
    if (value == "custom")      return EditorMaterialTemplate::Custom;
    return EditorMaterialTemplate::PbrLit;
}

bool SaveMaterialAssetFileAbsolute(const std::filesystem::path& absolutePath,
                                   const assets::MaterialAsset& asset);

void SetDefaultActiveTextureSlot(EditorFrameContext& ctx, const assets::MaterialAsset& asset);

std::string Trim(const std::string& s)
{
    size_t start = 0u;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start])))
        ++start;

    size_t end = s.size();
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1u])))
        --end;

    return s.substr(start, end - start);
}

std::vector<std::string> SplitWs(const std::string& s)
{
    std::vector<std::string> parts;
    std::string current;
    for (char ch : s)
    {
        if (std::isspace(static_cast<unsigned char>(ch)))
        {
            if (!current.empty())
            {
                parts.push_back(current);
                current.clear();
            }
        }
        else
        {
            current.push_back(ch);
        }
    }

    if (!current.empty())
        parts.push_back(current);
    return parts;
}

std::string RestAfterFirstToken(const std::string& line)
{
    const size_t pos = line.find_first_of(" \t\r\n");
    if (pos == std::string::npos)
        return {};
    return Trim(line.substr(pos + 1u));
}

bool IsSupportedTexturePath(const std::filesystem::path& path)
{
    const std::string ext = PathToUtf8(path.extension());
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" ||
           ext == ".dds" || ext == ".ktx" || ext == ".ktx2" ||
           ext == ".bmp" || ext == ".hdr";
}

const char* MaterialParamToken(assets::MaterialParam::Type type) noexcept
{
    switch (type)
    {
    case assets::MaterialParam::Type::Float:   return "float";
    case assets::MaterialParam::Type::Vec2:    return "vec2";
    case assets::MaterialParam::Type::Vec3:    return "vec3";
    case assets::MaterialParam::Type::Vec4:    return "vec4";
    case assets::MaterialParam::Type::Int:     return "int";
    case assets::MaterialParam::Type::Bool:    return "bool";
    case assets::MaterialParam::Type::Texture: return "texture";
    default:                                   return "float";
    }
}

bool IsBuiltInMaterialParamName(const std::string& name) noexcept
{
    static constexpr const char* kNames[] = {
        "baseColorFactor", "emissiveFactor", "metallicFactor", "roughnessFactor",
        "normalStrength", "normalScale", "occlusionStrength", "opacityFactor",
        "alphaCutoff", "uvScale", "uvOffset", "materialFeatureMask", "materialModel",
        "albedo", "baseColor", "baseColorMap", "normal", "normalMap", "orm",
        "ormMap", "emissive", "emissiveMap", "tAlbedo", "tNormal", "tORM",
        "tEmissive", "uAlbedo", "uNormal", "uORM", "uEmissive", "sLinear",
        "sLinearWrap"
    };
    if (!name.empty() && name[0] == '_')
        return true;
    for (const char* builtIn : kNames)
        if (name == builtIn)
            return true;
    return false;
}

assets::MaterialParam* FindAssetParam(std::vector<assets::MaterialParam>& params,
                                      const std::string& name) noexcept
{
    for (assets::MaterialParam& param : params)
        if (param.name == name)
            return &param;
    return nullptr;
}

renderer::MaterialParam ToRuntimeParam(const assets::MaterialParam& src)
{
    renderer::MaterialParam dst{};
    dst.name = src.name;
    switch (src.type)
    {
    case assets::MaterialParam::Type::Float:   dst.type = renderer::MaterialParam::Type::Float; break;
    case assets::MaterialParam::Type::Vec2:    dst.type = renderer::MaterialParam::Type::Vec2; break;
    case assets::MaterialParam::Type::Vec3:    dst.type = renderer::MaterialParam::Type::Vec3; break;
    case assets::MaterialParam::Type::Vec4:    dst.type = renderer::MaterialParam::Type::Vec4; break;
    case assets::MaterialParam::Type::Int:     dst.type = renderer::MaterialParam::Type::Int; break;
    case assets::MaterialParam::Type::Bool:    dst.type = renderer::MaterialParam::Type::Bool; break;
    case assets::MaterialParam::Type::Texture: dst.type = renderer::MaterialParam::Type::Texture; break;
    }
    switch (src.type)
    {
    case assets::MaterialParam::Type::Float:
    case assets::MaterialParam::Type::Vec2:
    case assets::MaterialParam::Type::Vec3:
    case assets::MaterialParam::Type::Vec4:
        dst.value.f[0] = src.value.f[0];
        dst.value.f[1] = src.value.f[1];
        dst.value.f[2] = src.value.f[2];
        dst.value.f[3] = src.value.f[3];
        break;
    case assets::MaterialParam::Type::Int:
        dst.value.i = src.value.i;
        break;
    case assets::MaterialParam::Type::Bool:
        dst.value.b = src.value.b;
        break;
    case assets::MaterialParam::Type::Texture:
        break;
    }
    dst.texture = src.texture;
    return dst;
}

std::filesystem::path AssetRoot(const EditorFrameContext& ctx)
{
    if (ctx.assetPipeline)
        return ctx.assetPipeline->GetAssetRoot();
    return std::filesystem::current_path() / "assets";
}

bool TryMakeAssetRelative(const std::filesystem::path& assetRoot,
                          const std::filesystem::path& candidate,
                          std::string& outRelative)
{
    if (candidate.empty())
        return false;

    const std::filesystem::path normalizedRoot = assetRoot.lexically_normal();
    const std::filesystem::path normalizedCandidate = candidate.lexically_normal();
    std::error_code ec;
    const std::filesystem::path relative = std::filesystem::relative(normalizedCandidate, normalizedRoot, ec);
    if (ec || relative.empty())
        return false;

    const std::string generic = PathToGenericUtf8(relative);
    if (generic.rfind("..", 0u) == 0u)
        return false;

    outRelative = generic;
    return true;
}

std::string NormalizeMaterialAssetPath(const EditorFrameContext& ctx, const std::filesystem::path& inputPath)
{
    if (inputPath.empty())
        return {};

    // Engine editor assets are not project assets. In particular, never rebase
    // editor/dpad_*.mat onto a same-named project material: the gizmo compares
    // its engine path every frame and would otherwise rebind the entire scene.
    if (inputPath.is_absolute() && !ctx.engineEditorDir.empty())
    {
        const auto normalized = inputPath.lexically_normal();
        const auto editorRelative = normalized.lexically_relative(
            PathFromUtf8(ctx.engineEditorDir).lexically_normal());
        if (!editorRelative.empty() && !editorRelative.is_absolute() &&
            *editorRelative.begin() != "..")
            return PathToGenericUtf8(normalized);
    }

    const std::filesystem::path assetRoot = AssetRoot(ctx).lexically_normal();
    std::string relativePath;

    const std::filesystem::path directCandidate = inputPath.is_absolute()
        ? inputPath.lexically_normal()
        : (assetRoot / inputPath).lexically_normal();
    if (TryMakeAssetRelative(assetRoot, directCandidate, relativePath))
        return relativePath;

    const std::filesystem::path cwdCandidate = inputPath.is_absolute()
        ? inputPath.lexically_normal()
        : std::filesystem::absolute(inputPath).lexically_normal();
    if (TryMakeAssetRelative(assetRoot, cwdCandidate, relativePath))
        return relativePath;

    auto trySuffixFromSegment = [&](const char* segmentName) -> bool
    {
        std::filesystem::path suffix;
        bool collect = false;
        for (const auto& part : cwdCandidate)
        {
            std::string segment = PathToUtf8(part);
            std::transform(segment.begin(), segment.end(), segment.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            if (collect)
                suffix /= part;
            else if (segment == segmentName)
                collect = true;
        }

        if (suffix.empty())
            return false;

        const std::filesystem::path rebased = (assetRoot / suffix).lexically_normal();
        if (!std::filesystem::exists(rebased))
            return false;
        return TryMakeAssetRelative(assetRoot, rebased, relativePath);
    };

    if (trySuffixFromSegment("assets"))
        return relativePath;
    if (trySuffixFromSegment("bin"))
        return relativePath;

    // Dateiname-Fallback: direkt im Asset-Root
    const std::filesystem::path filenameCandidate = assetRoot / inputPath.filename();
    if (!inputPath.filename().empty() && std::filesystem::exists(filenameCandidate) &&
        TryMakeAssetRelative(assetRoot, filenameCandidate, relativePath))
        return relativePath;

    // Rekursive Suche nach dem Dateinamen im Asset-Root-Baum (greift wenn die Datei
    // in einen Unterordner verschoben wurde und der alte Pfad nicht mehr existiert).
    if (!inputPath.filename().empty())
    {
        std::error_code ec;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(assetRoot, ec))
        {
            if (ec) break;
            if (!entry.is_regular_file(ec)) continue;
            if (entry.path().filename() != inputPath.filename()) continue;
            if (TryMakeAssetRelative(assetRoot, entry.path(), relativePath))
                return relativePath;
        }
    }

    return PathToGenericUtf8(inputPath);
}

std::string ToAssetRelativePath(const EditorFrameContext& ctx, const std::filesystem::path& absolutePath)
{
    if (absolutePath.empty())
        return {};
    return NormalizeMaterialAssetPath(ctx, absolutePath);
}

std::filesystem::path ToAbsoluteAssetPath(const EditorFrameContext& ctx, const std::string& relativePath)
{
    const std::filesystem::path path = PathFromUtf8(relativePath);
    return path.is_absolute() ? path : (AssetRoot(ctx) / path);
}

std::string ReplaceSuffix(const std::string& value, const char* suffix, const char* replacement)
{
    const size_t suffixLen = std::char_traits<char>::length(suffix);
    if (value.size() < suffixLen || value.compare(value.size() - suffixLen, suffixLen, suffix) != 0)
        return {};
    return value.substr(0u, value.size() - suffixLen) + replacement;
}

std::string ResolveRuntimeShaderPath(const EditorFrameContext& ctx,
                                     const std::string& shaderPath,
                                     assets::ShaderStage stage)
{
    if (ctx.shaderTarget != assets::ShaderTargetProfile::OpenGL_GLSL450)
        return shaderPath;

    std::string candidate;
    if (stage == assets::ShaderStage::Vertex)
        candidate = ReplaceSuffix(shaderPath, ".vs.hlsl", ".opengl.vs.glsl");
    else if (stage == assets::ShaderStage::Fragment)
        candidate = ReplaceSuffix(shaderPath, ".ps.hlsl", ".opengl.fs.glsl");
    if (candidate.empty())
        return shaderPath;

    if (std::filesystem::exists(ToAbsoluteAssetPath(ctx, candidate)))
        return candidate;
    return shaderPath;
}

ShaderHandle LoadRuntimeShader(EditorFrameContext& ctx,
                               const std::string& shaderPath,
                               assets::ShaderStage stage)
{
    if (!ctx.assetPipeline)
        return ShaderHandle::Invalid();
    return ctx.assetPipeline->LoadShader(ResolveRuntimeShaderPath(ctx, shaderPath, stage), stage);
}

std::filesystem::path ResolveEngineShaderPath(const EditorFrameContext& ctx, const char* filename)
{
    const std::filesystem::path path = PathFromUtf8(filename);
    if (path.is_absolute() || ctx.engineAssetRoot.empty())
        return path;
    return (PathFromUtf8(ctx.engineAssetRoot) / path).lexically_normal();
}

bool LoadLitShaders(EditorFrameContext& ctx,
                    ShaderHandle& outVS, ShaderHandle& outFS, ShaderHandle& outShadow,
                    bool skinned,
                    ShaderHandle* outShadowFS)
{
    if (!ctx.assetPipeline)
        return false;

    const bool opengl = ctx.shaderTarget == assets::ShaderTargetProfile::OpenGL_GLSL450;
    const char* vsPath       = opengl ? "lit.opengl.vs.glsl" : (skinned ? "skinned_lit.vs.hlsl" : "lit.vs.hlsl");
    const char* fsPath       = opengl ? "lit.opengl.fs.glsl" : "lit.ps.hlsl";
    const char* shadowVsPath = opengl ? "shadow.opengl.vs.glsl"
                             : (skinned ? "skinned_shadow.vs.hlsl" : "shadow.vs.hlsl");
    const char* shadowFsPath = opengl ? "shadow.opengl.fs.glsl" : "shadow.ps.hlsl";

    outVS     = ctx.assetPipeline->LoadShader(PathToUtf8(ResolveEngineShaderPath(ctx, vsPath)), assets::ShaderStage::Vertex);
    outFS     = ctx.assetPipeline->LoadShader(PathToUtf8(ResolveEngineShaderPath(ctx, fsPath)), assets::ShaderStage::Fragment);
    outShadow = ctx.assetPipeline->LoadShader(PathToUtf8(ResolveEngineShaderPath(ctx, shadowVsPath)), assets::ShaderStage::Vertex);
    if (outShadowFS)
        *outShadowFS = ctx.assetPipeline->LoadShader(PathToUtf8(ResolveEngineShaderPath(ctx, shadowFsPath)), assets::ShaderStage::Fragment);
    return outVS.IsValid() && outFS.IsValid() && outShadow.IsValid();
}

bool LoadPbrShaders(EditorFrameContext& ctx,
                    ShaderHandle& outVS, ShaderHandle& outFS, ShaderHandle& outShadow,
                    ShaderHandle* outShadowFS, bool skinned)
{
    if (!ctx.assetPipeline)
        return false;

    const bool opengl = ctx.shaderTarget == assets::ShaderTargetProfile::OpenGL_GLSL450;
    const bool forwardPlus = ctx.forwardPlusActive && !opengl;
    const char* fsPath = opengl ? "pbr_lit.opengl.fs.glsl" : (forwardPlus ? "pbr_forward_plus.ps.hlsl" : "pbr_lit.ps.hlsl");
    const char* vsPath = opengl ? "pbr_lit.opengl.vs.glsl"
                       : (skinned ? "skinned_pbr_lit.vs.hlsl" : "pbr_lit.vs.hlsl");
    const char* shadowVsPath = opengl ? "shadow_pbr.opengl.vs.glsl"
                             : (skinned ? "skinned_shadow_pbr.vs.hlsl" : "shadow_pbr.vs.hlsl");
    outVS     = ctx.assetPipeline->LoadShader(PathToUtf8(ResolveEngineShaderPath(ctx, vsPath)),
                                              assets::ShaderStage::Vertex);
    outFS     = ctx.assetPipeline->LoadShader(PathToUtf8(ResolveEngineShaderPath(ctx, fsPath)),
                                              assets::ShaderStage::Fragment);
    outShadow = ctx.assetPipeline->LoadShader(PathToUtf8(ResolveEngineShaderPath(ctx, shadowVsPath)),
                                              assets::ShaderStage::Vertex);
    if (outShadowFS)
        *outShadowFS = ctx.assetPipeline->LoadShader(PathToUtf8(ResolveEngineShaderPath(ctx, opengl ? "shadow_pbr.opengl.fs.glsl" : "shadow_pbr.ps.hlsl")),
                                                     assets::ShaderStage::Fragment);
    return outVS.IsValid() && outFS.IsValid() && outShadow.IsValid();
}

bool LoadUnlitShaders(EditorFrameContext& ctx, ShaderHandle& outVS, ShaderHandle& outFS, bool skinned = false)
{
    if (!ctx.assetPipeline)
        return false;

    const bool opengl = ctx.shaderTarget == assets::ShaderTargetProfile::OpenGL_GLSL450;
    const char* vsPath = opengl ? "quad_unlit.opengl.vs.glsl"
                       : (skinned ? "skinned_unlit.vs.hlsl" : "quad_unlit.vs.hlsl");
    outVS = ctx.assetPipeline->LoadShader(PathToUtf8(ResolveEngineShaderPath(ctx, vsPath)),
                                          assets::ShaderStage::Vertex);
    outFS = ctx.assetPipeline->LoadShader(PathToUtf8(ResolveEngineShaderPath(ctx, opengl ? "quad_unlit.opengl.fs.glsl" : "quad_unlit.ps.hlsl")),
                                          assets::ShaderStage::Fragment);
    return outVS.IsValid() && outFS.IsValid();
}

bool EnsureTangentsForPbr(assets::MeshAsset& mesh)
{
    bool changed = false;
    for (assets::SubMeshData& submesh : mesh.submeshes)
    {
        const uint32_t vc = static_cast<uint32_t>(submesh.positions.size() / 3u);

        // UVs fehlen → synthetische (0,0) einsetzen damit PbrLit-VertexContract erfüllt ist.
        if (submesh.uvs.size() < vc * 2u)
        {
            submesh.uvs.assign(vc * 2u, 0.f);
            changed = true;
        }

        if (assets::HasValidTangents(submesh))
            continue;
        if (!assets::EnsureTangents(submesh))
        {
            // Tangenten konnten nicht berechnet werden (z.B. keine Indices).
            // Standard-Tangenten einsetzen damit PBR trotzdem lädt.
            submesh.tangents.assign(vc * 4u, 0.f);
            for (uint32_t i = 0; i < vc; ++i)
            {
                submesh.tangents[i * 4u + 0u] = 1.f; // x
                submesh.tangents[i * 4u + 3u] = 1.f; // handedness
            }
            Debug::LogWarning("EnsureTangentsForPbr: Tangenten/UVs konnten fuer Submesh nicht berechnet werden — Fallback. Normal-Map auf diesem Submesh deaktiviert.");
        }
        submesh.rawInterleavedBytes.clear();
        submesh.rawVertexStride = 0u;
        changed = true;
    }

    if (changed)
    {
        mesh.gpuStatus.dirty = true;
        mesh.gpuStatus.uploaded = false;
    }
    return true;
}

bool ResolveMeshLayout(assets::MeshAsset& mesh,
                       const renderer::VertexLayoutContract& contract,
                       renderer::VertexLayout& outLayout,
                       const char* errorPrefix)
{
    if (mesh.submeshes.empty())
        return false;

    std::string layoutError;
    outLayout = assets::ResolveVertexLayout(contract, mesh.submeshes[0], &layoutError);
    if (outLayout.attributes.empty())
    {
        Debug::LogError("%s: VertexLayout fehlgeschlagen: %s", errorPrefix, layoutError.c_str());
        return false;
    }
    return true;
}

struct SharedMaterialBindingKey
{
    std::string materialPath;
    MeshHandle  mesh;

    bool operator==(const SharedMaterialBindingKey& other) const noexcept
    {
        return materialPath == other.materialPath && mesh == other.mesh;
    }
};

struct SharedMaterialBindingKeyHash
{
    size_t operator()(const SharedMaterialBindingKey& key) const noexcept
    {
        size_t h = std::hash<std::string>{}(key.materialPath);
        h ^= std::hash<MeshHandle>{}(key.mesh) + 0x9e3779b9u + (h << 6u) + (h >> 2u);
        return h;
    }
};

struct SharedMaterialBinding
{
    MaterialHandle runtimeMaterial = MaterialHandle::Invalid();
    uint64_t       structureHash   = 0u;
    std::string    baseColorPath;
    std::string    ormPath;
    std::string    normalPath;
    std::string    emissivePath;
    std::array<std::string, assets::kMaterialGraphMaxTextures> graphTexturePaths;
    std::array<TextureHandle, assets::kMaterialGraphMaxTextures> graphTextures{};
    TextureHandle  baseColorTexture = TextureHandle::Invalid();
    TextureHandle  ormTexture       = TextureHandle::Invalid();
    TextureHandle  normalTexture    = TextureHandle::Invalid();
    TextureHandle  emissiveTexture  = TextureHandle::Invalid();
    // Verhindert endlose Retry-Schleife wenn der Build fehlschlug
    bool           buildFailed      = false;
    bool           isPbr            = false;
};

std::unordered_map<SharedMaterialBindingKey, SharedMaterialBinding, SharedMaterialBindingKeyHash>& SharedMaterialBindings()
{
    static std::unordered_map<SharedMaterialBindingKey, SharedMaterialBinding, SharedMaterialBindingKeyHash> storage;
    return storage;
}

uint64_t HashMaterialStructure(const assets::MaterialAsset& asset, bool graphActive,
                               const std::string& ormPath, const std::string& normalPath) noexcept
{
    uint64_t h = std::hash<std::string>{}(asset.templateName);
    auto combine = [&](uint64_t value)
    {
        h ^= value + 0x9e3779b97f4a7c15ull + (h << 6u) + (h >> 2u);
    };
    auto combineStr = [&](const std::string& s)
    {
        combine(std::hash<std::string>{}(s));
    };

    combine(static_cast<uint64_t>(asset.alphaMode));
    combine(asset.doubleSided ? 1ull : 0ull);
    combine(asset.castShadows ? 1ull : 0ull);
    combine(asset.instanced ? 1ull : 0ull);
    combine(graphActive ? 1ull : 0ull);
    combineStr(asset.vertexShaderPath);
    combineStr(asset.fragmentShaderPath);

    // Vollständige Pfade hashen — nicht nur ob gesetzt.
    // Materialien mit denselben Texturtypes aber VERSCHIEDENEN Pfaden
    // (z.B. Material_Boden.mat vs Block.mat) erhalten sonst identische
    // Hashes → needsRebuild feuert fälschlicherweise nicht → falsche Texturen.
    combineStr(asset.baseColorTexture.path);
    combineStr(ormPath);
    combineStr(normalPath);
    combineStr(asset.emissiveTexture.path);
    for (const assets::MaterialParam& param : asset.params)
    {
        combineStr(param.name);
        combine(static_cast<uint64_t>(param.type));
        if (param.type == assets::MaterialParam::Type::Texture)
            combineStr(param.texturePath);
    }
    return h;
}

TextureHandle LoadRuntimeTexture(EditorFrameContext& ctx, const std::string& path,
                                 TextureLoadRole role = TextureLoadRole::Auto)
{
    if (!ctx.assetPipeline || path.empty())
        return TextureHandle::Invalid();

    const TextureHandle assetTexture = ctx.assetPipeline->LoadTexture(path, role);
    if (!assetTexture.IsValid())
        return TextureHandle::Invalid();

    ctx.assetPipeline->UploadPendingGpuAssets();
    return ctx.assetPipeline->GetGpuTexture(assetTexture);
}

// Inspector thumbnails must not reload/check files and scan all pending GPU assets
// at render frequency. Keep the asset handle, but resolve its current GPU handle
// every frame so a hot-reloaded texture is never displayed through a stale handle.
TextureHandle LoadInspectorThumbnail(EditorFrameContext& ctx, const std::string& path)
{
    if (!ctx.assetPipeline || path.empty())
        return TextureHandle::Invalid();
    struct Entry { TextureHandle asset; double nextCheck = 0.0; };
    static std::unordered_map<std::string, Entry> cache;
    static const assets::AssetPipeline* pipeline = nullptr;
    static std::string projectRoot;
    if (pipeline != ctx.assetPipeline || projectRoot != ctx.currentProjectRoot)
    {
        cache.clear();
        pipeline = ctx.assetPipeline;
        projectRoot = ctx.currentProjectRoot;
    }
    Entry& entry = cache[path];
    const double now = ImGui::GetTime();
    const auto* asset = ctx.registry.textures.Get(entry.asset);
    if (now >= entry.nextCheck || (entry.asset.IsValid() && (!asset || asset->path != path)))
    {
        entry.asset = ctx.assetPipeline->LoadTexture(path);
        entry.nextCheck = now + 0.5;
    }
    if (!entry.asset.IsValid())
        return TextureHandle::Invalid();
    return ctx.assetPipeline->GetGpuTexture(entry.asset);
}

void UpdateCachedTexture(EditorFrameContext& ctx,
                         const std::string& texturePath,
                         std::string& cachedPath,
                         TextureHandle& cachedTexture,
                         TextureLoadRole role = TextureLoadRole::Auto)
{
    // Pfadgleichheit reicht nicht aus: AssetPipeline/Hot-Reload kann die
    // GPU-Textur unter demselben Asset-Pfad neu erzeugen. In diesem Fall
    // muss das Runtime-Material den aktuellen GPU-Handle neu holen, sonst
    // bleiben stale Handles in Albedo/Normal/ORM Slots hängen.
    if (texturePath.empty())
    {
        cachedPath.clear();
        cachedTexture = TextureHandle::Invalid();
        return;
    }

    const TextureHandle refreshed = LoadRuntimeTexture(ctx, texturePath, role);
    if (cachedPath == texturePath && cachedTexture.IsValid() && !refreshed.IsValid())
        return;

    if (cachedPath == texturePath && cachedTexture == refreshed && cachedTexture.IsValid())
        return;

    cachedPath = texturePath;
    cachedTexture = refreshed;
}

assets::MaterialAsset* LoadMaterialAsset(EditorFrameContext& ctx, const std::string& relativePath)
{
    if (!ctx.assetPipeline || relativePath.empty())
        return nullptr;
    const MaterialHandle handle = ctx.assetPipeline->LoadMaterial(relativePath);
    if (!handle.IsValid())
        return nullptr;
    return ctx.registry.materials.Get(handle);
}

bool ReloadSelectedMaterialAsset(EditorFrameContext& ctx)
{
    assets::MaterialAsset* asset = LoadMaterialAsset(ctx, ctx.state.selectedMaterialAssetPath);
    if (!asset)
    {
        ctx.state.selectedMaterialAssetLoaded = false;
        ctx.state.selectedMaterialAssetDirty = false;
        return false;
    }

    ctx.state.selectedMaterialAssetData = *asset;
    ctx.state.selectedMaterialAssetPath = NormalizeMaterialAssetPath(ctx, PathFromUtf8(ctx.state.selectedMaterialAssetPath));
    ctx.state.selectedMaterialAssetNameDraft =
        PathToUtf8(PathFromUtf8(ctx.state.selectedMaterialAssetPath).stem());
    ctx.state.selectedMaterialAssetLoaded = true;
    ctx.state.selectedMaterialAssetDirty = false;
    return true;
}

bool ParseEditorMaterialAssetSource(const std::string& source,
                                    const std::string& normalizedPath,
                                    assets::MaterialAsset& outAsset)
{
    assets::MaterialAsset loaded;
    loaded.path = normalizedPath;
    loaded.debugName = PathToUtf8(PathFromUtf8(normalizedPath).filename());
    loaded.state = assets::AssetState::Loaded;

    std::istringstream in(source);
    std::string line;
    while (std::getline(in, line))
    {
        line = Trim(line);
        if (line.empty() || line[0] == '#')
            continue;

        const auto parts = SplitWs(line);
        if (parts.empty())
            continue;

        if (parts[0] == "graphOutputs" || parts[0] == "graphNode") loaded.graph.Read(line);
        else if (parts[0] == "template" && parts.size() >= 2) loaded.templateName = parts[1];
        else if (parts[0] == "vertex" && parts.size() >= 2) loaded.vertexShaderPath = RestAfterFirstToken(line);
        else if (parts[0] == "fragment" && parts.size() >= 2) loaded.fragmentShaderPath = RestAfterFirstToken(line);
        else if (parts[0] == "transparent" && parts.size() >= 2) loaded.transparent = (parts[1] == "1" || parts[1] == "true");
        else if (parts[0] == "doubleSided" && parts.size() >= 2) loaded.doubleSided = (parts[1] == "1" || parts[1] == "true");
        else if (parts[0] == "castShadows" && parts.size() >= 2) loaded.castShadows = (parts[1] == "1" || parts[1] == "true");
        else if (parts[0] == "instanced" && parts.size() >= 2) loaded.instanced = (parts[1] == "1" || parts[1] == "true");
        else if (parts[0] == "baseColorFactor" && parts.size() >= 5)
        {
            loaded.baseColorFactor = {
                std::stof(parts[1]), std::stof(parts[2]), std::stof(parts[3]), std::stof(parts[4])
            };
        }
        else if (parts[0] == "emissiveFactor" && parts.size() >= 4)
        {
            loaded.emissiveFactor = {
                std::stof(parts[1]), std::stof(parts[2]), std::stof(parts[3])
            };
        }
        else if (parts[0] == "metallicFactor" && parts.size() >= 2) loaded.metallicFactor = std::stof(parts[1]);
        else if (parts[0] == "roughnessFactor" && parts.size() >= 2) loaded.roughnessFactor = std::stof(parts[1]);
        else if (parts[0] == "alphaCutoff" && parts.size() >= 2) loaded.alphaCutoff = std::stof(parts[1]);
        else if (parts[0] == "normalScale" && parts.size() >= 2) loaded.normalScale = std::stof(parts[1]);
        else if (parts[0] == "occlusionStrength" && parts.size() >= 2) loaded.occlusionStrength = std::stof(parts[1]);
        else if (parts[0] == "emissiveStrength" && parts.size() >= 2) loaded.emissiveStrength = std::stof(parts[1]);
        else if (parts[0] == "uvScale" && parts.size() >= 3) loaded.uvScale = { std::stof(parts[1]), std::stof(parts[2]) };
        else if (parts[0] == "uvOffset" && parts.size() >= 3) loaded.uvOffset = { std::stof(parts[1]), std::stof(parts[2]) };
        else if (parts[0] == "uvProjection" && parts.size() >= 2) loaded.boxProjection = parts[1] == "box";
        else if (parts[0] == "boxTileSize" && parts.size() >= 2) loaded.boxTileSize = std::stof(parts[1]);
        else if (parts[0] == "alphaMode" && parts.size() >= 2)
        {
            if (parts[1] == "mask") loaded.alphaMode = assets::MaterialAlphaMode::Mask;
            else if (parts[1] == "blend") loaded.alphaMode = assets::MaterialAlphaMode::Blend;
            else loaded.alphaMode = assets::MaterialAlphaMode::Opaque;
        }
        else if (parts[0] == "baseColorTexture") loaded.baseColorTexture.path = RestAfterFirstToken(line);
        else if (parts[0] == "metallicRoughnessTexture") loaded.metallicRoughnessTexture.path = RestAfterFirstToken(line);
        else if (parts[0] == "normalTexture") loaded.normalTexture.path = RestAfterFirstToken(line);
        else if (parts[0] == "occlusionTexture") loaded.occlusionTexture.path = RestAfterFirstToken(line);
        else if (parts[0] == "emissiveTexture") loaded.emissiveTexture.path = RestAfterFirstToken(line);
        else if (parts[0] == "float" && parts.size() >= 3)
        {
            assets::MaterialParam p{}; p.name = parts[1]; p.type = assets::MaterialParam::Type::Float; p.value.f[0] = std::stof(parts[2]); loaded.params.push_back(p);
        }
        else if (parts[0] == "vec2" && parts.size() >= 4)
        {
            assets::MaterialParam p{}; p.name = parts[1]; p.type = assets::MaterialParam::Type::Vec2;
            p.value.f[0] = std::stof(parts[2]); p.value.f[1] = std::stof(parts[3]); loaded.params.push_back(p);
        }
        else if (parts[0] == "vec3" && parts.size() >= 5)
        {
            assets::MaterialParam p{}; p.name = parts[1]; p.type = assets::MaterialParam::Type::Vec3;
            p.value.f[0] = std::stof(parts[2]); p.value.f[1] = std::stof(parts[3]); p.value.f[2] = std::stof(parts[4]); loaded.params.push_back(p);
        }
        else if (parts[0] == "vec4" && parts.size() >= 6)
        {
            assets::MaterialParam p{}; p.name = parts[1]; p.type = assets::MaterialParam::Type::Vec4;
            p.value.f[0] = std::stof(parts[2]); p.value.f[1] = std::stof(parts[3]); p.value.f[2] = std::stof(parts[4]); p.value.f[3] = std::stof(parts[5]); loaded.params.push_back(p);
        }
        else if (parts[0] == "int" && parts.size() >= 3)
        {
            assets::MaterialParam p{}; p.name = parts[1]; p.type = assets::MaterialParam::Type::Int; p.value.i = static_cast<int32_t>(std::stoi(parts[2])); loaded.params.push_back(p);
        }
        else if (parts[0] == "bool" && parts.size() >= 3)
        {
            assets::MaterialParam p{}; p.name = parts[1]; p.type = assets::MaterialParam::Type::Bool; p.value.b = (parts[2] == "1" || parts[2] == "true"); loaded.params.push_back(p);
        }
        else if (parts[0] == "texture" && parts.size() >= 3)
        {
            assets::MaterialParam p{}; p.name = parts[1]; p.type = assets::MaterialParam::Type::Texture; p.texturePath = parts[2]; loaded.params.push_back(p);
        }
    }
    // Older PBR graphs saw the material-transformed mesh UV (not with box projection).
    if (loaded.templateName == "pbr-lit" && !loaded.boxProjection)
        loaded.graph.AdoptMaterialUV(loaded.uvScale, loaded.uvOffset);
    loaded.graph.rawMeshUV = true;

    outAsset.path                    = std::move(loaded.path);
    outAsset.debugName                = std::move(loaded.debugName);
    outAsset.state                    = loaded.state;
    outAsset.lastModifiedTimestamp    = loaded.lastModifiedTimestamp;
    outAsset.templateName             = std::move(loaded.templateName);
    outAsset.vertexShader             = loaded.vertexShader;
    outAsset.fragmentShader           = loaded.fragmentShader;
    outAsset.vertexShaderPath         = std::move(loaded.vertexShaderPath);
    outAsset.fragmentShaderPath       = std::move(loaded.fragmentShaderPath);
    outAsset.params                   = std::move(loaded.params);
    outAsset.gpuStatus                = loaded.gpuStatus;
    outAsset.transparent              = loaded.transparent;
    outAsset.doubleSided              = loaded.doubleSided;
    outAsset.castShadows              = loaded.castShadows;
    outAsset.instanced                = loaded.instanced;
    outAsset.baseColorFactor          = loaded.baseColorFactor;
    outAsset.metallicFactor           = loaded.metallicFactor;
    outAsset.roughnessFactor          = loaded.roughnessFactor;
    outAsset.emissiveFactor           = loaded.emissiveFactor;
    outAsset.emissiveStrength         = loaded.emissiveStrength;
    outAsset.alphaCutoff              = loaded.alphaCutoff;
    outAsset.alphaMode                = loaded.alphaMode;
    outAsset.baseColorTexture         = std::move(loaded.baseColorTexture);
    outAsset.metallicRoughnessTexture = std::move(loaded.metallicRoughnessTexture);
    outAsset.normalTexture            = std::move(loaded.normalTexture);
    outAsset.occlusionTexture         = std::move(loaded.occlusionTexture);
    outAsset.emissiveTexture          = std::move(loaded.emissiveTexture);
    outAsset.normalScale              = loaded.normalScale;
    outAsset.occlusionStrength        = loaded.occlusionStrength;
    outAsset.uvScale                  = loaded.uvScale;
    outAsset.uvOffset                 = loaded.uvOffset;
    outAsset.graph                    = std::move(loaded.graph);
    outAsset.boxProjection            = loaded.boxProjection;
    outAsset.boxTileSize              = loaded.boxTileSize;
    outAsset.params.erase(std::remove_if(outAsset.params.begin(), outAsset.params.end(),
        [](const assets::MaterialParam& p) { return IsBuiltInMaterialParamName(p.name); }),
        outAsset.params.end());
    return true;
}

jobs::ValueResult<EditorAsyncMaterialLoadResult> LoadMaterialAssetForEditorAsync(
    const std::filesystem::path& absolutePath,
    const std::string& normalizedPath)
{
    std::ifstream in(absolutePath, std::ios::binary);
    if (!in)
        return jobs::ValueResult<EditorAsyncMaterialLoadResult>::Fail("material file open failed");

    std::string source((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
    assets::MaterialAsset asset;
    if (!ParseEditorMaterialAssetSource(source, normalizedPath, asset))
        return jobs::ValueResult<EditorAsyncMaterialLoadResult>::Fail("material parse failed");

    return jobs::ValueResult<EditorAsyncMaterialLoadResult>::Success(
        EditorAsyncMaterialLoadResult{ normalizedPath, std::move(asset) });
}

void StartAsyncSelectedMaterialAssetLoad(EditorFrameContext& ctx, const std::string& normalizedPath)
{
    ctx.state.selectedMaterialAssetPath = normalizedPath;
    ctx.state.selectedMaterialAssetNameDraft = PathToUtf8(PathFromUtf8(normalizedPath).stem());
    ctx.state.selectedMaterialAssetLoaded = false;
    ctx.state.selectedMaterialAssetDirty = false;

    if (!ctx.jobSystem || !ctx.jobSystem->IsParallel())
    {
        ReloadSelectedMaterialAsset(ctx);
        if (ctx.state.selectedMaterialAssetLoaded)
        {
            ctx.state.newMaterialTemplate = ParseMaterialTemplate(ctx.state.selectedMaterialAssetData.templateName);
            SetDefaultActiveTextureSlot(ctx, ctx.state.selectedMaterialAssetData);
        }
        return;
    }

    if (ctx.state.materialAssetLoadInFlight)
    {
        ctx.state.queuedMaterialAssetPath = normalizedPath;
        return;
    }

    const std::filesystem::path absolutePath = ToAbsoluteAssetPath(ctx, normalizedPath);
    ctx.state.materialAssetLoadInFlight = true;
    ctx.state.materialAssetLoadFuture = std::move(ctx.jobSystem->DispatchReturn(
        [absolutePath, normalizedPath]() -> EditorAsyncMaterialLoadResult {
            auto result = LoadMaterialAssetForEditorAsync(absolutePath, normalizedPath);
            if (!result.Succeeded())
                throw std::runtime_error(result.task.errorMessage ? result.task.errorMessage
                                                                  : "material async load failed");
            return std::move(*result.value);
        }));
}

void PollAsyncSelectedMaterialAssetLoad(EditorFrameContext& ctx)
{
    if (!ctx.state.materialAssetLoadInFlight || !ctx.state.materialAssetLoadFuture.valid())
        return;

    if (ctx.state.materialAssetLoadFuture.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        return;

    const auto result = ctx.state.materialAssetLoadFuture.get();
    ctx.state.materialAssetLoadInFlight = false;

    if (result.Succeeded())
    {
        EditorAsyncMaterialLoadResult loaded = std::move(*result.value);
        if (NormalizeMaterialAssetPath(ctx, PathFromUtf8(ctx.state.selectedMaterialAssetPath)) == loaded.normalizedPath)
        {
            ctx.state.selectedMaterialAssetData = std::move(loaded.asset);
            ctx.state.selectedMaterialAssetPath = loaded.normalizedPath;
            ctx.state.selectedMaterialAssetNameDraft =
                PathToUtf8(PathFromUtf8(loaded.normalizedPath).stem());
            ctx.state.selectedMaterialAssetLoaded = true;
            ctx.state.selectedMaterialAssetDirty = false;
            ctx.state.newMaterialTemplate = ParseMaterialTemplate(ctx.state.selectedMaterialAssetData.templateName);
            SetDefaultActiveTextureSlot(ctx, ctx.state.selectedMaterialAssetData);
        }
    }

    if (!ctx.state.queuedMaterialAssetPath.empty())
    {
        const std::string queuedPath = ctx.state.queuedMaterialAssetPath;
        ctx.state.queuedMaterialAssetPath.clear();
        if (NormalizeMaterialAssetPath(ctx, PathFromUtf8(ctx.state.selectedMaterialAssetPath)) != queuedPath ||
            !ctx.state.selectedMaterialAssetLoaded)
        {
            StartAsyncSelectedMaterialAssetLoad(ctx, queuedPath);
        }
    }
}

bool IsSelectedMaterialAssetAlreadyLoaded(const EditorFrameContext& ctx,
                                          const std::filesystem::path& path)
{
    if (!ctx.state.selectedMaterialAssetLoaded)
        return false;

    const std::string normalizedCandidate = NormalizeMaterialAssetPath(ctx, path);
    const std::string normalizedSelected =
        NormalizeMaterialAssetPath(ctx, PathFromUtf8(ctx.state.selectedMaterialAssetPath));
    return !normalizedCandidate.empty() && normalizedCandidate == normalizedSelected;
}

void SetDefaultActiveTextureSlot(EditorFrameContext& ctx, const assets::MaterialAsset& asset)
{
    const EditorMaterialTemplate materialTemplate = ParseMaterialTemplate(asset.templateName);
    ctx.state.activeTextureParamName = materialTemplate == EditorMaterialTemplate::PbrLit ? "albedo" : "albedo";
}

bool SaveMaterialAssetFile(const EditorFrameContext& ctx,
                           const std::string& relativePath,
                           const assets::MaterialAsset& asset)
{
    const std::filesystem::path absolutePath = ToAbsoluteAssetPath(ctx, relativePath);
    return SaveMaterialAssetFileAbsolute(absolutePath, asset);
}

std::string SerializeMaterialOverride(const assets::MaterialAsset& asset)
{
    return assets::SerializeMaterialProperties(asset);
}

bool SaveMaterialAssetFileAbsolute(const std::filesystem::path& absolutePath,
                                   const assets::MaterialAsset& asset)
{
    std::error_code ec;
    std::filesystem::create_directories(absolutePath.parent_path(), ec);
    if (ec)
        return false;

    std::ofstream out(absolutePath, std::ios::binary | std::ios::trunc);
    if (!out)
        return false;

    out << SerializeMaterialOverride(asset);
    return out.good();
}

class MaterialSaveQueue
{
public:
    enum class SaveState : uint8_t
    {
        Idle,
        Pending,
        Saving,
        Saved,
        Failed
    };

    struct StatusSnapshot
    {
        SaveState state = SaveState::Idle;
        std::chrono::steady_clock::time_point lastUpdate{};
    };

    void Enqueue(const std::filesystem::path& absolutePath,
                 const assets::MaterialAsset& snapshot)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        StartWorkerIfNeeded();

        const std::string key = PathToGenericUtf8(absolutePath.lexically_normal());
        PendingSave& pending = m_pending[key];
        pending.absolutePath = absolutePath.lexically_normal();
        pending.snapshot = snapshot;
        pending.dueTime = std::chrono::steady_clock::now() + std::chrono::milliseconds(350);
        m_status[key] = StatusSnapshot{ SaveState::Pending, std::chrono::steady_clock::now() };
        m_wakeCv.notify_all();
    }

    [[nodiscard]] StatusSnapshot Query(const std::filesystem::path& absolutePath)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const std::string key = PathToGenericUtf8(absolutePath.lexically_normal());
        if (auto it = m_status.find(key); it != m_status.end())
            return it->second;
        return {};
    }

    ~MaterialSaveQueue()
    {
        Shutdown();
    }

private:
    struct PendingSave
    {
        std::filesystem::path absolutePath;
        assets::MaterialAsset snapshot;
        std::chrono::steady_clock::time_point dueTime{};
    };

    void StartWorkerIfNeeded()
    {
        if (m_started)
            return;

        m_stop = false;
        m_worker = std::thread([this]() { WorkerLoop(); });
        m_started = true;
    }

    void Shutdown()
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_started)
                return;
            m_stop = true;
            m_wakeCv.notify_all();
        }

        if (m_worker.joinable())
            m_worker.join();
        m_started = false;
    }

    void WorkerLoop()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        for (;;)
        {
            if (m_stop && m_pending.empty())
                break;

            if (m_pending.empty())
            {
                m_wakeCv.wait(lock, [&]() { return m_stop || !m_pending.empty(); });
                continue;
            }

            auto nextIt = std::min_element(
                m_pending.begin(), m_pending.end(),
                [](const auto& a, const auto& b) { return a.second.dueTime < b.second.dueTime; });
            const auto wakeTime = nextIt->second.dueTime;
            m_wakeCv.wait_until(lock, wakeTime, [&]() { return m_stop; });

            const auto now = std::chrono::steady_clock::now();
            std::vector<PendingSave> ready;
            for (auto it = m_pending.begin(); it != m_pending.end();)
            {
                if (m_stop || it->second.dueTime <= now)
                {
                    ready.push_back(std::move(it->second));
                    it = m_pending.erase(it);
                }
                else
                {
                    ++it;
                }
            }

            lock.unlock();
            for (const PendingSave& save : ready)
            {
                {
                    std::lock_guard<std::mutex> statusLock(m_mutex);
                    m_status[PathToGenericUtf8(save.absolutePath.lexically_normal())] =
                        StatusSnapshot{ SaveState::Saving, std::chrono::steady_clock::now() };
                }

                const bool ok = SaveMaterialAssetFileAbsolute(save.absolutePath, save.snapshot);
                {
                    std::lock_guard<std::mutex> statusLock(m_mutex);
                    m_status[PathToGenericUtf8(save.absolutePath.lexically_normal())] =
                        StatusSnapshot{ ok ? SaveState::Saved : SaveState::Failed,
                                        std::chrono::steady_clock::now() };
                }

                if (!ok)
                    Debug::LogError("Editor-Material: async save fehlgeschlagen '%s'",
                                    PathToUtf8(save.absolutePath).c_str());
            }
            lock.lock();
        }
    }

    std::mutex m_mutex;
    std::condition_variable m_wakeCv;
    std::unordered_map<std::string, PendingSave> m_pending;
    std::unordered_map<std::string, StatusSnapshot> m_status;
    std::thread m_worker;
    bool m_started = false;
    bool m_stop = false;
};

MaterialSaveQueue& AsyncMaterialSaveQueue()
{
    static MaterialSaveQueue queue;
    return queue;
}

MaterialSaveQueue::StatusSnapshot QueryMaterialSaveStatus(const EditorFrameContext& ctx,
                                                          const std::string& relativePath)
{
    if (relativePath.empty())
        return {};
    return AsyncMaterialSaveQueue().Query(ToAbsoluteAssetPath(ctx, relativePath));
}

void QueueMaterialAssetSave(const EditorFrameContext& ctx,
                            const std::string& relativePath,
                            const assets::MaterialAsset& asset)
{
    if (relativePath.empty())
        return;
    AsyncMaterialSaveQueue().Enqueue(ToAbsoluteAssetPath(ctx, relativePath), asset);
}

MaterialHandle CreatePbrMaterialFromAsset(EditorFrameContext& ctx,
                                          assets::MeshAsset& mesh,
                                          const assets::MaterialAsset& asset,
                                          const char* name)
{
    if (!EnsureTangentsForPbr(mesh))
    {
        Debug::LogError("Editor-Material: PBR braucht Tangenten");
        return MaterialHandle::Invalid();
    }

    const bool skinned = mesh.HasSkinning();
    renderer::VertexLayout layout{};
    if (!ResolveMeshLayout(mesh, skinned ? renderer::VertexContracts::SkinnedLit()
                                         : renderer::VertexContracts::PbrLit(),
                           layout, "Editor-Material"))
        return MaterialHandle::Invalid();

    ShaderHandle vs, fs, shadow, shadowFs;
    if (!LoadPbrShaders(ctx, vs, fs, shadow, &shadowFs, skinned))
        return MaterialHandle::Invalid();

    renderer::pbr::PbrMasterMaterial::Config config{};
    config.vs             = vs;
    config.fs             = fs;
    config.shadow         = shadow;
    config.shadowFs       = shadowFs;
    config.vertexLayout   = layout;
    config.cullMode       = asset.doubleSided ? MaterialCullMode::None : MaterialCullMode::Back;
    config.castShadows    = asset.castShadows;
    config.receiveShadows = true;
    config.materialGraph  = assets::CompileMaterialGraph(asset.graph).Active();
    config.openGL         = ctx.shaderTarget == assets::ShaderTargetProfile::OpenGL_GLSL450;

    renderer::pbr::PbrMasterMaterial master =
        renderer::pbr::PbrMasterMaterial::Create(ctx.materials, config);
    if (!master.IsValid())
        return MaterialHandle::Invalid();

    TextureHandle baseColorTex = LoadRuntimeTexture(ctx, asset.baseColorTexture.path);
    TextureHandle ormTex       = LoadRuntimeTexture(ctx, asset.PbrOrmPath(), TextureLoadRole::LinearData);
    TextureHandle normalTex    = LoadRuntimeTexture(ctx, asset.PbrNormalPath(), TextureLoadRole::NormalMap);
    TextureHandle emissiveTex  = LoadRuntimeTexture(ctx, asset.emissiveTexture.path);

    auto builder = master.CreateInstance(name);
    if (baseColorTex.IsValid()) builder.BaseColor(baseColorTex);
    else                        builder.BaseColor(asset.baseColorFactor);

    if (ormTex.IsValid())
    {
        builder.Roughness(ormTex, pbr::MaterialChannel::G, asset.PbrRoughness());
        builder.Metallic(ormTex, pbr::MaterialChannel::B, asset.PbrMetallic());
        builder.Occlusion(ormTex, pbr::MaterialChannel::R, asset.PbrOcclusion());
    }
    else
    {
        builder.Roughness(asset.PbrRoughness());
        builder.Metallic(asset.PbrMetallic());
        builder.Occlusion(asset.PbrOcclusion());
    }

    if (normalTex.IsValid())
        builder.Normal(normalTex, asset.PbrNormalStrength());
    if (emissiveTex.IsValid())
        builder.Emissive(emissiveTex);
    else
    {
        const math::Vec3 emissive = asset.RuntimeEmissive();
        builder.Emissive(emissive.x, emissive.y, emissive.z);
    }

    builder.Opacity(asset.baseColorFactor.w)
           .DoubleSided(asset.doubleSided)
           .IBL(false);

    if (asset.alphaMode == assets::MaterialAlphaMode::Mask)
        builder.AlphaTest(asset.alphaCutoff);

    return builder.Build();
}

MaterialHandle CreateUnlitMaterialFromAsset(EditorFrameContext& ctx,
                                            assets::MeshAsset& mesh,
                                            const assets::MaterialAsset& asset,
                                            const char* name)
{
    const bool skinned = mesh.HasSkinning();
    renderer::VertexLayout layout{};
    if (!ResolveMeshLayout(mesh, skinned ? renderer::VertexContracts::SkinnedUnlit()
                                         : renderer::VertexContracts::StaticLit(),
                           layout, "Editor-Material"))
        return MaterialHandle::Invalid();

    ShaderHandle vs, fs;
    if (!LoadUnlitShaders(ctx, vs, fs, skinned))
        return MaterialHandle::Invalid();

    renderer::unlit::UnlitMaterialCreateInfo info{};
    info.name               = name;
    info.vertexShader       = vs;
    info.fragmentShader     = fs;
    info.vertexLayout       = layout;
    info.baseColorFactor    = asset.baseColorFactor;
    info.emissiveFactor     = {asset.RuntimeEmissive().x, asset.RuntimeEmissive().y, asset.RuntimeEmissive().z, 1.f};
    info.opacityFactor      = asset.baseColorFactor.w;
    info.alphaCutoff        = asset.alphaCutoff;
    info.enableBaseColorMap = !asset.baseColorTexture.path.empty();
    info.enableEmissiveMap  = !asset.emissiveTexture.path.empty();
    info.alphaTest          = asset.alphaMode == assets::MaterialAlphaMode::Mask;
    info.doubleSided        = asset.doubleSided;
    info.castShadows        = asset.castShadows;
    info.instanced          = asset.instanced;
    info.cullMode           = asset.doubleSided ? MaterialCullMode::None : MaterialCullMode::Back;
    info.materialGraph      = assets::CompileMaterialGraph(asset.graph).Active();
    info.openGL             = ctx.shaderTarget == assets::ShaderTargetProfile::OpenGL_GLSL450;

    const MaterialHandle handle = renderer::unlit::UnlitMaterial::Register(ctx.materials, info);
    if (!handle.IsValid())
        return handle;

    if (const TextureHandle albedo = LoadRuntimeTexture(ctx, asset.baseColorTexture.path); albedo.IsValid())
        ctx.materials.SetTexture(handle, "albedo", albedo);
    if (const TextureHandle emissive = LoadRuntimeTexture(ctx, asset.emissiveTexture.path); emissive.IsValid())
        ctx.materials.SetTexture(handle, "emissive", emissive);
    ctx.materials.MarkDirty(handle);
    return handle;
}

MaterialHandle CreateLegacyLitMaterialFromAsset(EditorFrameContext& ctx,
                                                assets::MeshAsset& mesh,
                                                const assets::MaterialAsset& asset,
                                                const char* name)
{
    const std::string& normalPath = asset.PbrNormalPath();
    const bool hasNormalMap = !normalPath.empty();
    // skinned_lit.vs.hlsl reads tangents: generate them like the PBR path does.
    const bool skinned = mesh.HasSkinning() && EnsureTangentsForPbr(mesh);
    renderer::VertexLayout layout{};
    if (!ResolveMeshLayout(mesh, skinned      ? renderer::VertexContracts::SkinnedLit()
                               : hasNormalMap ? renderer::VertexContracts::PbrLit()
                                              : renderer::VertexContracts::StaticLit(),
                           layout, "Editor-Material"))
        return MaterialHandle::Invalid();

    ShaderHandle vs, fs, shadow, shadowFs;
    if (!LoadLitShaders(ctx, vs, fs, shadow, skinned, &shadowFs))
        return MaterialHandle::Invalid();

    renderer::lit::LitMaterialCreateInfo info{};
    info.name                 = name;
    info.vertexShader         = vs;
    info.fragmentShader       = fs;
    info.shadowShader         = shadow;
    info.shadowFragmentShader = shadowFs;
    info.vertexLayout         = layout;
    info.baseColorFactor      = asset.baseColorFactor;
    info.emissiveFactor       = {asset.RuntimeEmissive().x, asset.RuntimeEmissive().y, asset.RuntimeEmissive().z, 1.f};
    info.specularStrength     = asset.PbrMetallic();
    info.roughnessFactor      = asset.PbrRoughness();
    info.normalStrength       = asset.PbrNormalStrength();
    info.opacityFactor        = asset.baseColorFactor.w;
    info.alphaCutoff          = asset.alphaCutoff;
    info.enableBaseColorMap   = !asset.baseColorTexture.path.empty();
    info.enableNormalMap      = hasNormalMap;
    info.enableEmissiveMap    = !asset.emissiveTexture.path.empty();
    info.alphaTest            = asset.alphaMode == assets::MaterialAlphaMode::Mask;
    info.doubleSided          = asset.doubleSided;
    info.castShadows          = asset.castShadows;
    info.instanced            = asset.instanced;
    info.cullMode             = asset.doubleSided ? MaterialCullMode::None : MaterialCullMode::Back;
    info.materialGraph        = assets::CompileMaterialGraph(asset.graph).Active();
    info.openGL               = ctx.shaderTarget == assets::ShaderTargetProfile::OpenGL_GLSL450;

    const MaterialHandle handle = renderer::lit::LitMaterial::Register(ctx.materials, info);
    if (!handle.IsValid())
        return handle;

    if (const TextureHandle albedo = LoadRuntimeTexture(ctx, asset.baseColorTexture.path); albedo.IsValid())
        ctx.materials.SetTexture(handle, "albedo", albedo);
    if (const TextureHandle normal = LoadRuntimeTexture(ctx, normalPath); normal.IsValid())
        ctx.materials.SetTexture(handle, "normalMap", normal);
    if (const TextureHandle emissive = LoadRuntimeTexture(ctx, asset.emissiveTexture.path); emissive.IsValid())
        ctx.materials.SetTexture(handle, "emissive", emissive);
    ctx.materials.MarkDirty(handle);
    return handle;
}

MaterialHandle CreateDefaultWhiteMaterial(EditorFrameContext& ctx,
                                          assets::MeshAsset& mesh,
                                          const char* name)
{
    const bool skinned = mesh.HasSkinning() && EnsureTangentsForPbr(mesh);
    renderer::VertexLayout layout{};
    if (!ResolveMeshLayout(mesh, skinned ? renderer::VertexContracts::SkinnedLit()
                                         : renderer::VertexContracts::StaticLit(),
                           layout, "Editor-DefaultMaterial"))
        return MaterialHandle::Invalid();

    ShaderHandle vs, fs, shadow, shadowFs;
    if (!LoadLitShaders(ctx, vs, fs, shadow, skinned, &shadowFs))
        return MaterialHandle::Invalid();

    renderer::lit::LitMaterialCreateInfo info{};
    info.name                 = name;
    info.vertexShader         = vs;
    info.fragmentShader       = fs;
    info.shadowShader         = shadow;
    info.shadowFragmentShader = shadowFs;
    info.vertexLayout         = layout;
    info.baseColorFactor      = {1.f, 1.f, 1.f, 1.f};
    info.emissiveFactor     = {0.f, 0.f, 0.f, 1.f};
    info.specularStrength   = 0.f;
    info.roughnessFactor    = 1.f;
    info.opacityFactor      = 1.f;
    info.enableBaseColorMap = false;
    info.enableEmissiveMap  = false;
    info.alphaTest          = false;
    info.doubleSided        = false;
    info.castShadows        = true;
    info.cullMode           = MaterialCullMode::Back;
    return renderer::lit::LitMaterial::Register(ctx.materials, info);
}

// Feldnamen im PerMaterial-Block der Custom-Shader (HLSL "cbuffer PerMaterial",
// GLSL "uniform PerMaterial"). Quelltext-Scan, daher fuer alle Backends gleich.
// Pro Datei und Aenderungszeit zwischengespeichert (die UI fragt jeden Frame).
std::unordered_set<std::string> ScanPerMaterialFieldNames(const EditorFrameContext& ctx,
                                                          const assets::MaterialAsset& asset)
{
    struct CacheEntry
    {
        std::filesystem::file_time_type writeTime{};
        std::unordered_set<std::string> names;
    };
    static std::unordered_map<std::string, CacheEntry> cache;

    std::unordered_set<std::string> result;
    for (const std::string* shaderPath : { &asset.vertexShaderPath, &asset.fragmentShaderPath })
    {
        if (shaderPath->empty())
            continue;
        const std::filesystem::path path = ToAbsoluteAssetPath(ctx, *shaderPath);
        std::error_code ec;
        const auto writeTime = std::filesystem::last_write_time(path, ec);
        if (ec)
            continue;
        CacheEntry& entry = cache[PathToUtf8(path)];
        if (entry.writeTime != writeTime || entry.names.empty())
        {
            entry.writeTime = writeTime;
            entry.names.clear();
            std::ifstream in(path, std::ios::binary);
            bool inPerMaterial = false;
            std::string line;
            while (std::getline(in, line))
            {
                if (const size_t comment = line.find("//"); comment != std::string::npos)
                    line.erase(comment);
                if (line.find("PerMaterial") != std::string::npos &&
                    (line.find("cbuffer") != std::string::npos || line.find("uniform") != std::string::npos))
                {
                    inPerMaterial = true;
                    continue;
                }
                if (!inPerMaterial)
                    continue;
                if (line.find('}') != std::string::npos)
                {
                    inPerMaterial = false;
                    continue;
                }
                const std::vector<std::string> parts = SplitWs(line);
                if (parts.size() < 2u)
                    continue;
                std::string field = parts[1];
                field = field.substr(0, field.find_first_of(";[:"));
                if (!field.empty())
                    entry.names.insert(field);
            }
        }
        result.insert(entry.names.begin(), entry.names.end());
    }
    return result;
}

MaterialHandle CreateCustomMaterialFromAsset(EditorFrameContext& ctx,
                                             assets::MeshAsset& mesh,
                                             const assets::MaterialAsset& asset,
                                             const char* name)
{
    // Shader-Handles sind nicht serialisiert — bei Disk-Load (ResolveMaterialAssetBindings)
    // sind sie immer Invalid. On-demand laden damit der Custom-Pfad greift.
    ShaderHandle vs = asset.vertexShader;
    ShaderHandle fs = asset.fragmentShader;
    if ((!vs.IsValid() || !fs.IsValid()) && ctx.assetPipeline)
    {
        if (!vs.IsValid() && !asset.vertexShaderPath.empty())
            vs = LoadRuntimeShader(ctx, asset.vertexShaderPath, assets::ShaderStage::Vertex);
        if (!fs.IsValid() && !asset.fragmentShaderPath.empty())
            fs = LoadRuntimeShader(ctx, asset.fragmentShaderPath, assets::ShaderStage::Fragment);
    }
    if (!vs.IsValid() || !fs.IsValid())
    {
        Debug::LogError("Editor-Material: Custom-Shader — VS oder FS fehlt (Pfade: '%s', '%s')",
                        asset.vertexShaderPath.c_str(), asset.fragmentShaderPath.c_str());
        return CreateDefaultWhiteMaterial(ctx, mesh, name);
    }

    renderer::VertexLayout layout{};
    if (!ResolveMeshLayout(mesh, renderer::VertexContracts::StaticLit(), layout, "Editor-Material"))
        return MaterialHandle::Invalid();

    // Unlit-Infrastruktur mit benutzerdefinierten Shadern
    renderer::unlit::UnlitMaterialCreateInfo info{};
    info.name             = name;
    info.vertexShader     = vs;
    info.fragmentShader   = fs;
    info.vertexLayout     = layout;
    info.baseColorFactor  = asset.baseColorFactor;
    info.emissiveFactor   = {asset.RuntimeEmissive().x, asset.RuntimeEmissive().y, asset.RuntimeEmissive().z, 1.f};
    info.opacityFactor    = asset.baseColorFactor.w;
    info.alphaCutoff      = asset.alphaCutoff;
    // Standard-PBR-Faktoren: wirken nur, wenn der Shader sie in PerMaterial deklariert.
    info.metallicFactor    = asset.metallicFactor;
    info.roughnessFactor   = asset.roughnessFactor;
    info.occlusionStrength = asset.occlusionStrength;
    info.enableBaseColorMap = !asset.baseColorTexture.path.empty();
    info.enableEmissiveMap  = !asset.emissiveTexture.path.empty();
    info.alphaTest        = asset.alphaMode == assets::MaterialAlphaMode::Mask;
    info.doubleSided      = asset.doubleSided;
    info.castShadows      = asset.castShadows;
    info.instanced        = asset.instanced;
    info.cullMode         = asset.doubleSided ? MaterialCullMode::None : MaterialCullMode::Back;
    info.extraParameters.reserve(asset.params.size() + 1u);
    for (const assets::MaterialParam& param : asset.params)
    {
        if (IsBuiltInMaterialParamName(param.name))
            continue;
        renderer::MaterialParam runtimeParam = ToRuntimeParam(param);
        if (param.type == assets::MaterialParam::Type::Texture && !param.texturePath.empty())
            runtimeParam.texture = LoadRuntimeTexture(ctx, param.texturePath);
        info.extraParameters.push_back(runtimeParam);
    }
    // Normalen-Staerke ist kein Unlit-Standardfeld: nur uebergeben, wenn deklariert.
    {
        const std::unordered_set<std::string> declared = ScanPerMaterialFieldNames(ctx, asset);
        for (const char* normalName : { "normalScale", "normalStrength" })
        {
            if (!declared.count(normalName))
                continue;
            assets::MaterialParam normal{};
            normal.name = normalName;
            normal.type = assets::MaterialParam::Type::Float;
            normal.value.f[0] = asset.normalScale;
            info.extraParameters.push_back(ToRuntimeParam(normal));
        }
    }

    // Textur-Register und PerMaterial-Offsets aus dem kompilierten Shader lesen,
    // damit Engine-Slots und Shader-Register uebereinstimmen.
    {
        const assets::ShaderAsset* vsAsset = ctx.registry.shaders.Get(vs);
        const assets::ShaderAsset* fsAsset = ctx.registry.shaders.Get(fs);
        if (vsAsset && fsAsset)
        {
            std::string reflectError;
            if (!renderer::ReflectProgramTextureBindings(*vsAsset, *fsAsset, ctx.shaderTarget,
                                                         info.textureBindingOverrides, &reflectError))
                Debug::LogWarning("Editor-Material '%s': Textur-Register nicht lesbar (%s), "
                                  "eigene Texturen werden per Name/Reihenfolge gebunden",
                                  name ? name : "", reflectError.c_str());

            if (!renderer::ReflectProgramConstantBuffer(*vsAsset, *fsAsset, ctx.shaderTarget, "PerMaterial",
                                                        renderer::CBSlots::PerMaterial, info.perMaterialReflection, &reflectError))
                Debug::LogWarning("Editor-Material '%s': PerMaterial-Reflection nicht moeglich (%s), "
                                  "Parameter werden in fester Reihenfolge gepackt",
                                  name ? name : "", reflectError.c_str());
        }
    }

    const MaterialHandle handle = renderer::unlit::UnlitMaterial::Register(ctx.materials, info);
    if (!handle.IsValid())
        return handle;

    if (const TextureHandle albedo = LoadRuntimeTexture(ctx, asset.baseColorTexture.path); albedo.IsValid())
        ctx.materials.SetTexture(handle, "albedo", albedo);
    if (const TextureHandle emissive = LoadRuntimeTexture(ctx, asset.emissiveTexture.path); emissive.IsValid())
        ctx.materials.SetTexture(handle, "emissive", emissive);
    for (const assets::MaterialParam& param : asset.params)
        if (!IsBuiltInMaterialParamName(param.name) &&
            param.type == assets::MaterialParam::Type::Texture && !param.texturePath.empty())
            if (const TextureHandle tex = LoadRuntimeTexture(ctx, param.texturePath); tex.IsValid())
                ctx.materials.SetTexture(handle, param.name, tex);
    ctx.materials.MarkDirty(handle);
    return handle;
}

MaterialHandle CreateRuntimeMaterialFromAsset(EditorFrameContext& ctx,
                                              assets::MeshAsset& mesh,
                                              const assets::MaterialAsset& asset,
                                              const char* name)
{
    switch (ParseMaterialTemplate(asset.templateName))
    {
    case EditorMaterialTemplate::Unlit:
        return CreateUnlitMaterialFromAsset(ctx, mesh, asset, name);
    case EditorMaterialTemplate::LegacyLit:
        return CreateLegacyLitMaterialFromAsset(ctx, mesh, asset, name);
    case EditorMaterialTemplate::Custom:
        return CreateCustomMaterialFromAsset(ctx, mesh, asset, name);
    case EditorMaterialTemplate::PbrLit:
    default:
        return CreatePbrMaterialFromAsset(ctx, mesh, asset, name);
    }
}

void UpdateRuntimeMaterialParameters(EditorFrameContext& ctx,
                                     MaterialHandle runtimeMaterial,
                                     SharedMaterialBinding& binding,
                                     const assets::MaterialAsset& asset,
                                     const assets::CompiledMaterialGraph& graph,
                                     const std::string& ormPath, const std::string& normalPath)
{
    if (!runtimeMaterial.IsValid())
        return;

    const EditorMaterialTemplate materialTemplate = ParseMaterialTemplate(asset.templateName);
    const math::Vec3 runtimeEmissive = asset.RuntimeEmissive();
    const math::Vec4 emissiveVec4{runtimeEmissive.x, runtimeEmissive.y, runtimeEmissive.z, 0.f};

    UpdateCachedTexture(ctx, asset.baseColorTexture.path, binding.baseColorPath, binding.baseColorTexture);
    UpdateCachedTexture(ctx, ormPath, binding.ormPath, binding.ormTexture,
                        binding.isPbr ? TextureLoadRole::LinearData : TextureLoadRole::Auto);
    UpdateCachedTexture(ctx, normalPath, binding.normalPath, binding.normalTexture,
                        binding.isPbr ? TextureLoadRole::NormalMap : TextureLoadRole::Auto);
    UpdateCachedTexture(ctx, asset.emissiveTexture.path, binding.emissivePath, binding.emissiveTexture);

    switch (materialTemplate)
    {
    case EditorMaterialTemplate::PbrLit:
        ctx.materials.SetVec4(runtimeMaterial, "baseColorFactor", asset.baseColorFactor);
        ctx.materials.SetVec4(runtimeMaterial, "emissiveFactor", emissiveVec4);
        ctx.materials.SetFloat(runtimeMaterial, "metallicFactor", asset.PbrMetallic());
        ctx.materials.SetFloat(runtimeMaterial, "roughnessFactor", asset.PbrRoughness());
        ctx.materials.SetFloat(runtimeMaterial, "normalStrength", asset.PbrNormalStrength());
        ctx.materials.SetFloat(runtimeMaterial, "occlusionStrength", asset.PbrOcclusion());
        ctx.materials.SetFloat(runtimeMaterial, "opacityFactor", asset.baseColorFactor.w);
        ctx.materials.SetFloat(runtimeMaterial, "alphaCutoff", asset.alphaCutoff);
        ctx.materials.SetVec2(runtimeMaterial, "uvScale", asset.uvScale);
        ctx.materials.SetVec2(runtimeMaterial, "uvOffset", asset.uvOffset);
        ctx.materials.SetFloat(runtimeMaterial, "boxTileSize", asset.BoxProjectionTileSize());
        ctx.materials.SetTexture(runtimeMaterial, "albedo", binding.baseColorTexture);
        ctx.materials.SetTexture(runtimeMaterial, "normal", binding.normalTexture);
        ctx.materials.SetTexture(runtimeMaterial, "orm", binding.ormTexture);
        ctx.materials.SetTexture(runtimeMaterial, "emissive", binding.emissiveTexture);
        break;

    case EditorMaterialTemplate::LegacyLit:
        ctx.materials.SetVec4(runtimeMaterial, "baseColorFactor", asset.baseColorFactor);
        ctx.materials.SetVec4(runtimeMaterial, "emissiveFactor", emissiveVec4);
        ctx.materials.SetFloat(runtimeMaterial, "metallicFactor", asset.PbrMetallic());
        ctx.materials.SetFloat(runtimeMaterial, "roughnessFactor", asset.PbrRoughness());
        ctx.materials.SetFloat(runtimeMaterial, "normalStrength", asset.PbrNormalStrength());
        ctx.materials.SetFloat(runtimeMaterial, "opacityFactor", asset.baseColorFactor.w);
        ctx.materials.SetFloat(runtimeMaterial, "alphaCutoff", asset.alphaCutoff);
        ctx.materials.SetTexture(runtimeMaterial, "albedo", binding.baseColorTexture);
        ctx.materials.SetTexture(runtimeMaterial, "normalMap", binding.normalTexture);
        ctx.materials.SetTexture(runtimeMaterial, "emissive", binding.emissiveTexture);
        break;

    case EditorMaterialTemplate::Unlit:
        ctx.materials.SetVec4(runtimeMaterial, "baseColorFactor", asset.baseColorFactor);
        ctx.materials.SetVec4(runtimeMaterial, "emissiveFactor", emissiveVec4);
        ctx.materials.SetFloat(runtimeMaterial, "opacityFactor", asset.baseColorFactor.w);
        ctx.materials.SetFloat(runtimeMaterial, "alphaCutoff", asset.alphaCutoff);
        ctx.materials.SetTexture(runtimeMaterial, "albedo", binding.baseColorTexture);
        ctx.materials.SetTexture(runtimeMaterial, "emissive", binding.emissiveTexture);
        break;

    case EditorMaterialTemplate::Custom:
        ctx.materials.SetVec4(runtimeMaterial, "baseColorFactor", asset.baseColorFactor);
        ctx.materials.SetVec4(runtimeMaterial, "emissiveFactor", emissiveVec4);
        ctx.materials.SetFloat(runtimeMaterial, "opacityFactor", asset.baseColorFactor.w);
        ctx.materials.SetFloat(runtimeMaterial, "alphaCutoff", asset.alphaCutoff);
        ctx.materials.SetTexture(runtimeMaterial, "albedo", binding.baseColorTexture);
        ctx.materials.SetTexture(runtimeMaterial, "emissive", binding.emissiveTexture);
        for (const assets::MaterialParam& param : asset.params)
        {
            if (IsBuiltInMaterialParamName(param.name))
                continue;
            switch (param.type)
            {
            case assets::MaterialParam::Type::Float:
                ctx.materials.SetFloat(runtimeMaterial, param.name, param.value.f[0]);
                break;
            case assets::MaterialParam::Type::Vec2:
                ctx.materials.SetVec2(runtimeMaterial, param.name, {param.value.f[0], param.value.f[1]});
                break;
            case assets::MaterialParam::Type::Vec3:
                ctx.materials.SetVec3(runtimeMaterial, param.name, {param.value.f[0], param.value.f[1], param.value.f[2]});
                break;
            case assets::MaterialParam::Type::Vec4:
                ctx.materials.SetVec4(runtimeMaterial, param.name, {param.value.f[0], param.value.f[1], param.value.f[2], param.value.f[3]});
                break;
            case assets::MaterialParam::Type::Int:
                ctx.materials.SetInt(runtimeMaterial, param.name, param.value.i);
                break;
            case assets::MaterialParam::Type::Bool:
                ctx.materials.SetBool(runtimeMaterial, param.name, param.value.b);
                break;
            case assets::MaterialParam::Type::Texture:
                ctx.materials.SetTexture(runtimeMaterial, param.name, LoadRuntimeTexture(ctx, param.texturePath));
                break;
            }
        }
        break;
    }

    if (graph.Active())
    {
        for (size_t i = 0; i < binding.graphTextures.size(); ++i)
            UpdateCachedTexture(ctx, graph.textures[i], binding.graphTexturePaths[i], binding.graphTextures[i],
                                graph.textureLinear[i] ? TextureLoadRole::LinearData : TextureLoadRole::Auto);
        SetMaterialGraph(ctx.materials, runtimeMaterial, graph, binding.graphTextures, asset.uvScale, asset.uvOffset);
    }

    ctx.materials.MarkDirty(runtimeMaterial);
}

MaterialHandle AcquireSharedRuntimeMaterial(EditorFrameContext& ctx,
                                            MeshHandle meshHandle,
                                            const std::string& materialPath,
                                            assets::MeshAsset& mesh,
                                            const assets::MaterialAsset& asset)
{
    if (!asset.graph.nodes.empty() && ParseMaterialTemplate(asset.templateName) == EditorMaterialTemplate::Custom)
        return AcquireSharedRuntimeMaterial(ctx, meshHandle, materialPath, mesh, asset.WithGraphTextures());

    SharedMaterialBindingKey key{ materialPath, meshHandle };
    SharedMaterialBinding& binding = SharedMaterialBindings()[key];
    const bool isPbr = ParseMaterialTemplate(asset.templateName) == EditorMaterialTemplate::PbrLit;
    const auto graph = assets::CompileMaterialGraph(asset.graph);
    const auto& ormPath = asset.PbrOrmPath();
    const auto& normalPath = asset.PbrNormalPath();
    const uint64_t structureHash = HashMaterialStructure(asset, graph.Active(), ormPath, normalPath);

    // Prüfen ob Texturen die beim letzten Rebuild noch fehlten jetzt geladen sind.
    // Wenn ja: Rebuild erzwingen damit builder.Normal / builder.BaseColor usw.
    // mit gültigem Handle aufgerufen werden und die Float-Parameter (normalStrength
    // usw.) in parameterOverrides landen. Ohne diesen Check schlägt SetFloat
    // ("normalStrength", ...) lautlos fehl weil der Parameter nie registriert wurde.
    const auto textureNowValid = [&](const std::string& assetPath, const TextureHandle& cachedHandle,
                                     TextureLoadRole role = TextureLoadRole::Auto) -> bool
    {
        if (assetPath.empty() || cachedHandle.IsValid())
            return false; // kein Pfad oder bereits geladen
        return LoadRuntimeTexture(ctx, assetPath, role).IsValid();
    };

    const bool texturesBecameAvailable =
        textureNowValid(asset.baseColorTexture.path,        binding.baseColorTexture) ||
        textureNowValid(ormPath, binding.ormTexture, isPbr ? TextureLoadRole::LinearData : TextureLoadRole::Auto) ||
        textureNowValid(normalPath, binding.normalTexture, isPbr ? TextureLoadRole::NormalMap : TextureLoadRole::Auto) ||
        textureNowValid(asset.emissiveTexture.path,          binding.emissiveTexture);

    // Struktur-Änderung (z.B. Textur hinzugefügt) setzt buildFailed zurück.
    if (binding.structureHash != structureHash)
        binding.buildFailed = false;

    // buildFailed = true: nur bei Struktur-Änderung erneut versuchen, sonst nie.
    if (binding.buildFailed)
        return MaterialHandle::Invalid();

    const bool needsRebuild =
        !binding.runtimeMaterial.IsValid() ||
        !ctx.materials.GetDesc(binding.runtimeMaterial) ||
        binding.structureHash != structureHash ||
        texturesBecameAvailable;

    if (needsRebuild)
    {
        if (binding.runtimeMaterial.IsValid() && ctx.materials.GetDesc(binding.runtimeMaterial))
            ctx.materials.DestroyMaterial(binding.runtimeMaterial);

        const std::string debugName =
            PathToUtf8(PathFromUtf8(materialPath).stem()) + "_" + std::to_string(meshHandle.value);
        binding.runtimeMaterial =
            CreateRuntimeMaterialFromAsset(ctx, mesh, asset, debugName.c_str());
        binding.structureHash = structureHash;
        binding.buildFailed   = !binding.runtimeMaterial.IsValid();
        binding.isPbr         = isPbr;
        binding.baseColorPath.clear();
        binding.ormPath.clear();
        binding.normalPath.clear();
        binding.emissivePath.clear();
        binding.baseColorTexture = TextureHandle::Invalid();
        binding.ormTexture = TextureHandle::Invalid();
        binding.normalTexture = TextureHandle::Invalid();
        binding.emissiveTexture = TextureHandle::Invalid();
        binding.graphTexturePaths = {};
        binding.graphTextures = {};
    }

    UpdateRuntimeMaterialParameters(ctx, binding.runtimeMaterial, binding, asset, graph, ormPath, normalPath);
    return binding.runtimeMaterial;
}

bool ApplyMaterialAssetToMeshEntity(EditorFrameContext& ctx,
                                    EntityID entity,
                                    const std::string& materialPath,
                                    const uint32_t* submeshIndex = nullptr)
{
    const std::string normalizedMaterialPath = NormalizeMaterialAssetPath(ctx, PathFromUtf8(materialPath));
    auto* meshComp = ctx.world.Get<MeshComponent>(entity);
    if (!meshComp)
        return false;

    // Material-Pfad sofort auf der Component speichern — auch wenn das Mesh-Handle
    // noch ungueltig ist (z.B. nach Scene-Load vor ResolveMeshAssetBindings).
    // ResolveMaterialAssetBindings liest den Pfad und baut das Runtime-Material
    // sobald das Mesh gebunden wurde.
    if (auto* material = ctx.world.Get<MaterialComponent>(entity))
    {
        if (submeshIndex)
        {
            MaterialComponent::SlotOverride* slot = material->FindSlotOverride(*submeshIndex);
            if (!slot)
            {
                MaterialComponent::SlotOverride newSlot{};
                newSlot.submeshIndex = *submeshIndex;
                material->slotOverrides.push_back(std::move(newSlot));
                slot = &material->slotOverrides.back();
            }
            slot->materialAssetPath = normalizedMaterialPath;
        }
        else
        {
            material->materialAssetPath = normalizedMaterialPath;
        }
    }
    else
    {
        MaterialComponent component{};
        if (submeshIndex)
        {
            MaterialComponent::SlotOverride slot{};
            slot.submeshIndex = *submeshIndex;
            slot.materialAssetPath = normalizedMaterialPath;
            component.slotOverrides.push_back(std::move(slot));
        }
        else
        {
            component.materialAssetPath = normalizedMaterialPath;
        }
        ctx.world.Add<MaterialComponent>(entity, component);
    }

    // Runtime-Material nur erstellen wenn Mesh und Asset-Datei verfuegbar sind.
    assets::MeshAsset* mesh = ctx.registry.meshes.Get(meshComp->mesh);
    if (!mesh)
        return true;  // Pfad gespeichert; Material wird nach Mesh-Bindung angewandt

    const assets::MaterialAsset* materialAsset = nullptr;
    // Shortcut: In-Memory-Daten nur nutzen, wenn das Material im Editor geöffnet
    // ist UND der Nutzer nicht gespeichert hat (Dirty-State). Damit bleiben
    // unsaved Änderungen im Material-Editor sichtbar ohne die Disk zu schreiben.
    // Ist das Material gerade NICHT im Editor aktiv (z.B. Aufruf aus
    // ResolveMaterialAssetBindings nach einem Projekt-Reload), wird immer frisch
    // von Disk geladen → verhindert veraltete Daten ohne Texturen.
    // In-Memory-Daten verwenden wenn das Material geladen ist UND es ungespeicherte
    // Änderungen hat. Die alte Bedingung `materialWindowOpen` war zu eng — sie
    // schloss den Inspector-Pfad aus, bei dem das Materialeditor-Fenster nicht offen
    // ist. Folge: ReapplyMaterialAssetToBoundEntities las von Disk, wo der async-
    // Save noch nicht abgeschlossen war → erster Doppelklick auf Textur ohne Effekt.
    const bool useInMemory =
        ctx.state.selectedMaterialAssetLoaded &&
        ctx.state.selectedMaterialAssetDirty &&
        NormalizeMaterialAssetPath(ctx, PathFromUtf8(ctx.state.selectedMaterialAssetPath)) == normalizedMaterialPath;
    if (useInMemory)
    {
        materialAsset = &ctx.state.selectedMaterialAssetData;
    }
    else
    {
        materialAsset = LoadMaterialAsset(ctx, normalizedMaterialPath);
    }

    if (!materialAsset)
        return true;  // Pfad gespeichert; Asset-Datei konnte nicht geladen werden

    const auto* localComponent = ctx.world.Get<MaterialComponent>(entity);
    const auto* localSlot = submeshIndex && localComponent ? localComponent->FindSlotOverride(*submeshIndex) : nullptr;
    const std::string localSource = localSlot ? localSlot->materialOverrideSource :
        (!submeshIndex && localComponent ? localComponent->materialOverrideSource : std::string{});
    assets::MaterialAsset localAsset;
    if (!localSource.empty() && ParseEditorMaterialAssetSource(localSource, normalizedMaterialPath, localAsset))
        materialAsset = &localAsset;
    const std::string runtimeKey = localSource.empty() ? normalizedMaterialPath :
        normalizedMaterialPath + "#entity-" + std::to_string(entity.value) + "-slot-" +
        (submeshIndex ? std::to_string(*submeshIndex) : "all");
    const MaterialHandle runtimeMaterial =
        AcquireSharedRuntimeMaterial(ctx, meshComp->mesh, runtimeKey, *mesh, *materialAsset);
    if (!runtimeMaterial.IsValid())
        return false;

    auto* material = ctx.world.Get<MaterialComponent>(entity);
    if (material && submeshIndex)
    {
        if (MaterialComponent::SlotOverride* slot = material->FindSlotOverride(*submeshIndex))
            slot->material = runtimeMaterial;
    }
    else if (material)
        material->material = runtimeMaterial;

    return true;
}

void ReapplyMaterialAssetToBoundEntities(EditorFrameContext& ctx, const std::string& materialPath)
{
    const std::string normalizedMaterialPath = NormalizeMaterialAssetPath(ctx, PathFromUtf8(materialPath));
    ctx.world.ForEachAlive([&](EntityID id)
    {
        auto* material = ctx.world.Get<MaterialComponent>(id);
        auto* meshComp = ctx.world.Get<MeshComponent>(id);
        if (!material || !meshComp)
            return;

        if (NormalizeMaterialAssetPath(ctx, PathFromUtf8(material->materialAssetPath)) == normalizedMaterialPath)
            ApplyMaterialAssetToMeshEntity(ctx, id, normalizedMaterialPath);

        for (MaterialComponent::SlotOverride& slot : material->slotOverrides)
        {
            if (NormalizeMaterialAssetPath(ctx, PathFromUtf8(slot.materialAssetPath)) == normalizedMaterialPath)
                ApplyMaterialAssetToMeshEntity(ctx, id, normalizedMaterialPath, &slot.submeshIndex);
        }
    });
}

void PreviewMaterialAssetOnBoundEntities(EditorFrameContext& ctx,
                                         const std::string& materialPath,
                                         const assets::MaterialAsset& previewAsset)
{
    const std::string normalizedMaterialPath = NormalizeMaterialAssetPath(ctx, PathFromUtf8(materialPath));

    ctx.world.ForEachAlive([&](EntityID id)
    {
        auto* material = ctx.world.Get<MaterialComponent>(id);
        auto* meshComp = ctx.world.Get<MeshComponent>(id);
        if (!material || !meshComp)
            return;

        assets::MeshAsset* mesh = ctx.registry.meshes.Get(meshComp->mesh);
        if (!mesh)
            return;

        const MaterialHandle runtimeMaterial =
            AcquireSharedRuntimeMaterial(ctx, meshComp->mesh, normalizedMaterialPath, *mesh, previewAsset);
        if (!runtimeMaterial.IsValid())
            return;

        if (material->materialOverrideSource.empty() && NormalizeMaterialAssetPath(ctx, PathFromUtf8(material->materialAssetPath)) == normalizedMaterialPath)
        {
            material->material = runtimeMaterial;
            material->materialAssetPath = normalizedMaterialPath;
        }

        for (MaterialComponent::SlotOverride& slot : material->slotOverrides)
        {
            if (slot.materialOverrideSource.empty() && NormalizeMaterialAssetPath(ctx, PathFromUtf8(slot.materialAssetPath)) == normalizedMaterialPath)
            {
                slot.material = runtimeMaterial;
                slot.materialAssetPath = normalizedMaterialPath;
            }
        }
    });
}

void RevealMaterialTexture(EditorFrameContext& ctx, const std::string& path)
{
    if (auto* browser = ctx.state.assetBrowser)
    {
        const auto absolutePath = ToAbsoluteAssetPath(ctx, path).lexically_normal();
        browser->currentDir = absolutePath.parent_path();
        browser->highlightPath = absolutePath;
    }
}

void DrawTexturePathRow(EditorFrameContext& ctx,
                        const char* label,
                        const char* paramName,
                        std::string& value,
                        bool& dirty,
                        bool& commitChanges)
{
    constexpr float kThumbSize = 18.f;
    const bool hasTexture = !value.empty();
    const bool isActive   = (ctx.state.activeTextureParamName == paramName);

    ImGui::PushID(paramName);

    // ── Slot-Button (links, farbig wenn aktiv) ────────────────────────────────
    if (isActive)
        ImGui::PushStyleColor(ImGuiCol_Button, theme::kMenuOpen);

    if (ImGui::Button(label, ImVec2(90.f, kThumbSize + 2.f)))
        ctx.state.activeTextureParamName = paramName;

    if (isActive)
        ImGui::PopStyleColor();
    if (value_ui::StringMenu(label, value, "asset-texture"))
    {
        dirty = true; commitChanges = true;
        ctx.state.activeTextureParamName = paramName;
    }

    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(Translate("material.texture_activate"));

    // Drag&Drop auf den Slot-Button selbst
    if (ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("KROM_ASSET_TEXTURE"))
        {
            const char* texturePath = static_cast<const char*>(payload->Data);
            if (texturePath && texturePath[0] != '\0')
            {
                const std::string resolved = ToAssetRelativePath(ctx, PathFromUtf8(texturePath));
                if (IsSupportedTexturePath(PathFromUtf8(resolved)))
                {
                    value = resolved;
                    ctx.state.activeTextureParamName = paramName;
                    dirty = true;
                    commitChanges = true;
                }
            }
        }
        ImGui::EndDragDropTarget();
    }

    ImGui::SameLine();

    // ── Mini-Thumbnail oder Platzhalter ───────────────────────────────────────
    if (hasTexture && ctx.editorTextureId)
    {
        // Textur-Handle über LoadRuntimeTexture holen (gecacht)
        const std::filesystem::path assetRoot = AssetRoot(ctx);
        const std::filesystem::path texPath   = assetRoot.empty()
            ? PathFromUtf8(value)
            : (assetRoot / PathFromUtf8(value)).lexically_normal();

        const TextureHandle texHandle = LoadInspectorThumbnail(ctx, PathToUtf8(texPath));
        void* imguiId = texHandle.IsValid() ? ctx.editorTextureId(texHandle) : nullptr;

        const ImVec2 thumbMin = ImGui::GetCursorScreenPos();
        const ImVec2 thumbMax = { thumbMin.x + kThumbSize, thumbMin.y + kThumbSize };

        // InvisibleButton als interaktives Item für Drag-Drop + Doppelklick
        ImGui::InvisibleButton("##thumb", ImVec2(kThumbSize, kThumbSize));

        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (imguiId)
            dl->AddImage(reinterpret_cast<ImTextureID>(imguiId), thumbMin, thumbMax);
        else
            dl->AddRectFilled(thumbMin, thumbMax, IM_COL32(80, 80, 80, 255));
        dl->AddRect(thumbMin, thumbMax, IM_COL32(120, 120, 120, 255));

        // Doppelklick → Asset-Browser öffnen und Textur hervorheben
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            RevealMaterialTexture(ctx, value);

        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(Translate("material.texture_reveal"), value.c_str());

        // Drag&Drop auch auf das Thumbnail
        if (ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("KROM_ASSET_TEXTURE"))
            {
                const char* texturePath = static_cast<const char*>(payload->Data);
                if (texturePath && texturePath[0] != '\0')
                {
                    const std::string resolved = ToAssetRelativePath(ctx, PathFromUtf8(texturePath));
                    if (IsSupportedTexturePath(PathFromUtf8(resolved)))
                    {
                        value = resolved;
                        ctx.state.activeTextureParamName = paramName;
                        dirty = true;
                        commitChanges = true;
                    }
                }
            }
            ImGui::EndDragDropTarget();
        }
    }
    else
    {
        // Kein Thumbnail: kleiner grauer Platzhalter als Drop-Ziel
        const ImVec2 thumbMin = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##thumb", ImVec2(kThumbSize, kThumbSize));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(thumbMin,
                          { thumbMin.x + kThumbSize, thumbMin.y + kThumbSize },
                          IM_COL32(50, 50, 50, 200));
        dl->AddRect(thumbMin,
                    { thumbMin.x + kThumbSize, thumbMin.y + kThumbSize },
                    IM_COL32(100, 100, 100, 255));

        if (ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("KROM_ASSET_TEXTURE"))
            {
                const char* texturePath = static_cast<const char*>(payload->Data);
                if (texturePath && texturePath[0] != '\0')
                {
                    const std::string resolved = ToAssetRelativePath(ctx, PathFromUtf8(texturePath));
                    if (IsSupportedTexturePath(PathFromUtf8(resolved)))
                    {
                        value = resolved;
                        ctx.state.activeTextureParamName = paramName;
                        dirty = true;
                        commitChanges = true;
                    }
                }
            }
            ImGui::EndDragDropTarget();
        }
    }

    ImGui::SameLine();

    // ── Dateiname (gekürzt) ───────────────────────────────────────────────────
    const float clearButtonWidth = ImGui::CalcTextSize("Entfernen").x +
                                   ImGui::GetStyle().FramePadding.x * 2.f;
    const float pathWidth = std::max(40.f,
        ImGui::GetContentRegionAvail().x - clearButtonWidth - ImGui::GetStyle().ItemSpacing.x);
    if (hasTexture)
    {
        const std::string filename = PathToUtf8(PathFromUtf8(value).filename());
        ImGui::SetNextItemWidth(pathWidth);
        value_ui::TextUnformatted(filename.c_str());
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", value.c_str());
    }
    else
    {
        value_ui::TextDisabled(Translate("common.empty"));
    }

    // ── X-Button ──────────────────────────────────────────────────────────────
    ImGui::SameLine();
    if (!hasTexture)
        ImGui::BeginDisabled();
    if (ImGui::SmallButton("Entfernen"))
    {
        value.clear();
        dirty = true;
        commitChanges = true;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(Translate("material.remove_texture"));
    if (!hasTexture)
        ImGui::EndDisabled();

    ImGui::PopID();
}

std::string StripLineComment(const std::string& line)
{
    const size_t pos = line.find("//");
    return Trim(pos == std::string::npos ? line : line.substr(0u, pos));
}

void AddDiscoveredParam(assets::MaterialAsset& asset,
                        const std::string& typeToken,
                        std::string name)
{
    if (name.empty())
        return;
    while (!name.empty() && (name.back() == ';' || name.back() == ',' || name.back() == ':'))
        name.pop_back();
    const size_t arrayPos = name.find('[');
    if (arrayPos != std::string::npos)
        name.resize(arrayPos);
    if (name.empty() || IsBuiltInMaterialParamName(name) || FindAssetParam(asset.params, name))
        return;

    assets::MaterialParam param{};
    param.name = std::move(name);
    if (typeToken == "float" || typeToken == "half")
    {
        param.type = assets::MaterialParam::Type::Float;
        param.value.f[0] = 0.f;
    }
    else if (typeToken == "float2" || typeToken == "vec2" || typeToken == "half2")
    {
        param.type = assets::MaterialParam::Type::Vec2;
    }
    else if (typeToken == "float3" || typeToken == "vec3" || typeToken == "half3")
    {
        param.type = assets::MaterialParam::Type::Vec3;
    }
    else if (typeToken == "float4" || typeToken == "vec4" || typeToken == "half4")
    {
        param.type = assets::MaterialParam::Type::Vec4;
    }
    else if (typeToken == "int" || typeToken == "uint")
    {
        param.type = assets::MaterialParam::Type::Int;
    }
    else if (typeToken == "bool")
    {
        param.type = assets::MaterialParam::Type::Bool;
    }
    else if (typeToken == "Texture2D" || typeToken == "sampler2D")
    {
        param.type = assets::MaterialParam::Type::Texture;
    }
    else
    {
        return;
    }
    asset.params.push_back(param);
}

bool SyncCustomShaderParamsFromSource(EditorFrameContext& ctx, assets::MaterialAsset& asset)
{
    if (ParseMaterialTemplate(asset.templateName) != EditorMaterialTemplate::Custom)
        return false;

    const size_t oldCount = asset.params.size();
    asset.params.erase(std::remove_if(asset.params.begin(), asset.params.end(),
        [](const assets::MaterialParam& p) { return IsBuiltInMaterialParamName(p.name); }),
        asset.params.end());
    bool changed = false;
    changed = changed || asset.params.size() != oldCount;
    auto scanPath = [&](const std::string& shaderPath)
    {
        if (shaderPath.empty())
            return;
        std::ifstream in(ToAbsoluteAssetPath(ctx, shaderPath), std::ios::binary);
        if (!in)
            return;

        bool inPerMaterial = false;
        std::string line;
        while (std::getline(in, line))
        {
            line = StripLineComment(line);
            if (line.empty())
                continue;

            if (line.find("cbuffer PerMaterial") != std::string::npos ||
                line.find("uniform PerMaterial") != std::string::npos)
            {
                inPerMaterial = true;
                continue;
            }
            if (inPerMaterial && line.find('}') != std::string::npos)
            {
                inPerMaterial = false;
                continue;
            }

            const size_t before = asset.params.size();
            const auto parts = SplitWs(line);
            if (inPerMaterial && parts.size() >= 2u)
                AddDiscoveredParam(asset, parts[0], parts[1]);

            // Global-scope Textur-Deklarationen: "Texture2D name" / "uniform sampler2D name"
            if (!inPerMaterial && parts.size() >= 2u)
            {
                const std::string& typeToken = parts[0];
                const bool isHlslTex  = (typeToken == "Texture2D" || typeToken == "TextureCube"
                                      || typeToken == "Texture2DArray");
                const bool isGlslTex  = (typeToken == "sampler2D" || typeToken == "samplerCube");
                const bool isGlslUniformTex = (parts.size() >= 3u && typeToken == "uniform"
                                            && (parts[1] == "sampler2D" || parts[1] == "samplerCube"));
                if (isHlslTex)
                    AddDiscoveredParam(asset, "Texture2D", parts[1]);
                else if (isGlslTex)
                    AddDiscoveredParam(asset, "Texture2D", parts[1]);
                else if (isGlslUniformTex)
                    AddDiscoveredParam(asset, "Texture2D", parts[2]);
            }

            changed = changed || asset.params.size() != before;
        }
    };

    scanPath(asset.vertexShaderPath);
    scanPath(asset.fragmentShaderPath);
    return changed;
}

bool SanitizeMaterialForTemplate(assets::MaterialAsset& asset)
{
    const EditorMaterialTemplate materialTemplate = ParseMaterialTemplate(asset.templateName);
    if (materialTemplate == EditorMaterialTemplate::Custom)
    {
        // Custom shaders only receive textures, so other node links cannot apply.
        bool linksChanged = false;
        auto& graph = asset.graph;
        for (uint32_t* link : {&graph.baseColor, &graph.emissive, &graph.normal, &graph.orm})
        {
            const auto* node = graph.Find(*link);
            if (*link != 0 && (!node || node->type != assets::MaterialNodeType::Texture))
            {
                *link = 0;
                linksChanged = true;
            }
        }
        return linksChanged;
    }

    bool changed = false;
    auto clearString = [&](std::string& value)
    {
        if (!value.empty())
        {
            value.clear();
            changed = true;
        }
    };

    clearString(asset.vertexShaderPath);
    clearString(asset.fragmentShaderPath);
    if (asset.vertexShader.IsValid())
    {
        asset.vertexShader = ShaderHandle::Invalid();
        changed = true;
    }
    if (asset.fragmentShader.IsValid())
    {
        asset.fragmentShader = ShaderHandle::Invalid();
        changed = true;
    }
    if (!asset.params.empty())
    {
        asset.params.clear();
        changed = true;
    }
    return changed;
}

bool DrawCustomShaderParams(EditorFrameContext& ctx, assets::MaterialAsset& asset)
{
    bool dirty = false;
    if (asset.params.empty())
        return false;

    for (assets::MaterialParam& param : asset.params)
    {
        if (IsBuiltInMaterialParamName(param.name))
            continue;
        ImGui::PushID(param.name.c_str());
        switch (param.type)
        {
        case assets::MaterialParam::Type::Float:
            if (value_ui::DragFloat(param.name.c_str(), &param.value.f[0], 0.01f, -100.f, 100.f,
                                    "%.3f", ImGuiSliderFlags_AlwaysClamp))
                dirty = true;
            break;
        case assets::MaterialParam::Type::Vec2:
            if (value_ui::DragFloat2(param.name.c_str(), param.value.f, 0.01f))
                dirty = true;
            break;
        case assets::MaterialParam::Type::Vec3:
            if (param.name.find("Color") != std::string::npos || param.name.find("color") != std::string::npos)
            {
                if (value_ui::ColorEdit3(param.name.c_str(), param.value.f, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR))
                    dirty = true;
            }
            else if (value_ui::DragFloat3(param.name.c_str(), param.value.f, 0.01f))
                dirty = true;
            break;
        case assets::MaterialParam::Type::Vec4:
            if (param.name.find("Color") != std::string::npos || param.name.find("color") != std::string::npos)
            {
                if (value_ui::ColorEdit4(param.name.c_str(), param.value.f, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR))
                    dirty = true;
            }
            else if (value_ui::DragFloat4(param.name.c_str(), param.value.f, 0.01f))
                dirty = true;
            break;
        case assets::MaterialParam::Type::Int:
            if (value_ui::InputInt(param.name.c_str(), &param.value.i))
                dirty = true;
            break;
        case assets::MaterialParam::Type::Bool:
            if (value_ui::Checkbox(param.name.c_str(), &param.value.b))
                dirty = true;
            break;
        case assets::MaterialParam::Type::Texture:
        {
            bool commit = false;
            DrawTexturePathRow(ctx, param.name.c_str(), param.name.c_str(), param.texturePath, dirty, commit);
            break;
        }
        }
        ImGui::PopID();
    }
    return dirty;
}

std::string BrowseForShaderFile(EditorFrameContext& ctx)
{
    if (!platform::dialog::IsAvailable())
        return {};
    const platform::dialog::FileFilter filters[] = {
        { Translate("material.shader_files"), "*.hlsl;*.glsl;*.vert;*.frag;*.vs;*.ps" },
        { Translate("project.all_files"),   "*.*" }
    };
    const std::filesystem::path initialFolder = AssetRoot(ctx);
    return platform::dialog::BrowseForFile(
        Translate("material.select_shader"), filters, 2, nullptr, {}, PathToUtf8(initialFolder).c_str());
}

bool AssignCustomShader(EditorFrameContext& ctx, assets::ShaderStage stage,
                        std::string& path, ShaderHandle& handle, const std::string& selection)
{
    const auto file = PathFromUtf8(selection);
    const std::string ext = PathToUtf8(file.extension());
    if (!selection.empty() && ext != ".hlsl" && ext != ".glsl" && ext != ".vert" &&
        ext != ".frag" && ext != ".vs" && ext != ".ps")
        return false;

    const std::string normalized = NormalizeMaterialAssetPath(ctx, file);
    if (path == normalized)
        return false;
    const ShaderHandle loaded = normalized.empty() ? ShaderHandle::Invalid()
        : LoadRuntimeShader(ctx, normalized, stage);
    if (const auto* shader = ctx.registry.shaders.Get(loaded); shader && shader->stage != stage)
    {
        Debug::LogWarning("Materialeditor: Shader '%s' hat die falsche Stage.", normalized.c_str());
        return false;
    }
    path = normalized;
    handle = loaded;
    return true;
}

std::string SaveNewShaderSource(EditorFrameContext& ctx, assets::ShaderStage stage)
{
    const std::string& source = ctx.state.materialShaderDraft;
    if (source.find_first_not_of(" \t\r\n") == std::string::npos)
        return {};
    const auto material = ToAbsoluteAssetPath(ctx, ctx.state.selectedMaterialAssetPath);
    const auto folder = material.parent_path();
    std::error_code ec;
    if (!folder.empty())
        std::filesystem::create_directories(folder, ec);
    if (ec)
        return {};
    const bool vertex = stage == assets::ShaderStage::Vertex;
    const bool glsl = ctx.state.materialShaderDraftLanguage == 1;
    const char* extension = glsl ? (vertex ? ".vs.glsl" : ".fs.glsl") : (vertex ? ".vs.hlsl" : ".ps.hlsl");
    const auto file = MakeUniqueFilesystemPath(folder, PathToUtf8(material.stem()), extension);
#ifdef _WIN32
    auto* out = _wfopen(file.c_str(), L"wbx");
#else
    auto* out = std::fopen(file.c_str(), "wbx");
#endif
    if (!out)
        return {};
    const bool written = std::fwrite(source.data(), 1, source.size(), out) == source.size();
    const bool closed = std::fclose(out) == 0;
    if (!written || !closed)
    {
        std::filesystem::remove(file, ec);
        return {};
    }
    return PathToUtf8(file);
}

bool DrawCustomShaderSource(EditorFrameContext& ctx, assets::MaterialAsset& asset,
                            assets::ShaderStage stage, float width)
{
    const bool vertex = stage == assets::ShaderStage::Vertex;
    auto& path = vertex ? asset.vertexShaderPath : asset.fragmentShaderPath;
    auto& handle = vertex ? asset.vertexShader : asset.fragmentShader;
    const auto assign = [&](const std::string& selection) {
        return AssignCustomShader(ctx, stage, path, handle, selection);
    };
    bool changed = false;
    char buffer[1024];
    std::snprintf(buffer, sizeof(buffer), "%s", path.c_str());
    const bool unavailable = !path.empty() && ctx.assetPipeline && !handle.IsValid();
    if (unavailable)
        ImGui::PushStyleColor(ImGuiCol_Text, theme::kDanger);
    ImGui::SetNextItemWidth(width);
    if (value_ui::InputText("##shader_file", buffer, sizeof(buffer), ImGuiInputTextFlags_EnterReturnsTrue))
        changed = assign(buffer);
    if (unavailable)
    {
        ImGui::PopStyleColor();
        ImGui::SetItemTooltip("%s\n%s", path.c_str(), Translate(vertex ? "material.vertex_missing" : "material.fragment_missing"));
    }
    else
        ImGui::SetItemTooltip("%s", path.empty() ? Translate("material.graph_shader_drop") : path.c_str());
    if (ImGui::BeginDragDropTarget())
    {
        if (const auto* payload = ImGui::AcceptDragDropPayload("KROM_MOVE_FILE"))
            changed = assign(static_cast<const char*>(payload->Data)) || changed;
        ImGui::EndDragDropTarget();
    }

    ImGui::BeginDisabled(!platform::dialog::IsAvailable());
    if (ImGui::Button(Translate("material.graph_shader_file")))
    {
        const auto selected = BrowseForShaderFile(ctx);
        if (!selected.empty())
            changed = assign(selected) || changed;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    const std::string title = TranslateLabel(vertex ? "material.graph_vertex_shader" : "material.graph_pixel_shader",
                                             vertex ? "VertexShaderText" : "PixelShaderText");
    if (ImGui::Button(Translate("material.graph_shader_text")))
    {
        auto& state = ctx.state;
        state.materialShaderDraft.clear();
        if (!path.empty())
        {
            std::ifstream in(ToAbsoluteAssetPath(ctx, path), std::ios::binary);
            state.materialShaderDraft.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        state.materialShaderDraftAsset = state.selectedMaterialAssetPath;
        state.materialShaderDraftLanguage = path.empty()
            ? (ctx.shaderTarget == assets::ShaderTargetProfile::OpenGL_GLSL450 ? 1 : 0)
            : (path.ends_with(".glsl") || path.ends_with(".vert") || path.ends_with(".frag") ? 1 : 0);
        state.materialShaderDraftError = false;
        ImGui::OpenPopup(title.c_str());
    }
    if (ImGui::BeginPopupModal(title.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        auto& state = ctx.state;
        auto& source = state.materialShaderDraft;
        const auto resize = [](ImGuiInputTextCallbackData* data) {
            auto& text = *static_cast<std::string*>(data->UserData);
            text.resize(static_cast<size_t>(data->BufTextLen));
            data->Buf = text.data();
            return 0;
        };
        ImGui::RadioButton("HLSL", &state.materialShaderDraftLanguage, 0);
        ImGui::SameLine();
        ImGui::RadioButton("GLSL", &state.materialShaderDraftLanguage, 1);
        ImGui::InputTextMultiline("##shader_source", source.data(), source.capacity() + 1, ImVec2(620.f, 320.f),
                                  ImGuiInputTextFlags_CallbackResize | ImGuiInputTextFlags_AllowTabInput, resize, &source);
        ImGui::TextUnformatted(Translate("material.graph_shader_copy_hint"));
        const bool sameMaterial = state.materialShaderDraftAsset == state.selectedMaterialAssetPath;
        if (ImGui::Button(Translate("material.graph_shader_save")) && sameMaterial)
        {
            const std::string saved = SaveNewShaderSource(ctx, stage);
            state.materialShaderDraftError = saved.empty() || !assign(saved);
            if (!state.materialShaderDraftError)
            {
                changed = true;
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button(Translate("common.cancel")) || !sameMaterial)
            ImGui::CloseCurrentPopup();
        if (state.materialShaderDraftError)
            ImGui::TextUnformatted(Translate("material.graph_shader_save_error"));
        ImGui::EndPopup();
    }
    return changed;
}

bool RefreshCustomShaderParameters(EditorFrameContext& ctx, assets::MaterialAsset& asset)
{
    const auto refresh = [&](ShaderHandle& handle, const std::string& path, assets::ShaderStage stage) {
        if (!handle.IsValid() && !path.empty() && ctx.assetPipeline)
            handle = LoadRuntimeShader(ctx, path, stage);
        if (const auto* shader = ctx.registry.shaders.Get(handle); shader && shader->stage != stage)
            handle = ShaderHandle::Invalid();
    };
    refresh(asset.vertexShader, asset.vertexShaderPath, assets::ShaderStage::Vertex);
    refresh(asset.fragmentShader, asset.fragmentShaderPath, assets::ShaderStage::Fragment);
    return SyncCustomShaderParamsFromSource(ctx, asset);
}

bool RenameSelectedMaterialAsset(EditorFrameContext& ctx)
{
    if (ctx.state.selectedMaterialAssetPath.empty())
        return false;

    std::string draft = ctx.state.selectedMaterialAssetNameDraft;
    draft.erase(std::remove_if(draft.begin(), draft.end(), [](unsigned char ch) {
        return std::iscntrl(ch) != 0;
    }), draft.end());
    if (draft.empty())
        return false;
    if (draft.find_first_of("\\/:*?\"<>|") != std::string::npos)
        return false;

    std::filesystem::path currentRelative = PathFromUtf8(ctx.state.selectedMaterialAssetPath);
    std::filesystem::path renamedRelative = currentRelative.parent_path() / PathFromUtf8(draft);
    renamedRelative.replace_extension(".mat");

    const std::string currentNormalized = NormalizeMaterialAssetPath(ctx, currentRelative);
    const std::string renamedNormalized = NormalizeMaterialAssetPath(ctx, renamedRelative);
    if (renamedNormalized.empty() || renamedNormalized == currentNormalized)
        return false;

    const std::filesystem::path currentAbsolute = ToAbsoluteAssetPath(ctx, currentNormalized);
    const std::filesystem::path renamedAbsolute = ToAbsoluteAssetPath(ctx, renamedNormalized);
    if (std::filesystem::exists(renamedAbsolute))
        return false;

    std::error_code ec;
    std::filesystem::rename(currentAbsolute, renamedAbsolute, ec);
    if (ec)
        return false;

    ctx.world.ForEachAlive([&](EntityID id)
    {
        auto* material = ctx.world.Get<MaterialComponent>(id);
        if (!material)
            return;
        if (NormalizeMaterialAssetPath(ctx, PathFromUtf8(material->materialAssetPath)) == currentNormalized)
            material->materialAssetPath = renamedNormalized;
        for (MaterialComponent::SlotOverride& slot : material->slotOverrides)
            if (NormalizeMaterialAssetPath(ctx, PathFromUtf8(slot.materialAssetPath)) == currentNormalized)
                slot.materialAssetPath = renamedNormalized;
    });

    ctx.state.selectedMaterialAssetPath = renamedNormalized;
    ctx.state.selectedMaterialAssetNameDraft = PathToUtf8(PathFromUtf8(renamedNormalized).stem());
    ReloadSelectedMaterialAsset(ctx);
    ReapplyMaterialAssetToBoundEntities(ctx, renamedNormalized);
    return true;
}

void DrawMaterialPreview(EditorFrameContext& ctx, float previewSize)
{
    value_ui::TextDisabled(Translate("toolbar.preview"));

    const float size = std::max(1.f, previewSize);
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const ImVec2 end{start.x + size, start.y + size};

    ImGui::InvisibleButton("##materialPreviewImage", ImVec2(size, size));
    if (ImGui::IsItemHovered())
    {
        // Claim the wheel while hovering so the next frame routes it away from window scrolling.
        ImGui::SetKeyOwner(ImGuiKey_MouseWheelY, ImGui::GetItemID());
        const ImGuiIO& io = ImGui::GetIO();
        if (io.MouseWheel != 0.f)
            ctx.state.materialPreviewDistance =
                std::clamp(ctx.state.materialPreviewDistance - io.MouseWheel * 0.32f, 2.0f, 8.0f);
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Right))
        {
            const ImVec2 delta = io.MouseDelta;
            ctx.state.materialPreviewYawDeg -= delta.x * 0.35f;
            ctx.state.materialPreviewPitchDeg =
                std::clamp(ctx.state.materialPreviewPitchDeg + delta.y * 0.35f, -85.f, 85.f);
        }
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(start, end, IM_COL32(38, 40, 43, 255));

    void* texId = ctx.materialPreviewTexture ? ctx.materialPreviewTexture() : nullptr;
    if (texId)
    {
        const ImVec2 uv0 = ctx.materialPreviewFlipY ? ImVec2(0.f, 1.f) : ImVec2(0.f, 0.f);
        const ImVec2 uv1 = ctx.materialPreviewFlipY ? ImVec2(1.f, 0.f) : ImVec2(1.f, 1.f);
        dl->AddImage(reinterpret_cast<ImTextureID>(texId), start, end, uv0, uv1);
    }
    else
    {
        const char* label = Translate("material.no_preview");
        const ImVec2 textSize = ImGui::CalcTextSize(label);
        dl->AddText(ImVec2(start.x + (size - textSize.x) * 0.5f,
                           start.y + (size - textSize.y) * 0.5f),
                    IM_COL32(170, 170, 170, 255),
                    label);
    }
    dl->AddRect(start, end, IM_COL32(95, 98, 104, 255));
    ImGui::Dummy(ImVec2(0.f, 4.f));
}

bool MaterialAutoSave()
{
    return GetEditorPreferences().autoSaveEditors;
}

// With auto-save off, an action that leaves unsaved changes waits for the answer.
std::function<void()>& PendingMaterialAction()
{
    static std::function<void()> action;
    return action;
}

bool& MaterialUnsavedQuestion()
{
    static bool ask = false;
    return ask;
}

void DrawMaterialActions(EditorFrameContext& ctx)
{
    // Save button like in every editor; changes are saved automatically anyway.
    const bool canSave = ctx.state.selectedMaterialAssetLoaded &&
                         !ctx.state.selectedMaterialAssetPath.empty();
    if (!canSave)
        ImGui::BeginDisabled();
    if (card::SaveButton(TranslateLabel("common.save", "Speichern##material")))
        SaveSelectedMaterialAsset(ctx);
    if (!canSave)
        ImGui::EndDisabled();

    ImGui::SameLine();
    if (!MaterialAutoSave() && ctx.state.selectedMaterialAssetDirty)
    {
        ImGui::TextColored(ImVec4(0.96f, 0.82f, 0.30f, 1.f), "%s", Translate("common.unsaved_changes"));
        return;
    }
    const char* idleText = MaterialAutoSave() ? "material.autosaved" : "material.saved";
    const MaterialSaveQueue::StatusSnapshot saveStatus =
        QueryMaterialSaveStatus(ctx, ctx.state.selectedMaterialAssetPath);
    const auto now = std::chrono::steady_clock::now();
    const auto ageMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - saveStatus.lastUpdate).count();
    switch (saveStatus.state)
    {
    case MaterialSaveQueue::SaveState::Pending:
    case MaterialSaveQueue::SaveState::Saving:
        ImGui::TextColored(ImVec4(0.96f, 0.82f, 0.30f, 1.f), Translate("material.saving"));
        break;
    case MaterialSaveQueue::SaveState::Saved:
        if (ageMs < 2000)
            ImGui::TextColored(ImVec4(0.48f, 0.90f, 0.56f, 1.f), Translate("material.saved"));
        else
            value_ui::TextDisabled(Translate(idleText));
        break;
    case MaterialSaveQueue::SaveState::Failed:
        ImGui::TextColored(ImVec4(0.96f, 0.42f, 0.38f, 1.f), Translate("status.save_failed"));
        break;
    case MaterialSaveQueue::SaveState::Idle:
    default:
        value_ui::TextDisabled(Translate(idleText));
        break;
    }
}

bool DrawUvProperties(assets::MaterialAsset& asset, EditorMaterialTemplate type, const DrawMaterialUVInput& drawInput)
{
    bool dirty = false;
    const bool supportsBoxProjection = type == EditorMaterialTemplate::PbrLit;
    if (supportsBoxProjection)
    {
        int projection = asset.boxProjection ? 1 : 0;
        const char* projections[] = { Translate("material.uv_mesh"), Translate("material.uv_box") };
        if (value_ui::Combo(Translate("material.uv_projection"), &projection, projections, IM_ARRAYSIZE(projections)))
        {
            asset.boxProjection = projection == 1;
            dirty = true;
        }
    }
    if (supportsBoxProjection && asset.boxProjection)
    {
        float tileSize = asset.BoxProjectionTileSize();
        if (value_ui::DragFloat(Translate("material.box_tile_size"), &tileSize, 0.01f,
                                assets::MaterialAsset::kMinBoxTileSize, 100000.f, "%.3f"))
        {
            asset.boxTileSize = std::clamp(tileSize, assets::MaterialAsset::kMinBoxTileSize, 100000.f);
            dirty = true;
        }
    }
    else
    {
        // One source per texture: graph texture nodes have their own UV fields, these
        // apply only to textures set directly on the material. Greyed out while the
        // graph has texture nodes and nothing is set directly.
        const auto& graph = asset.graph;
        const auto direct = [&graph](const assets::MaterialTextureRef& texture, uint32_t link) {
            return !texture.path.empty() && graph.Find(link) == nullptr;
        };
        const bool anyDirect = direct(asset.baseColorTexture, graph.baseColor) ||
                               direct(asset.emissiveTexture, graph.emissive) ||
                               (type != EditorMaterialTemplate::Unlit && !asset.normalTexture.path.empty() &&
                                graph.ResolveTexture(graph.normal, {}).empty()) ||
                               (supportsBoxProjection && ((!asset.metallicRoughnessTexture.path.empty() &&
                                graph.ResolveTexture(graph.orm, {}).empty()) || !asset.occlusionTexture.path.empty()));
        const bool textureNodes = std::any_of(graph.nodes.begin(), graph.nodes.end(), [](const auto& node) {
            return node.type == assets::MaterialNodeType::Texture;
        });
        const bool perNode = textureNodes && !anyDirect;
        ImGui::BeginDisabled(perNode);
        const auto field = [&](const char* label, math::Vec2& value, uint32_t& link, float min, float max) {
            if (type != EditorMaterialTemplate::Custom)
                return drawInput(Translate(label), value, link, min, max);
            return value_ui::DragFloat2(Translate(label), &value.x, 0.01f, min, max, "%.3f");
        };
        dirty = field("material.uv_scale", asset.uvScale, asset.graph.uvScaleLink, -16.f, 16.f) || dirty;
        dirty = field("material.uv_offset", asset.uvOffset, asset.graph.uvOffsetLink, -1.f, 1.f) || dirty;
        ImGui::EndDisabled();
        if (perNode)
            value_ui::TextDisabled("%s", Translate("material.uv_per_node"));
        else if (textureNodes)
            value_ui::TextDisabled("%s", Translate("material.uv_direct_only"));
    }

    return dirty;
}

bool DrawRenderOptions(assets::MaterialAsset& asset, bool allowInstancing)
{
    bool dirty = false;
    if (value_ui::Checkbox(TranslateLabel("material.double_sided", "Double Sided").c_str(),  &asset.doubleSided))  dirty = true;
    ImGui::SameLine(0.f, 2.f * ImGui::GetStyle().ItemSpacing.x);
    if (value_ui::Checkbox(TranslateLabel("light.cast_shadows", "Cast Shadows").c_str(),  &asset.castShadows))  dirty = true;
    if (allowInstancing)
    {
        if (value_ui::Checkbox(TranslateLabel("material.instanced", "Instanced").c_str(), &asset.instanced))
            dirty = true;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(Translate("material.instanced_tip"));
    }

    int alphaMode = static_cast<int>(asset.alphaMode);
    const char* alphaModes[] = { Translate("material.opaque"), Translate("material.mask"), Translate("material.blend") };
    if (value_ui::Combo(TranslateLabel("material.alpha_mode", "Alpha Mode").c_str(), &alphaMode, alphaModes, IM_ARRAYSIZE(alphaModes)))
    {
        asset.alphaMode   = static_cast<assets::MaterialAlphaMode>(std::clamp(alphaMode, 0, 2));
        asset.transparent = asset.alphaMode == assets::MaterialAlphaMode::Blend;
        dirty = true;
    }
    if (asset.alphaMode != assets::MaterialAlphaMode::Opaque &&
        value_ui::DragFloat(TranslateLabel("material.alpha_cutoff", "Alpha Cutoff").c_str(), &asset.alphaCutoff,
                            0.005f, 0.f, 1.f, "%.3f", ImGuiSliderFlags_AlwaysClamp))
        dirty = true;

    return dirty;
}

bool DrawSurfaceScalars(EditorFrameContext& ctx, assets::MaterialAsset& asset, EditorMaterialTemplate currentTemplate)
{
    bool dirty = false;
    if (currentTemplate != EditorMaterialTemplate::Unlit)
    {
        // Custom: nur die PBR-Faktoren zeigen, die der Shader in PerMaterial deklariert.
        const bool isCustom = currentTemplate == EditorMaterialTemplate::Custom;
        const std::unordered_set<std::string> declared =
            isCustom ? ScanPerMaterialFieldNames(ctx, asset) : std::unordered_set<std::string>{};
        auto supports = [&](const char* field) { return !isCustom || declared.count(field) != 0u; };
        const auto scalarField = [](const std::string& label, float& value, float maximum) {
            return value_ui::DragFloat(label.c_str(), &value, maximum == 1.f ? 0.005f : 0.01f,
                                       0.f, maximum, "%.3f", ImGuiSliderFlags_AlwaysClamp);
        };
        if (supports("metallicFactor") &&
            scalarField(TranslateLabel("material.metallic", "Metallic"), asset.metallicFactor, 1.f)) dirty = true;
        if (supports("roughnessFactor") &&
            scalarField(TranslateLabel("material.roughness", "Roughness"), asset.roughnessFactor, 1.f)) dirty = true;
        if ((supports("normalScale") || (isCustom && declared.count("normalStrength"))) &&
            scalarField(TranslateLabel("material.normal_strength", "Normal Strength"), asset.normalScale, 4.f)) dirty = true;
        if (supports("occlusionStrength") &&
            scalarField(TranslateLabel("material.occlusion", "Occlusion"), asset.occlusionStrength, 4.f)) dirty = true;
    }

    return dirty;
}

void DrawSelectedMaterialEditor(EditorFrameContext& ctx)
{
    if (!ctx.state.selectedMaterialAssetLoaded && !ctx.state.selectedMaterialAssetPath.empty())
        ReloadSelectedMaterialAsset(ctx);

    if (!ctx.state.selectedMaterialAssetLoaded)
    {
        if (ctx.state.materialAssetLoadInFlight && !ctx.state.selectedMaterialAssetPath.empty())
        {
            value_ui::TextDisabled(Translate("material.loading"));
            ImGui::Separator();
            ImGui::TextWrapped(Translate("material.loading_path"), ctx.state.selectedMaterialAssetPath.c_str());
            return;
        }

        value_ui::TextDisabled(Translate("material.none"));
        ImGui::Separator();
        ImGui::TextWrapped(Translate("material.select_asset"));
        return;
    }

    assets::MaterialAsset& asset = ctx.state.selectedMaterialAssetData;
    EditorMaterialTemplate currentTemplate = ParseMaterialTemplate(asset.templateName);
    std::shared_ptr<assets::MaterialAsset> beforePaste;
    std::string beforeDraft;
    const std::string clipboardMaterialPath = ctx.state.selectedMaterialAssetPath;
    value_ui::Scope materialValueScope([&] {
        beforePaste = std::make_shared<assets::MaterialAsset>(asset);
        beforeDraft = ctx.state.selectedMaterialAssetNameDraft;
    }, [&] {
        const auto afterPaste = std::make_shared<assets::MaterialAsset>(asset);
        const auto afterDraft = ctx.state.selectedMaterialAssetNameDraft;
        auto apply = [&ctx, path = clipboardMaterialPath](const assets::MaterialAsset& snapshot, const std::string& draft) {
            if (ctx.state.selectedMaterialAssetPath == path)
            {
                ctx.state.selectedMaterialAssetData = snapshot;
                ctx.state.selectedMaterialAssetNameDraft = draft;
                ctx.state.selectedMaterialAssetDirty = !MaterialAutoSave();
            }
            PreviewMaterialAssetOnBoundEntities(ctx, path, snapshot);
            if (MaterialAutoSave() || ctx.state.selectedMaterialAssetPath != path)
                QueueMaterialAssetSave(ctx, path, snapshot);
        };
        RecordEditorValueChange(ctx, "history.paste_material",
            [apply, beforePaste, beforeDraft] { apply(*beforePaste, beforeDraft); },
            [apply, afterPaste, afterDraft] { apply(*afterPaste, afterDraft); });
    });
    bool dirty = false;

    if (!ctx.state.materialEditPending)
    {
        ctx.state.materialEditBaseline = asset;
        ctx.state.materialEditPath = ctx.state.selectedMaterialAssetPath;
    }

    DrawMaterialActions(ctx);
    ImGui::Separator();

    const ImVec2 available = ImGui::GetContentRegionAvail();
    const float layoutHeight = std::max(1.f, available.y);
    const float settingsWidth = std::clamp(available.x - 550.f, 240.f, 440.f);
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(ImGui::GetStyle().ItemSpacing.x * 0.5f, 0.f));
    constexpr ImGuiTableFlags layoutFlags = ImGuiTableFlags_SizingFixedFit |
        ImGuiTableFlags_NoPadOuterX | ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_NoHostExtendY;
    if (!ImGui::BeginTable("##material_layout", 2, layoutFlags, ImVec2(0.f, layoutHeight)))
    {
        ImGui::PopStyleVar();
        return;
    }
    ImGui::TableSetupColumn("##preview_and_properties", ImGuiTableColumnFlags_WidthFixed, settingsWidth);
    ImGui::TableSetupColumn("##textures", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableNextRow();
    // Apply type changes before drawing the settings for the selected template.
    ImGui::TableSetColumnIndex(1);

    // Name and file as label-left rows, like the inspector.
    {
        value_ui::InlineLabelScope rows;
        char nameBuffer[256];
        std::snprintf(nameBuffer, sizeof(nameBuffer), "%s", ctx.state.selectedMaterialAssetNameDraft.c_str());
        value_ui::RowLabel(Translate("material.name"), nullptr, false);
        const std::string renameLabel = TranslateLabel("common.rename", "Umbenennen");
        const float renameWidth = ImGui::CalcTextSize(renameLabel.c_str(), nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2.f;
        ImGui::SetNextItemWidth(std::max(1.f, ImGui::GetContentRegionAvail().x - renameWidth - ImGui::GetStyle().ItemSpacing.x));
        if (value_ui::InputText("##Materialname", nameBuffer, sizeof(nameBuffer)))
            ctx.state.selectedMaterialAssetNameDraft = nameBuffer;
        ImGui::SameLine();
        if (ImGui::Button(renameLabel.c_str(), ImVec2(renameWidth, 0.f)))
            RenameSelectedMaterialAsset(ctx);
        value_ui::RowLabel(Translate("common.file"), nullptr, false);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(ctx.state.selectedMaterialAssetPath.c_str());
    }

    bool templateChanged = false;
    const char* templates[] = { "PBR Lit", Translate("material.unlit"), Translate("material.legacy"), Translate("material.custom") };
    const auto drawMaterialType = [&]
    {
        int materialTemplate = static_cast<int>(currentTemplate);
        if (!value_ui::Combo("##material_type", &materialTemplate, templates, IM_ARRAYSIZE(templates)))
            return currentTemplate;
        const auto newTpl = static_cast<EditorMaterialTemplate>(std::clamp(materialTemplate, 0, 3));
        asset.templateName = MaterialTemplateToken(newTpl);
        ctx.state.newMaterialTemplate = newTpl;
        currentTemplate = newTpl;
        SanitizeMaterialForTemplate(asset);
        templateChanged = true;
        dirty = true;
        return currentTemplate;
    };
    ImGui::Separator();
    dirty = DrawMaterialGraph(ctx, asset, currentTemplate, drawMaterialType, [&](EditorMaterialTemplate type, const DrawMaterialUVInput& uvInput) {
        const bool pbr = type == EditorMaterialTemplate::PbrLit;
        const bool custom = type == EditorMaterialTemplate::Custom;
        bool settingsChanged = custom && RefreshCustomShaderParameters(ctx, asset);
        // Sections as inspector cards; the graph sets the item width of the material node.
        const float cardWidth = ImGui::CalcItemWidth() + ImGui::CalcTextSize(Translate("material.box_tile_size")).x +
                                ImGui::GetStyle().ItemInnerSpacing.x;
        const ImU32 iconColor = IM_COL32(235, 170, 85, 255); // the inspector's material colour
        const auto section = [&](const char* id, const char* key, const char* fallback, const char* glyph,
                                 const std::function<bool()>& body) {
            card::Card sectionCard;
            const std::string title = TranslateLabel(key, fallback);
            const std::string shown = title.substr(0, title.find("##"));
            if (card::Begin(sectionCard, id, shown.c_str(), glyph, iconColor, cardWidth, false))
            {
                ImGui::PushItemWidth(std::max(1.f, ImGui::CalcItemWidth() - card::kInset));
                settingsChanged = body() || settingsChanged;
                ImGui::PopItemWidth();
            }
            card::End(sectionCard);
        };
        section("##uv_card", "material.uv_coordinates", "UV Coordinates", ICON_LC_GRID_3X3,
                [&] { return DrawUvProperties(asset, type, uvInput); });
        section("##render_card", "material.render_options", "Render Options", ICON_LC_SETTINGS,
                [&] { return DrawRenderOptions(asset, !pbr); });
        if (custom)
            section("##shader_card", "material.shader_parameters", "Shader Parameters", ICON_LC_CODE, [&] {
                const bool scalars = DrawSurfaceScalars(ctx, asset, type);
                return DrawCustomShaderParams(ctx, asset) || scalars;
            });
        return settingsChanged;
    }, [&](assets::ShaderStage stage, float width) {
        return DrawCustomShaderSource(ctx, asset, stage, width);
    }) || dirty;

    ImGui::TableSetColumnIndex(0);
    if (ImGui::BeginChild("##material_preview_pane", ImVec2(0.f, 0.f), ImGuiChildFlags_None))
    {
        const ImVec2 paneSize = ImGui::GetContentRegionAvail();
        const float previewSize = std::min(paneSize.x, paneSize.y);
        DrawMaterialPreview(ctx, previewSize);
    }
    ImGui::EndChild();
    ImGui::EndTable();
    ImGui::PopStyleVar();

    if (dirty)
    {
        dirty = SanitizeMaterialForTemplate(asset) || dirty;
        ctx.state.selectedMaterialAssetDirty = true;

        // Custom-Shader-Pfade sofort SYNCHRON speichern — der async Save-Queue
        // kommt zu spaet: ein async Reload wuerde die Pfade vorher ueberschreiben.
        // Without auto-save the change stays in memory until Save.
        if (MaterialAutoSave() &&
            ((currentTemplate == EditorMaterialTemplate::Custom &&
              (!asset.vertexShaderPath.empty() || !asset.fragmentShaderPath.empty())) ||
             templateChanged))
        {
            SaveMaterialAssetFile(ctx, ctx.state.selectedMaterialAssetPath, asset);
        }

        PreviewMaterialAssetOnBoundEntities(ctx, ctx.state.selectedMaterialAssetPath, asset);
        if (MaterialAutoSave())
            SaveSelectedMaterialAsset(ctx);
    }

    // A whole drag or slider gesture becomes a single undo step.
    auto& state = ctx.state;
    state.materialEditPending = state.materialEditPending || dirty;
    const bool interacting = ImGui::IsAnyItemActive() || ImGui::IsAnyMouseDown() || ImGui::GetDragDropPayload();
    if (state.materialEditPending && !interacting)
    {
        state.materialEditPending = false;
        if (state.materialEditPath == state.selectedMaterialAssetPath &&
            assets::SerializeMaterialProperties(state.materialEditBaseline) != assets::SerializeMaterialProperties(asset))
        {
            const auto before = std::make_shared<assets::MaterialAsset>(state.materialEditBaseline);
            const auto after = std::make_shared<assets::MaterialAsset>(asset);
            auto apply = [&ctx, path = state.materialEditPath](const assets::MaterialAsset& snapshot) {
                if (ctx.state.selectedMaterialAssetPath == path)
                {
                    ctx.state.selectedMaterialAssetData = snapshot;
                    ctx.state.selectedMaterialAssetDirty = !MaterialAutoSave();
                    ctx.state.newMaterialTemplate = ParseMaterialTemplate(snapshot.templateName);
                }
                PreviewMaterialAssetOnBoundEntities(ctx, path, snapshot);
                if (MaterialAutoSave() || ctx.state.selectedMaterialAssetPath != path)
                    QueueMaterialAssetSave(ctx, path, snapshot);
            };
            RecordEditorValueChange(ctx, "history.edit_material",
                [apply, before] { apply(*before); },
                [apply, after] { apply(*after); });
        }
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Öffentliche Implementierung — aufrufbar aus EditorUI.cpp (Ctrl+S)
// ---------------------------------------------------------------------------

bool SetEntityMaterialOverride(EditorFrameContext& ctx, EntityID entity, int slotIndex, const assets::MaterialAsset& asset)
{
    if (!ctx.world.Get<MeshComponent>(entity)) return false;
    const auto* component = ctx.world.Get<MaterialComponent>(entity);
    const auto* slot = component && slotIndex >= 0 ? component->FindSlotOverride(static_cast<uint32_t>(slotIndex)) : nullptr;
    const std::string source = SerializeMaterialOverride(asset);
    if ((slot ? slot->materialOverrideSource : (slotIndex < 0 && component ? component->materialOverrideSource : std::string{})) == source) return false;
    std::string path = slot ? slot->materialAssetPath : (component ? component->materialAssetPath : std::string{});
    if (path.empty()) path = ctx.state.selectedMaterialAssetPath;
    if (path.empty()) return false;
    return ExecuteSceneMutation(ctx, "history.edit_material", [&] {
        // A selected imported/inherited slot need not have a local override yet.
        // Create it only after capturing the undo snapshot.
        auto* material = ctx.world.Get<MaterialComponent>(entity);
        if (!material) material = &ctx.world.Add<MaterialComponent>(entity);
        if (slotIndex >= 0)
        {
            auto* target = material->FindSlotOverride(static_cast<uint32_t>(slotIndex));
            if (!target)
            {
                MaterialComponent::SlotOverride added;
                added.submeshIndex = static_cast<uint32_t>(slotIndex);
                material->slotOverrides.push_back(std::move(added));
                target = &material->slotOverrides.back();
            }
            target->materialAssetPath = path;
            target->materialOverrideSource = source;
        }
        else
        {
            material->materialAssetPath = path;
            material->materialOverrideSource = source;
        }
        const uint32_t index = static_cast<uint32_t>(slotIndex);
        ApplyMaterialAssetToMeshEntity(ctx, entity, path, slotIndex >= 0 ? &index : nullptr);
    });
}

bool ResetEntityMaterialOverride(EditorFrameContext& ctx, EntityID entity, int slotIndex, const std::filesystem::path& materialPath)
{
    return ExecuteSceneMutation(ctx, "history.edit_material", [&] {
        if (slotIndex >= 0)
            ApplyMaterialAssetToEntitySlot(ctx, entity, static_cast<uint32_t>(slotIndex), materialPath);
        else
            ApplyMaterialAssetToEntity(ctx, entity, materialPath);
    });
}

bool CreateEntityMaterialCopy(EditorFrameContext& ctx, EntityID entity, int slotIndex, const std::filesystem::path& materialPath)
{
    const std::string normalizedPath = NormalizeMaterialAssetPath(ctx, materialPath);
    if (normalizedPath.empty())
        return false;

    std::string localSource;
    if (const auto* component = ctx.world.Get<MaterialComponent>(entity))
    {
        const auto* slot = slotIndex >= 0 ? component->FindSlotOverride(static_cast<uint32_t>(slotIndex)) : nullptr;
        localSource = slot ? slot->materialOverrideSource : (slotIndex < 0 ? component->materialOverrideSource : std::string{});
    }

    // Unsaved editor state wins over the file, local values win over both.
    assets::MaterialAsset copy;
    bool loaded = false;
    if (!localSource.empty())
        loaded = ParseEditorMaterialAssetSource(localSource, normalizedPath, copy);
    else if (ctx.state.selectedMaterialAssetLoaded && ctx.state.selectedMaterialAssetPath == normalizedPath)
    {
        copy = ctx.state.selectedMaterialAssetData;
        loaded = true;
    }
    else
    {
        std::ifstream in(ToAbsoluteAssetPath(ctx, normalizedPath), std::ios::binary);
        const std::string source((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        loaded = in.good() || in.eof() ? ParseEditorMaterialAssetSource(source, normalizedPath, copy) : false;
    }
    if (!loaded)
    {
        Debug::LogWarning("Material copy failed, source could not be read: %s", normalizedPath.c_str());
        return false;
    }

    const std::filesystem::path sourceAbsolute = ToAbsoluteAssetPath(ctx, normalizedPath);
    const std::filesystem::path copyPath = MakeUniqueFilesystemPath(sourceAbsolute.parent_path(),
        PathToUtf8(sourceAbsolute.stem()) + "_copy", ".mat");
    if (!SaveMaterialAssetFile(ctx, ToAssetRelativePath(ctx, copyPath), copy))
    {
        Debug::LogWarning("Material copy could not be written: %s", PathToUtf8(copyPath).c_str());
        return false;
    }

    const bool assigned = ExecuteSceneMutation(ctx, "history.assign_material", [&] {
        if (slotIndex >= 0)
            ApplyMaterialAssetToEntitySlot(ctx, entity, static_cast<uint32_t>(slotIndex), copyPath);
        else
            ApplyMaterialAssetToEntity(ctx, entity, copyPath);
    });
    OpenMaterialAsset(ctx, copyPath);
    return assigned;
}

bool SaveSelectedMaterialAssetIfChanged(EditorFrameContext& ctx)
{
    return ctx.state.selectedMaterialAssetLoaded && ctx.state.selectedMaterialAssetDirty &&
           SaveSelectedMaterialAsset(ctx);
}

bool SaveSelectedMaterialAsset(EditorFrameContext& ctx)
{
    if (ctx.state.selectedMaterialAssetPath.empty())
        return false;

    SanitizeMaterialForTemplate(ctx.state.selectedMaterialAssetData);
    QueueMaterialAssetSave(ctx, ctx.state.selectedMaterialAssetPath, ctx.state.selectedMaterialAssetData);
    ctx.state.selectedMaterialAssetDirty = false;
    return true;
}

void SelectMaterialAsset(EditorFrameContext& ctx, const std::filesystem::path& path)
{
    const std::string normalizedPath = NormalizeMaterialAssetPath(ctx, path);
    if (normalizedPath.empty())
        return;

    if (ctx.state.materialAssetLoadInFlight)
    {
        ctx.state.queuedMaterialAssetPath = normalizedPath;
        return;
    }

    if (ctx.state.selectedMaterialAssetLoaded &&
        NormalizeMaterialAssetPath(ctx, PathFromUtf8(ctx.state.selectedMaterialAssetPath)) == normalizedPath)
    {
        ctx.state.selectedMaterialAssetPath = normalizedPath;
        return;
    }

    if (ctx.state.selectedMaterialAssetDirty)
    {
        // Without auto-save the open editor asks; a closed one cannot, so it saves.
        if (!MaterialAutoSave() && ctx.state.materialWindowOpen)
        {
            PendingMaterialAction() = [&ctx, normalizedPath] { StartAsyncSelectedMaterialAssetLoad(ctx, normalizedPath); };
            MaterialUnsavedQuestion() = true;
            return;
        }
        SaveSelectedMaterialAsset(ctx);
    }

    StartAsyncSelectedMaterialAssetLoad(ctx, normalizedPath);
}

void OpenMaterialAsset(EditorFrameContext& ctx, const std::filesystem::path& path)
{
    if (!IsSelectedMaterialAssetAlreadyLoaded(ctx, path))
        SelectMaterialAsset(ctx, path);
    ctx.state.materialWindowOpen = true;
    ctx.state.materialWindowFocusRequest = true;
}

void QueueOpenMaterialAsset(EditorFrameContext& ctx, const std::filesystem::path& path)
{
    ctx.state.pendingOpenMaterialAssetPath = NormalizeMaterialAssetPath(ctx, path);
}

void FlushPendingMaterialOpen(EditorFrameContext& ctx)
{
    PollAsyncSelectedMaterialAssetLoad(ctx);

    if (ctx.state.pendingOpenMaterialAssetPath.empty())
        return;

    const std::filesystem::path pendingPath = PathFromUtf8(ctx.state.pendingOpenMaterialAssetPath);
    ctx.state.pendingOpenMaterialAssetPath.clear();
    OpenMaterialAsset(ctx, pendingPath);
}

bool CreateMaterialAsset(EditorFrameContext& ctx, const std::filesystem::path& directory)
{
    const std::filesystem::path targetDir = directory.empty() ? AssetRoot(ctx) : directory;
    const std::filesystem::path materialPath =
        MakeUniqueFilesystemPath(targetDir, "NewMaterial", ".mat");

    assets::MaterialAsset asset{};
    asset.templateName    = MaterialTemplateToken(ctx.state.newMaterialTemplate);
    asset.castShadows     = true;
    asset.doubleSided     = false;
    asset.transparent     = false;
    asset.alphaMode       = assets::MaterialAlphaMode::Opaque;
    asset.baseColorFactor = {1.f, 1.f, 1.f, 1.f};
    asset.metallicFactor  = 0.f;
    asset.roughnessFactor = 0.65f;
    asset.emissiveFactor  = {0.f, 0.f, 0.f};

    const std::string relativePath = ToAssetRelativePath(ctx, materialPath);
    if (!SaveMaterialAssetFile(ctx, relativePath, asset))
        return false;

    OpenMaterialAsset(ctx, materialPath);
    ctx.state.selectedMaterialAssetDirty = false;
    return true;
}

// Weist einem Mesh-Entity das Default-Material zu, falls es nach dem Load keines
// hat. Greift nicht, wenn bereits ein gültiges Material gesetzt ist oder das
// MeshAsset eigene materialHandles liefert (Import-Materialien nicht überschreiben).
static void EnsureDefaultMaterialIfMissing(EditorFrameContext& ctx, EntityID id)
{
    auto* meshComp = ctx.world.Get<MeshComponent>(id);
    if (!meshComp || !meshComp->mesh.IsValid())
        return;

    if (const auto* material = ctx.world.Get<MaterialComponent>(id); material && material->material.IsValid())
        return;

    if (const assets::MeshAsset* mesh = ctx.registry.meshes.Get(meshComp->mesh))
        for (const MaterialHandle& h : mesh->materialHandles)
            if (h.IsValid())
                return;

    AssignDefaultMaterialToEntity(ctx, id);
}

void ResolveMaterialAssetBindings(EditorFrameContext& ctx, std::span<const EntityID> entities)
{
    if (!ctx.assetPipeline)
        return;

    const auto resolve = [&](EntityID id)
    {
        auto* meshComp = ctx.world.Get<MeshComponent>(id);
        if (!meshComp)
            return;

        auto* material = ctx.world.Get<MaterialComponent>(id);
        if (material)
        {
            if (!material->materialAssetPath.empty())
            {
                material->materialAssetPath = NormalizeMaterialAssetPath(ctx, PathFromUtf8(material->materialAssetPath));
                ApplyMaterialAssetToMeshEntity(ctx, id, material->materialAssetPath);
            }

            for (MaterialComponent::SlotOverride& slot : material->slotOverrides)
            {
                if (slot.materialAssetPath.empty())
                    continue;
                slot.materialAssetPath = NormalizeMaterialAssetPath(ctx, PathFromUtf8(slot.materialAssetPath));
                ApplyMaterialAssetToMeshEntity(ctx, id, slot.materialAssetPath, &slot.submeshIndex);
            }
        }

        // Mesh-Entity ohne aufgelöstes Material → Default-Material zuweisen, damit das
        // Objekt nach Scene-Load nicht unsichtbar bleibt (z.B. glTF ganz ohne Material).
        EnsureDefaultMaterialIfMissing(ctx, id);
    };
    if (entities.empty())
        ctx.world.ForEachAlive(resolve);
    else
        for (const EntityID id : entities)
            resolve(id);
}

void TickMaterialTextureSync(EditorFrameContext& ctx)
{
    if (!ctx.assetPipeline)
        return;

    // Pro Frame: höchstens ein Material-Reapply um Frame-Spikes zu vermeiden.
    // Nur Materialien die gerade NICHT im Editor bearbeitet werden (dirty=false),
    // um Konflikte mit unsaved Änderungen zu vermeiden.
    // Only an unsaved material needs protection from texture reapplication.
    // Path normalization can touch the filesystem: do it once, and only then.
    const bool protectSelected = ctx.state.selectedMaterialAssetLoaded &&
                                 ctx.state.selectedMaterialAssetDirty;
    const std::string normalizedSelected = protectSelected
        ? NormalizeMaterialAssetPath(ctx, PathFromUtf8(ctx.state.selectedMaterialAssetPath))
        : std::string{};
    for (const auto& [key, binding] : SharedMaterialBindings())
    {
        if (!binding.runtimeMaterial.IsValid())
            continue;

        // Nicht eingreifen wenn das Material gerade im Editor bearbeitet wird
        if (protectSelected && normalizedSelected == key.materialPath)
            continue;

        // Prüfen ob eine Textur deren Pfad gesetzt ist noch nicht geladen wurde
        // und jetzt geladen werden kann.
        bool textureNowAvailable =
            (!binding.baseColorPath.empty() && !binding.baseColorTexture.IsValid() &&
             LoadRuntimeTexture(ctx, binding.baseColorPath).IsValid()) ||
            (!binding.ormPath.empty()        && !binding.ormTexture.IsValid()        &&
             LoadRuntimeTexture(ctx, binding.ormPath,
                                binding.isPbr ? TextureLoadRole::LinearData : TextureLoadRole::Auto).IsValid()) ||
            (!binding.normalPath.empty()     && !binding.normalTexture.IsValid()     &&
             LoadRuntimeTexture(ctx, binding.normalPath,
                                binding.isPbr ? TextureLoadRole::NormalMap : TextureLoadRole::Auto).IsValid()) ||
            (!binding.emissivePath.empty()   && !binding.emissiveTexture.IsValid()   &&
             LoadRuntimeTexture(ctx, binding.emissivePath).IsValid());

        for (size_t i = 0; i < binding.graphTextures.size() && !textureNowAvailable; ++i)
            textureNowAvailable = !binding.graphTexturePaths[i].empty() && !binding.graphTextures[i].IsValid() &&
                LoadRuntimeTexture(ctx, binding.graphTexturePaths[i]).IsValid();

        if (!textureNowAvailable)
            continue;

        // ReapplyMaterialAssetToBoundEntities nutzt denselben Pfad wie die
        // reguläre Zuweisung und behandelt den useInMemory-Flag korrekt.
        ReapplyMaterialAssetToBoundEntities(ctx, key.materialPath);
        break; // nur eines pro Frame
    }
}

bool ApplySelectedMaterialToEntity(EditorFrameContext& ctx, EntityID entity)
{
    if (ctx.state.selectedMaterialAssetPath.empty())
        return false;
    return ApplyMaterialAssetToMeshEntity(ctx, entity, ctx.state.selectedMaterialAssetPath);
}

bool ApplyMaterialAssetToEntity(EditorFrameContext& ctx,
                                EntityID entity,
                                const std::filesystem::path& materialPath)
{
    if (auto* c = ctx.world.Get<MaterialComponent>(entity)) c->materialOverrideSource.clear();
    return ApplyMaterialAssetToMeshEntity(ctx, entity, NormalizeMaterialAssetPath(ctx, materialPath));
}

bool ApplyMaterialAssetToEntitySlot(EditorFrameContext& ctx,
                                    EntityID entity,
                                    uint32_t submeshIndex,
                                    const std::filesystem::path& materialPath)
{
    if (auto* c = ctx.world.Get<MaterialComponent>(entity))
        if (auto* slot = c->FindSlotOverride(submeshIndex)) slot->materialOverrideSource.clear();
    const std::string normalizedPath = NormalizeMaterialAssetPath(ctx, materialPath);
    return ApplyMaterialAssetToMeshEntity(ctx, entity, normalizedPath, &submeshIndex);
}

bool AssignDefaultMaterialToEntity(EditorFrameContext& ctx, EntityID entity)
{
    auto* meshComp = ctx.world.Get<MeshComponent>(entity);
    if (!meshComp || !meshComp->mesh.IsValid())
        return false;

    assets::MeshAsset* mesh = ctx.registry.meshes.Get(meshComp->mesh);
    if (!mesh)
        return false;

    const std::string materialName = mesh->debugName.empty()
        ? "DefaultMaterial"
        : ("DefaultMaterial_" + mesh->debugName);

    // Default-Material als PBR erstellen — analog zum Modell-Drop-Pfad. Der PBR-Shader
    // (pbr_lit.ps.hlsl) nutzt dasselbe Lighting-Modell wie frisch reingezogene Objekte
    // → Drop und Reload sehen identisch aus. Farbe: weiß, matt.
    assets::MaterialAsset defaultAsset{};
    defaultAsset.baseColorFactor = {1.f, 1.f, 1.f, 1.f};
    defaultAsset.metallicFactor  = 0.f;
    defaultAsset.roughnessFactor = 0.6f;

    MaterialHandle handle = CreatePbrMaterialFromAsset(ctx, *mesh, defaultAsset, materialName.c_str());
    if (!handle.IsValid())  // Fallback (z.B. skinned Mesh ohne PBR-Pfad): Lit-Default
    {
        // Achtung: CreateDefaultWhiteMaterial nutzt immer StaticLit-Layout — für ein
        // skinned Mesh ist das falsch und kann zu fehlerhaftem Rendering führen.
        Debug::LogWarning("Editor-DefaultMaterial: PBR-Default fuer '%s' fehlgeschlagen, "
                          "nutze Lit-Fallback (StaticLit-Layout - bei skinned Meshes ggf. fehlerhaft)",
                          materialName.c_str());
        handle = CreateDefaultWhiteMaterial(ctx, *mesh, materialName.c_str());
    }
    if (!handle.IsValid())
        return false;

    if (auto* material = ctx.world.Get<MaterialComponent>(entity))
    {
        material->material = handle;
        material->materialAssetPath.clear();
    }
    else
    {
        MaterialComponent component{};
        component.material = handle;
        ctx.world.Add<MaterialComponent>(entity, component);
    }

    if (mesh->materialHandles.empty())
        mesh->materialHandles.push_back(handle);
    else
        for (MaterialHandle& h : mesh->materialHandles)
            h = handle;

    return true;
}

bool AssignThumbnailWhiteMaterialToEntity(EditorFrameContext& ctx, EntityID entity)
{
    auto* meshComp = ctx.world.Get<MeshComponent>(entity);
    if (!meshComp || !meshComp->mesh.IsValid())
        return false;

    assets::MeshAsset* mesh = ctx.registry.meshes.Get(meshComp->mesh);
    if (!mesh)
        return false;

    assets::MaterialAsset asset{};
    asset.templateName = MaterialTemplateToken(EditorMaterialTemplate::Unlit);
    asset.baseColorFactor = {1.f, 1.f, 1.f, 1.f};
    asset.emissiveFactor = {0.f, 0.f, 0.f};
    asset.alphaMode = assets::MaterialAlphaMode::Opaque;
    asset.transparent = false;
    asset.doubleSided = true;
    asset.castShadows = false;

    const std::string materialName = mesh->debugName.empty()
        ? "ThumbnailWhite"
        : ("ThumbnailWhite_" + mesh->debugName);
    const MaterialHandle handle = CreateUnlitMaterialFromAsset(ctx, *mesh, asset, materialName.c_str());
    if (!handle.IsValid())
        return false;

    if (auto* material = ctx.world.Get<MaterialComponent>(entity))
    {
        material->material = handle;
        material->materialAssetPath.clear();
        material->slotOverrides.clear();
    }
    else
    {
        MaterialComponent component{};
        component.material = handle;
        ctx.world.Add<MaterialComponent>(entity, component);
    }

    if (mesh->materialHandles.empty())
        mesh->materialHandles.push_back(handle);
    else
        for (MaterialHandle& h : mesh->materialHandles)
            h = handle;

    meshComp->castShadows = false;
    meshComp->receiveShadows = false;
    return true;
}

void AssignTextureToSelectedMaterial(EditorFrameContext& ctx,
                                     TextureHandle,
                                     const std::string& texParamName,
                                     const std::string& fileName)
{
    if (!ctx.state.selectedMaterialAssetLoaded && !ReloadSelectedMaterialAsset(ctx))
        return;

    const std::string resolvedPath = ToAssetRelativePath(ctx, PathFromUtf8(fileName));
    if (!IsSupportedTexturePath(PathFromUtf8(resolvedPath)))
    {
        Debug::LogWarning("Materialeditor: Texturpfad ignoriert, keine gueltige Textur: %s",
                          resolvedPath.c_str());
        return;
    }
    assets::MaterialAsset& asset = ctx.state.selectedMaterialAssetData;

    if (texParamName == "albedo" || texParamName == "baseColor")
        asset.baseColorTexture.path = resolvedPath;
    else if (texParamName == "orm")
        asset.metallicRoughnessTexture.path = resolvedPath;
    else if (texParamName == "normal")
        asset.normalTexture.path = resolvedPath;
    else if (texParamName == "emissive")
        asset.emissiveTexture.path = resolvedPath;
    else if (texParamName == "occlusion")
        asset.occlusionTexture.path = resolvedPath;
    ctx.state.selectedMaterialAssetDirty = true;

    // ReapplyMaterialAssetToBoundEntities muss VOR SaveSelectedMaterialAsset aufgerufen
    // werden: SaveSelectedMaterialAsset setzt dirty=false sofort (auch wenn der
    // Disk-Write noch in der Queue ist). Danach liest ReapplyMaterialAssetToBoundEntities
    // von Disk — wo der neue Textur-Pfad noch nicht steht (async Save).
    // Reihenfolge: Reapply (dirty=true → useInMemory=true) → dann Save.
    ReapplyMaterialAssetToBoundEntities(ctx, ctx.state.selectedMaterialAssetPath);
    if (MaterialAutoSave() || !ctx.state.materialWindowOpen)
        SaveSelectedMaterialAsset(ctx);
}

void DrawMaterialLibrary(EditorFrameContext& ctx)
{
    if (!ctx.state.materialWindowOpen)
        return;

    auto& placement = ctx.state.materialWindowPlacement;

    // Not pinned to the editor window: with multi-viewports this window may be
    // dragged out and becomes its own OS window (e.g. on a second monitor).
    ImGui::SetNextWindowPos(detail::EditorWindowPos(280.f, 10.f), ImGuiCond_FirstUseEver);
    const float availableHeight = std::max(1.f, ImGui::GetMainViewport()->WorkSize.y - 20.f);
    ImGui::SetNextWindowSize(ImVec2(1120.f, std::min(760.f, availableHeight)), ImGuiCond_FirstUseEver);
    if (!placement.maximized)
        ImGui::SetNextWindowSizeConstraints(
            ImVec2(900.f, std::min(640.f, availableHeight)), ImVec2(2000.f, 1400.f));
    const auto flags = detail::PrepareEditorWindowPlacement(placement);
    if (ctx.state.materialWindowFocusRequest)
    {
        ImGui::SetNextWindowFocus();
        ImGui::SetNextWindowCollapsed(false);
        ImGui::SetNextWindowScroll(ImVec2(-1.f, 0.f));
    }
    if (!ImGui::Begin(TranslateLabel("material.title", "Materialeditor##editor").c_str(), &ctx.state.materialWindowOpen, flags))
    {
        ctx.state.materialWindowFocusRequest = false;
        ImGui::End();
        return;
    }
    ctx.state.materialWindowFocusRequest = false;
    // Closing with unsaved changes waits for the answer.
    if (!ctx.state.materialWindowOpen && ctx.state.selectedMaterialAssetDirty && !MaterialAutoSave())
    {
        ctx.state.materialWindowOpen = true;
        PendingMaterialAction() = [&ctx] { ctx.state.materialWindowOpen = false; };
        MaterialUnsavedQuestion() = true;
    }

    detail::DrawEditorWindowSizeButton(placement);

    DrawSelectedMaterialEditor(ctx);

    if (MaterialUnsavedQuestion())
    {
        MaterialUnsavedQuestion() = false;
        ImGui::OpenPopup("##material_unsaved");
    }
    if (ImGui::BeginPopupModal("##material_unsaved", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar))
    {
        ImGui::TextUnformatted(Translate("material.unsaved"));
        const auto finish = [&](bool run) {
            std::function<void()> action = std::move(PendingMaterialAction());
            PendingMaterialAction() = nullptr;
            ImGui::CloseCurrentPopup();
            if (run && action)
                action();
        };
        if (card::SaveButton(TranslateLabel("common.save", "Save##material_unsaved")))
        {
            SaveSelectedMaterialAsset(ctx);
            finish(true);
        }
        ImGui::SameLine();
        if (ImGui::Button(TranslateLabel("animation.discard", "Discard").c_str()))
        {
            // Back to the file; the scene objects showed the unsaved values.
            const std::string path = ctx.state.selectedMaterialAssetPath;
            std::ifstream in(ToAbsoluteAssetPath(ctx, path), std::ios::binary);
            const std::string source((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            assets::MaterialAsset onDisk;
            if (ParseEditorMaterialAssetSource(source, path, onDisk))
            {
                ctx.state.selectedMaterialAssetData = onDisk;
                PreviewMaterialAssetOnBoundEntities(ctx, path, onDisk);
            }
            ctx.state.selectedMaterialAssetDirty = false;
            finish(true);
        }
        ImGui::SameLine();
        if (ImGui::Button(TranslateLabel("common.cancel", "Cancel").c_str()))
            finish(false);
        ImGui::EndPopup();
    }

    ImGui::End();
}

void ClearSharedMaterialBindings()
{
    SharedMaterialBindings().clear();
}

} // namespace engine::renderer::addons::editor

#else

namespace engine::renderer::addons::editor {

void SelectMaterialAsset(EditorFrameContext&, const std::filesystem::path&) {}
void OpenMaterialAsset(EditorFrameContext&, const std::filesystem::path&) {}
void QueueOpenMaterialAsset(EditorFrameContext&, const std::filesystem::path&) {}
void FlushPendingMaterialOpen(EditorFrameContext&) {}
bool CreateMaterialAsset(EditorFrameContext&, const std::filesystem::path&) { return false; }
void ResolveMaterialAssetBindings(EditorFrameContext&, std::span<const EntityID>) {}
bool ApplySelectedMaterialToEntity(EditorFrameContext&, EntityID) { return false; }
bool ApplyMaterialAssetToEntity(EditorFrameContext&, EntityID, const std::filesystem::path&) { return false; }
bool ApplyMaterialAssetToEntitySlot(EditorFrameContext&, EntityID, uint32_t, const std::filesystem::path&) { return false; }
void TickMaterialTextureSync(EditorFrameContext&) {}
bool AssignDefaultMaterialToEntity(EditorFrameContext&, EntityID) { return false; }
bool AssignThumbnailWhiteMaterialToEntity(EditorFrameContext&, EntityID) { return false; }
void AssignTextureToSelectedMaterial(EditorFrameContext&, TextureHandle, const std::string&, const std::string&) {}
bool SaveSelectedMaterialAsset(EditorFrameContext&) { return false; }
bool SaveSelectedMaterialAssetIfChanged(EditorFrameContext&) { return false; }
bool SetEntityMaterialOverride(EditorFrameContext&, EntityID, int, const assets::MaterialAsset&) { return false; }
void DrawMaterialLibrary(EditorFrameContext&) {}
bool ResetEntityMaterialOverride(EditorFrameContext&, EntityID, int, const std::filesystem::path&) { return false; }
bool CreateEntityMaterialCopy(EditorFrameContext&, EntityID, int, const std::filesystem::path&) { return false; }
void ClearSharedMaterialBindings() {}

} // namespace engine::renderer::addons::editor

#endif
