#include "KromRuntimeMaterials.hpp"

#include "addons/lit/LitMaterial.hpp"
#include "addons/mesh_renderer/MeshRendererComponents.hpp"
#include "addons/pbr/PbrInstanceBuilder.hpp"
#include "addons/pbr/PbrMasterMaterial.hpp"
#include "addons/unlit/UnlitMaterial.hpp"
#include "assets/MeshTangents.hpp"
#include "assets/VertexLayoutBridge.hpp"
#include "core/Debug.hpp"
#include "core/PathUtf8.hpp"
#include "ecs/Components.hpp"
#include "ecs/World.hpp"
#include "renderer/MaterialDomain.hpp"
#include "renderer/MaterialGraphBinding.hpp"
#include "renderer/ShaderBindingModel.hpp"
#include "renderer/ShaderConstantBufferReflection.hpp"

#include <algorithm>
#include <array>
#include <memory>
#include <string_view>
#include <vector>

using engine::PathFromUtf8;
using engine::PathToGenericUtf8;
using engine::PathToUtf8;

namespace Krom::internal {

void GenerateMissingNormals(engine::assets::SubMeshData& submesh)
{
    const size_t vertexCount = submesh.positions.size() / 3u;
    if (vertexCount == 0u)
        return;

    if (submesh.normals.size() < vertexCount * 3u)
        submesh.normals.resize(vertexCount * 3u, 0.0f);

    std::vector<engine::math::Vec3> generated(vertexCount, engine::math::Vec3::Zero());
    const auto indexAt = [&](size_t i) -> uint32_t {
        return submesh.indices.empty() ? static_cast<uint32_t>(i) : submesh.indices[i];
    };
    const size_t indexCount = submesh.indices.empty() ? vertexCount : submesh.indices.size();
    for (size_t i = 0; i + 2u < indexCount; i += 3u)
    {
        const uint32_t ia = indexAt(i);
        const uint32_t ib = indexAt(i + 1u);
        const uint32_t ic = indexAt(i + 2u);
        if (ia >= vertexCount || ib >= vertexCount || ic >= vertexCount)
            continue;

        const engine::math::Vec3 a{submesh.positions[ia * 3u], submesh.positions[ia * 3u + 1u], submesh.positions[ia * 3u + 2u]};
        const engine::math::Vec3 b{submesh.positions[ib * 3u], submesh.positions[ib * 3u + 1u], submesh.positions[ib * 3u + 2u]};
        const engine::math::Vec3 c{submesh.positions[ic * 3u], submesh.positions[ic * 3u + 1u], submesh.positions[ic * 3u + 2u]};
        const engine::math::Vec3 normal = engine::math::Vec3::Cross(b - a, c - a).Normalized();
        if (normal.LengthSq() <= engine::math::EPSILON)
            continue;

        for (const uint32_t idx : {ia, ib, ic})
            generated[idx] += normal;
    }

    for (size_t vertex = 0u; vertex < vertexCount; ++vertex)
    {
        const size_t i = vertex * 3u;
        engine::math::Vec3 normal{submesh.normals[i], submesh.normals[i + 1u], submesh.normals[i + 2u]};
        if (normal.LengthSq() <= engine::math::EPSILON)
            normal = generated[vertex].LengthSq() > engine::math::EPSILON
                ? generated[vertex].Normalized()
                : engine::math::Vec3::Up();
        else
            normal = normal.Normalized();

        submesh.normals[i] = normal.x;
        submesh.normals[i + 1u] = normal.y;
        submesh.normals[i + 2u] = normal.z;
    }
}

std::string ResolveEngineAssetPath(const RuntimeMaterialContext& ctx, const char* path)
{
    if (!path || path[0] == '\0')
        return {};

    const std::filesystem::path requested = PathFromUtf8(path);
    if (requested.is_absolute() && std::filesystem::exists(requested))
        return PathToUtf8(requested);

    // 1. Projekt-Assets (Custom-Shader des Users)
    if (!ctx.assetRoot.empty() && std::filesystem::exists(ctx.assetRoot / requested))
        return PathToUtf8(std::filesystem::absolute(ctx.assetRoot / requested));

    // 2. Engine-Assets (builtin Shader neben der EXE / im CWD)
    if (!ctx.engineAssetRoot.empty() && std::filesystem::exists(ctx.engineAssetRoot / requested))
        return PathToUtf8(std::filesystem::absolute(ctx.engineAssetRoot / requested));

    return path;
}

engine::ShaderHandle LoadEngineShader(const RuntimeMaterialContext& ctx,
                                      const char* path,
                                      engine::assets::ShaderStage stage)
{
    if (!ctx.assetPipeline)
        return engine::ShaderHandle::Invalid();

    return ctx.assetPipeline->LoadShader(ResolveEngineAssetPath(ctx, path), stage);
}

static engine::TextureHandle LoadRuntimeTexture(const RuntimeMaterialContext& ctx, const std::string& path,
                                                engine::assets::TextureLoadRole role);

static void ApplyRuntimeMaterialGraph(const RuntimeMaterialContext& ctx, engine::MaterialHandle material,
                                      const engine::assets::CompiledMaterialGraph& graph,
                                      const engine::assets::MaterialAsset& asset)
{
    std::array<engine::TextureHandle, engine::assets::kMaterialGraphMaxTextures> textures{};
    for (size_t i = 0; i < textures.size(); ++i)
        textures[i] = LoadRuntimeTexture(ctx, graph.textures[i],
                                         graph.textureNormal[i] ? engine::assets::TextureLoadRole::NormalMap :
                                         graph.textureLinear[i] ? engine::assets::TextureLoadRole::LinearData
                                                                : engine::assets::TextureLoadRole::Auto);
    engine::renderer::SetMaterialGraph(ctx.materialSystem, material, graph, textures, asset.uvScale, asset.uvOffset);
    ctx.materialSystem.MarkDirty(material);
}

static bool EnsureTangentsForPbr(engine::assets::MeshAsset& mesh);

engine::MaterialHandle CreateRuntimeLitMaterialFromAssets(const RuntimeMaterialContext& ctx,
                                                          engine::assets::MeshAsset& mesh,
                                                          const engine::assets::MaterialAsset* asset,
                                                          const char* name)
{
    if (mesh.submeshes.empty())
    {
        engine::Debug::LogError("krom.h: runtime material '%s' cannot be created because mesh has no submeshes",
            name ? name : "KromRuntimeLit");
        return engine::MaterialHandle::Invalid();
    }

    const bool skinned = mesh.HasSkinning();
    // The skinned lit layout requires tangents; models often ship without them.
    if (skinned && !EnsureTangentsForPbr(mesh))
    {
        engine::Debug::LogError("krom.h: skinned material '%s' requires tangents", name ? name : "KromRuntimeLit");
        return engine::MaterialHandle::Invalid();
    }
    const char* vsPath = skinned ? "skinned_lit.vs.hlsl" : "lit.vs.hlsl";
    const char* fsPath = "lit.ps.hlsl";
    const char* shadowPath = skinned ? "skinned_shadow.vs.hlsl" : "shadow.vs.hlsl";
    const char* shadowFsPath = "shadow.ps.hlsl";
    const bool opengl = ctx.openGL;
    if (opengl)
    {
        vsPath = "lit.opengl.vs.glsl";
        fsPath = "lit.opengl.fs.glsl";
        shadowPath = "shadow.opengl.vs.glsl";
        shadowFsPath = "shadow.opengl.fs.glsl";
    }

    const engine::ShaderHandle vs = LoadEngineShader(ctx, vsPath, engine::assets::ShaderStage::Vertex);
    const engine::ShaderHandle fs = LoadEngineShader(ctx, fsPath, engine::assets::ShaderStage::Fragment);
    const engine::ShaderHandle shadow = LoadEngineShader(ctx, shadowPath, engine::assets::ShaderStage::Vertex);
    if (!vs.IsValid() || !fs.IsValid() || !shadow.IsValid())
    {
        engine::Debug::LogError("krom.h: failed to load runtime lit shaders vs='%s' fs='%s' shadow='%s'",
            vsPath, fsPath, shadowPath);
        return engine::MaterialHandle::Invalid();
    }

    const bool hasLegacyNormalMap = asset && !asset->PbrNormalPath().empty();
    std::string layoutError;
    const auto contract = skinned
        ? engine::renderer::VertexContracts::SkinnedLit()
        : hasLegacyNormalMap
        ? engine::renderer::VertexContracts::PbrLit()
        : engine::renderer::VertexContracts::StaticLit();
    const engine::renderer::VertexLayout layout =
        engine::assets::ResolveVertexLayout(contract, mesh.submeshes[0], &layoutError);
    if (layout.attributes.empty())
    {
        engine::Debug::LogError("krom.h: vertex layout resolution failed for '%s': %s",
            name ? name : "<material>", layoutError.c_str());
        return engine::MaterialHandle::Invalid();
    }

    engine::TextureHandle albedoTexture = engine::TextureHandle::Invalid();
    engine::TextureHandle normalTexture = engine::TextureHandle::Invalid();
    engine::TextureHandle emissiveTexture = engine::TextureHandle::Invalid();
    if (asset && !asset->baseColorTexture.path.empty())
        albedoTexture = ctx.assetPipeline->LoadTexture(asset->baseColorTexture.path);
    if (hasLegacyNormalMap)
        normalTexture = ctx.assetPipeline->LoadTexture(asset->PbrNormalPath());
    if (asset && !asset->emissiveTexture.path.empty())
        emissiveTexture = ctx.assetPipeline->LoadTexture(asset->emissiveTexture.path);
    if (albedoTexture.IsValid() || normalTexture.IsValid() || emissiveTexture.IsValid())
    {
        ctx.assetPipeline->UploadPendingGpuAssets();
        if (albedoTexture.IsValid())
            albedoTexture = ctx.assetPipeline->GetGpuTexture(albedoTexture);
        if (normalTexture.IsValid())
            normalTexture = ctx.assetPipeline->GetGpuTexture(normalTexture);
        if (emissiveTexture.IsValid())
            emissiveTexture = ctx.assetPipeline->GetGpuTexture(emissiveTexture);
    }

    engine::renderer::lit::LitMaterialCreateInfo info{};
    info.name = name ? name : "KromRuntimeLit";
    info.vertexShader = vs;
    info.fragmentShader = fs;
    info.shadowShader = shadow;
    info.shadowFragmentShader = LoadEngineShader(ctx, shadowFsPath, engine::assets::ShaderStage::Fragment);
    info.vertexLayout = layout;
    info.baseColorFactor = asset ? asset->baseColorFactor : engine::math::Vec4::One();
    info.emissiveFactor = asset
        ? engine::math::Vec4{asset->RuntimeEmissive().x, asset->RuntimeEmissive().y, asset->RuntimeEmissive().z, 1.0f}
        : engine::math::Vec4::Zero();
    info.specularStrength = asset ? asset->PbrMetallic() : 0.0f;
    info.roughnessFactor = asset ? asset->PbrRoughness() : 1.0f;
    info.normalStrength = asset ? asset->PbrNormalStrength() : 1.0f;
    const engine::assets::CompiledMaterialGraph graph =
        asset ? engine::assets::CompileMaterialGraph(asset->graph) : engine::assets::CompiledMaterialGraph{};
    info.materialGraph = graph.Active();
    info.openGL = opengl;
    info.opacityFactor = asset ? asset->baseColorFactor.w : 1.0f;
    info.alphaCutoff = asset ? asset->alphaCutoff : 0.5f;
    info.enableBaseColorMap = albedoTexture.IsValid();
    info.enableNormalMap = normalTexture.IsValid();
    info.enableEmissiveMap = emissiveTexture.IsValid();
    info.alphaTest = asset && asset->alphaMode == engine::assets::MaterialAlphaMode::Mask;
    info.doubleSided = asset && asset->doubleSided;
    info.castShadows = !asset || asset->castShadows;
    info.cullMode = (asset && asset->doubleSided)
        ? engine::renderer::MaterialCullMode::None
        : engine::renderer::MaterialCullMode::Back;

    const engine::MaterialHandle handle =
        engine::renderer::lit::LitMaterial::Register(ctx.materialSystem, info);
    if (!handle.IsValid())
    {
        engine::Debug::LogError("krom.h: failed to register runtime lit material '%s'",
            name ? name : "KromRuntimeLit");
        return handle;
    }

    engine::renderer::lit::LitMaterial litMaterial(ctx.materialSystem, handle);
    if (albedoTexture.IsValid())
        (void)litMaterial.SetAlbedo(albedoTexture);
    if (normalTexture.IsValid())
        (void)litMaterial.SetNormal(normalTexture);
    if (emissiveTexture.IsValid())
        (void)litMaterial.SetEmissive(emissiveTexture);
    if (graph.Active())
        ApplyRuntimeMaterialGraph(ctx, handle, graph, *asset);
    return handle;
}

static bool EnsureTangentsForPbr(engine::assets::MeshAsset& mesh)
{
    bool changed = false;
    for (engine::assets::SubMeshData& submesh : mesh.submeshes)
    {
        GenerateMissingNormals(submesh);
        if (engine::assets::HasValidTangents(submesh))
            continue;
        if (!engine::assets::EnsureTangents(submesh))
            return false;
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

static engine::TextureHandle LoadRuntimeTexture(const RuntimeMaterialContext& ctx, const std::string& path,
                                                engine::assets::TextureLoadRole role = engine::assets::TextureLoadRole::Auto)
{
    if (!ctx.assetPipeline || path.empty())
        return engine::TextureHandle::Invalid();
    const engine::TextureHandle assetTexture = ctx.assetPipeline->LoadTexture(path, role);
    if (!assetTexture.IsValid())
        return engine::TextureHandle::Invalid();
    ctx.texturePaths[assetTexture.value] = path;
    ctx.assetPipeline->UploadPendingGpuAssets();
    return ctx.assetPipeline->GetGpuTexture(assetTexture);
}

static bool IsBuiltInMaterialParamName(const std::string& name) noexcept
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

static engine::renderer::MaterialParam ToRuntimeParam(const engine::assets::MaterialParam& src)
{
    engine::renderer::MaterialParam dst{};
    dst.name = src.name;
    switch (src.type)
    {
    case engine::assets::MaterialParam::Type::Float:   dst.type = engine::renderer::MaterialParam::Type::Float; break;
    case engine::assets::MaterialParam::Type::Vec2:    dst.type = engine::renderer::MaterialParam::Type::Vec2; break;
    case engine::assets::MaterialParam::Type::Vec3:    dst.type = engine::renderer::MaterialParam::Type::Vec3; break;
    case engine::assets::MaterialParam::Type::Vec4:    dst.type = engine::renderer::MaterialParam::Type::Vec4; break;
    case engine::assets::MaterialParam::Type::Int:     dst.type = engine::renderer::MaterialParam::Type::Int; break;
    case engine::assets::MaterialParam::Type::Bool:    dst.type = engine::renderer::MaterialParam::Type::Bool; break;
    case engine::assets::MaterialParam::Type::Texture: dst.type = engine::renderer::MaterialParam::Type::Texture; break;
    }
    switch (src.type)
    {
    case engine::assets::MaterialParam::Type::Float:
    case engine::assets::MaterialParam::Type::Vec2:
    case engine::assets::MaterialParam::Type::Vec3:
    case engine::assets::MaterialParam::Type::Vec4:
        dst.value.f[0] = src.value.f[0];
        dst.value.f[1] = src.value.f[1];
        dst.value.f[2] = src.value.f[2];
        dst.value.f[3] = src.value.f[3];
        break;
    case engine::assets::MaterialParam::Type::Int:
        dst.value.i = src.value.i;
        break;
    case engine::assets::MaterialParam::Type::Bool:
        dst.value.b = src.value.b;
        break;
    case engine::assets::MaterialParam::Type::Texture:
        dst.texture = src.texture;
        break;
    }
    return dst;
}

static std::string ResolveRuntimeShaderPath(const RuntimeMaterialContext& ctx,
                                            const std::string& shaderPath,
                                            engine::assets::ShaderStage stage)
{
    if (!ctx.openGL)
        return shaderPath;

    std::filesystem::path candidate = PathFromUtf8(shaderPath);
    const std::string filename = PathToUtf8(candidate.filename());
    std::string variantName;
    if (stage == engine::assets::ShaderStage::Vertex)
    {
        constexpr std::string_view suffix = ".vs.hlsl";
        if (filename.size() > suffix.size() &&
            filename.compare(filename.size() - suffix.size(), suffix.size(), suffix.data(), suffix.size()) == 0)
            variantName = filename.substr(0, filename.size() - suffix.size()) + ".opengl.vs.glsl";
    }
    else if (stage == engine::assets::ShaderStage::Fragment)
    {
        constexpr std::string_view suffix = ".ps.hlsl";
        if (filename.size() > suffix.size() &&
            filename.compare(filename.size() - suffix.size(), suffix.size(), suffix.data(), suffix.size()) == 0)
            variantName = filename.substr(0, filename.size() - suffix.size()) + ".opengl.fs.glsl";
    }
    if (variantName.empty())
        return shaderPath;

    candidate.replace_filename(PathFromUtf8(variantName));
    const std::string variantPath = PathToGenericUtf8(candidate);
    const std::string resolved = ResolveEngineAssetPath(ctx, variantPath.c_str());
    return std::filesystem::exists(PathFromUtf8(resolved)) ? variantPath : shaderPath;
}

static engine::ShaderHandle LoadRuntimeShader(const RuntimeMaterialContext& ctx,
                                              const std::string& shaderPath,
                                              engine::assets::ShaderStage stage)
{
    if (!ctx.assetPipeline)
        return engine::ShaderHandle::Invalid();
    const std::string resolvedShaderPath = ResolveRuntimeShaderPath(ctx, shaderPath, stage);
    return ctx.assetPipeline->LoadShader(ResolveEngineAssetPath(ctx, resolvedShaderPath.c_str()), stage);
}

// Custom-Shader: Textur-Register und PerMaterial-Offsets aus dem kompilierten
// Shader lesen (wie im Editor), unabhaengig vom Backend-Addon.
static void ReflectCustomShaderBindings(const RuntimeMaterialContext& ctx,
                                        engine::ShaderHandle vs,
                                        engine::ShaderHandle fs,
                                        engine::renderer::unlit::UnlitMaterialCreateInfo& info)
{
    const engine::assets::ShaderAsset* vsAsset = ctx.assetRegistry.shaders.Get(vs);
    const engine::assets::ShaderAsset* fsAsset = ctx.assetRegistry.shaders.Get(fs);
    const engine::renderer::IDevice* device = ctx.renderSystem.GetDevice();
    if (!vsAsset || !fsAsset || !device)
        return;

    const engine::assets::ShaderTargetProfile target = device->GetShaderTargetProfile();
    std::string error;
    if (!engine::renderer::ReflectProgramTextureBindings(*vsAsset, *fsAsset, target, info.textureBindingOverrides, &error))
        engine::Debug::LogWarning("krom.h: texture register reflection failed for '%s' (%s), custom textures use name/order slots",
                                  info.name.c_str(), error.c_str());
    if (!engine::renderer::ReflectProgramConstantBuffer(*vsAsset, *fsAsset, target,
                                                        "PerMaterial", engine::renderer::CBSlots::PerMaterial,
                                                        info.perMaterialReflection, &error))
        engine::Debug::LogWarning("krom.h: PerMaterial reflection failed for '%s' (%s), packing parameters in fixed order",
                                  info.name.c_str(), error.c_str());
}

static engine::MaterialHandle CreateRuntimeUnlitMaterialFromAssets(const RuntimeMaterialContext& ctx,
                                                                   engine::assets::MeshAsset& mesh,
                                                                   const engine::assets::MaterialAsset& asset,
                                                                   const char* name,
                                                                   engine::ShaderHandle customVS = engine::ShaderHandle::Invalid(),
                                                                   engine::ShaderHandle customFS = engine::ShaderHandle::Invalid())
{
    using namespace engine;

    for (assets::SubMeshData& submesh : mesh.submeshes)
        GenerateMissingNormals(submesh);

    // Custom shaders bring their own vertex stage and stay on the static layout.
    const bool skinned = mesh.HasSkinning() && !customVS.IsValid();

    std::string layoutError;
    const renderer::VertexLayout layout = assets::ResolveVertexLayout(
        skinned ? renderer::VertexContracts::SkinnedUnlit() : renderer::VertexContracts::StaticLit(),
        mesh.submeshes[0], &layoutError);
    if (layout.attributes.empty())
    {
        Debug::LogError("krom.h: unlit/custom vertex layout failed for '%s': %s",
                        name ? name : "<material>", layoutError.c_str());
        return MaterialHandle::Invalid();
    }

    ShaderHandle vs = customVS;
    ShaderHandle fs = customFS;
    if (!vs.IsValid() || !fs.IsValid())
    {
        const bool opengl = ctx.openGL;
        vs = LoadEngineShader(ctx, opengl ? "quad_unlit.opengl.vs.glsl"
                                          : (skinned ? "skinned_unlit.vs.hlsl" : "quad_unlit.vs.hlsl"),
                              assets::ShaderStage::Vertex);
        fs = LoadEngineShader(ctx, opengl ? "quad_unlit.opengl.fs.glsl" : "quad_unlit.ps.hlsl", assets::ShaderStage::Fragment);
    }
    if (!vs.IsValid() || !fs.IsValid())
    {
        Debug::LogError("krom.h: failed to load unlit/custom shaders for '%s'",
                        name ? name : "<material>");
        return MaterialHandle::Invalid();
    }

    renderer::unlit::UnlitMaterialCreateInfo info{};
    info.name = name ? name : "KromRuntimeUnlit";
    info.vertexShader = vs;
    info.fragmentShader = fs;
    info.vertexLayout = layout;
    info.baseColorFactor = asset.baseColorFactor;
    info.emissiveFactor = {asset.RuntimeEmissive().x, asset.RuntimeEmissive().y, asset.RuntimeEmissive().z, 1.f};
    info.opacityFactor = asset.baseColorFactor.w;
    info.alphaCutoff = asset.alphaCutoff;
    // Standard-PBR-Faktoren: wirken nur bei Custom-Shadern, die sie deklarieren.
    info.metallicFactor = asset.metallicFactor;
    info.roughnessFactor = asset.roughnessFactor;
    info.occlusionStrength = asset.occlusionStrength;
    info.enableBaseColorMap = !asset.baseColorTexture.path.empty();
    info.enableEmissiveMap = !asset.emissiveTexture.path.empty();
    info.alphaTest = asset.alphaMode == assets::MaterialAlphaMode::Mask;
    info.doubleSided = asset.doubleSided;
    info.castShadows = asset.castShadows;
    info.cullMode = asset.doubleSided ? renderer::MaterialCullMode::None : renderer::MaterialCullMode::Back;
    const assets::CompiledMaterialGraph graph =
        customVS.IsValid() ? assets::CompiledMaterialGraph{} : assets::CompileMaterialGraph(asset.graph);
    info.materialGraph = graph.Active();
    info.openGL = ctx.openGL;

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
    if (customVS.IsValid())
    {
        // Normalen-Staerke ist kein Unlit-Standardfeld; Custom-Shader lesen sie per Name.
        assets::MaterialParam normal{};
        normal.name = "normalScale";
        normal.type = assets::MaterialParam::Type::Float;
        normal.value.f[0] = asset.normalScale;
        info.extraParameters.push_back(ToRuntimeParam(normal));
    }
    if (customVS.IsValid())
        ReflectCustomShaderBindings(ctx, vs, fs, info);

    const MaterialHandle handle = renderer::unlit::UnlitMaterial::Register(ctx.materialSystem, info);
    if (!handle.IsValid())
        return handle;

    if (const TextureHandle albedo = LoadRuntimeTexture(ctx, asset.baseColorTexture.path); albedo.IsValid())
        ctx.materialSystem.SetTexture(handle, "albedo", albedo);
    if (const TextureHandle emissive = LoadRuntimeTexture(ctx, asset.emissiveTexture.path); emissive.IsValid())
        ctx.materialSystem.SetTexture(handle, "emissive", emissive);
    for (const assets::MaterialParam& param : asset.params)
    {
        if (IsBuiltInMaterialParamName(param.name) || param.type != assets::MaterialParam::Type::Texture || param.texturePath.empty())
            continue;
        if (const TextureHandle tex = LoadRuntimeTexture(ctx, param.texturePath); tex.IsValid())
            ctx.materialSystem.SetTexture(handle, param.name, tex);
    }
    if (graph.Active())
        ApplyRuntimeMaterialGraph(ctx, handle, graph, asset);
    ctx.materialSystem.MarkDirty(handle);
    return handle;
}

static engine::MaterialHandle CreateRuntimeCustomMaterialFromAssets(const RuntimeMaterialContext& ctx,
                                                                    engine::assets::MeshAsset& mesh,
                                                                    const engine::assets::MaterialAsset& asset,
                                                                    const char* name)
{
    engine::ShaderHandle vs = asset.vertexShader;
    engine::ShaderHandle fs = asset.fragmentShader;
    if (!vs.IsValid() && !asset.vertexShaderPath.empty())
        vs = LoadRuntimeShader(ctx, asset.vertexShaderPath, engine::assets::ShaderStage::Vertex);
    if (!fs.IsValid() && !asset.fragmentShaderPath.empty())
        fs = LoadRuntimeShader(ctx, asset.fragmentShaderPath, engine::assets::ShaderStage::Fragment);
    if (!vs.IsValid() || !fs.IsValid())
    {
        engine::Debug::LogError("krom.h: custom material '%s' missing shaders (vs='%s', fs='%s')",
                                name ? name : "<material>",
                                asset.vertexShaderPath.c_str(),
                                asset.fragmentShaderPath.c_str());
        return engine::MaterialHandle::Invalid();
    }
    return CreateRuntimeUnlitMaterialFromAssets(ctx, mesh, asset.WithGraphTextures(), name, vs, fs);
}

static uint64_t RuntimeMaterialCacheKey(LPMATERIAL material, LPMESH mesh) noexcept
{
    return (static_cast<uint64_t>(material.value) << 32u) | static_cast<uint64_t>(mesh.value);
}

static engine::MaterialHandle CreateRuntimePbrMaterialFromAssets(const RuntimeMaterialContext& ctx,
                                                                 engine::assets::MeshAsset& mesh,
                                                                 const engine::assets::MaterialAsset& asset,
                                                                 const char* name)
{
    using namespace engine;
    if (!EnsureTangentsForPbr(mesh))
    {
        Debug::LogError("krom.h: PBR material requires tangents");
        return MaterialHandle::Invalid();
    }

    const bool opengl = ctx.openGL;
    const bool skinned = mesh.HasSkinning();

    std::string layoutError;
    renderer::VertexLayout layout = assets::ResolveVertexLayout(
        skinned ? renderer::VertexContracts::SkinnedLit() : renderer::VertexContracts::PbrLit(),
        mesh.submeshes[0], &layoutError);
    if (layout.attributes.empty())
    {
        Debug::LogError("krom.h: PBR vertex layout failed: %s", layoutError.c_str());
        return MaterialHandle::Invalid();
    }

    const bool forwardPlus = ctx.enableForwardPlus && !opengl;
    const char* vsPath = opengl ? "pbr_lit.opengl.vs.glsl"
                       : (skinned ? "skinned_pbr_lit.vs.hlsl" : "pbr_lit.vs.hlsl");
    const char* fsPath = opengl ? "pbr_lit.opengl.fs.glsl" : (forwardPlus ? "pbr_forward_plus.ps.hlsl" : "pbr_lit.ps.hlsl");
    const char* shadowPath = opengl ? "shadow_pbr.opengl.vs.glsl"
                           : (skinned ? "skinned_shadow_pbr.vs.hlsl" : "shadow_pbr.vs.hlsl");
    const char* shadowFsPath = opengl ? "shadow_pbr.opengl.fs.glsl" : "shadow_pbr.ps.hlsl";

    const ShaderHandle vs = LoadEngineShader(ctx, vsPath, assets::ShaderStage::Vertex);
    const ShaderHandle fs = LoadEngineShader(ctx, fsPath, assets::ShaderStage::Fragment);
    const ShaderHandle shadow = LoadEngineShader(ctx, shadowPath, assets::ShaderStage::Vertex);
    const ShaderHandle shadowFs = LoadEngineShader(ctx, shadowFsPath, assets::ShaderStage::Fragment);
    if (!vs.IsValid() || !fs.IsValid() || !shadow.IsValid())
    {
        Debug::LogError("krom.h: failed to load PBR shaders vs='%s' fs='%s' shadow='%s'",
            vsPath, fsPath, shadowPath);
        return MaterialHandle::Invalid();
    }

    renderer::pbr::PbrMasterMaterial::Config config{};
    config.vs = vs;
    config.fs = fs;
    config.shadow = shadow;
    config.shadowFs = shadowFs;
    config.vertexLayout = layout;
    config.cullMode = asset.doubleSided ? renderer::MaterialCullMode::None : renderer::MaterialCullMode::Back;
    config.castShadows = asset.castShadows;
    config.receiveShadows = true;
    const auto graph = assets::CompileMaterialGraph(asset.graph);
    config.materialGraph = graph.Active();
    config.openGL = opengl;

    renderer::pbr::PbrMasterMaterial master =
        renderer::pbr::PbrMasterMaterial::Create(ctx.materialSystem, config);
    if (!master.IsValid())
        return MaterialHandle::Invalid();

    TextureHandle baseColorTex = LoadRuntimeTexture(ctx, asset.baseColorTexture.path);
    TextureHandle ormTex = LoadRuntimeTexture(ctx, asset.PbrOrmPath(), assets::TextureLoadRole::LinearData);
    TextureHandle normalTex = LoadRuntimeTexture(ctx, asset.PbrNormalPath(), assets::TextureLoadRole::NormalMap);
    TextureHandle emissiveTex = LoadRuntimeTexture(ctx, asset.emissiveTexture.path);

    auto builder = master.CreateInstance(name ? name : "KromPBR");
    builder.Specular(asset.PbrSpecular());
    if (baseColorTex.IsValid()) builder.BaseColor(baseColorTex);
    else builder.BaseColor(asset.baseColorFactor);

    if (ormTex.IsValid())
    {
        builder.Occlusion(ormTex, renderer::pbr::MaterialChannel::R, asset.PbrOcclusion());
        builder.Roughness(ormTex, renderer::pbr::MaterialChannel::G, asset.PbrRoughness());
        builder.Metallic(ormTex, renderer::pbr::MaterialChannel::B, asset.PbrMetallic());
    }
    else
    {
        builder.Occlusion(asset.PbrOcclusion());
        builder.Roughness(asset.PbrRoughness());
        builder.Metallic(asset.PbrMetallic());
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

    const MaterialHandle material = builder.Build();
    if (material.IsValid())
    {
        // baseColorFactor wird im Builder nur gesetzt wenn KEINE Textur verwendet wird.
        // Hat das Material eine BaseColor-Textur, muss der Factor explizit nachgesetzt
        // werden, damit er die Textur im Shader korrekt multipliziert (Standard-PBR).
        if (baseColorTex.IsValid())
            ctx.materialSystem.SetVec4(material, "baseColorFactor", asset.baseColorFactor);
        // The builder sets the factor only without a texture, like baseColorFactor above.
        const math::Vec3 emissiveFactor = asset.RuntimeEmissive();
        ctx.materialSystem.SetVec4(material, "emissiveFactor", {emissiveFactor.x, emissiveFactor.y, emissiveFactor.z, 0.f});
        ctx.materialSystem.SetVec2(material, "uvScale", asset.uvScale);
        ctx.materialSystem.SetVec2(material, "uvOffset", asset.uvOffset);
        // Computed graph normals have no direct normal texture for the builder to set this from.
        ctx.materialSystem.SetFloat(material, "normalStrength", asset.PbrNormalStrength());
        ctx.materialSystem.SetFloat(material, "boxTileSize", asset.BoxProjectionTileSize());
        engine::renderer::pbr::SetPbrClearcoat(ctx.materialSystem, material, asset.ClearcoatStrength(),
                                               asset.ClearcoatRoughness());
        if (graph.Active())
        {
            ApplyRuntimeMaterialGraph(ctx, material, graph, asset);
        }
        ctx.materialSystem.MarkDirty(material);
    }
    return material;
}

LPMATERIAL RealizeMaterialForEntity(const RuntimeMaterialContext& ctx, LPENTITY entity, LPMATERIAL material)
{
    if (!ctx.world || !material.IsValid())
        return material;

    const engine::assets::MaterialAsset* asset = ctx.assetRegistry.materials.Get(material);
    if (!asset)
        return material;  // Kein AssetRegistry-Handle → bereits ein MaterialSystem-Handle

    const engine::MeshComponent* meshComp = ctx.world->Get<engine::MeshComponent>(entity);
    if (!meshComp || !meshComp->mesh.IsValid())
        return material;

    engine::assets::MeshAsset* mesh = ctx.assetRegistry.meshes.Get(meshComp->mesh);
    if (!mesh || mesh->submeshes.empty())
        return material;

    for (engine::assets::SubMeshData& submesh : mesh->submeshes)
        GenerateMissingNormals(submesh);

    const uint64_t cacheKey = RuntimeMaterialCacheKey(material, meshComp->mesh);
    if (const auto cached = ctx.runtimeMaterialCache.find(cacheKey); cached != ctx.runtimeMaterialCache.end())
        return cached->second.IsValid() ? cached->second : material;

    if (asset->templateName == "custom")
    {
        const LPMATERIAL realized = CreateRuntimeCustomMaterialFromAssets(
            ctx, *mesh, *asset,
            asset->debugName.empty() ? nullptr : asset->debugName.c_str());
        if (realized.IsValid())
            ctx.runtimeMaterialCache[cacheKey] = realized;
        return realized.IsValid() ? realized : material;
    }

    if (asset->templateName == "unlit")
    {
        const LPMATERIAL realized = CreateRuntimeUnlitMaterialFromAssets(
            ctx, *mesh, *asset,
            asset->debugName.empty() ? nullptr : asset->debugName.c_str());
        if (realized.IsValid())
            ctx.runtimeMaterialCache[cacheKey] = realized;
        return realized.IsValid() ? realized : material;
    }

    if (asset->templateName == "pbr-lit")
    {
        const LPMATERIAL realized = CreateRuntimePbrMaterialFromAssets(
            ctx, *mesh, *asset,
            asset->debugName.empty() ? nullptr : asset->debugName.c_str());
        if (realized.IsValid())
            ctx.runtimeMaterialCache[cacheKey] = realized;
        return realized.IsValid() ? realized : material;
    }

    const LPMATERIAL realized = CreateRuntimeLitMaterialFromAssets(
        ctx, *mesh, asset,
        asset->debugName.empty() ? nullptr : asset->debugName.c_str());
    if (realized.IsValid())
        ctx.runtimeMaterialCache[cacheKey] = realized;
    return realized.IsValid() ? realized : material;
}

static bool ParseImportedMeshAssetPath(const std::string& assetPath,
                                       std::string& basePath,
                                       size_t& meshIndex)
{
    const size_t hashPos = assetPath.rfind('#');
    if (hashPos == std::string::npos)
        return false;

    constexpr std::string_view meshPrefix = "mesh/";
    const std::string_view fragment(assetPath.data() + hashPos + 1u,
                                    assetPath.size() - hashPos - 1u);
    if (fragment.size() <= meshPrefix.size() ||
        fragment.compare(0u, meshPrefix.size(), meshPrefix) != 0)
        return false;

    try
    {
        meshIndex = static_cast<size_t>(std::stoul(
            std::string(fragment.substr(meshPrefix.size()))));
    }
    catch (const std::exception&)
    {
        return false;
    }

    basePath = assetPath.substr(0u, hashPos);
    return true;
}

static uint32_t RequiredRuntimeMaterialSlotCount(const engine::assets::MeshAsset& mesh)
{
    uint32_t count = static_cast<uint32_t>(mesh.materialHandles.size());
    for (const engine::assets::SubMeshData& submesh : mesh.submeshes)
        count = std::max(count, submesh.materialIndex + 1u);
    return count;
}

static engine::MaterialHandle CreateRuntimeMaterialFromImportedAsset(
                                                                     const RuntimeMaterialContext& ctx,
                                                                     engine::assets::MeshAsset& mesh,
                                                                     const engine::assets::MaterialAsset& asset,
                                                                     const char* name)
{
    if (asset.templateName == "custom")
        return CreateRuntimeCustomMaterialFromAssets(ctx, mesh, asset, name);
    if (asset.templateName == "unlit")
        return CreateRuntimeUnlitMaterialFromAssets(ctx, mesh, asset, name);
    if (asset.templateName == "pbr-lit")
    {
        const LPMATERIAL pbr = CreateRuntimePbrMaterialFromAssets(ctx, mesh, asset, name);
        if (pbr.IsValid())
            return pbr;
    }
    return CreateRuntimeLitMaterialFromAssets(ctx, mesh, &asset, name);
}

static bool MaterialComponentHasExplicitAssignment(const engine::MaterialComponent& material)
{
    if (material.material.IsValid() || !material.materialAssetPath.empty())
        return true;
    for (const engine::MaterialComponent::SlotOverride& slot : material.slotOverrides)
        if (slot.material.IsValid() || !slot.materialAssetPath.empty())
            return true;
    return false;
}

void ApplyImportedMeshMaterialsForEntity(
                                         const RuntimeMaterialContext& ctx,
                                         LPENTITY entity,
                                         std::unordered_map<std::string, engine::assets::ImportedAssetBundle>& bundleCache,
                                         std::unordered_map<std::string, LPMATERIAL>& runtimeMaterialCache)
{
    if (!ctx.world || !ctx.assetPipeline || !ctx.world->IsAlive(entity))
        return;

    const auto* meshComp = ctx.world->Get<engine::MeshComponent>(entity);
    if (!meshComp || !meshComp->mesh.IsValid() || meshComp->meshAssetPath.empty())
        return;

    auto* material = ctx.world->Get<engine::MaterialComponent>(entity);
    if (material && MaterialComponentHasExplicitAssignment(*material))
        return;

    std::string basePath;
    size_t meshIndex = 0u;
    if (!ParseImportedMeshAssetPath(meshComp->meshAssetPath, basePath, meshIndex))
        return;

    engine::assets::ImportedAssetBundle* bundle = nullptr;
    if (auto cached = bundleCache.find(basePath); cached != bundleCache.end())
    {
        bundle = &cached->second;
    }
    else
    {
        engine::assets::ImportedAssetBundle imported = ctx.assetPipeline->ImportBundle(basePath);
        if (!imported.Ok())
            return;
        auto [it, inserted] = bundleCache.emplace(basePath, std::move(imported));
        (void)inserted;
        bundle = &it->second;
    }

    if (!bundle || meshIndex >= bundle->meshes.size())
        return;

    engine::assets::MeshAsset* runtimeMesh = ctx.assetRegistry.meshes.Get(meshComp->mesh);
    if (!runtimeMesh || runtimeMesh->submeshes.empty())
        return;

    const engine::assets::MeshAsset& importedMesh = bundle->meshes[meshIndex];
    const uint32_t requiredSlots = RequiredRuntimeMaterialSlotCount(importedMesh);
    if (requiredSlots == 0u || bundle->materials.empty())
        return;

    std::vector<LPMATERIAL> runtimeMaterials(requiredSlots, LPMATERIAL::Invalid());
    for (uint32_t slotIndex = 0u; slotIndex < requiredSlots; ++slotIndex)
    {
        if (slotIndex >= bundle->materials.size())
            continue;

        const engine::assets::MaterialAsset& importedMaterial = bundle->materials[slotIndex];
        const std::string cacheKey =
            meshComp->meshAssetPath + "#mat/" + std::to_string(slotIndex);
        if (const auto cached = runtimeMaterialCache.find(cacheKey);
            cached != runtimeMaterialCache.end())
        {
            runtimeMaterials[slotIndex] = cached->second;
            continue;
        }

        const std::string runtimeName = importedMaterial.debugName.empty()
            ? (PathToUtf8(PathFromUtf8(basePath).stem()) + "_mat" + std::to_string(slotIndex))
            : importedMaterial.debugName;
        const LPMATERIAL runtimeMaterial = CreateRuntimeMaterialFromImportedAsset(
            ctx, *runtimeMesh, importedMaterial, runtimeName.c_str());
        if (runtimeMaterial.IsValid())
        {
            runtimeMaterialCache[cacheKey] = runtimeMaterial;
            runtimeMaterials[slotIndex] = runtimeMaterial;
        }
    }

    if (!material)
    {
        ctx.world->Add<engine::MaterialComponent>(entity);
        material = ctx.world->Get<engine::MaterialComponent>(entity);
    }
    if (!material)
        return;

    if (runtimeMesh->submeshes.size() == 1u)
    {
        const uint32_t materialIndex = runtimeMesh->submeshes[0].materialIndex;
        if (materialIndex < runtimeMaterials.size() && runtimeMaterials[materialIndex].IsValid())
            material->material = runtimeMaterials[materialIndex];
        return;
    }

    for (uint32_t submeshIndex = 0u;
         submeshIndex < runtimeMesh->submeshes.size();
         ++submeshIndex)
    {
        const uint32_t materialIndex = runtimeMesh->submeshes[submeshIndex].materialIndex;
        if (materialIndex >= runtimeMaterials.size() ||
            !runtimeMaterials[materialIndex].IsValid())
            continue;

        engine::MaterialComponent::SlotOverride* slot = material->FindSlotOverride(submeshIndex);
        if (!slot)
        {
            engine::MaterialComponent::SlotOverride newSlot{};
            newSlot.submeshIndex = submeshIndex;
            material->slotOverrides.push_back(newSlot);
            slot = &material->slotOverrides.back();
        }
        if (!slot->material.IsValid() && slot->materialAssetPath.empty())
            slot->material = runtimeMaterials[materialIndex];
    }
}

// Default lit material (matt white) for meshes without a material. Uses the
// same lit path as imported materials so that the vertex layout is resolved per
// mesh (static/skinned). Cached per mesh handle in runtimeMaterialCache:
// material.value == 0 gives the key mesh.value, which cannot collide with real
// materials (their material.value is not 0).
static LPMATERIAL GetOrCreateDefaultLitMaterial(const RuntimeMaterialContext& ctx, LPMESH meshHandle)
{
    if (!meshHandle.IsValid())
        return NULL_MATERIAL;

    engine::assets::MeshAsset* mesh = ctx.assetRegistry.meshes.Get(meshHandle);
    if (!mesh || mesh->submeshes.empty())
        return NULL_MATERIAL;

    const uint64_t cacheKey = RuntimeMaterialCacheKey(NULL_MATERIAL, meshHandle);
    if (const auto cached = ctx.runtimeMaterialCache.find(cacheKey); cached != ctx.runtimeMaterialCache.end())
        return cached->second;

    for (engine::assets::SubMeshData& submesh : mesh->submeshes)
        GenerateMissingNormals(submesh);

    engine::assets::MaterialAsset defaultMat{};
    defaultMat.debugName       = "DefaultLit";
    defaultMat.baseColorFactor = engine::math::Vec4{1.0f, 1.0f, 1.0f, 1.0f};
    defaultMat.metallicFactor  = 0.0f;
    defaultMat.roughnessFactor = 0.65f;

    const LPMATERIAL handle = CreateRuntimeLitMaterialFromAssets(ctx, *mesh, &defaultMat, "DefaultLit");
    ctx.runtimeMaterialCache[cacheKey] = handle;
    return handle;
}

void EnsureDefaultMaterialForEntity(const RuntimeMaterialContext& ctx, LPENTITY entity)
{
    if (!ctx.world || !ctx.world->IsAlive(entity))
        return;

    const auto* meshComp = ctx.world->Get<engine::MeshComponent>(entity);
    if (!meshComp || !meshComp->mesh.IsValid())
        return;

    if (const auto* mat = ctx.world->Get<engine::MaterialComponent>(entity); mat && mat->material.IsValid())
        return;

    if (const engine::assets::MeshAsset* mesh = ctx.assetRegistry.meshes.Get(meshComp->mesh))
    {
        for (const auto& mh : mesh->materialHandles)
            if (mh.IsValid())
                return;  // Import liefert eigene Submesh-Materialien → nicht überschreiben
    }

    const LPMATERIAL fallback = GetOrCreateDefaultLitMaterial(ctx, meshComp->mesh);
    if (!fallback.IsValid())
        return;

    if (!ctx.world->Has<engine::MaterialComponent>(entity))
        ctx.world->Add<engine::MaterialComponent>(entity, engine::MaterialComponent{fallback});
    else
        ctx.world->Get<engine::MaterialComponent>(entity)->material = fallback;
}

LPMATERIAL BuildCodeMaterial(const RuntimeMaterialContext& ctx,
                             const char* albedoPath,
                             const engine::math::Vec4& baseColorFactor,
                             float roughness,
                             float metallic)
{
    auto asset = std::make_unique<engine::assets::MaterialAsset>();
    if (albedoPath && albedoPath[0] != '\0')
    {
        asset->baseColorTexture.path = albedoPath;
        asset->debugName = albedoPath;
    }
    else
    {
        asset->debugName = "KromCodeMaterial";
    }
    asset->baseColorFactor   = baseColorFactor;
    asset->metallicFactor    = metallic;
    asset->roughnessFactor   = roughness;
    asset->castShadows       = true;
    return ctx.assetRegistry.materials.Add(std::move(asset));
}

LPMATERIAL BuildPbrCodeMaterial(const RuntimeMaterialContext& ctx, const PBR& pbr)
{
    auto asset = std::make_unique<engine::assets::MaterialAsset>();
    asset->templateName = "pbr-lit";
    asset->debugName = pbr.color.texture && pbr.color.texture[0] != '\0'
        ? pbr.color.texture
        : "KromPBRMaterial";
    asset->baseColorFactor = pbr.color.color;
    asset->roughnessFactor = pbr.roughness;
    asset->metallicFactor = pbr.metallic;
    asset->occlusionStrength = pbr.occlusion;
    asset->normalScale = pbr.normalStrength;
    asset->uvScale = pbr.uvScale;
    asset->uvOffset = pbr.uvOffset;
    asset->castShadows = true;

    if (pbr.color.texture && pbr.color.texture[0] != '\0')
        asset->baseColorTexture.path = pbr.color.texture;
    if (pbr.normal && pbr.normal[0] != '\0')
        asset->normalTexture.path = pbr.normal;
    if (pbr.orm && pbr.orm[0] != '\0')
        asset->metallicRoughnessTexture.path = pbr.orm;

    return ctx.assetRegistry.materials.Add(std::move(asset));
}

} // namespace Krom::internal
