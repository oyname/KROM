#include "addons/editor/EditorMaterialLibrary.hpp"

#include "addons/editor/EditorFileNaming.hpp"
#include "addons/editor/EditorAssetBrowser.hpp"
#include "addons/editor/EditorFeature.hpp"

#ifdef KROM_EDITOR_HAS_IMGUI

// Shader-Reflection — Backend-abhaengig
#if defined(KROM_APP_BACKEND_DX11)
#  include "addons/dx11/DX11ShaderReflector.hpp"
#elif defined(KROM_APP_BACKEND_VULKAN)
#  include "addons/vulkan/VKShaderReflector.hpp"
#endif

#include "addons/lit/LitMaterial.hpp"
#include "addons/mesh_renderer/MeshRendererComponents.hpp"
#include "addons/pbr/PbrInstanceBuilder.hpp"
#include "addons/pbr/PbrMasterMaterial.hpp"
#include "addons/pbr/PbrSlot.hpp"
#include "addons/unlit/UnlitMaterial.hpp"
#include "assets/AssetPipeline.hpp"
#include "assets/MeshTangents.hpp"
#include "assets/VertexLayoutBridge.hpp"
#include "core/Logger.hpp"
#include "ecs/Components.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include "platform/NativeFileDialog.hpp"
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
    const std::string ext = path.extension().string();
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

    const std::string generic = relative.generic_string();
    if (generic.rfind("..", 0u) == 0u)
        return false;

    outRelative = generic;
    return true;
}

std::string NormalizeMaterialAssetPath(const EditorFrameContext& ctx, const std::filesystem::path& inputPath)
{
    if (inputPath.empty())
        return {};

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
            std::string segment = part.string();
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

    return inputPath.generic_string();
}

std::string ToAssetRelativePath(const EditorFrameContext& ctx, const std::filesystem::path& absolutePath)
{
    if (absolutePath.empty())
        return {};
    return NormalizeMaterialAssetPath(ctx, absolutePath);
}

std::filesystem::path ToAbsoluteAssetPath(const EditorFrameContext& ctx, const std::string& relativePath)
{
    const std::filesystem::path path(relativePath);
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
    const std::filesystem::path path(filename ? filename : "");
    if (path.is_absolute() || ctx.engineAssetRoot.empty())
        return path;
    return (std::filesystem::path(ctx.engineAssetRoot) / path).lexically_normal();
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
    const char* shadowVsPath = opengl ? "shadow.opengl.vs.glsl" : "shadow.vs.hlsl";
    const char* shadowFsPath = opengl ? "shadow.opengl.fs.glsl" : "shadow.ps.hlsl";

    outVS     = ctx.assetPipeline->LoadShader(ResolveEngineShaderPath(ctx, vsPath).string(), assets::ShaderStage::Vertex);
    outFS     = ctx.assetPipeline->LoadShader(ResolveEngineShaderPath(ctx, fsPath).string(), assets::ShaderStage::Fragment);
    outShadow = ctx.assetPipeline->LoadShader(ResolveEngineShaderPath(ctx, shadowVsPath).string(), assets::ShaderStage::Vertex);
    if (outShadowFS)
        *outShadowFS = ctx.assetPipeline->LoadShader(ResolveEngineShaderPath(ctx, shadowFsPath).string(), assets::ShaderStage::Fragment);
    return outVS.IsValid() && outFS.IsValid() && outShadow.IsValid();
}

bool LoadPbrShaders(EditorFrameContext& ctx,
                    ShaderHandle& outVS, ShaderHandle& outFS, ShaderHandle& outShadow,
                    ShaderHandle* outShadowFS)
{
    if (!ctx.assetPipeline)
        return false;

    const bool opengl = ctx.shaderTarget == assets::ShaderTargetProfile::OpenGL_GLSL450;
    outVS     = ctx.assetPipeline->LoadShader(ResolveEngineShaderPath(ctx, opengl ? "pbr_lit.opengl.vs.glsl"    : "pbr_lit.vs.hlsl").string(),
                                              assets::ShaderStage::Vertex);
    outFS     = ctx.assetPipeline->LoadShader(ResolveEngineShaderPath(ctx, opengl ? "pbr_lit.opengl.fs.glsl"    : "pbr_lit.ps.hlsl").string(),
                                              assets::ShaderStage::Fragment);
    outShadow = ctx.assetPipeline->LoadShader(ResolveEngineShaderPath(ctx, opengl ? "shadow_pbr.opengl.vs.glsl" : "shadow_pbr.vs.hlsl").string(),
                                              assets::ShaderStage::Vertex);
    if (outShadowFS)
        *outShadowFS = ctx.assetPipeline->LoadShader(ResolveEngineShaderPath(ctx, opengl ? "shadow_pbr.opengl.fs.glsl" : "shadow_pbr.ps.hlsl").string(),
                                                     assets::ShaderStage::Fragment);
    return outVS.IsValid() && outFS.IsValid() && outShadow.IsValid();
}

bool LoadUnlitShaders(EditorFrameContext& ctx, ShaderHandle& outVS, ShaderHandle& outFS)
{
    if (!ctx.assetPipeline)
        return false;

    const bool opengl = ctx.shaderTarget == assets::ShaderTargetProfile::OpenGL_GLSL450;
    outVS = ctx.assetPipeline->LoadShader(ResolveEngineShaderPath(ctx, opengl ? "quad_unlit.opengl.vs.glsl" : "quad_unlit.vs.hlsl").string(),
                                          assets::ShaderStage::Vertex);
    outFS = ctx.assetPipeline->LoadShader(ResolveEngineShaderPath(ctx, opengl ? "quad_unlit.opengl.fs.glsl" : "quad_unlit.ps.hlsl").string(),
                                          assets::ShaderStage::Fragment);
    return outVS.IsValid() && outFS.IsValid();
}

bool SliderFloatWithDoubleClickInput(const char* label,
                                     float* value,
                                     float minValue,
                                     float maxValue,
                                     const char* format = "%.3f",
                                     ImGuiSliderFlags flags = ImGuiSliderFlags_AlwaysClamp)
{
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems)
        return false;

    ImGuiContext& g = *GImGui;
    const ImGuiStyle& style = g.Style;
    const ImGuiID id = window->GetID(label);
    const float width = ImGui::CalcItemWidth();
    const char* labelEnd = ImGui::FindRenderedTextEnd(label);
    const ImVec2 labelSize = ImGui::CalcTextSize(label, labelEnd, false);
    const ImVec2 frameMin = window->DC.CursorPos;
    const ImVec2 frameMax(frameMin.x + width, frameMin.y + labelSize.y + style.FramePadding.y * 2.0f);
    const ImRect frameRect(frameMin, frameMax);
    const float totalWidth = frameMax.x + (labelSize.x > 0.0f ? style.ItemInnerSpacing.x + labelSize.x : 0.0f);
    const ImVec2 totalMax(totalWidth, frameMax.y);
    const ImRect totalRect(frameMin, totalMax);

    ImGui::ItemSize(totalRect, style.FramePadding.y);
    if (!ImGui::ItemAdd(totalRect, id, &frameRect, ImGuiItemFlags_Inputable))
        return false;

    const bool hovered = ImGui::ItemHoverable(frameRect, id, g.LastItemData.ItemFlags);
    bool tempInputIsActive = ImGui::TempInputIsActive(id);

    if (!tempInputIsActive)
    {
        const bool clicked = hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left, ImGuiInputFlags_None, id);
        const bool doubleClicked = hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left, id);
        const bool makeActive = clicked || doubleClicked || g.NavActivateId == id;

        if (makeActive && (clicked || doubleClicked))
            ImGui::SetKeyOwner(ImGuiKey_MouseLeft, id);

        if (makeActive && (doubleClicked ||
            (g.NavActivateId == id && (g.NavActivateFlags & ImGuiActivateFlags_PreferInput))))
        {
            tempInputIsActive = true;
        }

        if (makeActive)
            memcpy(&g.ActiveIdValueOnActivation, value, sizeof(float));

        if (makeActive && !tempInputIsActive)
        {
            ImGui::SetActiveID(id, window);
            ImGui::SetFocusID(id, window);
            ImGui::FocusWindow(window);
            g.ActiveIdUsingNavDirMask |= (1 << ImGuiDir_Left) | (1 << ImGuiDir_Right);
        }
    }

    if (tempInputIsActive)
        return ImGui::TempInputScalar(frameRect, id, label, ImGuiDataType_Float, value, format, &minValue, &maxValue);

    const ImU32 frameColor = ImGui::GetColorU32(g.ActiveId == id
        ? ImGuiCol_FrameBgActive
        : hovered ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg);
    ImGui::RenderNavCursor(frameRect, id);
    ImGui::RenderFrame(frameRect.Min, frameRect.Max, frameColor, false, style.FrameRounding);
    ImGui::RenderFrameBorder(frameRect.Min, frameRect.Max, style.FrameRounding);

    ImRect grabRect;
    const bool valueChanged = ImGui::SliderBehavior(
        frameRect, id, ImGuiDataType_Float, value, &minValue, &maxValue, format, flags, &grabRect);
    if (valueChanged)
        ImGui::MarkItemEdited(id);

    if (grabRect.Max.x > grabRect.Min.x)
        window->DrawList->AddRectFilled(
            grabRect.Min, grabRect.Max,
            ImGui::GetColorU32(g.ActiveId == id ? ImGuiCol_SliderGrabActive : ImGuiCol_SliderGrab),
            style.GrabRounding);

    char valueBuffer[64];
    const char* valueBufferEnd = valueBuffer + ImGui::DataTypeFormatString(
        valueBuffer, IM_ARRAYSIZE(valueBuffer), ImGuiDataType_Float, value, format);
    ImGui::RenderTextClipped(frameRect.Min, frameRect.Max, valueBuffer, valueBufferEnd, nullptr, ImVec2(0.5f, 0.5f));
    if (labelSize.x > 0.0f)
        ImGui::RenderText(
            ImVec2(frameRect.Max.x + style.ItemInnerSpacing.x, frameRect.Min.y + style.FramePadding.y),
            label, labelEnd, false);

    IMGUI_TEST_ENGINE_ITEM_INFO(id, label, g.LastItemData.StatusFlags | ImGuiItemStatusFlags_Inputable);
    return valueChanged;
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
    TextureHandle  baseColorTexture = TextureHandle::Invalid();
    TextureHandle  ormTexture       = TextureHandle::Invalid();
    TextureHandle  normalTexture    = TextureHandle::Invalid();
    TextureHandle  emissiveTexture  = TextureHandle::Invalid();
    // Verhindert endlose Retry-Schleife wenn der Build fehlschlug
    bool           buildFailed      = false;
};

