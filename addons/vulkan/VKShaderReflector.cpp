#include "VKShaderReflector.hpp"
#include "renderer/ShaderBindingModel.hpp"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace engine::renderer::vulkan {

namespace {

constexpr uint32_t SpvMagicNumber = 0x07230203u;
constexpr uint16_t OpName = 5u;
constexpr uint16_t OpMemberDecorate = 72u;
constexpr uint16_t OpDecorate = 71u;
constexpr uint16_t OpTypeBool = 20u;
constexpr uint16_t OpTypeInt = 21u;
constexpr uint16_t OpTypeFloat = 22u;
constexpr uint16_t OpTypeVector = 23u;
constexpr uint16_t OpTypeImage = 25u;
constexpr uint16_t OpTypeSampler = 26u;
constexpr uint16_t OpTypeSampledImage = 27u;
constexpr uint16_t OpTypeArray = 28u;
constexpr uint16_t OpTypeRuntimeArray = 29u;
constexpr uint16_t OpTypeStruct = 30u;
constexpr uint16_t OpTypePointer = 32u;
constexpr uint16_t OpConstant = 43u;
constexpr uint16_t OpVariable = 59u;

constexpr uint32_t SpvStorageClassUniformConstant = 0u;
constexpr uint32_t SpvStorageClassUniform = 2u;
constexpr uint32_t SpvStorageClassStorageBuffer = 12u;

constexpr uint32_t SpvDecorationBlock = 2u;
constexpr uint32_t SpvDecorationBufferBlock = 3u;
constexpr uint32_t SpvDecorationBinding = 33u;
constexpr uint32_t SpvDecorationDescriptorSet = 34u;
constexpr uint32_t SpvDecorationOffset = 35u;

constexpr uint32_t SpvDim2D = 1u;
constexpr uint32_t SpvDimCube = 3u;

struct SpirvType
{
    enum class Kind : uint8_t
    {
        Unknown = 0,
        Bool,
        Int,
        Float,
        Vector,
        Image,
        Sampler,
        SampledImage,
        Array,
        RuntimeArray,
        Struct,
        Pointer,
    } kind = Kind::Unknown;

