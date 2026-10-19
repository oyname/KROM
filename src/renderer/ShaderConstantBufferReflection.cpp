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
constexpr uint32_t kStorageClassUniform = 2u;

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

// ---------------------------------------------------------------------------
// GLSL std140
// ---------------------------------------------------------------------------
namespace {

std::string StripGlslComments(std::string_view source)
{
    std::string result;
    result.reserve(source.size());
    for (size_t i = 0u; i < source.size();)
    {
        if (source.compare(i, 2u, "//") == 0)
        {
            while (i < source.size() && source[i] != '\n') ++i;
        }
        else if (source.compare(i, 2u, "/*") == 0)
        {
            const size_t end = source.find("*/", i + 2u);
            i = end == std::string_view::npos ? source.size() : end + 2u;
            result.push_back(' ');
        }
        else
        {
            result.push_back(source[i++]);
        }
    }
    return result;
}

bool IsIdentChar(char c) noexcept
{
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

std::vector<std::string> TokenizeDeclaration(std::string_view text)
{
    std::vector<std::string> tokens;
    for (size_t i = 0u; i < text.size();)
    {
        const char c = text[i];
        if (std::isspace(static_cast<unsigned char>(c)))
        {
            ++i;
        }
        else if (IsIdentChar(c))
        {
            size_t end = i;
            while (end < text.size() && IsIdentChar(text[end])) ++end;
            tokens.emplace_back(text.substr(i, end - i));
            i = end;
        }
        else
        {
            tokens.emplace_back(1u, c);
            ++i;
        }
    }
    return tokens;
}

struct Std140Type
{
    uint32_t size = 0u;
    uint32_t alignment = 0u;
};

bool ResolveStd140Type(const std::string& name, Std140Type& out) noexcept
{
    static const std::unordered_map<std::string, Std140Type> kTypes = {
        { "float", {4u, 4u} },  { "int", {4u, 4u} },   { "uint", {4u, 4u} },   { "bool", {4u, 4u} },
        { "vec2", {8u, 8u} },   { "ivec2", {8u, 8u} }, { "uvec2", {8u, 8u} },  { "bvec2", {8u, 8u} },
        { "vec3", {12u, 16u} }, { "ivec3", {12u, 16u} }, { "uvec3", {12u, 16u} }, { "bvec3", {12u, 16u} },
        { "vec4", {16u, 16u} }, { "ivec4", {16u, 16u} }, { "uvec4", {16u, 16u} }, { "bvec4", {16u, 16u} },
        // Matrizen: Spalten als vec4-Array (std140), spaltenweise.
        { "mat2", {32u, 16u} }, { "mat3", {48u, 16u} }, { "mat4", {64u, 16u} },
        { "mat2x2", {32u, 16u} }, { "mat3x3", {48u, 16u} }, { "mat4x4", {64u, 16u} },
    };
    const auto it = kTypes.find(name);
    if (it == kTypes.end())
        return false;
    out = it->second;
    return true;
}

bool IsGlslQualifier(const std::string& token) noexcept
{
    return token == "highp" || token == "mediump" || token == "lowp" || token == "precise" ||
           token == "row_major" || token == "column_major" || token == "flat";
}

} // namespace

bool ReflectGlslStd140Block(std::string_view source,
                            std::string_view blockName,
                            ReflectedConstantBuffer& out,
                            std::string* outError)
{
    out = {};
    const std::string text = StripGlslComments(source);

    // "uniform <blockName> {" suchen (davor optional layout(std140, ...)).
    size_t bodyBegin = std::string::npos;
    for (size_t pos = text.find("uniform"); pos != std::string::npos; pos = text.find("uniform", pos + 7u))
    {
        if ((pos > 0u && IsIdentChar(text[pos - 1u])) || (pos + 7u < text.size() && IsIdentChar(text[pos + 7u])))
            continue;
        size_t cursor = pos + 7u;
        while (cursor < text.size() && std::isspace(static_cast<unsigned char>(text[cursor]))) ++cursor;
        if (text.compare(cursor, blockName.size(), blockName) != 0)
            continue;
        cursor += blockName.size();
        if (cursor < text.size() && IsIdentChar(text[cursor]))
            continue;
        while (cursor < text.size() && std::isspace(static_cast<unsigned char>(text[cursor]))) ++cursor;
        if (cursor < text.size() && text[cursor] == '{')
        {
            bodyBegin = cursor + 1u;
            break;
        }
    }
    if (bodyBegin == std::string::npos)
    {
        SetReflectError(outError, "GLSL source has no uniform block '" + std::string(blockName) + "'");
        return false;
    }
    const size_t bodyEnd = text.find('}', bodyBegin);
    if (bodyEnd == std::string::npos)
    {
        SetReflectError(outError, "GLSL uniform block '" + std::string(blockName) + "' is not closed");
        return false;
    }

    std::string body;
    {
        // Praeprozessor-Zeilen im Block werden ignoriert.
        const std::string_view raw(text.data() + bodyBegin, bodyEnd - bodyBegin);
        size_t lineStart = 0u;
        while (lineStart <= raw.size())
        {
            size_t lineEnd = raw.find('\n', lineStart);
            if (lineEnd == std::string_view::npos) lineEnd = raw.size();
            const std::string_view line = raw.substr(lineStart, lineEnd - lineStart);
            const size_t first = line.find_first_not_of(" \t\r");
            if (first == std::string_view::npos || line[first] != '#')
            {
                body.append(line);
                body.push_back('\n');
            }
            lineStart = lineEnd + 1u;
        }
    }

    uint32_t offset = 0u;
    size_t declBegin = 0u;
    for (size_t declEnd = body.find(';'); declEnd != std::string::npos; declEnd = body.find(';', declBegin))
    {
        std::vector<std::string> tokens = TokenizeDeclaration(std::string_view(body).substr(declBegin, declEnd - declBegin));
        declBegin = declEnd + 1u;

        size_t t = 0u;
        if (t < tokens.size() && tokens[t] == "layout")
        {
            while (t < tokens.size() && tokens[t] != ")") ++t;
            ++t;
        }
        while (t < tokens.size() && IsGlslQualifier(tokens[t])) ++t;
        if (t >= tokens.size())
            continue;

        Std140Type type{};
        if (!ResolveStd140Type(tokens[t], type))
        {
            SetReflectError(outError, "GLSL block '" + std::string(blockName) + "': unsupported type '" + tokens[t] + "'");
            return false;
        }
        ++t;

        // Ein oder mehrere Deklaratoren: name, name[N]
        while (t < tokens.size())
        {
            const std::string name = tokens[t++];
            uint32_t arrayLength = 0u;
            if (t + 2u < tokens.size() && tokens[t] == "[" && tokens[t + 2u] == "]")
            {
                arrayLength = static_cast<uint32_t>(std::strtoul(tokens[t + 1u].c_str(), nullptr, 10));
                if (arrayLength == 0u)
                {
                    SetReflectError(outError, "GLSL block '" + std::string(blockName) + "': array size of '" + name + "' is not a literal");
                    return false;
                }
                t += 3u;
            }

            uint32_t size = type.size;
            uint32_t alignment = type.alignment;
            if (arrayLength > 0u)
            {
                // std140: Array-Elemente auf 16 Byte ausgerichtet.
                const uint32_t stride = AlignUp(type.size, 16u);
                size = stride * arrayLength;
                alignment = 16u;
            }
            offset = AlignUp(offset, alignment);
            out.fields.push_back({ name, offset, size });
            offset += size;

            if (t < tokens.size() && tokens[t] == ",")
                ++t;
            else
                break;
        }
    }

    FinishBlock(out, offset);
    return true;
}

// ---------------------------------------------------------------------------
// DXBC
// ---------------------------------------------------------------------------
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
    if (!bytecode || byteSize == 0u)
    {
        SetReflectError(outError, "no DXBC bytecode");
        return false;
    }