std::unordered_map<SharedMaterialBindingKey, SharedMaterialBinding, SharedMaterialBindingKeyHash>& SharedMaterialBindings()
{
    static std::unordered_map<SharedMaterialBindingKey, SharedMaterialBinding, SharedMaterialBindingKeyHash> storage;
    return storage;
}

uint64_t HashMaterialStructure(const assets::MaterialAsset& asset) noexcept
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
    combineStr(asset.vertexShaderPath);
    combineStr(asset.fragmentShaderPath);

    // Vollständige Pfade hashen — nicht nur ob gesetzt.
    // Materialien mit denselben Texturtypes aber VERSCHIEDENEN Pfaden
    // (z.B. Material_Boden.mat vs Block.mat) erhalten sonst identische
    // Hashes → needsRebuild feuert fälschlicherweise nicht → falsche Texturen.
    combineStr(asset.baseColorTexture.path);
    combineStr(asset.metallicRoughnessTexture.path);
    combineStr(asset.normalTexture.path);
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

TextureHandle LoadRuntimeTexture(EditorFrameContext& ctx, const std::string& path)
{
    if (!ctx.assetPipeline || path.empty())
        return TextureHandle::Invalid();

    const TextureHandle assetTexture = ctx.assetPipeline->LoadTexture(path);
    if (!assetTexture.IsValid())
        return TextureHandle::Invalid();

    ctx.assetPipeline->UploadPendingGpuAssets();
    return ctx.assetPipeline->GetGpuTexture(assetTexture);
}

void UpdateCachedTexture(EditorFrameContext& ctx,
                         const std::string& texturePath,
                         std::string& cachedPath,
                         TextureHandle& cachedTexture)
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

    const TextureHandle refreshed = LoadRuntimeTexture(ctx, texturePath);
    if (cachedPath == texturePath && cachedTexture.IsValid() && !refreshed.IsValid())
        return;

    if (cachedPath == texturePath && cachedTexture == refreshed && cachedTexture.IsValid())
        return;

    cachedPath = texturePath;
    cachedTexture = refreshed;
}

