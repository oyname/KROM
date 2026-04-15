#include "renderer/MaterialCBLayout.hpp"
#include "renderer/MaterialFeatureEval.hpp"
#include <algorithm>
#include <cstring>

namespace engine::renderer {

namespace {

constexpr size_t SemanticIndex(MaterialSemantic semantic) noexcept
{
    return static_cast<size_t>(semantic);
}

MaterialParam MakeFloatParam(const char* name, float v)
{
    MaterialParam p{};
    p.name = name;
    p.type = MaterialParam::Type::Float;
    p.value.f[0] = v;
    return p;
}

MaterialParam MakeVec4Param(const char* name, float x, float y, float z, float w)
{
    MaterialParam p{};
    p.name = name;
    p.type = MaterialParam::Type::Vec4;
    p.value.f[0] = x;
    p.value.f[1] = y;
    p.value.f[2] = z;
    p.value.f[3] = w;
    return p;
}

MaterialParam MakeIntParam(const char* name, int32_t v)
{
    MaterialParam p{};
    p.name = name;
    p.type = MaterialParam::Type::Int;
    p.value.i = v;
    return p;
}

std::vector<MaterialParam> BuildCanonicalParams(const MaterialDesc& desc, const MaterialInstance& inst)
{
    if (!desc.params.empty())
        return inst.instanceParams;

    std::vector<MaterialParam> params;

    const auto appendOrReplace = [&](const MaterialParam& param)
    {
        auto it = std::find_if(params.begin(), params.end(), [&](const MaterialParam& existing) {
            return existing.name == param.name;
        });
        if (it != params.end())
            *it = param;
        else
            params.push_back(param);
    };

    const auto baseColor = MaterialFeatureEval::ResolveSemanticValue(desc, inst, MaterialSemantic::BaseColor);
    const auto emissive = MaterialFeatureEval::ResolveSemanticValue(desc, inst, MaterialSemantic::Emissive);
    const auto metallic = MaterialFeatureEval::ResolveSemanticValue(desc, inst, MaterialSemantic::Metallic);
    const auto roughness = MaterialFeatureEval::ResolveSemanticValue(desc, inst, MaterialSemantic::Roughness);
    const auto occlusion = MaterialFeatureEval::ResolveSemanticValue(desc, inst, MaterialSemantic::Occlusion);
    const auto opacity = MaterialFeatureEval::ResolveSemanticValue(desc, inst, MaterialSemantic::Opacity);
    const auto alphaCutoff = MaterialFeatureEval::ResolveSemanticValue(desc, inst, MaterialSemantic::AlphaCutoff);
    appendOrReplace(MakeVec4Param("baseColorFactor", baseColor.data[0], baseColor.data[1], baseColor.data[2], baseColor.data[3]));
    appendOrReplace(MakeVec4Param("emissiveFactor", emissive.data[0], emissive.data[1], emissive.data[2], emissive.data[3]));
    appendOrReplace(MakeFloatParam("metallicFactor", metallic.data[0]));
    appendOrReplace(MakeFloatParam("roughnessFactor", roughness.data[0]));
    appendOrReplace(MakeFloatParam("occlusionStrength", occlusion.data[0]));
    appendOrReplace(MakeFloatParam("opacityFactor", opacity.data[0]));
    appendOrReplace(MakeFloatParam("alphaCutoff", alphaCutoff.data[0]));
    appendOrReplace(MakeIntParam("materialFeatureMask", static_cast<int32_t>(inst.featureMask)));
    appendOrReplace(MakeFloatParam("materialModel", desc.model == MaterialModel::Unlit ? 1.f : 0.f));

    return params;
}

} // namespace

CbLayout MaterialCBLayout::Build(const std::vector<MaterialParam>& params) noexcept
{
    CbLayout layout;
    uint32_t offset = 0u;

    for (const auto& p : params)
    {
        if (p.type == MaterialParam::Type::Texture || p.type == MaterialParam::Type::Sampler)
            continue;

        CbFieldDesc field{};
        field.name = p.name;
        field.offset = offset;
        field.arrayCount = 1u;
        field.type = p.type;

        uint32_t fieldSize = 16u;
        switch (p.type)
        {
        case MaterialParam::Type::Float: fieldSize = 4u; break;
        case MaterialParam::Type::Vec2:  fieldSize = 8u; break;
        case MaterialParam::Type::Vec3:  fieldSize = 16u; break;
        case MaterialParam::Type::Vec4:  fieldSize = 16u; break;
        case MaterialParam::Type::Int:   fieldSize = 4u; break;
        case MaterialParam::Type::Bool:  fieldSize = 4u; break;
        default: break;
        }
        field.size = fieldSize;

        const uint32_t boundaryOffset = (offset / 16u + 1u) * 16u;
        if (fieldSize > 4u && (offset % 16u) + fieldSize > 16u)
        {
            offset = boundaryOffset;
            field.offset = offset;
        }

        layout.fields.push_back(field);
        offset += fieldSize;
    }

    layout.totalSize = (offset + 15u) & ~15u;
    if (layout.totalSize == 0u)
        layout.totalSize = 16u;
    return layout;
}

void MaterialCBLayout::BuildCBData(MaterialInstance& inst, const MaterialDesc& desc)
{
    const std::vector<MaterialParam> params = BuildCanonicalParams(desc, inst);
    const CbLayout& layout = inst.cbLayout;
    inst.cbData.assign(layout.totalSize, 0u);

    for (const auto& p : params)
    {
        if (p.type == MaterialParam::Type::Texture || p.type == MaterialParam::Type::Sampler)
            continue;

        const uint32_t offset = layout.GetOffset(p.name);
        if (offset == UINT32_MAX)
            continue;

        float* dst = reinterpret_cast<float*>(inst.cbData.data() + offset);
        switch (p.type)
        {
        case MaterialParam::Type::Float: dst[0] = p.value.f[0]; break;
        case MaterialParam::Type::Vec2:  dst[0] = p.value.f[0]; dst[1] = p.value.f[1]; break;
        case MaterialParam::Type::Vec3:  dst[0] = p.value.f[0]; dst[1] = p.value.f[1]; dst[2] = p.value.f[2]; break;
        case MaterialParam::Type::Vec4:  dst[0] = p.value.f[0]; dst[1] = p.value.f[1]; dst[2] = p.value.f[2]; dst[3] = p.value.f[3]; break;
        case MaterialParam::Type::Int:   std::memcpy(dst, &p.value.i, 4u); break;
        case MaterialParam::Type::Bool:  { const uint32_t b = p.value.b ? 1u : 0u; std::memcpy(dst, &b, 4u); break; }
        default: break;
        }
    }
}

} // namespace engine::renderer
