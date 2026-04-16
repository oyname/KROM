#include "renderer/MaterialCBLayout.hpp"
#include <algorithm>
#include <cstring>

namespace engine::renderer {

namespace {

MaterialParam::Type ToMaterialParamType(ParameterType type) noexcept
{
    switch (type)
    {
    case ParameterType::Float: return MaterialParam::Type::Float;
    case ParameterType::Vec2: return MaterialParam::Type::Vec2;
    case ParameterType::Vec3: return MaterialParam::Type::Vec3;
    case ParameterType::Vec4: return MaterialParam::Type::Vec4;
    case ParameterType::Int: return MaterialParam::Type::Int;
    case ParameterType::Bool: return MaterialParam::Type::Bool;
    case ParameterType::Texture2D:
    case ParameterType::TextureCube: return MaterialParam::Type::Texture;
    case ParameterType::Sampler: return MaterialParam::Type::Sampler;
    case ParameterType::StructuredBuffer: return MaterialParam::Type::Buffer;
    case ParameterType::ConstantBuffer:
    case ParameterType::Unknown:
    default: return MaterialParam::Type::Float;
    }
}

constexpr uint32_t Align16(uint32_t value) noexcept
{
    return (value + 15u) & ~15u;
}

constexpr uint32_t PackedByteSize(ParameterType type) noexcept
{
    switch (type)
    {
    case ParameterType::Float: return 4u;
    case ParameterType::Vec2: return 8u;
    case ParameterType::Vec3: return 12u;
    case ParameterType::Vec4: return 16u;
    case ParameterType::Int: return 4u;
    case ParameterType::Bool: return 4u;
    default: return 0u;
    }
}

bool IsResourceParameter(ParameterType type) noexcept
{
    switch (type)
    {
    case ParameterType::Texture2D:
    case ParameterType::TextureCube:
    case ParameterType::Sampler:
    case ParameterType::StructuredBuffer:
        return true;
    default:
        return false;
    }
}

} // namespace

CbLayout MaterialCBLayout::Build(const ShaderParameterLayout& layout) noexcept
{
    CbLayout result{};
    uint32_t cbOffset = 0u;

    for (uint32_t i = 0u; i < layout.slotCount; ++i)
    {
        const ParameterSlot& slot = layout.slots[i];
        if (IsResourceParameter(slot.type))
            continue;

        const uint32_t packedSize = PackedByteSize(slot.type);
        if (packedSize == 0u)
            continue;

        const uint32_t rowOffset = cbOffset & 15u;
        const bool startsNewRow = packedSize == 16u || (rowOffset + packedSize) > 16u;
        if (startsNewRow)
            cbOffset = Align16(cbOffset);

        CbFieldDesc field{};
        field.name = std::string(slot.Name());
        field.offset = cbOffset;
        field.size = slot.byteSize != 0u ? slot.byteSize : packedSize;
        field.arrayCount = slot.elementCount == 0u ? 1u : slot.elementCount;
        field.type = ToMaterialParamType(slot.type);
        result.fields.push_back(std::move(field));

        cbOffset += packedSize;
    }

    result.totalSize = Align16(cbOffset);
    return result;
}

void MaterialCBLayout::BuildCBData(MaterialInstance& inst)
{
    inst.cbLayout = Build(inst.layout);
    inst.cbData = inst.parameters.ConstantData();
    if (inst.cbData.size() < inst.cbLayout.totalSize)
        inst.cbData.resize(inst.cbLayout.totalSize, 0u);
}

} // namespace engine::renderer