    uint32_t width = 0u;
    uint32_t componentTypeId = 0u;
    uint32_t componentCount = 0u;
    uint32_t sampledTypeId = 0u;
    uint32_t imageTypeId = 0u;
    uint32_t pointeeTypeId = 0u;
    uint32_t storageClass = 0u;
    uint32_t arrayLengthId = 0u;
    uint32_t dim = 0u;
    std::vector<uint32_t> memberTypeIds;
};

struct SpirvVariable
{
    uint32_t resultTypeId = 0u;
    uint32_t resultId = 0u;
    uint32_t storageClass = 0u;
};

ShaderStageMask ToStageMask(assets::ShaderStage stage) noexcept
{
    switch (stage)
    {
    case assets::ShaderStage::Vertex: return ShaderStageMask::Vertex;
    case assets::ShaderStage::Fragment: return ShaderStageMask::Fragment;
    case assets::ShaderStage::Compute: return ShaderStageMask::Compute;
    case assets::ShaderStage::Geometry: return ShaderStageMask::Geometry;
    case assets::ShaderStage::Hull: return ShaderStageMask::Hull;
    case assets::ShaderStage::Domain: return ShaderStageMask::Domain;
    default: return ShaderStageMask::None;
    }
}

const assets::CompiledShaderArtifact* FindSpirvArtifact(const assets::ShaderAsset& shader) noexcept
{
    auto it = std::find_if(shader.compiledArtifacts.begin(), shader.compiledArtifacts.end(), [&](const assets::CompiledShaderArtifact& artifact) {
        return artifact.target == assets::ShaderTargetProfile::Vulkan_SPIRV &&
               artifact.stage == shader.stage &&
               !artifact.bytecode.empty();
    });
    return it != shader.compiledArtifacts.end() ? &(*it) : nullptr;
}

const std::vector<uint8_t>* ResolveSpirvBytes(const assets::ShaderAsset& shader) noexcept
{
    if (const auto* artifact = FindSpirvArtifact(shader))
        return &artifact->bytecode;
    if (!shader.bytecode.empty())
        return &shader.bytecode;
    return nullptr;
}

bool AddOrMergeSlot(ShaderParameterLayout& layout, const ParameterSlot& candidate) noexcept
{
    if (!candidate.IsValid())
        return false;

    for (uint32_t i = 0u; i < layout.slotCount; ++i)
    {
        ParameterSlot& existing = layout.slots[i];
        const bool sameName = existing.Name() == candidate.Name() && candidate.Name() != std::string_view{};
        const bool sameBinding = existing.type == candidate.type && existing.binding == candidate.binding && existing.set == candidate.set;
        if (!sameName && !sameBinding)
            continue;

        existing.stageFlags = existing.stageFlags | candidate.stageFlags;
        existing.byteSize = std::max(existing.byteSize, candidate.byteSize);
        existing.elementCount = std::max(existing.elementCount, candidate.elementCount);
        layout.RecalculateHash();
        return true;
    }

    return layout.AddSlot(candidate);
}

bool ReadLiteralString(const uint32_t* words, uint16_t wordCount, uint32_t firstOperandWord, std::string& out)
{
    if (wordCount <= firstOperandWord)
        return false;

    const char* str = reinterpret_cast<const char*>(&words[firstOperandWord]);
    const size_t maxBytes = static_cast<size_t>(wordCount - firstOperandWord) * sizeof(uint32_t);
    size_t len = 0u;
    while (len < maxBytes && str[len] != '\0')
        ++len;
    out.assign(str, len);
    return true;
}

uint32_t NormalizeBinding(ParameterType type, uint32_t binding) noexcept
{
    switch (type)
    {
    case ParameterType::Texture2D:
    case ParameterType::TextureCube:
    case ParameterType::StructuredBuffer:
        return binding >= BindingRegisterRanges::ShaderResourceBase
            ? binding - BindingRegisterRanges::ShaderResourceBase
            : binding;
    case ParameterType::Sampler:
        return binding >= BindingRegisterRanges::SamplerBase
            ? binding - BindingRegisterRanges::SamplerBase
            : binding;
    case ParameterType::ConstantBuffer:
        return binding >= BindingRegisterRanges::ConstantBufferBase
            ? binding - BindingRegisterRanges::ConstantBufferBase
            : binding;
    default:
        return binding;
    }
}

uint32_t ComputeTypeSize(uint32_t typeId,
                         const std::unordered_map<uint32_t, SpirvType>& types,
                         const std::unordered_map<uint32_t, uint32_t>& constants,
                         const std::unordered_map<uint32_t, std::unordered_map<uint32_t, uint32_t>>& memberOffsets);

uint32_t ComputeStructSize(uint32_t structId,
                           const SpirvType& type,
                           const std::unordered_map<uint32_t, SpirvType>& types,
                           const std::unordered_map<uint32_t, uint32_t>& constants,
                           const std::unordered_map<uint32_t, std::unordered_map<uint32_t, uint32_t>>& memberOffsets)
{
    uint32_t size = 0u;
    const auto offsetsIt = memberOffsets.find(structId);
    for (uint32_t i = 0u; i < static_cast<uint32_t>(type.memberTypeIds.size()); ++i)
    {
        const uint32_t memberTypeId = type.memberTypeIds[i];
        const uint32_t memberSize = ComputeTypeSize(memberTypeId, types, constants, memberOffsets);
        uint32_t memberOffset = size;
        if (offsetsIt != memberOffsets.end())
        {
            const auto memberIt = offsetsIt->second.find(i);
            if (memberIt != offsetsIt->second.end())
                memberOffset = memberIt->second;
        }
        size = std::max(size, memberOffset + memberSize);
    }
    return size;
}

uint32_t ComputeTypeSize(uint32_t typeId,
                         const std::unordered_map<uint32_t, SpirvType>& types,
                         const std::unordered_map<uint32_t, uint32_t>& constants,
                         const std::unordered_map<uint32_t, std::unordered_map<uint32_t, uint32_t>>& memberOffsets)
{
    const auto it = types.find(typeId);
    if (it == types.end())
        return 0u;

    const SpirvType& type = it->second;
    switch (type.kind)
    {
    case SpirvType::Kind::Bool:
    case SpirvType::Kind::Int:
    case SpirvType::Kind::Float:
        return std::max(type.width / 8u, 4u);
    case SpirvType::Kind::Vector:
    {
        const uint32_t componentSize = ComputeTypeSize(type.componentTypeId, types, constants, memberOffsets);
        return (type.componentCount == 3u ? 4u : type.componentCount) * componentSize;
    }
    case SpirvType::Kind::Array:
    {
        const uint32_t elemSize = ComputeTypeSize(type.componentTypeId, types, constants, memberOffsets);
        const auto lenIt = constants.find(type.arrayLengthId);
        return elemSize * (lenIt != constants.end() ? lenIt->second : 1u);
    }
    case SpirvType::Kind::Struct:
        return ComputeStructSize(typeId, type, types, constants, memberOffsets);
    default:
        return 0u;
    }
}

ParameterType ResolveDescriptorType(const SpirvVariable& variable,
                                    const std::unordered_map<uint32_t, SpirvType>& types,
                                    const std::unordered_map<uint32_t, bool>& blockTypes) noexcept
{
    const auto pointerIt = types.find(variable.resultTypeId);
    if (pointerIt == types.end() || pointerIt->second.kind != SpirvType::Kind::Pointer)
        return ParameterType::Unknown;

    const SpirvType& pointerType = pointerIt->second;
    const auto pointeeIt = types.find(pointerType.pointeeTypeId);
    if (pointeeIt == types.end())
        return ParameterType::Unknown;

    const SpirvType& pointeeType = pointeeIt->second;
    if (variable.storageClass == SpvStorageClassUniform)
    {
        if (blockTypes.find(pointerType.pointeeTypeId) != blockTypes.end())
            return ParameterType::ConstantBuffer;
    }

    if (variable.storageClass == SpvStorageClassStorageBuffer)
        return ParameterType::StructuredBuffer;

    if (variable.storageClass != SpvStorageClassUniformConstant)
        return ParameterType::Unknown;

    if (pointeeType.kind == SpirvType::Kind::Sampler)
        return ParameterType::Sampler;
    if (pointeeType.kind == SpirvType::Kind::Image)
        return pointeeType.dim == SpvDimCube ? ParameterType::TextureCube : ParameterType::Texture2D;
    if (pointeeType.kind == SpirvType::Kind::SampledImage)
    {
        const auto imageIt = types.find(pointeeType.imageTypeId);
        if (imageIt != types.end() && imageIt->second.dim == SpvDimCube)
            return ParameterType::TextureCube;
        return ParameterType::Texture2D;
    }

    return ParameterType::Unknown;
}

bool ReflectSingle(const assets::ShaderAsset& shader,
                   ShaderParameterLayout& outLayout,
                   std::string* outError)
{
    const std::vector<uint8_t>* bytecode = ResolveSpirvBytes(shader);
    if (!bytecode || bytecode->empty() || (bytecode->size() % sizeof(uint32_t)) != 0u)
    {
        outLayout.Clear();
        if (outError)
            *outError = "shader has no valid SPIR-V bytecode for Vulkan reflection";
        return false;
    }

    const uint32_t* words = reinterpret_cast<const uint32_t*>(bytecode->data());
    const size_t wordCount = bytecode->size() / sizeof(uint32_t);
    if (wordCount < 5u || words[0] != SpvMagicNumber)
    {
        outLayout.Clear();
        if (outError)
            *outError = "invalid SPIR-V header";
        return false;
    }

    std::unordered_map<uint32_t, std::string> names;
    std::unordered_map<uint32_t, uint32_t> bindings;
    std::unordered_map<uint32_t, uint32_t> descriptorSets;
    std::unordered_map<uint32_t, SpirvType> types;
    std::unordered_map<uint32_t, SpirvVariable> variables;
    std::unordered_map<uint32_t, uint32_t> constants;
    std::unordered_map<uint32_t, bool> blockTypes;
    std::unordered_map<uint32_t, std::unordered_map<uint32_t, uint32_t>> memberOffsets;

    for (size_t index = 5u; index < wordCount;)
    {
        const uint32_t firstWord = words[index];
        const uint16_t op = static_cast<uint16_t>(firstWord & 0xFFFFu);
        const uint16_t count = static_cast<uint16_t>(firstWord >> 16u);
        if (count == 0u || index + count > wordCount)
        {
            outLayout.Clear();
            if (outError)
                *outError = "malformed SPIR-V instruction stream";
            return false;
        }

        const uint32_t* inst = &words[index];
        switch (op)
        {
        case OpName:
        {
            std::string name;
            if (ReadLiteralString(inst, count, 2u, name))
                names[inst[1]] = std::move(name);
            break;
        }
        case OpDecorate:
            if (count >= 4u)
            {
                const uint32_t targetId = inst[1];
                const uint32_t decoration = inst[2];
                const uint32_t value = inst[3];
                if (decoration == SpvDecorationBinding)
                    bindings[targetId] = value;
                else if (decoration == SpvDecorationDescriptorSet)
                    descriptorSets[targetId] = value;
                else if (decoration == SpvDecorationBlock || decoration == SpvDecorationBufferBlock)
                    blockTypes[targetId] = true;
            }
            else if (count >= 3u)
            {
                const uint32_t targetId = inst[1];
                const uint32_t decoration = inst[2];
                if (decoration == SpvDecorationBlock || decoration == SpvDecorationBufferBlock)
                    blockTypes[targetId] = true;
            }
            break;
        case OpMemberDecorate:
            if (count >= 5u && inst[3] == SpvDecorationOffset)
                memberOffsets[inst[1]][inst[2]] = inst[4];
            break;
        case OpTypeBool:
            types[inst[1]].kind = SpirvType::Kind::Bool;
            types[inst[1]].width = 32u;
            break;
        case OpTypeInt:
            if (count >= 4u)
            {
                SpirvType type{};
                type.kind = SpirvType::Kind::Int;
                type.width = inst[2];
                types[inst[1]] = type;
            }
            break;
        case OpTypeFloat:
            if (count >= 3u)
            {
                SpirvType type{};
                type.kind = SpirvType::Kind::Float;
                type.width = inst[2];
                types[inst[1]] = type;
            }
            break;
        case OpTypeVector:
            if (count >= 4u)
            {
                SpirvType type{};
                type.kind = SpirvType::Kind::Vector;
                type.componentTypeId = inst[2];
                type.componentCount = inst[3];
                types[inst[1]] = type;
            }
            break;
        case OpTypeImage:
            if (count >= 9u)
            {
                SpirvType type{};
                type.kind = SpirvType::Kind::Image;
                type.sampledTypeId = inst[2];
                type.dim = inst[3];
                types[inst[1]] = type;
            }
            break;
        case OpTypeSampler:
        {
            SpirvType type{};
            type.kind = SpirvType::Kind::Sampler;
            types[inst[1]] = type;
            break;
        }
        case OpTypeSampledImage:
            if (count >= 3u)
            {
                SpirvType type{};
                type.kind = SpirvType::Kind::SampledImage;
                type.imageTypeId = inst[2];
                types[inst[1]] = type;
            }
            break;
        case OpTypeArray:
            if (count >= 4u)
            {
                SpirvType type{};
                type.kind = SpirvType::Kind::Array;
                type.componentTypeId = inst[2];
                type.arrayLengthId = inst[3];
                types[inst[1]] = type;
            }
            break;
        case OpTypeRuntimeArray:
            if (count >= 3u)
            {
                SpirvType type{};
                type.kind = SpirvType::Kind::RuntimeArray;
                type.componentTypeId = inst[2];
                types[inst[1]] = type;
            }
            break;
        case OpTypeStruct:
        {
            SpirvType type{};
            type.kind = SpirvType::Kind::Struct;
            for (uint16_t i = 2u; i < count; ++i)
                type.memberTypeIds.push_back(inst[i]);
            types[inst[1]] = std::move(type);
            break;
        }
        case OpTypePointer:
            if (count >= 4u)
            {
                SpirvType type{};
                type.kind = SpirvType::Kind::Pointer;
                type.storageClass = inst[2];
                type.pointeeTypeId = inst[3];
                types[inst[1]] = type;
            }
            break;
        case OpConstant:
            if (count >= 4u)
                constants[inst[2]] = inst[3];
            break;
        case OpVariable:
            if (count >= 4u)
            {
                SpirvVariable variable{};
                variable.resultTypeId = inst[1];
                variable.resultId = inst[2];
                variable.storageClass = inst[3];
                variables[variable.resultId] = variable;
            }
            break;
        default:
            break;
        }

        index += count;
    }

    ShaderParameterLayout layout{};
    const ShaderStageMask stageMask = ToStageMask(shader.stage);

    for (const auto& [resultId, variable] : variables)
    {
        const ParameterType type = ResolveDescriptorType(variable, types, blockTypes);
        if (type == ParameterType::Unknown)
            continue;

        const auto bindingIt = bindings.find(resultId);
        if (bindingIt == bindings.end())
            continue;

        ParameterSlot slot{};
        const auto nameIt = names.find(resultId);
        slot.SetName(nameIt != names.end() ? nameIt->second : ("binding_" + std::to_string(bindingIt->second)));
        slot.type = type;
        slot.binding = NormalizeBinding(type, bindingIt->second);
        slot.set = descriptorSets.count(resultId) ? descriptorSets[resultId] : 0u;
        slot.stageFlags = stageMask;
        slot.elementCount = 1u;

        if (type == ParameterType::ConstantBuffer)
        {
            const auto pointerIt = types.find(variable.resultTypeId);
            if (pointerIt != types.end())
                slot.byteSize = ComputeTypeSize(pointerIt->second.pointeeTypeId, types, constants, memberOffsets);
        }

        if (!AddOrMergeSlot(layout, slot))
        {
            outLayout.Clear();
            if (outError)
                *outError = "shader parameter layout is full while reflecting SPIR-V";
            return false;
        }
    }

    outLayout = layout;
    if (!outLayout.IsValid())
    {
        if (outError)
            *outError = "Vulkan shader reflection found no descriptor-backed material slots";
        return false;
    }
    return true;
}

} // namespace