EntityID FindFirstMeshDescendant(ecs::World& world, EntityID root)
{
    if (!root.IsValid())
        return NULL_ENTITY;

    EntityID found = NULL_ENTITY;
    world.ForEachAlive([&](EntityID id)
    {
        if (found.IsValid() || !world.Get<MeshComponent>(id))
            return;

        EntityID cur = id;
        while (cur.IsValid())
        {
            if (cur == root)
            {
                found = id;
                return;
            }
            const auto* parent = world.Get<ParentComponent>(cur);
            cur = parent ? parent->parent : NULL_ENTITY;
        }
    });
    return found;
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
    ctx.state.selectedMaterialAssetPath = NormalizeMaterialAssetPath(ctx, ctx.state.selectedMaterialAssetPath);
    ctx.state.selectedMaterialAssetNameDraft =
        std::filesystem::path(ctx.state.selectedMaterialAssetPath).stem().string();
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
    loaded.debugName = std::filesystem::path(normalizedPath).filename().string();
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

        if (parts[0] == "template" && parts.size() >= 2) loaded.templateName = parts[1];
        else if (parts[0] == "vertex" && parts.size() >= 2) loaded.vertexShaderPath = RestAfterFirstToken(line);
        else if (parts[0] == "fragment" && parts.size() >= 2) loaded.fragmentShaderPath = RestAfterFirstToken(line);
        else if (parts[0] == "transparent" && parts.size() >= 2) loaded.transparent = (parts[1] == "1" || parts[1] == "true");
        else if (parts[0] == "doubleSided" && parts.size() >= 2) loaded.doubleSided = (parts[1] == "1" || parts[1] == "true");
        else if (parts[0] == "castShadows" && parts.size() >= 2) loaded.castShadows = (parts[1] == "1" || parts[1] == "true");
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
        else if (parts[0] == "uvScale" && parts.size() >= 3) loaded.uvScale = { std::stof(parts[1]), std::stof(parts[2]) };
        else if (parts[0] == "uvOffset" && parts.size() >= 3) loaded.uvOffset = { std::stof(parts[1]), std::stof(parts[2]) };
        else if (parts[0] == "alphaMode" && parts.size() >= 2)
        {
            if (parts[1] == "mask") loaded.alphaMode = assets::MaterialAlphaMode::Mask;
            else if (parts[1] == "blend") loaded.alphaMode = assets::MaterialAlphaMode::Blend;
            else loaded.alphaMode = assets::MaterialAlphaMode::Opaque;
        }
        else if (parts[0] == "baseColorTexture" && parts.size() >= 2) loaded.baseColorTexture.path = RestAfterFirstToken(line);
        else if (parts[0] == "metallicRoughnessTexture" && parts.size() >= 2) loaded.metallicRoughnessTexture.path = RestAfterFirstToken(line);
        else if (parts[0] == "normalTexture" && parts.size() >= 2) loaded.normalTexture.path = RestAfterFirstToken(line);
        else if (parts[0] == "occlusionTexture" && parts.size() >= 2) loaded.occlusionTexture.path = RestAfterFirstToken(line);
        else if (parts[0] == "emissiveTexture" && parts.size() >= 2) loaded.emissiveTexture.path = RestAfterFirstToken(line);
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

    outAsset.path                     = std::move(loaded.path);
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
    outAsset.baseColorFactor          = loaded.baseColorFactor;
    outAsset.metallicFactor           = loaded.metallicFactor;
    outAsset.roughnessFactor          = loaded.roughnessFactor;
    outAsset.emissiveFactor           = loaded.emissiveFactor;
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
    ctx.state.selectedMaterialAssetNameDraft = std::filesystem::path(normalizedPath).stem().string();
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
        if (NormalizeMaterialAssetPath(ctx, ctx.state.selectedMaterialAssetPath) == loaded.normalizedPath)
        {
            ctx.state.selectedMaterialAssetData = std::move(loaded.asset);
            ctx.state.selectedMaterialAssetPath = loaded.normalizedPath;
            ctx.state.selectedMaterialAssetNameDraft =
                std::filesystem::path(loaded.normalizedPath).stem().string();
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
        if (NormalizeMaterialAssetPath(ctx, ctx.state.selectedMaterialAssetPath) != queuedPath ||
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
        NormalizeMaterialAssetPath(ctx, ctx.state.selectedMaterialAssetPath);
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

    const auto alphaModeText = [&]() -> const char* {
        switch (asset.alphaMode)
        {
        case assets::MaterialAlphaMode::Mask:  return "mask";
        case assets::MaterialAlphaMode::Blend: return "blend";
        default:                               return "opaque";
        }
    };

    const EditorMaterialTemplate materialTemplate = ParseMaterialTemplate(asset.templateName);

    out << "template " << asset.templateName << "\n";
    if (materialTemplate == EditorMaterialTemplate::Custom && !asset.vertexShaderPath.empty())
        out << "vertex " << asset.vertexShaderPath << "\n";
    if (materialTemplate == EditorMaterialTemplate::Custom && !asset.fragmentShaderPath.empty())
        out << "fragment " << asset.fragmentShaderPath << "\n";
    out << "doubleSided " << (asset.doubleSided ? "true" : "false") << "\n";
    out << "castShadows " << (asset.castShadows ? "true" : "false") << "\n";
    out << "transparent " << (asset.transparent ? "true" : "false") << "\n";
    out << "alphaMode " << alphaModeText() << "\n";
    out << "alphaCutoff " << asset.alphaCutoff << "\n";
    out << "baseColorFactor "
        << asset.baseColorFactor.x << " "
        << asset.baseColorFactor.y << " "
        << asset.baseColorFactor.z << " "
        << asset.baseColorFactor.w << "\n";
    out << "emissiveFactor "
        << asset.emissiveFactor.x << " "
        << asset.emissiveFactor.y << " "
        << asset.emissiveFactor.z << "\n";
    out << "metallicFactor " << asset.metallicFactor << "\n";
    out << "roughnessFactor " << asset.roughnessFactor << "\n";
    out << "normalScale " << asset.normalScale << "\n";
    out << "occlusionStrength " << asset.occlusionStrength << "\n";
    if (asset.uvScale.x != 1.f || asset.uvScale.y != 1.f)
        out << "uvScale " << asset.uvScale.x << " " << asset.uvScale.y << "\n";
    if (asset.uvOffset.x != 0.f || asset.uvOffset.y != 0.f)
        out << "uvOffset " << asset.uvOffset.x << " " << asset.uvOffset.y << "\n";

    if (!asset.baseColorTexture.path.empty())
        out << "baseColorTexture " << asset.baseColorTexture.path << "\n";
    if (!asset.metallicRoughnessTexture.path.empty())
        out << "metallicRoughnessTexture " << asset.metallicRoughnessTexture.path << "\n";
    if (!asset.normalTexture.path.empty())
        out << "normalTexture " << asset.normalTexture.path << "\n";
    if (!asset.occlusionTexture.path.empty())
        out << "occlusionTexture " << asset.occlusionTexture.path << "\n";
    if (!asset.emissiveTexture.path.empty())
        out << "emissiveTexture " << asset.emissiveTexture.path << "\n";

    for (const assets::MaterialParam& param : asset.params)
    {
        if (materialTemplate != EditorMaterialTemplate::Custom)
            continue;
        if (param.name.empty() || IsBuiltInMaterialParamName(param.name))
            continue;

        out << MaterialParamToken(param.type) << " " << param.name;
        switch (param.type)
        {
        case assets::MaterialParam::Type::Float:
            out << " " << param.value.f[0];
            break;
        case assets::MaterialParam::Type::Vec2:
            out << " " << param.value.f[0] << " " << param.value.f[1];
            break;
        case assets::MaterialParam::Type::Vec3:
            out << " " << param.value.f[0] << " " << param.value.f[1] << " " << param.value.f[2];
            break;
        case assets::MaterialParam::Type::Vec4:
            out << " " << param.value.f[0] << " " << param.value.f[1] << " " << param.value.f[2] << " " << param.value.f[3];
            break;
        case assets::MaterialParam::Type::Int:
            out << " " << param.value.i;
            break;
        case assets::MaterialParam::Type::Bool:
            out << " " << (param.value.b ? "true" : "false");
            break;
        case assets::MaterialParam::Type::Texture:
            out << " " << param.texturePath;
            break;
        }
        out << "\n";
    }

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

        const std::string key = absolutePath.lexically_normal().generic_string();
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
        const std::string key = absolutePath.lexically_normal().generic_string();
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
                    m_status[save.absolutePath.lexically_normal().generic_string()] =
                        StatusSnapshot{ SaveState::Saving, std::chrono::steady_clock::now() };
                }

                const bool ok = SaveMaterialAssetFileAbsolute(save.absolutePath, save.snapshot);
                {
                    std::lock_guard<std::mutex> statusLock(m_mutex);
                    m_status[save.absolutePath.lexically_normal().generic_string()] =
                        StatusSnapshot{ ok ? SaveState::Saved : SaveState::Failed,
                                        std::chrono::steady_clock::now() };
                }

                if (!ok)
                    Debug::LogError("Editor-Material: async save fehlgeschlagen '%s'",
                                    save.absolutePath.string().c_str());
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

    renderer::VertexLayout layout{};
    if (!ResolveMeshLayout(mesh, renderer::VertexContracts::PbrLit(), layout, "Editor-Material"))
        return MaterialHandle::Invalid();

    ShaderHandle vs, fs, shadow, shadowFs;
    if (!LoadPbrShaders(ctx, vs, fs, shadow, &shadowFs))
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

    renderer::pbr::PbrMasterMaterial master =
        renderer::pbr::PbrMasterMaterial::Create(ctx.materials, config);
    if (!master.IsValid())
        return MaterialHandle::Invalid();

    TextureHandle baseColorTex = LoadRuntimeTexture(ctx, asset.baseColorTexture.path);
    TextureHandle ormTex       = LoadRuntimeTexture(ctx, asset.metallicRoughnessTexture.path);
    TextureHandle normalTex    = LoadRuntimeTexture(ctx, asset.normalTexture.path);
    TextureHandle emissiveTex  = LoadRuntimeTexture(ctx, asset.emissiveTexture.path);

    auto builder = master.CreateInstance(name);
    if (baseColorTex.IsValid()) builder.BaseColor(baseColorTex);
    else                        builder.BaseColor(asset.baseColorFactor);

    if (ormTex.IsValid())
    {
        builder.Roughness(ormTex, pbr::MaterialChannel::G, asset.roughnessFactor);
        builder.Metallic(ormTex, pbr::MaterialChannel::B, asset.metallicFactor);
        builder.Occlusion(ormTex, pbr::MaterialChannel::R, asset.occlusionStrength);
    }
    else
    {
        builder.Roughness(asset.roughnessFactor);
        builder.Metallic(asset.metallicFactor);
        builder.Occlusion(asset.occlusionStrength);
    }

    if (normalTex.IsValid())
        builder.Normal(normalTex, asset.normalScale);
    if (emissiveTex.IsValid())
        builder.Emissive(emissiveTex);
    else
        builder.Emissive(asset.emissiveFactor.x, asset.emissiveFactor.y, asset.emissiveFactor.z);

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
    renderer::VertexLayout layout{};
    if (!ResolveMeshLayout(mesh, renderer::VertexContracts::StaticLit(), layout, "Editor-Material"))
        return MaterialHandle::Invalid();

    ShaderHandle vs, fs;
    if (!LoadUnlitShaders(ctx, vs, fs))
        return MaterialHandle::Invalid();

    renderer::unlit::UnlitMaterialCreateInfo info{};
    info.name               = name;
    info.vertexShader       = vs;
    info.fragmentShader     = fs;
    info.vertexLayout       = layout;
    info.baseColorFactor    = asset.baseColorFactor;
    info.emissiveFactor     = {asset.emissiveFactor.x, asset.emissiveFactor.y, asset.emissiveFactor.z, 1.f};
    info.opacityFactor      = asset.baseColorFactor.w;
    info.alphaCutoff        = asset.alphaCutoff;
    info.enableBaseColorMap = !asset.baseColorTexture.path.empty();
    info.enableEmissiveMap  = !asset.emissiveTexture.path.empty();
    info.alphaTest          = asset.alphaMode == assets::MaterialAlphaMode::Mask;
    info.doubleSided        = asset.doubleSided;
    info.castShadows        = asset.castShadows;
    info.cullMode           = asset.doubleSided ? MaterialCullMode::None : MaterialCullMode::Back;

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
    renderer::VertexLayout layout{};
    if (!ResolveMeshLayout(mesh, renderer::VertexContracts::StaticLit(), layout, "Editor-Material"))
        return MaterialHandle::Invalid();

    ShaderHandle vs, fs, shadow, shadowFs;
    if (!LoadLitShaders(ctx, vs, fs, shadow, false, &shadowFs))
        return MaterialHandle::Invalid();

    renderer::lit::LitMaterialCreateInfo info{};
    info.name                 = name;
    info.vertexShader         = vs;
    info.fragmentShader       = fs;
    info.shadowShader         = shadow;
    info.shadowFragmentShader = shadowFs;
    info.vertexLayout         = layout;
    info.baseColorFactor      = asset.baseColorFactor;
    info.emissiveFactor       = {asset.emissiveFactor.x, asset.emissiveFactor.y, asset.emissiveFactor.z, 1.f};
    info.specularStrength     = asset.metallicFactor;
    info.roughnessFactor      = asset.roughnessFactor;
    info.opacityFactor        = asset.baseColorFactor.w;
    info.alphaCutoff          = asset.alphaCutoff;
    info.enableBaseColorMap   = !asset.baseColorTexture.path.empty();
    info.enableEmissiveMap    = !asset.emissiveTexture.path.empty();
    info.alphaTest            = asset.alphaMode == assets::MaterialAlphaMode::Mask;
    info.doubleSided          = asset.doubleSided;
    info.castShadows          = asset.castShadows;
    info.cullMode             = asset.doubleSided ? MaterialCullMode::None : MaterialCullMode::Back;

    const MaterialHandle handle = renderer::lit::LitMaterial::Register(ctx.materials, info);
    if (!handle.IsValid())
        return handle;

    if (const TextureHandle albedo = LoadRuntimeTexture(ctx, asset.baseColorTexture.path); albedo.IsValid())
        ctx.materials.SetTexture(handle, "albedo", albedo);
    if (const TextureHandle emissive = LoadRuntimeTexture(ctx, asset.emissiveTexture.path); emissive.IsValid())
        ctx.materials.SetTexture(handle, "emissive", emissive);
    ctx.materials.MarkDirty(handle);
    return handle;
}

MaterialHandle CreateDefaultWhiteMaterial(EditorFrameContext& ctx,
                                          assets::MeshAsset& mesh,
                                          const char* name)
{
    renderer::VertexLayout layout{};
    if (!ResolveMeshLayout(mesh, renderer::VertexContracts::StaticLit(), layout, "Editor-DefaultMaterial"))
        return MaterialHandle::Invalid();

    ShaderHandle vs, fs, shadow, shadowFs;
    if (!LoadLitShaders(ctx, vs, fs, shadow, false, &shadowFs))
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
    info.emissiveFactor   = {asset.emissiveFactor.x, asset.emissiveFactor.y, asset.emissiveFactor.z, 1.f};
    info.opacityFactor    = asset.baseColorFactor.w;
    info.alphaCutoff      = asset.alphaCutoff;
    info.enableBaseColorMap = !asset.baseColorTexture.path.empty();
    info.enableEmissiveMap  = !asset.emissiveTexture.path.empty();
    info.alphaTest        = asset.alphaMode == assets::MaterialAlphaMode::Mask;
    info.doubleSided      = asset.doubleSided;
    info.castShadows      = asset.castShadows;
    info.cullMode         = asset.doubleSided ? MaterialCullMode::None : MaterialCullMode::Back;
    info.extraParameters.reserve(asset.params.size());
    for (const assets::MaterialParam& param : asset.params)
    {
        if (IsBuiltInMaterialParamName(param.name))
            continue;
        renderer::MaterialParam runtimeParam = ToRuntimeParam(param);
        if (param.type == assets::MaterialParam::Type::Texture && !param.texturePath.empty())
            runtimeParam.texture = LoadRuntimeTexture(ctx, param.texturePath);
        info.extraParameters.push_back(runtimeParam);
    }

    // Tatsächliche GPU-Textur-Binding-Slots aus kompiliertem Shader lesen —
    // verhindert Mismatch zwischen engine-seitig zugewiesenem Slot und Shader-Register.
    {
        const assets::ShaderAsset* vsAsset = ctx.registry.shaders.Get(vs);
        const assets::ShaderAsset* fsAsset = ctx.registry.shaders.Get(fs);
        if (vsAsset && fsAsset)
        {
            renderer::ShaderParameterLayout reflectedLayout{};
#if defined(KROM_APP_BACKEND_DX11)
            dx11::DX11ShaderReflector reflector;
            reflector.ReflectProgram(*vsAsset, *fsAsset, reflectedLayout, nullptr);
#elif defined(KROM_APP_BACKEND_VULKAN)
            vulkan::VKShaderReflector reflector;
            reflector.ReflectProgram(*vsAsset, *fsAsset, reflectedLayout, nullptr);
#endif
            for (uint32_t i = 0u; i < reflectedLayout.slotCount; ++i)
            {
                const auto& slot = reflectedLayout.slots[i];
                if (slot.type == renderer::MaterialParameterType::Texture2D ||
                    slot.type == renderer::MaterialParameterType::TextureCube)
                    info.textureBindingOverrides[std::string(slot.Name())] = slot.binding;
            }
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
                                     const assets::MaterialAsset& asset)
{
    if (!runtimeMaterial.IsValid())
        return;

    const EditorMaterialTemplate materialTemplate = ParseMaterialTemplate(asset.templateName);
    const math::Vec4 emissiveVec4{
        asset.emissiveFactor.x,
        asset.emissiveFactor.y,
        asset.emissiveFactor.z,
        0.f
    };

    UpdateCachedTexture(ctx, asset.baseColorTexture.path, binding.baseColorPath, binding.baseColorTexture);
    UpdateCachedTexture(ctx, asset.metallicRoughnessTexture.path, binding.ormPath, binding.ormTexture);
    UpdateCachedTexture(ctx, asset.normalTexture.path, binding.normalPath, binding.normalTexture);
    UpdateCachedTexture(ctx, asset.emissiveTexture.path, binding.emissivePath, binding.emissiveTexture);

    switch (materialTemplate)
    {
    case EditorMaterialTemplate::PbrLit:
        ctx.materials.SetVec4(runtimeMaterial, "baseColorFactor", asset.baseColorFactor);
        ctx.materials.SetVec4(runtimeMaterial, "emissiveFactor", emissiveVec4);
        ctx.materials.SetFloat(runtimeMaterial, "metallicFactor", asset.metallicFactor);
        ctx.materials.SetFloat(runtimeMaterial, "roughnessFactor", asset.roughnessFactor);
        ctx.materials.SetFloat(runtimeMaterial, "normalStrength", asset.normalScale);
        ctx.materials.SetFloat(runtimeMaterial, "occlusionStrength", asset.occlusionStrength);
        ctx.materials.SetFloat(runtimeMaterial, "opacityFactor", asset.baseColorFactor.w);
        ctx.materials.SetFloat(runtimeMaterial, "alphaCutoff", asset.alphaCutoff);
        ctx.materials.SetVec2(runtimeMaterial, "uvScale", asset.uvScale);
        ctx.materials.SetVec2(runtimeMaterial, "uvOffset", asset.uvOffset);
        ctx.materials.SetTexture(runtimeMaterial, "albedo", binding.baseColorTexture);
        ctx.materials.SetTexture(runtimeMaterial, "normal", binding.normalTexture);
        ctx.materials.SetTexture(runtimeMaterial, "orm", binding.ormTexture);
        ctx.materials.SetTexture(runtimeMaterial, "emissive", binding.emissiveTexture);
        break;

    case EditorMaterialTemplate::LegacyLit:
        ctx.materials.SetVec4(runtimeMaterial, "baseColorFactor", asset.baseColorFactor);
        ctx.materials.SetVec4(runtimeMaterial, "emissiveFactor", emissiveVec4);
        ctx.materials.SetFloat(runtimeMaterial, "metallicFactor", asset.metallicFactor);
        ctx.materials.SetFloat(runtimeMaterial, "roughnessFactor", asset.roughnessFactor);
        ctx.materials.SetFloat(runtimeMaterial, "opacityFactor", asset.baseColorFactor.w);
        ctx.materials.SetFloat(runtimeMaterial, "alphaCutoff", asset.alphaCutoff);
        ctx.materials.SetTexture(runtimeMaterial, "albedo", binding.baseColorTexture);
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

    ctx.materials.MarkDirty(runtimeMaterial);
}

MaterialHandle AcquireSharedRuntimeMaterial(EditorFrameContext& ctx,
                                            MeshHandle meshHandle,
                                            const std::string& materialPath,
                                            assets::MeshAsset& mesh,
                                            const assets::MaterialAsset& asset)
{
    SharedMaterialBindingKey key{ materialPath, meshHandle };
    SharedMaterialBinding& binding = SharedMaterialBindings()[key];
    const uint64_t structureHash = HashMaterialStructure(asset);

    // Prüfen ob Texturen die beim letzten Rebuild noch fehlten jetzt geladen sind.
    // Wenn ja: Rebuild erzwingen damit builder.Normal / builder.BaseColor usw.
    // mit gültigem Handle aufgerufen werden und die Float-Parameter (normalStrength
    // usw.) in parameterOverrides landen. Ohne diesen Check schlägt SetFloat
    // ("normalStrength", ...) lautlos fehl weil der Parameter nie registriert wurde.
    const auto textureNowValid = [&](const std::string& assetPath,
                                     const TextureHandle& cachedHandle) -> bool
    {
        if (assetPath.empty() || cachedHandle.IsValid())
            return false; // kein Pfad oder bereits geladen
        return LoadRuntimeTexture(ctx, assetPath).IsValid();
    };

    const bool texturesBecameAvailable =
        textureNowValid(asset.baseColorTexture.path,        binding.baseColorTexture) ||
        textureNowValid(asset.metallicRoughnessTexture.path, binding.ormTexture)       ||
        textureNowValid(asset.normalTexture.path,            binding.normalTexture)    ||
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
            std::filesystem::path(materialPath).stem().string() + "_" + std::to_string(meshHandle.value);
        binding.runtimeMaterial =
            CreateRuntimeMaterialFromAsset(ctx, mesh, asset, debugName.c_str());
        binding.structureHash = structureHash;
        binding.buildFailed   = !binding.runtimeMaterial.IsValid();
        binding.baseColorPath.clear();
        binding.ormPath.clear();
        binding.normalPath.clear();
        binding.emissivePath.clear();
        binding.baseColorTexture = TextureHandle::Invalid();
        binding.ormTexture = TextureHandle::Invalid();
        binding.normalTexture = TextureHandle::Invalid();
        binding.emissiveTexture = TextureHandle::Invalid();
    }

    UpdateRuntimeMaterialParameters(ctx, binding.runtimeMaterial, binding, asset);
    return binding.runtimeMaterial;
}

bool ApplyMaterialAssetToMeshEntity(EditorFrameContext& ctx,
                                    EntityID entity,
                                    const std::string& materialPath,
                                    const uint32_t* submeshIndex = nullptr)
{
    const std::string normalizedMaterialPath = NormalizeMaterialAssetPath(ctx, materialPath);
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
        NormalizeMaterialAssetPath(ctx, ctx.state.selectedMaterialAssetPath) == normalizedMaterialPath;
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

    const MaterialHandle runtimeMaterial =
        AcquireSharedRuntimeMaterial(ctx, meshComp->mesh, normalizedMaterialPath, *mesh, *materialAsset);
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

void CollectMeshTargets(EditorFrameContext& ctx, EntityID root, std::vector<EntityID>& outTargets)
{
    outTargets.clear();
    ctx.world.ForEachAlive([&](EntityID id)
    {
        if (!ctx.world.Get<MeshComponent>(id))
            return;

        EntityID cur = id;
        while (cur.IsValid())
        {
            if (cur == root)
            {
                outTargets.push_back(id);
                return;
            }
            const auto* parent = ctx.world.Get<ParentComponent>(cur);
            cur = parent ? parent->parent : NULL_ENTITY;
        }
    });
}

void ReapplyMaterialAssetToBoundEntities(EditorFrameContext& ctx, const std::string& materialPath)
{
    const std::string normalizedMaterialPath = NormalizeMaterialAssetPath(ctx, materialPath);
    ctx.world.ForEachAlive([&](EntityID id)
    {
        auto* material = ctx.world.Get<MaterialComponent>(id);
        auto* meshComp = ctx.world.Get<MeshComponent>(id);
        if (!material || !meshComp)
            return;

        if (NormalizeMaterialAssetPath(ctx, material->materialAssetPath) == normalizedMaterialPath)
            ApplyMaterialAssetToMeshEntity(ctx, id, normalizedMaterialPath);

        for (MaterialComponent::SlotOverride& slot : material->slotOverrides)
        {
            if (NormalizeMaterialAssetPath(ctx, slot.materialAssetPath) == normalizedMaterialPath)
                ApplyMaterialAssetToMeshEntity(ctx, id, normalizedMaterialPath, &slot.submeshIndex);
        }
    });
}

void PreviewMaterialAssetOnBoundEntities(EditorFrameContext& ctx,
                                         const std::string& materialPath,
                                         const assets::MaterialAsset& previewAsset)
{
    const std::string normalizedMaterialPath = NormalizeMaterialAssetPath(ctx, materialPath);

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

        if (NormalizeMaterialAssetPath(ctx, material->materialAssetPath) == normalizedMaterialPath)
        {
            material->material = runtimeMaterial;
            material->materialAssetPath = normalizedMaterialPath;
        }

        for (MaterialComponent::SlotOverride& slot : material->slotOverrides)
        {
            if (NormalizeMaterialAssetPath(ctx, slot.materialAssetPath) == normalizedMaterialPath)
            {
                slot.material = runtimeMaterial;
                slot.materialAssetPath = normalizedMaterialPath;
            }
        }
    });
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
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.24f, 0.42f, 0.62f, 1.0f));

    if (ImGui::Button(label, ImVec2(90.f, kThumbSize + 2.f)))
        ctx.state.activeTextureParamName = paramName;

    if (isActive)
        ImGui::PopStyleColor();

    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Klicken: Slot aktivieren\nTextur aus dem Asset Browser hierher ziehen");

    // Drag&Drop auf den Slot-Button selbst
    if (ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("KROM_ASSET_TEXTURE"))
        {
            const char* texturePath = static_cast<const char*>(payload->Data);
            if (texturePath && texturePath[0] != '\0')
            {
                const std::string resolved = ToAssetRelativePath(ctx, std::filesystem::path(texturePath));
                if (IsSupportedTexturePath(resolved))
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
            ? std::filesystem::path(value)
            : (assetRoot / value).lexically_normal();

        const TextureHandle texHandle = LoadRuntimeTexture(ctx, texPath.string());
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
        {
            if (ctx.state.assetBrowser)
            {
                const std::filesystem::path root    = AssetRoot(ctx);
                const std::filesystem::path absPath = root.empty()
                    ? std::filesystem::path(value)
                    : (root / value).lexically_normal();
                ctx.state.assetBrowser->currentDir    = absPath.parent_path();
                ctx.state.assetBrowser->highlightPath = absPath;
            }
        }

        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s\nDoppelklick: Im Asset Browser anzeigen", value.c_str());

        // Drag&Drop auch auf das Thumbnail
        if (ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("KROM_ASSET_TEXTURE"))
            {
                const char* texturePath = static_cast<const char*>(payload->Data);
                if (texturePath && texturePath[0] != '\0')
                {
                    const std::string resolved = ToAssetRelativePath(ctx, std::filesystem::path(texturePath));
                    if (IsSupportedTexturePath(resolved))
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
                    const std::string resolved = ToAssetRelativePath(ctx, std::filesystem::path(texturePath));
                    if (IsSupportedTexturePath(resolved))
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
        const std::string filename = std::filesystem::path(value).filename().string();
        ImGui::SetNextItemWidth(pathWidth);
        ImGui::TextUnformatted(filename.c_str());
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", value.c_str());
    }
    else
    {
        ImGui::TextDisabled("<leer>");
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
        ImGui::SetTooltip("Textur aus diesem Slot entfernen");
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
        return false;

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

    ImGui::Separator();
    ImGui::TextDisabled("Shader-Parameter");
    for (assets::MaterialParam& param : asset.params)
    {
        if (IsBuiltInMaterialParamName(param.name))
            continue;
        ImGui::PushID(param.name.c_str());
        switch (param.type)
        {
        case assets::MaterialParam::Type::Float:
            if (SliderFloatWithDoubleClickInput(param.name.c_str(), &param.value.f[0], -100.f, 100.f, "%.3f", ImGuiSliderFlags_None))
                dirty = true;
            break;
        case assets::MaterialParam::Type::Vec2:
            if (ImGui::DragFloat2(param.name.c_str(), param.value.f, 0.01f))
                dirty = true;
            break;
        case assets::MaterialParam::Type::Vec3:
            if (param.name.find("Color") != std::string::npos || param.name.find("color") != std::string::npos)
            {
                if (ImGui::ColorEdit3(param.name.c_str(), param.value.f, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR))
                    dirty = true;
            }
            else if (ImGui::DragFloat3(param.name.c_str(), param.value.f, 0.01f))
                dirty = true;
            break;
        case assets::MaterialParam::Type::Vec4:
            if (param.name.find("Color") != std::string::npos || param.name.find("color") != std::string::npos)
            {
                if (ImGui::ColorEdit4(param.name.c_str(), param.value.f, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR))
                    dirty = true;
            }
            else if (ImGui::DragFloat4(param.name.c_str(), param.value.f, 0.01f))
                dirty = true;
            break;
        case assets::MaterialParam::Type::Int:
            if (ImGui::InputInt(param.name.c_str(), &param.value.i))
                dirty = true;
            break;
        case assets::MaterialParam::Type::Bool:
            if (ImGui::Checkbox(param.name.c_str(), &param.value.b))
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
        { "Shader-Dateien", "*.hlsl;*.glsl;*.vert;*.frag;*.vs;*.ps" },
        { "Alle Dateien",   "*.*" }
    };
    const std::filesystem::path initialFolder = AssetRoot(ctx);
    return platform::dialog::BrowseForFile(
        "Shader auswaehlen", filters, 2, nullptr, nullptr, initialFolder.string().c_str());
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

    std::filesystem::path currentRelative(ctx.state.selectedMaterialAssetPath);
    std::filesystem::path renamedRelative = currentRelative.parent_path() / draft;
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
        if (NormalizeMaterialAssetPath(ctx, material->materialAssetPath) == currentNormalized)
            material->materialAssetPath = renamedNormalized;
        for (MaterialComponent::SlotOverride& slot : material->slotOverrides)
            if (NormalizeMaterialAssetPath(ctx, slot.materialAssetPath) == currentNormalized)
                slot.materialAssetPath = renamedNormalized;
    });

    ctx.state.selectedMaterialAssetPath = renamedNormalized;
    ctx.state.selectedMaterialAssetNameDraft = std::filesystem::path(renamedNormalized).stem().string();
    ReloadSelectedMaterialAsset(ctx);
    ReapplyMaterialAssetToBoundEntities(ctx, renamedNormalized);
    return true;
}

/// Liest Shader-Felder aus dem Custom-Shader und befüllt asset.params.
/// Bestehende Werte in asset.params werden NICHT überschrieben.
/// Wird aufgerufen wenn template=custom und VS+FS geladen sind.
static void ReflectAndUpdateCustomParams(EditorFrameContext& ctx,
                                         assets::MaterialAsset& asset)
{
    if (!ctx.registry.shaders.Get(asset.vertexShader) ||
        !ctx.registry.shaders.Get(asset.fragmentShader))
        return;

    const assets::ShaderAsset* vsAsset = ctx.registry.shaders.Get(asset.vertexShader);
    const assets::ShaderAsset* fsAsset = ctx.registry.shaders.Get(asset.fragmentShader);
    if (!vsAsset || !fsAsset) return;

    // Bekannte Standard-Variablen in PerMaterial — NICHT als Custom anzeigen
    static const std::unordered_set<std::string> kStandardVars = {
        "baseColorFactor", "emissiveFactor", "metallicFactor", "roughnessFactor",
        "occlusionStrength", "opacityFactor", "alphaCutoff",
        "materialFeatureMask", "materialModel", "_pad0",
    };

    // Felder aus PerMaterial (b2) und custom Cbuffer (b3+) lesen
    auto addFields = [&](std::vector<assets::MaterialParam>& outParams,
                         const char* cbName, uint32_t bindingSlot)
    {
        // Hilfslambda: fields verarbeiten (type-unabhaengig via Template)
        auto processFields = [&](auto& fields)
        {
            if (fields.empty()) return;

            for (const auto& field : fields)
            {
                if (kStandardVars.count(field.name)) continue;

                auto existingIt = std::find_if(asset.params.begin(), asset.params.end(),
                    [&](const assets::MaterialParam& p) { return p.name == field.name; });

                if (existingIt != asset.params.end())
                    existingIt->type = field.ToParamType();
                else
                {
                    assets::MaterialParam param;
                    param.name = field.name;
                    param.type = field.ToParamType();
                    param.value.f[0] = param.value.f[1] = param.value.f[2] = param.value.f[3] = 0.f;
                    asset.params.push_back(std::move(param));
                }
            }

            // Veraltete Params entfernen
            asset.params.erase(
                std::remove_if(asset.params.begin(), asset.params.end(),
                    [&](const assets::MaterialParam& p) {
                        if (p.type == assets::MaterialParam::Type::Texture) return false;
                        if (kStandardVars.count(p.name)) return false;
                        return std::none_of(fields.begin(), fields.end(),
                            [&](const auto& f) { return f.name == p.name; });
                    }),
                asset.params.end());
        };

#if defined(KROM_APP_BACKEND_DX11)
        dx11::DX11ShaderReflector reflector;
        std::vector<dx11::CBufferField> fields;
        if (!reflector.ReflectCBufferFields(*vsAsset, cbName, fields, nullptr))
            reflector.ReflectCBufferFields(*fsAsset, cbName, fields, nullptr);
        processFields(fields);
#elif defined(KROM_APP_BACKEND_VULKAN)
        vulkan::VKShaderReflector reflector;
        std::vector<vulkan::CBufferField> fields;
        if (!reflector.ReflectCBufferFields(*fsAsset, bindingSlot, fields, nullptr))
            reflector.ReflectCBufferFields(*vsAsset, bindingSlot, fields, nullptr);
        processFields(fields);
#else
        (void)cbName; (void)bindingSlot;
#endif
    };

    addFields(asset.params, "PerMaterial", 2u);  // Standard-Slot mit custom Namen
    addFields(asset.params, "UserParams",  3u);  // Optionaler custom Cbuffer
}


void DrawMaterialPreview(EditorFrameContext& ctx, float previewSize)
{
    ImGui::TextDisabled("Vorschau");

    const float size = std::max(240.0f, previewSize);
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const ImVec2 end{start.x + size, start.y + size};

    ImGui::InvisibleButton("##materialPreviewImage", ImVec2(size, size));
    if (ImGui::IsItemHovered())
    {
        const ImGuiIO& io = ImGui::GetIO();
        if (io.MouseWheel != 0.f)
            ctx.state.materialPreviewDistance =
                std::clamp(ctx.state.materialPreviewDistance - io.MouseWheel * 0.32f, 2.0f, 8.0f);
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Right))
        {
            const ImVec2 delta = io.MouseDelta;
            ctx.state.materialPreviewYawDeg += delta.x * 0.35f;
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
        const char* label = "Keine Vorschau";
        const ImVec2 textSize = ImGui::CalcTextSize(label);
        dl->AddText(ImVec2(start.x + (size - textSize.x) * 0.5f,
                           start.y + (size - textSize.y) * 0.5f),
                    IM_COL32(170, 170, 170, 255),
                    label);
    }
    dl->AddRect(start, end, IM_COL32(95, 98, 104, 255));
    ImGui::Dummy(ImVec2(0.f, 4.f));
}

void DrawSelectedMaterialEditor(EditorFrameContext& ctx, bool showWindowActions)
{
    if (!ctx.state.selectedMaterialAssetLoaded && !ctx.state.selectedMaterialAssetPath.empty())
        ReloadSelectedMaterialAsset(ctx);

    if (!ctx.state.selectedMaterialAssetLoaded)
    {
        if (ctx.state.materialAssetLoadInFlight && !ctx.state.selectedMaterialAssetPath.empty())
        {
            ImGui::TextDisabled("Material wird geladen...");
            ImGui::Separator();
            ImGui::TextWrapped("Bitte kurz warten: %s", ctx.state.selectedMaterialAssetPath.c_str());
            return;
        }

        ImGui::TextDisabled("Kein Material-Asset ausgewaehlt");
        ImGui::Separator();
        ImGui::TextWrapped("Material im Assets-Fenster auswaehlen, dann hier bearbeiten.");
        return;
    }

    assets::MaterialAsset& asset = ctx.state.selectedMaterialAssetData;
    bool dirty = false;

    ImGui::Text("Datei: %s", ctx.state.selectedMaterialAssetPath.c_str());
    bool previewGroupOpen = false;
    if (showWindowActions)
    {
        ImGui::BeginGroup();
        const float availableWidth = ImGui::GetContentRegionAvail().x;
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        constexpr float kMinPropertyWidth = 430.0f;
        const float previewSize = std::clamp(availableWidth - kMinPropertyWidth - spacing,
                                             300.0f,
                                             512.0f);
        DrawMaterialPreview(ctx, previewSize);
        ImGui::EndGroup();
        ImGui::SameLine();
        ImGui::BeginGroup();
        const float propertyItemWidth = std::max(180.0f, ImGui::GetContentRegionAvail().x - 150.0f);
        ImGui::PushItemWidth(propertyItemWidth);
        previewGroupOpen = true;
    }

    char nameBuffer[256];
    std::snprintf(nameBuffer, sizeof(nameBuffer), "%s", ctx.state.selectedMaterialAssetNameDraft.c_str());
    if (ImGui::InputText("Materialname", nameBuffer, sizeof(nameBuffer)))
        ctx.state.selectedMaterialAssetNameDraft = nameBuffer;
    ImGui::SameLine();
    if (ImGui::Button("Umbenennen"))
        RenameSelectedMaterialAsset(ctx);
    ImGui::Separator();

    // ── 1. Texturen ───────────────────────────────────────────────────────────
    ImGui::TextDisabled("Texturen");
    bool ignoredCommit = false;
    DrawTexturePathRow(ctx, "Base Color", "albedo",   asset.baseColorTexture.path,          dirty, ignoredCommit);
    DrawTexturePathRow(ctx, "Normal",     "normal",   asset.normalTexture.path,              dirty, ignoredCommit);
    DrawTexturePathRow(ctx, "ORM",        "orm",      asset.metallicRoughnessTexture.path,   dirty, ignoredCommit);
    DrawTexturePathRow(ctx, "Emissive",   "emissive", asset.emissiveTexture.path,            dirty, ignoredCommit);

    // ── 2. UV ─────────────────────────────────────────────────────────────────
    ImGui::Separator();
    ImGui::TextDisabled("UV");
    if (ImGui::DragFloat2("UV Scale",  &asset.uvScale.x,  0.01f, -16.f, 16.f, "%.3f"))
        dirty = true;
    if (ImGui::DragFloat2("UV Offset", &asset.uvOffset.x, 0.01f,  -1.f,  1.f, "%.3f"))
        dirty = true;

    // ── 3. Material-Eigenschaften ─────────────────────────────────────────────
    ImGui::Separator();

    EditorMaterialTemplate currentTemplate = ParseMaterialTemplate(asset.templateName);
    int materialTemplate = static_cast<int>(currentTemplate);
    bool templateChanged = false;
    if (ImGui::Combo("Template", &materialTemplate, "PBR Lit\0Unlit\0Legacy Lit\0Custom\0"))
    {
        const auto newTpl = static_cast<EditorMaterialTemplate>(std::clamp(materialTemplate, 0, 3));
        asset.templateName = MaterialTemplateToken(newTpl);
        ctx.state.newMaterialTemplate = newTpl;
        currentTemplate = newTpl;
        SanitizeMaterialForTemplate(asset);
        templateChanged = true;
        dirty = true;
    }

    // ── Custom: Shader-Pfade ──────────────────────────────────────────────────
    if (currentTemplate == EditorMaterialTemplate::Custom)
    {
        ImGui::Separator();
        ImGui::TextDisabled("Shader");

        // Hilfslambda: Shader-Pfad aus Drag-Drop lesen.
        // Asset-Browser sendet KROM_MOVE_FILE (absoluter Pfad) fuer alle Dateien.
        // Wir akzeptieren das und konvertieren in einen Asset-relativen Pfad.
        auto acceptShaderDrop = [&](std::string& outPath, assets::ShaderStage stage,
                                    ShaderHandle& outHandle) -> bool
        {
            bool changed = false;
            // KROM_MOVE_FILE = absoluter Pfad (alle Nicht-Material Dateien im Asset-Browser)
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("KROM_MOVE_FILE"))
            {
                const std::string absPath = static_cast<const char*>(p->Data);
                const std::string ext = std::filesystem::path(absPath).extension().string();
                // Nur Shader-Dateien akzeptieren
                if (ext == ".hlsl" || ext == ".glsl" || ext == ".vert" ||
                    ext == ".frag" || ext == ".vs"   || ext == ".ps")
                {
                    // Relativen Asset-Pfad bilden (wenn moeglich)
                    outPath = NormalizeMaterialAssetPath(ctx, absPath);
                    if (outPath.empty()) outPath = absPath; // Fallback: absoluter Pfad
                    if (ctx.assetPipeline)
                        outHandle = LoadRuntimeShader(ctx, outPath, stage);
                    changed = true;
                }
            }
            return changed;
        };
        auto chooseShaderPath = [&](std::string& outPath, assets::ShaderStage stage,
                                    ShaderHandle& outHandle, const std::string& pickedPath) -> bool
        {
            if (pickedPath.empty())
                return false;
            const std::string ext = std::filesystem::path(pickedPath).extension().string();
            if (ext != ".hlsl" && ext != ".glsl" && ext != ".vert" &&
                ext != ".frag" && ext != ".vs" && ext != ".ps")
                return false;

            outPath = NormalizeMaterialAssetPath(ctx, pickedPath);
            if (outPath.empty())
                outPath = pickedPath;
            if (ctx.assetPipeline)
                outHandle = LoadRuntimeShader(ctx, outPath, stage);
            if (!outHandle.IsValid())
            {
                Debug::LogError("Materialeditor: Shader konnte nicht geladen werden: %s", outPath.c_str());
                return true;
            }
            if (const assets::ShaderAsset* shader = ctx.registry.shaders.Get(outHandle);
                shader && shader->stage != stage)
            {
                Debug::LogError("Materialeditor: Shader '%s' hat die falsche Stage (erwartet %u, geladen %u)",
                                outPath.c_str(),
                                static_cast<unsigned>(stage),
                                static_cast<unsigned>(shader->stage));
                outHandle = ShaderHandle::Invalid();
            }
            return true;
        };

        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.15f, 0.15f, 0.22f, 1.f));

        // ── Vertex Shader ─────────────────────────────────────────────────────
        {
            char buf[512]{};
            std::snprintf(buf, sizeof(buf), "%s", asset.vertexShaderPath.c_str());
            ImGui::SetNextItemWidth(-92.f);
            if (ImGui::InputText("##VS", buf, sizeof(buf),
                                 ImGuiInputTextFlags_EnterReturnsTrue))
            {
                asset.vertexShaderPath = buf;
                if (!asset.vertexShaderPath.empty() && ctx.assetPipeline)
                {
                    asset.vertexShader = LoadRuntimeShader(
                        ctx, asset.vertexShaderPath, assets::ShaderStage::Vertex);
                    if (const assets::ShaderAsset* shader = ctx.registry.shaders.Get(asset.vertexShader);
                        shader && shader->stage != assets::ShaderStage::Vertex)
                    {
                        Debug::LogError("Materialeditor: Vertex-Shader-Pfad zeigt auf falsche Stage: %s",
                                        asset.vertexShaderPath.c_str());
                        asset.vertexShader = ShaderHandle::Invalid();
                    }
                }
                dirty = true;
            }
            if (ImGui::BeginDragDropTarget())
            {
                if (acceptShaderDrop(asset.vertexShaderPath,
                                     assets::ShaderStage::Vertex, asset.vertexShader))
                    dirty = true;
                ImGui::EndDragDropTarget();
            }
            ImGui::SameLine();
            if (!platform::dialog::IsAvailable())
                ImGui::BeginDisabled();
            if (ImGui::Button("...##BrowseVS", ImVec2(28.f, 0.f)))
            {
                if (chooseShaderPath(asset.vertexShaderPath,
                                     assets::ShaderStage::Vertex,
                                     asset.vertexShader,
                                     BrowseForShaderFile(ctx)))
                    dirty = true;
            }
            if (!platform::dialog::IsAvailable())
                ImGui::EndDisabled();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Vertex-Shader aus Datei waehlen");
            ImGui::SameLine();
            ImGui::TextDisabled("VS");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Vertex-Shader\n.vs.hlsl / .vert.glsl\nAus Asset-Browser hierher ziehen\noder Pfad eingeben + Enter.");
        }

        // ── Fragment Shader ───────────────────────────────────────────────────
        {
            char buf[512]{};
            std::snprintf(buf, sizeof(buf), "%s", asset.fragmentShaderPath.c_str());
            ImGui::SetNextItemWidth(-92.f);
            if (ImGui::InputText("##FS", buf, sizeof(buf),
                                 ImGuiInputTextFlags_EnterReturnsTrue))
            {
                asset.fragmentShaderPath = buf;
                if (!asset.fragmentShaderPath.empty() && ctx.assetPipeline)
                {
                    asset.fragmentShader = LoadRuntimeShader(
                        ctx, asset.fragmentShaderPath, assets::ShaderStage::Fragment);
                    if (const assets::ShaderAsset* shader = ctx.registry.shaders.Get(asset.fragmentShader);
                        shader && shader->stage != assets::ShaderStage::Fragment)
                    {
                        Debug::LogError("Materialeditor: Fragment-Shader-Pfad zeigt auf falsche Stage: %s",
                                        asset.fragmentShaderPath.c_str());
                        asset.fragmentShader = ShaderHandle::Invalid();
                    }
                }
                dirty = true;
            }
            if (ImGui::BeginDragDropTarget())
            {
                if (acceptShaderDrop(asset.fragmentShaderPath,
                                     assets::ShaderStage::Fragment, asset.fragmentShader))
                    dirty = true;
                ImGui::EndDragDropTarget();
            }
            ImGui::SameLine();
            if (!platform::dialog::IsAvailable())
                ImGui::BeginDisabled();
            if (ImGui::Button("...##BrowseFS", ImVec2(28.f, 0.f)))
            {
                if (chooseShaderPath(asset.fragmentShaderPath,
                                     assets::ShaderStage::Fragment,
                                     asset.fragmentShader,
                                     BrowseForShaderFile(ctx)))
                    dirty = true;
            }
            if (!platform::dialog::IsAvailable())
                ImGui::EndDisabled();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Fragment-Shader aus Datei waehlen");
            ImGui::SameLine();
            ImGui::TextDisabled("FS");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Fragment-Shader\n.ps.hlsl / .frag.glsl\nAus Asset-Browser hierher ziehen\noder Pfad eingeben + Enter.");
        }

        ImGui::PopStyleColor();

        if (!asset.vertexShader.IsValid() && !asset.vertexShaderPath.empty() && ctx.assetPipeline)
            asset.vertexShader = LoadRuntimeShader(ctx, asset.vertexShaderPath, assets::ShaderStage::Vertex);
        if (!asset.fragmentShader.IsValid() && !asset.fragmentShaderPath.empty() && ctx.assetPipeline)
            asset.fragmentShader = LoadRuntimeShader(ctx, asset.fragmentShaderPath, assets::ShaderStage::Fragment);
        if (const assets::ShaderAsset* shader = ctx.registry.shaders.Get(asset.vertexShader);
            shader && shader->stage != assets::ShaderStage::Vertex)
            asset.vertexShader = ShaderHandle::Invalid();
        if (const assets::ShaderAsset* shader = ctx.registry.shaders.Get(asset.fragmentShader);
            shader && shader->stage != assets::ShaderStage::Fragment)
            asset.fragmentShader = ShaderHandle::Invalid();
        if (SyncCustomShaderParamsFromSource(ctx, asset))
            dirty = true;

        // Status + automatische Reflection wenn beide Shader geladen
        const bool vsOk = asset.vertexShader.IsValid();
        const bool fsOk = asset.fragmentShader.IsValid();
        if (!vsOk || !fsOk)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.4f, 0.3f, 1.f));
            if (!vsOk) ImGui::TextUnformatted("  Vertex-Shader nicht geladen");
            if (!fsOk) ImGui::TextUnformatted("  Fragment-Shader nicht geladen");
            ImGui::PopStyleColor();
        }
        else
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 1.f, 0.5f, 1.f));
            ImGui::TextUnformatted("  VS + FS bereit");
            ImGui::PopStyleColor();
            // Parameter automatisch aus Shader lesen
            ReflectAndUpdateCustomParams(ctx, asset);
        }
        ImGui::Separator();
    }

    if (currentTemplate == EditorMaterialTemplate::Custom)
        dirty = DrawCustomShaderParams(ctx, asset) || dirty;

    if (ImGui::ColorEdit4("Base Color", &asset.baseColorFactor.x))
        dirty = true;

    if (ImGui::ColorEdit3("Emissive", &asset.emissiveFactor.x, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR))
        dirty = true;

    if (currentTemplate != EditorMaterialTemplate::Unlit &&
        currentTemplate != EditorMaterialTemplate::Custom)
    {
        if (SliderFloatWithDoubleClickInput("Metallic",        &asset.metallicFactor,    0.f, 1.f)) dirty = true;
        if (SliderFloatWithDoubleClickInput("Roughness",       &asset.roughnessFactor,   0.f, 1.f)) dirty = true;
        if (SliderFloatWithDoubleClickInput("Normal Strength", &asset.normalScale,        0.f, 4.f)) dirty = true;
        if (SliderFloatWithDoubleClickInput("Occlusion",       &asset.occlusionStrength,  0.f, 4.f)) dirty = true;
    }

    if (ImGui::Checkbox("Double Sided",  &asset.doubleSided))  dirty = true;
    if (ImGui::Checkbox("Cast Shadows",  &asset.castShadows))  dirty = true;

    int alphaMode = static_cast<int>(asset.alphaMode);
    if (ImGui::Combo("Alpha Mode", &alphaMode, "Opaque\0Mask\0Blend\0"))
    {
        asset.alphaMode   = static_cast<assets::MaterialAlphaMode>(std::clamp(alphaMode, 0, 2));
        asset.transparent = asset.alphaMode == assets::MaterialAlphaMode::Blend;
        dirty = true;
    }
    if (asset.alphaMode != assets::MaterialAlphaMode::Opaque &&
        SliderFloatWithDoubleClickInput("Alpha Cutoff", &asset.alphaCutoff, 0.f, 1.f))
        dirty = true;


    if (dirty)
    {
        dirty = SanitizeMaterialForTemplate(asset) || dirty;
        ctx.state.selectedMaterialAssetDirty = true;

        // Custom-Shader-Pfade sofort SYNCHRON speichern — der async Save-Queue
        // kommt zu spaet: ein async Reload wuerde die Pfade vorher ueberschreiben.
        if ((currentTemplate == EditorMaterialTemplate::Custom &&
             (!asset.vertexShaderPath.empty() || !asset.fragmentShaderPath.empty())) ||
            templateChanged)
        {
            SaveMaterialAssetFile(ctx, ctx.state.selectedMaterialAssetPath, asset);
        }

        PreviewMaterialAssetOnBoundEntities(ctx, ctx.state.selectedMaterialAssetPath, asset);
        SaveSelectedMaterialAsset(ctx);
    }

    ImGui::Separator();

    // Expliziter Speichern-Button — auch wenn kein dirty-Flag gesetzt ist,
    // z.B. nach externem Umbenennen oder zum bewussten Bestätigen.
    const bool canSave = ctx.state.selectedMaterialAssetLoaded &&
                         !ctx.state.selectedMaterialAssetPath.empty();
    if (!canSave)
        ImGui::BeginDisabled();
    if (ImGui::Button("Speichern##material"))
        SaveSelectedMaterialAsset(ctx);
    if (!canSave)
        ImGui::EndDisabled();

    ImGui::SameLine();
    const MaterialSaveQueue::StatusSnapshot saveStatus =
        QueryMaterialSaveStatus(ctx, ctx.state.selectedMaterialAssetPath);
    const auto now = std::chrono::steady_clock::now();
    const auto ageMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - saveStatus.lastUpdate).count();
    switch (saveStatus.state)
    {
    case MaterialSaveQueue::SaveState::Pending:
    case MaterialSaveQueue::SaveState::Saving:
        ImGui::TextColored(ImVec4(0.96f, 0.82f, 0.30f, 1.f), "Speichert...");
        break;
    case MaterialSaveQueue::SaveState::Saved:
        if (ageMs < 2000)
            ImGui::TextColored(ImVec4(0.48f, 0.90f, 0.56f, 1.f), "Gespeichert");
        else
            ImGui::TextDisabled("(automatisch gespeichert)");
        break;
    case MaterialSaveQueue::SaveState::Failed:
        ImGui::TextColored(ImVec4(0.96f, 0.42f, 0.38f, 1.f), "Speichern fehlgeschlagen!");
        break;
    case MaterialSaveQueue::SaveState::Idle:
    default:
        ImGui::TextDisabled("(automatisch gespeichert)");
        break;
    }

    if (!showWindowActions)
        return;

    ImGui::Separator();
    const bool canAssign = ctx.state.selectedEntity.IsValid();
    if (!canAssign)
        ImGui::BeginDisabled();
    if (ImGui::Button("Auswahl zuweisen"))
    {
        SaveSelectedMaterialAsset(ctx);
        std::vector<EntityID> meshTargets;
        CollectMeshTargets(ctx, ctx.state.selectedEntity, meshTargets);
        if (meshTargets.empty())
        {
            const EntityID meshEntity = FindFirstMeshDescendant(ctx.world, ctx.state.selectedEntity);
            if (meshEntity.IsValid())
                meshTargets.push_back(meshEntity);
        }

        for (EntityID target : meshTargets)
            ApplyMaterialAssetToMeshEntity(ctx, target, ctx.state.selectedMaterialAssetPath);
    }
    if (!canAssign)
        ImGui::EndDisabled();
    if (!canAssign)
        ImGui::SetItemTooltip("Entity mit Mesh auswaehlen");

    ImGui::SameLine();
    const bool canReapply = !ctx.state.selectedMaterialAssetPath.empty();
    if (!canReapply)
        ImGui::BeginDisabled();
    if (ImGui::Button("Auf gebundene Meshes anwenden"))
    {
        SaveSelectedMaterialAsset(ctx);
        ReapplyMaterialAssetToBoundEntities(ctx, ctx.state.selectedMaterialAssetPath);
    }
    if (!canReapply)
        ImGui::EndDisabled();

    if (previewGroupOpen)
    {
        ImGui::PopItemWidth();
        ImGui::EndGroup();
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Öffentliche Implementierung — aufrufbar aus EditorUI.cpp (Ctrl+S)
// ---------------------------------------------------------------------------

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
        NormalizeMaterialAssetPath(ctx, ctx.state.selectedMaterialAssetPath) == normalizedPath)
    {
        ctx.state.selectedMaterialAssetPath = normalizedPath;
        return;
    }

    if (ctx.state.selectedMaterialAssetDirty)
        SaveSelectedMaterialAsset(ctx);

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

    const std::filesystem::path pendingPath = ctx.state.pendingOpenMaterialAssetPath;
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

