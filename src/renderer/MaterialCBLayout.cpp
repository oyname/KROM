#include "renderer/MaterialCBLayout.hpp"
#include <algorithm>
#include <cstring>

namespace engine::renderer {

namespace {

MaterialParam::Type ToMaterialParamType(MaterialParameterType type) noexcept
{
    switch (type)
    {
    case MaterialParameterType::Float: return MaterialParam::Type::Float;
    case MaterialParameterType::Vec2: return MaterialParam::Type::Vec2;
    case MaterialParameterType::Vec3: return MaterialParam::Type::Vec3;
    case MaterialParameterType::Vec4: return MaterialParam::Type::Vec4;
    case MaterialParameterType::Int: return MaterialParam::Type::Int;
    case MaterialParameterType::Bool: return MaterialParam::Type::Bool;
    case MaterialParameterType::Texture2D:
    case MaterialParameterType::TextureCube: return MaterialParam::Type::Texture;
    case MaterialParameterType::Sampler: return MaterialParam::Type::Sampler;
    case MaterialParameterType::StructuredBuffer: return MaterialParam::Type::Buffer;
    case MaterialParameterType::ConstantBuffer:
    case MaterialParameterType::Unknown:
    default: return MaterialParam::Type::Float;
    }
}

constexpr uint32_t Align16(uint32_t value) noexcept
{
    return (value + 15u) & ~15u;
}

constexpr uint32_t PackedByteSize(MaterialParameterType type) noexcept
{
    switch (type)
    {
    case MaterialParameterType::Float: return 4u;
    case MaterialParameterType::Vec2: return 8u;
    case MaterialParameterType::Vec3: return 12u;
    case MaterialParameterType::Vec4: return 16u;
    case MaterialParameterType::Int: return 4u;
    case MaterialParameterType::Bool: return 4u;
    default: return 0u;
    }
}

bool IsResourceParameter(MaterialParameterType type) noexcept
{
    switch (type)
    {
    case MaterialParameterType::Texture2D:
    case MaterialParameterType::TextureCube:
    case MaterialParameterType::Sampler:
    case MaterialParameterType::StructuredBuffer:
        return true;
    default:
        return false;
    }
}

} // namespace

CbLayout MaterialCBLayout::Build(const MaterialParameterLayout& layout) noexcept
{
    CbLayout result{};
    uint32_t cbOffset = 0u;

    for (uint32_t i = 0u; i < layout.slotCount; ++i)
    {
        const MaterialParameterSlot& slot = layout.slots[i];
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

void MaterialCBLayout::BuildCBData(const MaterialParameterLayout& layout,
                                   const ParameterBlob& parameters,
                                   CbLayout& outLayout,
                                   std::vector<uint8_t>& outData)
{
    outLayout = Build(layout);
    outData = parameters.ConstantData();
    if (outData.size() < outLayout.totalSize)
        outData.resize(outLayout.totalSize, 0u);
}

} // namespace engine::renderer
