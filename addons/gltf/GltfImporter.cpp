// =============================================================================
// KROM Engine - addons/gltf/GltfImporter.cpp
// cgltf-Implementierung: Skeleton, Skinned Mesh, Animation.
// =============================================================================
// cgltf ist eine single-header library (jkuhlmann/cgltf, zero dependencies).
// Ablageort: third_party/cgltf/cgltf.h
#define CGLTF_IMPLEMENTATION
#include "cgltf.h"

#include "GltfImporter.hpp"
#include "GltfAnimationMerge.hpp"

#include "assets/AssetRegistry.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>

namespace engine::addons::gltf {
namespace {

// ─── Extension ───────────────────────────────────────────────────────────────

std::string_view GetExtension(std::string_view path) noexcept
{
    const auto dot = path.rfind('.');
    return dot != std::string_view::npos ? path.substr(dot) : "";
}

// ─── Accessor helpers ─────────────────────────────────────────────────────────

math::Vec3 ReadVec3(const cgltf_accessor* acc, cgltf_size idx) noexcept
{
    float v[3] = {};
    cgltf_accessor_read_float(acc, idx, v, 3);
    return { v[0], v[1], v[2] };
}

math::Quat ReadQuat(const cgltf_accessor* acc, cgltf_size idx) noexcept
{
    float v[4] = {};
    cgltf_accessor_read_float(acc, idx, v, 4);
    return { v[0], v[1], v[2], v[3] }; // glTF xyzw == Engine xyzw
}

// glTF speichert Matrizen column-major; Engine Mat4 ist ebenfalls column-major
// (m[col][row]) — direktes memcpy ist korrekt.
math::Mat4 ReadMat4(const cgltf_accessor* acc, cgltf_size idx) noexcept
{
    float v[16] = {};
    cgltf_accessor_read_float(acc, idx, v, 16);
    math::Mat4 mat{};
    static_assert(sizeof(mat.m) == sizeof(v));
    std::memcpy(&mat.m[0][0], v, sizeof(v));
    return mat;
}

math::Mat4 Mat4FromColumnMajor(const cgltf_float* v) noexcept
{
    math::Mat4 mat{};
    static_assert(sizeof(mat.m) == sizeof(float) * 16u);
    std::memcpy(&mat.m[0][0], v, sizeof(float) * 16u);
    return mat;
}

math::Vec3 ColumnLength(const math::Mat4& m, int col) noexcept
{
    return { m.m[col][0], m.m[col][1], m.m[col][2] };
}

math::Quat QuatFromRotationMatrix(const math::Mat4& m) noexcept
{
    const float trace = m.m[0][0] + m.m[1][1] + m.m[2][2];
    if (trace > 0.f)
    {
        const float s = std::sqrt(trace + 1.f) * 2.f;
        return { (m.m[1][2] - m.m[2][1]) / s,
                 (m.m[2][0] - m.m[0][2]) / s,
                 (m.m[0][1] - m.m[1][0]) / s,
                 0.25f * s };
    }
    if (m.m[0][0] > m.m[1][1] && m.m[0][0] > m.m[2][2])
    {
        const float s = std::sqrt(1.f + m.m[0][0] - m.m[1][1] - m.m[2][2]) * 2.f;
        return { 0.25f * s,
                 (m.m[1][0] + m.m[0][1]) / s,
                 (m.m[2][0] + m.m[0][2]) / s,
                 (m.m[1][2] - m.m[2][1]) / s };
    }
    if (m.m[1][1] > m.m[2][2])
    {
        const float s = std::sqrt(1.f + m.m[1][1] - m.m[0][0] - m.m[2][2]) * 2.f;
        return { (m.m[1][0] + m.m[0][1]) / s,
                 0.25f * s,
                 (m.m[2][1] + m.m[1][2]) / s,
                 (m.m[2][0] - m.m[0][2]) / s };
    }

    const float s = std::sqrt(1.f + m.m[2][2] - m.m[0][0] - m.m[1][1]) * 2.f;
    return { (m.m[2][0] + m.m[0][2]) / s,
             (m.m[2][1] + m.m[1][2]) / s,
             0.25f * s,
             (m.m[0][1] - m.m[1][0]) / s };
}

void DecomposeTRS(const math::Mat4& m,
                  math::Vec3& outTranslation,
                  math::Quat& outRotation,
                  math::Vec3& outScale) noexcept
{
    outTranslation = { m.m[3][0], m.m[3][1], m.m[3][2] };

    const math::Vec3 c0 = ColumnLength(m, 0);
    const math::Vec3 c1 = ColumnLength(m, 1);
    const math::Vec3 c2 = ColumnLength(m, 2);
    outScale = { c0.Length(), c1.Length(), c2.Length() };

    math::Mat4 r = math::Mat4::Identity();
    if (outScale.x > math::EPSILON)
    {
        r.m[0][0] = m.m[0][0] / outScale.x;
        r.m[0][1] = m.m[0][1] / outScale.x;
        r.m[0][2] = m.m[0][2] / outScale.x;
    }
    if (outScale.y > math::EPSILON)
    {
        r.m[1][0] = m.m[1][0] / outScale.y;
        r.m[1][1] = m.m[1][1] / outScale.y;
        r.m[1][2] = m.m[1][2] / outScale.y;
    }
    if (outScale.z > math::EPSILON)
    {
        r.m[2][0] = m.m[2][0] / outScale.z;
        r.m[2][1] = m.m[2][1] / outScale.z;
        r.m[2][2] = m.m[2][2] / outScale.z;
    }

    outRotation = QuatFromRotationMatrix(r).Normalized();
}

assets::ImportedSceneNode ImportNode(const cgltf_data& data,
                                     const cgltf_node& node,
                                     bool hasSkeleton) noexcept
{
    assets::ImportedSceneNode imported;
    imported.name = node.name ? node.name : "";

    if (node.parent)
        imported.parentIndex = static_cast<int32_t>(cgltf_node_index(&data, node.parent));
    if (node.mesh)
        imported.meshIndex = static_cast<int32_t>(cgltf_mesh_index(&data, node.mesh));
    if (node.skin && hasSkeleton)
        imported.skinIndex = 0;

    if (node.has_matrix)
    {
        const math::Mat4 local = Mat4FromColumnMajor(node.matrix);
        DecomposeTRS(local, imported.translation, imported.rotation, imported.scale);
        return imported;
    }

    imported.translation = { node.translation[0], node.translation[1], node.translation[2] };
    imported.rotation = math::Quat{
        node.rotation[0], node.rotation[1], node.rotation[2], node.rotation[3]
    }.Normalized();
    imported.scale = { node.scale[0], node.scale[1], node.scale[2] };
    return imported;
}

float ReadScalar(const cgltf_accessor* acc, cgltf_size idx) noexcept
{
    float v = 0.f;
    cgltf_accessor_read_float(acc, idx, &v, 1);
    return v;
}

// ─── Skeleton ────────────────────────────────────────────────────────────────

std::string ResolveTexturePath(const cgltf_texture_view& view,
                               std::string_view          filePath)
{
    if (!view.texture || !view.texture->image || !view.texture->image->uri)
        return {};

    std::string uri = view.texture->image->uri;
    if (uri.rfind("data:", 0) == 0)
        return {};

    namespace fs = std::filesystem;
    fs::path p(uri);
    if (p.is_absolute())
        return p.lexically_normal().string();

    return (fs::path(std::string(filePath)).parent_path() / p).lexically_normal().string();
}

assets::MaterialTextureRef ImportTextureRef(const cgltf_texture_view& view,
                                            std::string_view          filePath)
{
    assets::MaterialTextureRef ref;
    ref.path = ResolveTexturePath(view, filePath);
    ref.texCoord = view.texcoord >= 0 ? static_cast<uint32_t>(view.texcoord) : 0u;
    return ref;
}

assets::MaterialAlphaMode ImportAlphaMode(cgltf_alpha_mode mode) noexcept
{
    switch (mode)
    {
    case cgltf_alpha_mode_mask:  return assets::MaterialAlphaMode::Mask;
    case cgltf_alpha_mode_blend: return assets::MaterialAlphaMode::Blend;
    default:                    return assets::MaterialAlphaMode::Opaque;
    }
}

assets::MaterialAsset ImportMaterial(const cgltf_material& mat,
                                     std::string_view      filePath)
{
    assets::MaterialAsset out;
    out.debugName = mat.name ? mat.name : "material";
    out.path = std::string(filePath);
    out.state = assets::AssetState::Loaded;

    const auto& pbr = mat.pbr_metallic_roughness;
    out.baseColorFactor = {
        pbr.base_color_factor[0],
        pbr.base_color_factor[1],
        pbr.base_color_factor[2],
        pbr.base_color_factor[3],
    };
    out.metallicFactor = pbr.metallic_factor;
    out.roughnessFactor = pbr.roughness_factor;
    out.emissiveFactor = {
        mat.emissive_factor[0],
        mat.emissive_factor[1],
        mat.emissive_factor[2],
    };
    out.alphaMode = ImportAlphaMode(mat.alpha_mode);
    out.alphaCutoff = mat.alpha_cutoff;
    out.transparent = out.alphaMode == assets::MaterialAlphaMode::Blend;
    out.doubleSided = mat.double_sided != 0;

    out.baseColorTexture = ImportTextureRef(pbr.base_color_texture, filePath);
    out.metallicRoughnessTexture = ImportTextureRef(pbr.metallic_roughness_texture, filePath);
    out.normalTexture = ImportTextureRef(mat.normal_texture, filePath);
    out.occlusionTexture = ImportTextureRef(mat.occlusion_texture, filePath);
    out.emissiveTexture = ImportTextureRef(mat.emissive_texture, filePath);
    out.normalScale = mat.normal_texture.scale;
    out.occlusionStrength = mat.occlusion_texture.scale;
    return out;
}

assets::SkeletonAsset ImportSkeleton(
    const cgltf_skin& skin,
    std::string_view   filePath,
    std::unordered_map<const cgltf_node*, int32_t>& outJointMap)
{
    assets::SkeletonAsset skel;
    skel.debugName = skin.name ? skin.name : "skeleton";
    skel.path      = std::string(filePath);
    skel.state     = assets::AssetState::Loaded;
    skel.bones.reserve(skin.joints_count);
    outJointMap.clear();
    outJointMap.reserve(skin.joints_count);

    // Node → Joint-Index Lookup fuer parentIndex-Aufloesung
    std::unordered_map<const cgltf_node*, cgltf_size> sourceJointIndex;
    sourceJointIndex.reserve(skin.joints_count);
    for (cgltf_size i = 0; i < skin.joints_count; ++i)
        sourceJointIndex[skin.joints[i]] = i;

    std::vector<uint8_t> visited(skin.joints_count, 0u);

    std::function<void(cgltf_size)> appendJoint = [&](cgltf_size sourceIndex)
    {
        if (sourceIndex >= skin.joints_count || visited[sourceIndex])
            return;

        const cgltf_node* joint = skin.joints[sourceIndex];
        if (joint->parent)
        {
            const auto parentSource = sourceJointIndex.find(joint->parent);
            if (parentSource != sourceJointIndex.end())
                appendJoint(parentSource->second);
        }

        visited[sourceIndex] = 1u;

        assets::Bone bone;
        bone.name = joint->name ? joint->name : "";

        bone.parentIndex = -1;
        if (joint->parent)
        {
            const auto it = outJointMap.find(joint->parent);
            if (it != outJointMap.end())
                bone.parentIndex = it->second;
        }

        bone.inverseBindMatrix = skin.inverse_bind_matrices
            ? ReadMat4(skin.inverse_bind_matrices, sourceIndex)
            : math::Mat4::Identity();

        outJointMap[joint] = static_cast<int32_t>(skel.bones.size());
        skel.bones.push_back(std::move(bone));
    };

    for (cgltf_size i = 0; i < skin.joints_count; ++i)
        appendJoint(i);

    return skel;
}

// ─── Skinned Mesh ────────────────────────────────────────────────────────────

assets::SubMeshData ImportPrimitive(const cgltf_data& data,
                                    const cgltf_primitive& prim)
{
    assets::SubMeshData sub;
    if (prim.material)
        sub.materialIndex = static_cast<uint32_t>(cgltf_material_index(&data, prim.material));

    const cgltf_accessor* posAcc     = nullptr;
    const cgltf_accessor* normAcc    = nullptr;
    const cgltf_accessor* uvAcc      = nullptr;
    const cgltf_accessor* jointsAcc  = nullptr;
    const cgltf_accessor* weightsAcc = nullptr;

    for (cgltf_size ai = 0; ai < prim.attributes_count; ++ai)
    {
        const cgltf_attribute& attr = prim.attributes[ai];
        switch (attr.type)
        {
        case cgltf_attribute_type_position: posAcc    = attr.data; break;
        case cgltf_attribute_type_normal:   normAcc   = attr.data; break;
        case cgltf_attribute_type_texcoord: if (attr.index == 0) uvAcc      = attr.data; break;
        case cgltf_attribute_type_joints:   if (attr.index == 0) jointsAcc  = attr.data; break;
        case cgltf_attribute_type_weights:  if (attr.index == 0) weightsAcc = attr.data; break;
        default: break;
        }
    }

    if (!posAcc) return sub;

    const cgltf_size vc = posAcc->count;

    sub.positions.reserve(vc * 3);
    for (cgltf_size v = 0; v < vc; ++v)
    {
        const math::Vec3 p = ReadVec3(posAcc, v);
        sub.positions.push_back(p.x);
        sub.positions.push_back(p.y);
        sub.positions.push_back(p.z);
    }

    if (normAcc)
    {
        sub.normals.reserve(vc * 3);
        for (cgltf_size v = 0; v < vc; ++v)
        {
            const math::Vec3 n = ReadVec3(normAcc, v);
            sub.normals.push_back(n.x);
            sub.normals.push_back(n.y);
            sub.normals.push_back(n.z);
        }
    }

    if (uvAcc)
    {
        sub.uvs.reserve(vc * 2);
        for (cgltf_size v = 0; v < vc; ++v)
        {
            float uv[2] = {};
            cgltf_accessor_read_float(uvAcc, v, uv, 2);
            sub.uvs.push_back(uv[0]);
            sub.uvs.push_back(uv[1]);
        }
    }

    if (jointsAcc && weightsAcc)
    {
        sub.boneIndices.reserve(vc * 4);
        sub.boneWeights.reserve(vc * 4);
        for (cgltf_size v = 0; v < vc; ++v)
        {
            uint32_t j[4] = {};
            cgltf_accessor_read_uint(jointsAcc, v, j, 4);
            sub.boneIndices.push_back(j[0]);
            sub.boneIndices.push_back(j[1]);
            sub.boneIndices.push_back(j[2]);
            sub.boneIndices.push_back(j[3]);

            float w[4] = {};
            cgltf_accessor_read_float(weightsAcc, v, w, 4);
            const float sum = w[0] + w[1] + w[2] + w[3];
            const float inv = (sum > 1e-6f) ? 1.f / sum : 0.f;
            sub.boneWeights.push_back(w[0] * inv);
            sub.boneWeights.push_back(w[1] * inv);
            sub.boneWeights.push_back(w[2] * inv);
            sub.boneWeights.push_back(w[3] * inv);
        }
    }

    if (prim.indices)
    {
        const cgltf_size ic = prim.indices->count;
        sub.indices.reserve(ic);
        for (cgltf_size i = 0; i < ic; ++i)
        {
            uint32_t idx = 0u;
            cgltf_accessor_read_uint(prim.indices, i, &idx, 1);
            sub.indices.push_back(idx);
        }
    }

    return sub;
}

// ─── Animation ───────────────────────────────────────────────────────────────
// Pro Channel im glTF wird ein eigener assets::AnimationChannel erzeugt.
// Kein Merge: T/R/S bleiben getrennte Channels mit eigenem Zeitgitter.
//
// targetIndex-Strategie:
//   • Node ist Skeleton-Joint (in jointMap): targetIndex = Bone-Index
//     → SampleForSkeleton nutzt diesen direkt für die Bone-Palette.
//   • Sonstige Szenen-Node: targetIndex = globaler glTF-Node-Index
//     → Node-Animations-Pfad nutzt targetName für den Binding-Scan.

assets::AnimationInterpolation ToInterpolation(cgltf_interpolation_type mode) noexcept
{
    switch (mode)
    {
    case cgltf_interpolation_type_step:         return assets::AnimationInterpolation::Step;
    case cgltf_interpolation_type_cubic_spline: return assets::AnimationInterpolation::CubicSpline;
    default:                                    return assets::AnimationInterpolation::Linear;
    }
}

assets::AnimationClip ImportAnimation(
    const cgltf_data&                                     data,
    const cgltf_animation&                                anim,
    const std::unordered_map<const cgltf_node*, int32_t>& jointMap,
    std::string_view                                      filePath)
{
    assets::AnimationClip clip;
    clip.name    = anim.name ? anim.name : "animation";
    clip.path    = std::string(filePath);
    clip.state   = assets::AssetState::Loaded;
    clip.looping = false;
    clip.duration = 0.f;

    for (cgltf_size ci = 0; ci < anim.channels_count; ++ci)
    {
        const cgltf_animation_channel& ch = anim.channels[ci];
        if (!ch.target_node || !ch.sampler) continue;

        const cgltf_accessor* timeAcc = ch.sampler->input;
        const cgltf_accessor* valAcc  = ch.sampler->output;
        if (!timeAcc || !valAcc || timeAcc->count == 0) continue;

        assets::AnimationChannel channel;
        channel.targetName  = ch.target_node->name ? ch.target_node->name : "";
        channel.interpolation = ToInterpolation(ch.sampler->interpolation);

        // Bone-Index wenn Joint, sonst globaler Node-Index für Binding-Cache
        const auto jit = jointMap.find(ch.target_node);
        channel.targetIndex = (jit != jointMap.end())
            ? jit->second
            : static_cast<int32_t>(cgltf_node_index(&data, ch.target_node));

        // Zeitgitter lesen
        const cgltf_size kc = timeAcc->count;
        channel.times.resize(kc);
        for (cgltf_size k = 0; k < kc; ++k)
            channel.times[k] = ReadScalar(timeAcc, k);

        if (!channel.times.empty())
            clip.duration = std::max(clip.duration, channel.times.back());

        // CubicSpline hat 3 Einträge pro Keyframe (inTangent, value, outTangent)
        const bool isCubic    = (channel.interpolation == assets::AnimationInterpolation::CubicSpline);
        const cgltf_size vCnt = isCubic ? kc * 3u : kc;

        switch (ch.target_path)
        {
        case cgltf_animation_path_type_translation:
            channel.property = assets::AnimationTargetProperty::Translation;
            channel.vec3Values.resize(vCnt);
            for (cgltf_size k = 0; k < vCnt; ++k)
                channel.vec3Values[k] = ReadVec3(valAcc, k);
            break;

        case cgltf_animation_path_type_rotation:
            channel.property = assets::AnimationTargetProperty::Rotation;
            channel.quatValues.resize(vCnt);
            for (cgltf_size k = 0; k < vCnt; ++k)
                channel.quatValues[k] = ReadQuat(valAcc, k).Normalized();
            break;

        case cgltf_animation_path_type_scale:
            channel.property = assets::AnimationTargetProperty::Scale;
            channel.vec3Values.resize(vCnt);
            for (cgltf_size k = 0; k < vCnt; ++k)
                channel.vec3Values[k] = ReadVec3(valAcc, k);
            break;

        default:
            continue;  // Morph-Weights u.Ä. ignorieren
        }

        clip.channels.push_back(std::move(channel));
    }

    return clip;
}

} // namespace

// ─── GltfImporter ────────────────────────────────────────────────────────────

bool GltfImporter::CanImport(std::string_view extension) const
{
    return extension == ".gltf" || extension == ".glb";
}

assets::ImportedAssetBundle GltfImporter::Import(std::string_view path)
{
    assets::ImportedAssetBundle bundle;

    const std::string pathStr(path);
    cgltf_options options{};
    cgltf_data*   data = nullptr;

    if (cgltf_parse_file(&options, pathStr.c_str(), &data) != cgltf_result_success)
    {
        bundle.error = "cgltf_parse_file failed: " + pathStr;
        return bundle;
    }

    if (cgltf_load_buffers(&options, data, pathStr.c_str()) != cgltf_result_success)
    {
        cgltf_free(data);
        bundle.error = "cgltf_load_buffers failed: " + pathStr;
        return bundle;
    }

    // ── Skeletons (alle Skins) ──
    // Globale jointMap: Node → Bone-Index; bei mehreren Skins gewinnt der erste Eintrag.
    // Animationen referenzieren Joints aus beliebigen Skins, daher eine gemeinsame Map.
    std::unordered_map<const cgltf_node*, int32_t> jointMap;
    bundle.materials.reserve(data->materials_count);
    for (cgltf_size mi = 0; mi < data->materials_count; ++mi)
        bundle.materials.push_back(ImportMaterial(data->materials[mi], path));

    bundle.skeletons.reserve(data->skins_count);
    for (cgltf_size si = 0; si < data->skins_count; ++si)
    {
        std::unordered_map<const cgltf_node*, int32_t> skinJointMap;
        bundle.skeletons.push_back(ImportSkeleton(data->skins[si], path, skinJointMap));
        for (auto& [node, idx] : skinJointMap)
            jointMap.try_emplace(node, idx);
    }

    // ── Meshes ──
    bundle.nodes.reserve(data->nodes_count);
    for (cgltf_size ni = 0; ni < data->nodes_count; ++ni)
        bundle.nodes.push_back(ImportNode(*data, data->nodes[ni], !bundle.skeletons.empty()));

    for (cgltf_size mi = 0; mi < data->meshes_count; ++mi)
    {
        const cgltf_mesh& mesh = data->meshes[mi];
        assets::MeshAsset meshAsset;
        meshAsset.debugName = mesh.name ? mesh.name : "mesh";
        meshAsset.state     = assets::AssetState::Loaded;
        meshAsset.path      = pathStr;

        for (cgltf_size pi = 0; pi < mesh.primitives_count; ++pi)
            meshAsset.submeshes.push_back(ImportPrimitive(*data, mesh.primitives[pi]));

        // Skin-Zuordnung: Node mit diesem Mesh findet den tatsächlichen Skin-Index.
        int32_t skinIdx = -1;
        if (!bundle.skeletons.empty())
        {
            for (cgltf_size ni = 0; ni < data->nodes_count; ++ni)
            {
                const cgltf_node& node = data->nodes[ni];
                if (node.mesh == &mesh && node.skin)
                {
                    skinIdx = static_cast<int32_t>(node.skin - data->skins);
                    break;
                }
            }
        }

        bundle.meshes.push_back(std::move(meshAsset));
        bundle.meshSkinIndex.push_back(skinIdx);
    }

    // ── Animationen ──
    for (cgltf_size ai = 0; ai < data->animations_count; ++ai)
    {
        bundle.animations.push_back(
            ImportAnimation(*data, data->animations[ai], jointMap, path));
    }

    cgltf_free(data);
    return bundle;
}

} // namespace engine::addons::gltf
