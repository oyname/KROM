#pragma once
#include "assets/AssetRegistry.hpp"
#include <sstream>
#include <map>

namespace engine::assets {
inline const char* MaterialPropertyParamToken(MaterialParam::Type type)
{
    switch(type) {
    case MaterialParam::Type::Float: return "float";
    case MaterialParam::Type::Vec2: return "vec2";
    case MaterialParam::Type::Vec3: return "vec3";
    case MaterialParam::Type::Vec4: return "vec4";
    case MaterialParam::Type::Int: return "int";
    case MaterialParam::Type::Bool: return "bool";
    case MaterialParam::Type::Texture: return "texture";
    }
    return "float";
}
inline std::string SerializeMaterialProperties(const assets::MaterialAsset& asset)
{
    std::ostringstream out;
    out.precision(9);
    const auto alphaModeText = [&]() -> const char* {
        switch (asset.alphaMode)
        {
        case assets::MaterialAlphaMode::Mask:  return "mask";
        case assets::MaterialAlphaMode::Blend: return "blend";
        default:                               return "opaque";
        }
    };

    const bool custom = asset.templateName == "custom";

    out << "template " << asset.templateName << "\n";
    if (custom)
        out << "vertex " << asset.vertexShaderPath << "\n";
    if (custom)
        out << "fragment " << asset.fragmentShaderPath << "\n";
    out << "doubleSided " << (asset.doubleSided ? "true" : "false") << "\n";
    out << "castShadows " << (asset.castShadows ? "true" : "false") << "\n";
    out << "instanced " << (asset.instanced ? "true" : "false") << "\n";
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
    out << "emissiveStrength " << asset.emissiveStrength << "\n";
        out << "uvScale " << asset.uvScale.x << " " << asset.uvScale.y << "\n";
        out << "uvOffset " << asset.uvOffset.x << " " << asset.uvOffset.y << "\n";
        out << "uvProjection " << (asset.boxProjection ? "box" : "mesh") << "\n";
        out << "boxTileSize " << asset.boxTileSize << "\n";

        out << "baseColorTexture " << asset.baseColorTexture.path << "\n";
        out << "metallicRoughnessTexture " << asset.metallicRoughnessTexture.path << "\n";
        out << "normalTexture " << asset.normalTexture.path << "\n";
        out << "occlusionTexture " << asset.occlusionTexture.path << "\n";
        out << "emissiveTexture " << asset.emissiveTexture.path << "\n";

    for (const assets::MaterialParam& param : asset.params)
    {
        if (!custom)
            continue;
        if (param.name.empty())
            continue;

        out << MaterialPropertyParamToken(param.type) << " " << param.name;
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

    asset.graph.Write(out);
    return out.str();
}

inline std::map<std::string,std::string> MaterialPropertyLines(const std::string& source)
{
    std::map<std::string,std::string> result;
    std::istringstream input(source); std::string line;
    while (std::getline(input,line)) {
        std::istringstream tokens(line); std::string key, name; tokens >> key;
        if (key.empty() || key[0]=='#') continue;
        if (key=="float" || key=="vec2" || key=="vec3" || key=="vec4" || key=="int" || key=="bool" || key=="texture" || key=="graphNode") {
            tokens >> name; key += ":" + name;
        }
        result[key]=line;
    }
    return result;
}
} // namespace engine::assets