bool VKShaderReflector::Reflect(const assets::ShaderAsset& shader,
                                ShaderParameterLayout& outLayout,
                                std::string* outError) const
{
    return ReflectSingle(shader, outLayout, outError);
}

bool VKShaderReflector::ReflectProgram(const assets::ShaderAsset& vertexShader,
                                       const assets::ShaderAsset& fragmentShader,
                                       ShaderParameterLayout& outLayout,
                                       std::string* outError) const
{
    outLayout.Clear();

    ShaderParameterLayout vertexLayout{};
    if (!ReflectSingle(vertexShader, vertexLayout, outError))
        return false;
    for (uint32_t i = 0u; i < vertexLayout.slotCount; ++i)
    {
        if (!AddOrMergeSlot(outLayout, vertexLayout.slots[i]))
        {
            outLayout.Clear();
            if (outError)
                *outError = "failed to merge reflected Vulkan vertex layout";
            return false;
        }
    }

    ShaderParameterLayout fragmentLayout{};
    if (!ReflectSingle(fragmentShader, fragmentLayout, outError))
        return false;
    for (uint32_t i = 0u; i < fragmentLayout.slotCount; ++i)
    {
        if (!AddOrMergeSlot(outLayout, fragmentLayout.slots[i]))
        {
            outLayout.Clear();
            if (outError)
                *outError = "failed to merge reflected Vulkan fragment layout";
            return false;
        }
    }

    outLayout.RecalculateHash();
    return outLayout.IsValid();
}

} // namespace engine::renderer::vulkan
