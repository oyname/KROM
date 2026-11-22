#include "renderer/ShaderConstantBufferReflection.hpp"
#include "ShaderCompilerInternal.hpp"
#include "renderer/ShaderBindingModel.hpp"
#include "renderer/ShaderCompiler.hpp"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <unordered_map>
#include <vector>
#ifdef _WIN32
#   ifndef NOMINMAX
#       define NOMINMAX
#   endif
#   include <windows.h>
#   include <d3dcompiler.h>
#endif

namespace engine::renderer {

namespace {

constexpr uint32_t AlignUp(uint32_t value, uint32_t alignment) noexcept
{
    return (value + alignment - 1u) & ~(alignment - 1u);
}

void SetReflectError(std::string* outError, const std::string& message)
{
    if (outError)
        *outError = message;
}

void FinishBlock(ReflectedConstantBuffer& out, uint32_t declaredSize)
{
    uint32_t end = declaredSize;
    for (const ReflectedCBField& field : out.fields)
        end = std::max(end, field.byteOffset + field.byteSize);
    out.byteSize = AlignUp(end, 16u);
    out.valid = true;
}

// ---------------------------------------------------------------------------
// SPIR-V
// ---------------------------------------------------------------------------
constexpr uint32_t kSpvMagic = 0x07230203u;
constexpr uint16_t kOpName = 5u;
constexpr uint16_t kOpMemberName = 6u;
constexpr uint16_t kOpTypeBool = 20u;
constexpr uint16_t kOpTypeInt = 21u;
constexpr uint16_t kOpTypeFloat = 22u;
constexpr uint16_t kOpTypeVector = 23u;
constexpr uint16_t kOpTypeMatrix = 24u;
constexpr uint16_t kOpTypeImage = 25u;
constexpr uint16_t kOpTypeSampledImage = 27u;
constexpr uint16_t kOpTypeArray = 28u;
constexpr uint16_t kOpTypeStruct = 30u;
constexpr uint16_t kOpTypePointer = 32u;
constexpr uint16_t kOpConstant = 43u;
constexpr uint16_t kOpVariable = 59u;
constexpr uint16_t kOpDecorate = 71u;
constexpr uint16_t kOpMemberDecorate = 72u;
constexpr uint32_t kDecorationArrayStride = 6u;
constexpr uint32_t kDecorationMatrixStride = 7u;
constexpr uint32_t kDecorationBinding = 33u;
constexpr uint32_t kDecorationOffset = 35u;
constexpr uint32_t kStorageClassUniformConstant = 0u;
constexpr uint32_t kStorageClassUniform = 2u;
constexpr uint32_t kImageSampledStorage = 2u; // OpTypeImage "Sampled" = 2: Storage-Image (UAV)
constexpr uint32_t kImageDimBuffer = 5u;

// Logischer Textur-Slot oder UINT32_MAX, wenn ausserhalb von TexSlots.
uint32_t ToTextureSlot(uint32_t logicalSlot) noexcept
{
    return logicalSlot < TexSlots::COUNT ? logicalSlot : UINT32_MAX;
}

struct SpvType
{
    uint16_t op = 0u;
    uint32_t width = 0u;          // Int/Float
    uint32_t componentType = 0u;  // Vector/Matrix/Array
    uint32_t componentCount = 0u; // Vector/Matrix
    uint32_t lengthId = 0u;       // Array
    uint32_t storageClass = 0u;   // Pointer
    uint32_t pointee = 0u;        // Pointer
    std::vector<uint32_t> members; // Struct
};

std::string ReadSpvString(const uint32_t* inst, uint16_t count, uint16_t firstWord)
{
    if (count <= firstWord)
        return {};
    const char* chars = reinterpret_cast<const char*>(inst + firstWord);
    const size_t maxLen = static_cast<size_t>(count - firstWord) * 4u;
    return std::string(chars, strnlen(chars, maxLen));
}

} // namespace

bool ReflectSpirvConstantBuffer(const void* bytecode, size_t byteSize,
                                std::string_view cbufferName,
                                uint32_t logicalSlot,
                                ReflectedConstantBuffer& out,
                                std::string* outError)
{
    out = {};
    if (!bytecode || byteSize < 20u || (byteSize % 4u) != 0u)
    {
        SetReflectError(outError, "no valid SPIR-V bytecode");
        return false;
    }
    const uint32_t* words = static_cast<const uint32_t*>(bytecode);
    const size_t wordCount = byteSize / 4u;
    if (words[0] != kSpvMagic)
    {
        SetReflectError(outError, "invalid SPIR-V header");
        return false;
    }

    std::unordered_map<uint32_t, std::string> names;
    std::unordered_map<uint32_t, std::unordered_map<uint32_t, std::string>> memberNames;
    std::unordered_map<uint32_t, std::unordered_map<uint32_t, uint32_t>> memberOffsets;
    std::unordered_map<uint32_t, std::unordered_map<uint32_t, uint32_t>> memberMatrixStrides;
    std::unordered_map<uint32_t, uint32_t> arrayStrides;
    std::unordered_map<uint32_t, uint32_t> bindings;
    std::unordered_map<uint32_t, SpvType> types;
    std::unordered_map<uint32_t, uint32_t> constants;
    std::vector<std::pair<uint32_t, uint32_t>> uniformVariables; // (variableId, pointerTypeId)

    for (size_t index = 5u; index < wordCount;)
    {
        const uint16_t op = static_cast<uint16_t>(words[index] & 0xFFFFu);
        const uint16_t count = static_cast<uint16_t>(words[index] >> 16u);
        if (count == 0u || index + count > wordCount)
            break;
        const uint32_t* inst = &words[index];
        index += count;

        switch (op)
        {
        case kOpName:
            if (count >= 3u) names[inst[1]] = ReadSpvString(inst, count, 2u);
            break;
        case kOpMemberName:
            if (count >= 4u) memberNames[inst[1]][inst[2]] = ReadSpvString(inst, count, 3u);
            break;
        case kOpDecorate:
            if (count >= 4u && inst[2] == kDecorationBinding) bindings[inst[1]] = inst[3];
            if (count >= 4u && inst[2] == kDecorationArrayStride) arrayStrides[inst[1]] = inst[3];
            break;
        case kOpMemberDecorate:
            if (count >= 5u && inst[3] == kDecorationOffset) memberOffsets[inst[1]][inst[2]] = inst[4];
            if (count >= 5u && inst[3] == kDecorationMatrixStride) memberMatrixStrides[inst[1]][inst[2]] = inst[4];
            break;
        case kOpTypeBool:
            if (count >= 2u) { SpvType t{}; t.op = op; t.width = 32u; types[inst[1]] = t; }
            break;
        case kOpTypeInt:
        case kOpTypeFloat:
            if (count >= 3u) { SpvType t{}; t.op = op; t.width = inst[2]; types[inst[1]] = t; }
            break;
        case kOpTypeVector:
        case kOpTypeMatrix:
            if (count >= 4u) { SpvType t{}; t.op = op; t.componentType = inst[2]; t.componentCount = inst[3]; types[inst[1]] = t; }
            break;
        case kOpTypeArray:
            if (count >= 4u) { SpvType t{}; t.op = op; t.componentType = inst[2]; t.lengthId = inst[3]; types[inst[1]] = t; }
            break;
        case kOpTypeStruct:
        {
            SpvType t{};
            t.op = op;
            for (uint16_t m = 2u; m < count; ++m)
                t.members.push_back(inst[m]);
            types[inst[1]] = std::move(t);
            break;
        }
        case kOpTypePointer:
            if (count >= 4u) { SpvType t{}; t.op = op; t.storageClass = inst[2]; t.pointee = inst[3]; types[inst[1]] = t; }
            break;
        case kOpConstant:
            if (count >= 4u) constants[inst[2]] = inst[3];
            break;
        case kOpVariable:
            if (count >= 4u && inst[3] == kStorageClassUniform)
                uniformVariables.emplace_back(inst[2], inst[1]);
            break;
        default:
            break;
        }
    }

    const std::string typeName = "type." + std::string(cbufferName);
    uint32_t nameMatch = 0u;
    uint32_t bindingMatch = 0u;
    for (const auto& [variableId, pointerTypeId] : uniformVariables)
    {
        const auto pointerIt = types.find(pointerTypeId);
        if (pointerIt == types.end() || pointerIt->second.op != kOpTypePointer)
            continue;
        const uint32_t structId = pointerIt->second.pointee;
        const auto structIt = types.find(structId);
        if (structIt == types.end() || structIt->second.op != kOpTypeStruct)
            continue;

        const auto structName = names.find(structId);
        const auto variableName = names.find(variableId);
        if ((structName != names.end() && (structName->second == cbufferName || structName->second == typeName)) ||
            (variableName != names.end() && variableName->second == cbufferName))
        {
            nameMatch = structId;
            break;
        }
        const auto bindingIt = bindings.find(variableId);
        if (bindingIt != bindings.end() && bindingIt->second == BindingRegisterRanges::CB(logicalSlot))
            bindingMatch = structId;
    }

    const uint32_t structId = nameMatch != 0u ? nameMatch : bindingMatch;
    if (structId == 0u)
    {
        SetReflectError(outError, "SPIR-V module has no uniform block '" + std::string(cbufferName) + "'");
        return false;
    }

    // Groesse eines Members; Arrays und Matrizen ueber ihre Strides.
    const auto scalarSize = [&](uint32_t typeId) -> uint32_t {
        const auto it = types.find(typeId);
        return (it != types.end() && it->second.width != 0u) ? it->second.width / 8u : 4u;
    };
    const SpvType& structType = types.at(structId);
    for (uint32_t member = 0u; member < static_cast<uint32_t>(structType.members.size()); ++member)
    {
        const auto nameIt = memberNames[structId].find(member);
        const auto offsetIt = memberOffsets[structId].find(member);
        if (nameIt == memberNames[structId].end() || nameIt->second.empty() || offsetIt == memberOffsets[structId].end())
            continue;
        const auto typeIt = types.find(structType.members[member]);
        if (typeIt == types.end())
            continue;

        const SpvType& type = typeIt->second;
        uint32_t size = 0u;
        switch (type.op)
        {
        case kOpTypeBool:
        case kOpTypeInt:
        case kOpTypeFloat:
            size = scalarSize(structType.members[member]);
            break;
        case kOpTypeVector:
            size = type.componentCount * scalarSize(type.componentType);
            break;
        case kOpTypeMatrix:
        {
            const auto strideIt = memberMatrixStrides[structId].find(member);
            const uint32_t stride = strideIt != memberMatrixStrides[structId].end() ? strideIt->second : 16u;
            size = type.componentCount * stride;
            break;
        }
        case kOpTypeArray:
        {
            const auto lengthIt = constants.find(type.lengthId);
            const auto strideIt = arrayStrides.find(structType.members[member]);
            if (lengthIt != constants.end() && strideIt != arrayStrides.end())
                size = lengthIt->second * strideIt->second;
            break;
        }
        default:
            break;
        }
        if (size == 0u)
            continue;
        out.fields.push_back({ nameIt->second, offsetIt->second, size });
    }

    FinishBlock(out, 0u);
    return true;
}

bool ReflectSpirvTextureBindings(const void* bytecode, size_t byteSize,
                                 ReflectedTextureBindings& out,
                                 std::string* outError)
{
    out.clear();
    if (!bytecode || byteSize < 20u || (byteSize % 4u) != 0u)
    {
        SetReflectError(outError, "no valid SPIR-V bytecode");
        return false;
    }
    const uint32_t* words = static_cast<const uint32_t*>(bytecode);
    const size_t wordCount = byteSize / 4u;
    if (words[0] != kSpvMagic)
    {
        SetReflectError(outError, "invalid SPIR-V header");
        return false;
    }

    std::unordered_map<uint32_t, std::string> names;
    std::unordered_map<uint32_t, uint32_t> bindings;
    std::unordered_map<uint32_t, SpvType> types;
    std::unordered_map<uint32_t, bool> images; // Image-Typ -> sampelbare Textur
    std::vector<std::pair<uint32_t, uint32_t>> resourceVariables; // (variableId, pointerTypeId)

    for (size_t index = 5u; index < wordCount;)
    {
        const uint16_t op = static_cast<uint16_t>(words[index] & 0xFFFFu);
        const uint16_t count = static_cast<uint16_t>(words[index] >> 16u);
        if (count == 0u || index + count > wordCount)
            break;
        const uint32_t* inst = &words[index];
        index += count;

        switch (op)
        {
        case kOpName:
            if (count >= 3u) names[inst[1]] = ReadSpvString(inst, count, 2u);
            break;
        case kOpDecorate:
            if (count >= 4u && inst[2] == kDecorationBinding) bindings[inst[1]] = inst[3];
            break;
        case kOpTypeImage:
            // inst[3] = Dim, inst[7] = Sampled
            if (count >= 9u) images[inst[1]] = inst[3] != kImageDimBuffer && inst[7] != kImageSampledStorage;
            break;
        case kOpTypeSampledImage:
            if (count >= 3u) { SpvType t{}; t.op = op; t.componentType = inst[2]; types[inst[1]] = t; }
            break;
        case kOpTypeArray:
            if (count >= 4u) { SpvType t{}; t.op = op; t.componentType = inst[2]; types[inst[1]] = t; }
            break;
        case kOpTypePointer:
            if (count >= 4u) { SpvType t{}; t.op = op; t.storageClass = inst[2]; t.pointee = inst[3]; types[inst[1]] = t; }
            break;
        case kOpVariable:
            if (count >= 4u && inst[3] == kStorageClassUniformConstant)
                resourceVariables.emplace_back(inst[2], inst[1]);
            break;
        default:
            break;
        }
    }

    // Separate Textur (DXC: Texture2D) oder Combined Sampler (GLSL: sampler2D),
    // auch als Array.
    const auto isTexture = [&](uint32_t typeId) {
        for (int depth = 0; depth < 3; ++depth)
        {
            if (const auto image = images.find(typeId); image != images.end())
                return image->second;
            const auto type = types.find(typeId);
            if (type == types.end() || (type->second.op != kOpTypeSampledImage && type->second.op != kOpTypeArray))
                return false;
            typeId = type->second.componentType;
        }
        return false;
    };

    for (const auto& [variableId, pointerTypeId] : resourceVariables)
    {
        const auto pointer = types.find(pointerTypeId);
        const auto name = names.find(variableId);
        const auto binding = bindings.find(variableId);
        if (pointer == types.end() || pointer->second.op != kOpTypePointer ||
            name == names.end() || name->second.empty() || binding == bindings.end() ||
            !isTexture(pointer->second.pointee))
            continue;
        // DXC verschiebt tN per -fvk-t-shift auf ShaderResourceBase + N. GLSL-
        // Quellen fuer Vulkan tragen das logische Binding direkt (wie VKShaderReflector).
        const uint32_t raw = binding->second;
        const uint32_t slot = ToTextureSlot(raw >= BindingRegisterRanges::ShaderResourceBase
                                                ? raw - BindingRegisterRanges::ShaderResourceBase
                                                : raw);
        if (slot != UINT32_MAX)
            out.emplace(name->second, slot);
    }
    return true;
}

// ---------------------------------------------------------------------------
// DXBC
// ---------------------------------------------------------------------------
#ifdef _WIN32
namespace {

ID3D11ShaderReflection* CreateDxbcReflection(const void* bytecode, size_t byteSize, std::string* outError)
{
    if (!bytecode || byteSize == 0u)
    {
        SetReflectError(outError, "no DXBC bytecode");
        return nullptr;
    }

    using D3DReflectFn = HRESULT (WINAPI*)(LPCVOID, SIZE_T, REFIID, void**);
    const auto reflect = reinterpret_cast<D3DReflectFn>(internal::ResolveD3DReflectProc(outError));
    if (!reflect)
        return nullptr;

    // IID_ID3D11ShaderReflection (d3dcompiler_47); hier definiert, damit der
    // Core nicht gegen dxguid.lib linken muss.
    static constexpr GUID kIIDShaderReflection =
        { 0x8d536ca1, 0x0cca, 0x4956, { 0xa8, 0x37, 0x78, 0x69, 0x63, 0x75, 0x55, 0x84 } };

    ID3D11ShaderReflection* reflection = nullptr;
    if (FAILED(reflect(bytecode, byteSize, kIIDShaderReflection, reinterpret_cast<void**>(&reflection))) || !reflection)
    {
        SetReflectError(outError, "D3DReflect failed");
        return nullptr;
    }
    return reflection;
}

} // namespace
#endif

bool ReflectDxbcConstantBuffer(const void* bytecode, size_t byteSize,
                               std::string_view cbufferName,
                               ReflectedConstantBuffer& out,
                               std::string* outError)
{
    out = {};
#ifndef _WIN32
    (void)bytecode; (void)byteSize; (void)cbufferName;
    SetReflectError(outError, "DXBC reflection is only available on Windows");
    return false;
#else
    ID3D11ShaderReflection* reflection = CreateDxbcReflection(bytecode, byteSize, outError);
    if (!reflection)
        return false;

    const std::string name(cbufferName);
    ID3D11ShaderReflectionConstantBuffer* cb = reflection->GetConstantBufferByName(name.c_str());
    D3D11_SHADER_BUFFER_DESC cbDesc{};
    if (!cb || FAILED(cb->GetDesc(&cbDesc)))
    {
        reflection->Release();
        SetReflectError(outError, "DXBC has no cbuffer '" + name + "'");
        return false;
    }

    for (UINT v = 0u; v < cbDesc.Variables; ++v)
    {
        ID3D11ShaderReflectionVariable* variable = cb->GetVariableByIndex(v);
        D3D11_SHADER_VARIABLE_DESC variableDesc{};
        if (!variable || FAILED(variable->GetDesc(&variableDesc)) || !variableDesc.Name)
            continue;
        out.fields.push_back({ variableDesc.Name, variableDesc.StartOffset, variableDesc.Size });
    }
    reflection->Release();

    FinishBlock(out, cbDesc.Size);
    return true;
#endif
}

bool ReflectDxbcTextureBindings(const void* bytecode, size_t byteSize,
                                ReflectedTextureBindings& out,
                                std::string* outError)
{
    out.clear();
#ifndef _WIN32
    (void)bytecode; (void)byteSize;
    SetReflectError(outError, "DXBC reflection is only available on Windows");
    return false;
#else
    ID3D11ShaderReflection* reflection = CreateDxbcReflection(bytecode, byteSize, outError);
    if (!reflection)
        return false;

    D3D11_SHADER_DESC shaderDesc{};
    if (FAILED(reflection->GetDesc(&shaderDesc)))
    {
        reflection->Release();
        SetReflectError(outError, "D3DReflect GetDesc failed");
        return false;
    }
    for (UINT r = 0u; r < shaderDesc.BoundResources; ++r)
    {
        D3D11_SHADER_INPUT_BIND_DESC bindDesc{};
        if (FAILED(reflection->GetResourceBindingDesc(r, &bindDesc)) || !bindDesc.Name ||
            bindDesc.Type != D3D_SIT_TEXTURE || bindDesc.Dimension == D3D_SRV_DIMENSION_BUFFER)
            continue;
        if (const uint32_t slot = ToTextureSlot(bindDesc.BindPoint); slot != UINT32_MAX)
            out.emplace(bindDesc.Name, slot);
    }
    reflection->Release();
    return true;
#endif
}

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------
bool ReflectConstantBuffer(const assets::CompiledShaderArtifact& artifact,
                           std::string_view cbufferName,
                           uint32_t logicalSlot,
                           ReflectedConstantBuffer& out,
                           std::string* outError)
{
    switch (artifact.target)
    {
    case assets::ShaderTargetProfile::DirectX11_SM5:
        return ReflectDxbcConstantBuffer(artifact.bytecode.data(), artifact.bytecode.size(), cbufferName, out, outError);
    case assets::ShaderTargetProfile::Vulkan_SPIRV:
        return ReflectSpirvConstantBuffer(artifact.bytecode.data(), artifact.bytecode.size(), cbufferName, logicalSlot, out, outError);
    default:
        out = {};
        SetReflectError(outError, std::string("no constant buffer reflection for target ") + ShaderCompiler::ToString(artifact.target));
        return false;
    }
}

bool ReflectTextureBindings(const assets::CompiledShaderArtifact& artifact,
                            ReflectedTextureBindings& out,
                            std::string* outError)
{
    switch (artifact.target)
    {
    case assets::ShaderTargetProfile::DirectX11_SM5:
        return ReflectDxbcTextureBindings(artifact.bytecode.data(), artifact.bytecode.size(), out, outError);
    case assets::ShaderTargetProfile::Vulkan_SPIRV:
        return ReflectSpirvTextureBindings(artifact.bytecode.data(), artifact.bytecode.size(), out, outError);
    default:
        out.clear();
        SetReflectError(outError, std::string("no texture binding reflection for target ") + ShaderCompiler::ToString(artifact.target));
        return false;
    }
}

namespace {

// Vorhandenes Artefakt fuer target oder temporaer kompiliert (in storage).
const assets::CompiledShaderArtifact* FindOrCompileArtifact(const assets::ShaderAsset& shader,
                                                           assets::ShaderTargetProfile target,
                                                           assets::CompiledShaderArtifact& storage,
                                                           std::string& error)
{
    const auto existing = std::find_if(shader.compiledArtifacts.begin(), shader.compiledArtifacts.end(),
        [&](const assets::CompiledShaderArtifact& artifact) {
            return artifact.target == target && artifact.stage == shader.stage && artifact.IsValid();
        });
    if (existing != shader.compiledArtifacts.end())
        return &(*existing);
    if (ShaderCompiler::CompileForTarget(shader, target, storage, &error))
        return &storage;
    return nullptr;
}

} // namespace

bool ReflectProgramTextureBindings(const assets::ShaderAsset& vertexShader,
                                   const assets::ShaderAsset& fragmentShader,
                                   assets::ShaderTargetProfile target,
                                   ReflectedTextureBindings& out,
                                   std::string* outError)
{
    out.clear();
    bool reflected = false;
    std::string lastError;
    for (const assets::ShaderAsset* shader : { &vertexShader, &fragmentShader })
    {
        assets::CompiledShaderArtifact compiled{};
        const assets::CompiledShaderArtifact* artifact = FindOrCompileArtifact(*shader, target, compiled, lastError);
        ReflectedTextureBindings stage;
        if (!artifact || !ReflectTextureBindings(*artifact, stage, &lastError))
            continue;
        out.insert(stage.begin(), stage.end());
        reflected = true;
    }

    if (!reflected)
        SetReflectError(outError, lastError.empty() ? "no shader stage could be reflected" : lastError);
    return reflected;
}

bool ReflectProgramConstantBuffer(const assets::ShaderAsset& vertexShader,
                                  const assets::ShaderAsset& fragmentShader,
                                  assets::ShaderTargetProfile target,
                                  std::string_view cbufferName,
                                  uint32_t logicalSlot,
                                  ReflectedConstantBuffer& out,
                                  std::string* outError)
{
    out = {};
    std::string lastError;
    for (const assets::ShaderAsset* shader : { &vertexShader, &fragmentShader })
    {
        assets::CompiledShaderArtifact compiled{};
        const assets::CompiledShaderArtifact* artifact = FindOrCompileArtifact(*shader, target, compiled, lastError);
        if (!artifact)
            continue;

        ReflectedConstantBuffer stage{};
        if (!ReflectConstantBuffer(*artifact, cbufferName, logicalSlot, stage, &lastError))
            continue;

        // Beide Stufen deklarieren denselben Block; Felder vereinigen.
        for (const ReflectedCBField& field : stage.fields)
            if (!out.Find(field.name))
                out.fields.push_back(field);
        out.byteSize = std::max(out.byteSize, stage.byteSize);
        out.valid = true;
    }

    if (!out.valid)
        SetReflectError(outError, lastError.empty() ? "constant buffer not found" : lastError);
    return out.valid;
}

} // namespace engine::renderer