    using D3DReflectFn = HRESULT (WINAPI*)(LPCVOID, SIZE_T, REFIID, void**);
    const auto reflect = reinterpret_cast<D3DReflectFn>(internal::ResolveD3DReflectProc(outError));
    if (!reflect)
        return false;

    // IID_ID3D11ShaderReflection (d3dcompiler_47); hier definiert, damit der
    // Core nicht gegen dxguid.lib linken muss.
    static constexpr GUID kIIDShaderReflection =
        { 0x8d536ca1, 0x0cca, 0x4956, { 0xa8, 0x37, 0x78, 0x69, 0x63, 0x75, 0x55, 0x84 } };

    ID3D11ShaderReflection* reflection = nullptr;
    if (FAILED(reflect(bytecode, byteSize, kIIDShaderReflection, reinterpret_cast<void**>(&reflection))) || !reflection)
    {
        SetReflectError(outError, "D3DReflect failed");
        return false;
    }

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
    case assets::ShaderTargetProfile::OpenGL_GLSL450:
        return ReflectGlslStd140Block(artifact.sourceText, cbufferName, out, outError);
    default:
        out = {};
        SetReflectError(outError, std::string("no constant buffer reflection for target ") + ShaderCompiler::ToString(artifact.target));
        return false;
    }
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
        const auto existing = std::find_if(shader->compiledArtifacts.begin(), shader->compiledArtifacts.end(),
            [&](const assets::CompiledShaderArtifact& artifact) {
                return artifact.target == target && artifact.stage == shader->stage && artifact.IsValid();
            });

        assets::CompiledShaderArtifact compiled{};
        const assets::CompiledShaderArtifact* artifact = nullptr;
        if (existing != shader->compiledArtifacts.end())
            artifact = &(*existing);
        else if (ShaderCompiler::CompileForTarget(*shader, target, compiled, &lastError))
            artifact = &compiled;
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