void ResolveMaterialAssetBindings(EditorFrameContext& ctx)
{
    if (!ctx.assetPipeline)
        return;

    ctx.world.ForEachAlive([&](EntityID id)
    {
        auto* material = ctx.world.Get<MaterialComponent>(id);
        auto* meshComp = ctx.world.Get<MeshComponent>(id);
        if (!material || !meshComp)
            return;

        if (!material->materialAssetPath.empty())
        {
            material->materialAssetPath = NormalizeMaterialAssetPath(ctx, material->materialAssetPath);
            ApplyMaterialAssetToMeshEntity(ctx, id, material->materialAssetPath);
        }

        for (MaterialComponent::SlotOverride& slot : material->slotOverrides)
        {
            if (slot.materialAssetPath.empty())
                continue;
            slot.materialAssetPath = NormalizeMaterialAssetPath(ctx, slot.materialAssetPath);
            ApplyMaterialAssetToMeshEntity(ctx, id, slot.materialAssetPath, &slot.submeshIndex);
        }
    });
}

void TickMaterialTextureSync(EditorFrameContext& ctx)
{
    if (!ctx.assetPipeline)
        return;

    // Pro Frame: höchstens ein Material-Reapply um Frame-Spikes zu vermeiden.
    // Nur Materialien die gerade NICHT im Editor bearbeitet werden (dirty=false),
    // um Konflikte mit unsaved Änderungen zu vermeiden.
    for (const auto& [key, binding] : SharedMaterialBindings())
    {
        if (!binding.runtimeMaterial.IsValid())
            continue;

        // Nicht eingreifen wenn das Material gerade im Editor bearbeitet wird
        const std::string normalizedSelected =
            NormalizeMaterialAssetPath(ctx, ctx.state.selectedMaterialAssetPath);
        if (ctx.state.selectedMaterialAssetLoaded &&
            ctx.state.selectedMaterialAssetDirty  &&
            normalizedSelected == key.materialPath)
            continue;

        // Prüfen ob eine Textur deren Pfad gesetzt ist noch nicht geladen wurde
        // und jetzt geladen werden kann.
        const bool textureNowAvailable =
            (!binding.baseColorPath.empty() && !binding.baseColorTexture.IsValid() &&
             LoadRuntimeTexture(ctx, binding.baseColorPath).IsValid()) ||
            (!binding.ormPath.empty()        && !binding.ormTexture.IsValid()        &&
             LoadRuntimeTexture(ctx, binding.ormPath).IsValid())        ||
            (!binding.normalPath.empty()     && !binding.normalTexture.IsValid()     &&
             LoadRuntimeTexture(ctx, binding.normalPath).IsValid())     ||
            (!binding.emissivePath.empty()   && !binding.emissiveTexture.IsValid()   &&
             LoadRuntimeTexture(ctx, binding.emissivePath).IsValid());

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
    return ApplyMaterialAssetToMeshEntity(ctx, entity, NormalizeMaterialAssetPath(ctx, materialPath));
}

bool ApplyMaterialAssetToEntitySlot(EditorFrameContext& ctx,
                                    EntityID entity,
                                    uint32_t submeshIndex,
                                    const std::filesystem::path& materialPath)
{
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
        ? "DefaultWhite"
        : ("DefaultWhite_" + mesh->debugName);
    const MaterialHandle handle = CreateDefaultWhiteMaterial(ctx, *mesh, materialName.c_str());
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

    const std::string resolvedPath = ToAssetRelativePath(ctx, std::filesystem::path(fileName));
    if (!IsSupportedTexturePath(resolvedPath))
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
    SaveSelectedMaterialAsset(ctx);
}

void DrawMaterialLibrary(EditorFrameContext& ctx)
{
    if (!ctx.state.materialWindowOpen)
        return;

    ImGui::SetNextWindowPos(ImVec2(280.f, 10.f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(1120.f, 560.f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(900.f, 460.f), ImVec2(2000.f, 1400.f));
    if (ctx.state.materialWindowFocusRequest)
    {
        ImGui::SetNextWindowFocus();
        ImGui::SetNextWindowCollapsed(false);
    }
    if (!ImGui::Begin("Materialeditor##editor", &ctx.state.materialWindowOpen))
    {
        ctx.state.materialWindowFocusRequest = false;
        ImGui::End();
        return;
    }
    ctx.state.materialWindowFocusRequest = false;

    DrawSelectedMaterialEditor(ctx, true);

    ImGui::End();
}

void DrawSelectedMaterialInspector(EditorFrameContext& ctx)
{
    DrawSelectedMaterialEditor(ctx, false);
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
void ResolveMaterialAssetBindings(EditorFrameContext&) {}
bool ApplySelectedMaterialToEntity(EditorFrameContext&, EntityID) { return false; }
bool ApplyMaterialAssetToEntity(EditorFrameContext&, EntityID, const std::filesystem::path&) { return false; }
bool ApplyMaterialAssetToEntitySlot(EditorFrameContext&, EntityID, uint32_t, const std::filesystem::path&) { return false; }
void TickMaterialTextureSync(EditorFrameContext&) {}
bool AssignDefaultMaterialToEntity(EditorFrameContext&, EntityID) { return false; }
bool AssignThumbnailWhiteMaterialToEntity(EditorFrameContext&, EntityID) { return false; }
void AssignTextureToSelectedMaterial(EditorFrameContext&, TextureHandle, const std::string&, const std::string&) {}
bool SaveSelectedMaterialAsset(EditorFrameContext&) { return false; }
void DrawMaterialLibrary(EditorFrameContext&) {}
void DrawSelectedMaterialInspector(EditorFrameContext&) {}
void ClearSharedMaterialBindings() {}

} // namespace engine::renderer::addons::editor

#endif
