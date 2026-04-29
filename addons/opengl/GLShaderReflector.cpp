#include "GLShaderReflector.hpp"
#include "core/Debug.hpp"
#include "renderer/ShaderBindingModel.hpp"
#include "renderer/ShaderCompiler.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#ifdef KROM_OPENGL_BACKEND
#   if defined(_WIN32)
#       ifndef WIN32_LEAN_AND_MEAN
#           define WIN32_LEAN_AND_MEAN
#       endif
#       ifndef NOMINMAX
#           define NOMINMAX
#       endif
#       include <windows.h>
#       include "OpenGLWin32Loader.hpp"
#   endif
#   if defined(__APPLE__)
#       include <OpenGL/gl3.h>
#   else
#       include <GL/gl.h>
#       include "glext.h"
#   endif
#endif

namespace engine::renderer::opengl {

namespace {

#ifdef KROM_OPENGL_BACKEND
constexpr GLenum kGLUniformBlockIndexInvalid = 0xFFFFFFFFu;
constexpr GLenum kGLVertexShader = 0x8B31u;
constexpr GLenum kGLFragmentShader = 0x8B30u;
constexpr GLenum kGLCompileStatus = 0x8B81u;
constexpr GLenum kGLLinkStatus = 0x8B82u;
constexpr GLenum kGLInfoLogLength = 0x8B84u;
constexpr GLenum kGLActiveUniforms = 0x8B86u;
constexpr GLenum kGLActiveUniformMaxLength = 0x8B87u;
constexpr GLenum kGLActiveUniformBlocks = 0x8A36u;
constexpr GLenum kGLActiveUniformBlockMaxNameLength = 0x8A35u;
constexpr GLenum kGLUniformBlockDataSize = 0x8A40u;
constexpr GLenum kGLUniformBlockActiveUniforms = 0x8A42u;
constexpr GLenum kGLUniformBlockActiveUniformIndices = 0x8A43u;
constexpr GLenum kGLSampler2D = 0x8B5Eu;
constexpr GLenum kGLSamplerCube = 0x8B60u;
constexpr GLenum kGLFloat = 0x1406u;
constexpr GLenum kGLFloatVec2 = 0x8B50u;
constexpr GLenum kGLFloatVec3 = 0x8B51u;
constexpr GLenum kGLFloatVec4 = 0x8B52u;
constexpr GLenum kGLInt = 0x1404u;
constexpr GLenum kGLBool = 0x8B56u;
#endif

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

const assets::CompiledShaderArtifact* FindGLArtifact(const assets::ShaderAsset& shader) noexcept
{
    auto it = std::find_if(shader.compiledArtifacts.begin(), shader.compiledArtifacts.end(), [&](const assets::CompiledShaderArtifact& artifact) {
        return artifact.target == assets::ShaderTargetProfile::OpenGL_GLSL450 &&
               artifact.stage == shader.stage &&
               !artifact.sourceText.empty();
    });
    return it != shader.compiledArtifacts.end() ? &(*it) : nullptr;
}

std::string ResolveGLSource(const assets::ShaderAsset& shader)
{
    if (const auto* artifact = FindGLArtifact(shader))
        return artifact->sourceText;
    if (!shader.sourceCode.empty())
        return shader.sourceCode;
    if (!shader.bytecode.empty())
        return std::string(reinterpret_cast<const char*>(shader.bytecode.data()), shader.bytecode.size());
    return {};
}

#ifdef KROM_OPENGL_BACKEND
GLuint CompileShader(GLenum type, const std::string& source, std::string* outError)
{
    GLuint shader = glCreateShader(type);
    if (!shader)
    {
        if (outError)
            *outError = "glCreateShader failed";
        return 0u;
    }

    const char* src = source.c_str();
    const GLint len = static_cast<GLint>(source.size());
    glShaderSource(shader, 1, &src, &len);
    glCompileShader(shader);

    GLint status = 0;
    glGetShaderiv(shader, kGLCompileStatus, &status);
    if (status != 0)
        return shader;

    GLint logLen = 0;
    glGetShaderiv(shader, kGLInfoLogLength, &logLen);
    std::string log(static_cast<size_t>(std::max(logLen, 1)), '\0');
    glGetShaderInfoLog(shader, logLen, nullptr, log.data());
    glDeleteShader(shader);
    if (outError)
        *outError = log;
    return 0u;
}

std::string MakeFallbackSource(GLenum stage)
{
    if (stage == kGLVertexShader)
    {
        return R"(#version 410 core
layout(location = 0) in vec3 aPosition;
void main() { gl_Position = vec4(aPosition, 1.0); }
)";
    }

    return R"(#version 410 core
out vec4 FragColor;
void main() { FragColor = vec4(1.0); }
)";
}

bool BuildProgram(const std::string& vertexSource,
                  const std::string& fragmentSource,
                  GLuint& outProgram,
                  std::string* outError)
{
    outProgram = 0u;
    GLuint vs = CompileShader(kGLVertexShader, vertexSource, outError);
    if (!vs)
        return false;

    GLuint fs = CompileShader(kGLFragmentShader, fragmentSource, outError);
    if (!fs)
    {
        glDeleteShader(vs);
        return false;
    }

    GLuint program = glCreateProgram();
    if (!program)
    {
        glDeleteShader(vs);
        glDeleteShader(fs);
        if (outError)
            *outError = "glCreateProgram failed";
        return false;
    }

    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);
    glDetachShader(program, vs);
    glDetachShader(program, fs);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint linked = 0;
    glGetProgramiv(program, kGLLinkStatus, &linked);
    if (linked != 0)
    {
        outProgram = program;
        return true;
    }

    GLint logLen = 0;
    glGetProgramiv(program, kGLInfoLogLength, &logLen);
    std::string log(static_cast<size_t>(std::max(logLen, 1)), '\0');
    glGetProgramInfoLog(program, logLen, nullptr, log.data());
    glDeleteProgram(program);
    if (outError)
        *outError = log;
    return false;
}

ParameterType MapUniformType(GLenum uniformType) noexcept
{
    switch (uniformType)
    {
    case kGLSampler2D: return ParameterType::Texture2D;
    case kGLSamplerCube: return ParameterType::TextureCube;
    case kGLFloat: return ParameterType::Float;
    case kGLFloatVec2: return ParameterType::Vec2;
    case kGLFloatVec3: return ParameterType::Vec3;
    case kGLFloatVec4: return ParameterType::Vec4;
    case kGLInt: return ParameterType::Int;
    case kGLBool: return ParameterType::Bool;
    default: return ParameterType::Unknown;
    }
}

uint32_t MapUniformByteSize(ParameterType type) noexcept
{
    switch (type)
    {
    case ParameterType::Float: return 4u;
    case ParameterType::Vec2: return 8u;
    case ParameterType::Vec3: return 16u;
    case ParameterType::Vec4: return 16u;
    case ParameterType::Int: return 4u;
    case ParameterType::Bool: return 4u;
    default: return 0u;
    }
}

void TrimArraySuffix(std::string& name)
{
    if (const size_t pos = name.find("[0]"); pos != std::string::npos)
        name.resize(pos);
}

bool ReflectProgramLayout(GLuint program,
                          ShaderStageMask stages,
                          ShaderParameterLayout& outLayout,
                          std::string* outError)
{
    ShaderParameterLayout layout{};

    GLint blockCount = 0;
    GLint blockNameMax = 0;
    glGetProgramiv(program, kGLActiveUniformBlocks, &blockCount);
    glGetProgramiv(program, kGLActiveUniformBlockMaxNameLength, &blockNameMax);
    std::unordered_set<GLuint> blockUniformIndices;

    std::string blockName(static_cast<size_t>(std::max(blockNameMax, 1)), '\0');
    for (GLint blockIndex = 0; blockIndex < blockCount; ++blockIndex)
    {
        GLsizei actualLen = 0;
        glGetActiveUniformBlockName(program, static_cast<GLuint>(blockIndex), blockNameMax, &actualLen, blockName.data());
        blockName[static_cast<size_t>(actualLen)] = '\0';

        GLint dataSize = 0;
        glGetActiveUniformBlockiv(program, static_cast<GLuint>(blockIndex), kGLUniformBlockDataSize, &dataSize);

        GLint activeUniformCount = 0;
        glGetActiveUniformBlockiv(program, static_cast<GLuint>(blockIndex), kGLUniformBlockActiveUniforms, &activeUniformCount);
        std::vector<GLint> indices(static_cast<size_t>(std::max(activeUniformCount, 0)));
        if (!indices.empty())
        {
            glGetActiveUniformBlockiv(program,
                                      static_cast<GLuint>(blockIndex),
                                      kGLUniformBlockActiveUniformIndices,
                                      indices.data());
            for (GLint index : indices)
            {
                if (index >= 0)
                    blockUniformIndices.insert(static_cast<GLuint>(index));
            }
        }

        ParameterSlot slot{};
        slot.SetName(blockName.c_str());
        slot.type = ParameterType::ConstantBuffer;
        slot.binding = static_cast<uint32_t>(blockIndex);
        slot.set = 0u;
        slot.stageFlags = stages;
        slot.byteSize = static_cast<uint32_t>(std::max(dataSize, 0));
        slot.elementCount = 1u;
        if (!AddOrMergeSlot(layout, slot))
        {
            if (outError)
                *outError = "shader parameter layout is full while reflecting GL uniform blocks";
            outLayout.Clear();
            return false;
        }
    }

    GLint uniformCount = 0;
    GLint nameMax = 0;
    glGetProgramiv(program, kGLActiveUniforms, &uniformCount);
    glGetProgramiv(program, kGLActiveUniformMaxLength, &nameMax);
    std::string uniformName(static_cast<size_t>(std::max(nameMax, 1)), '\0');

    for (GLint uniformIndex = 0; uniformIndex < uniformCount; ++uniformIndex)
    {
        if (blockUniformIndices.find(static_cast<GLuint>(uniformIndex)) != blockUniformIndices.end())
            continue;

        GLsizei actualLen = 0;
        GLint size = 0;
        GLenum type = 0u;
        glGetActiveUniform(program,
                           static_cast<GLuint>(uniformIndex),
                           nameMax,
                           &actualLen,
                           &size,
                           &type,
                           uniformName.data());
        uniformName[static_cast<size_t>(actualLen)] = '\0';

        std::string name = uniformName.c_str();
        TrimArraySuffix(name);
        const ParameterType mappedType = MapUniformType(type);
        if (mappedType == ParameterType::Unknown)
            continue;

        const GLint location = glGetUniformLocation(program, name.c_str());
        if (location < 0)
            continue;

        ParameterSlot slot{};
        slot.SetName(name);
        slot.type = mappedType;
        slot.binding = static_cast<uint32_t>(location);
        slot.set = 0u;
        slot.stageFlags = stages;
        slot.byteSize = MapUniformByteSize(mappedType);
        slot.elementCount = static_cast<uint32_t>(std::max(size, 1));
        if (!AddOrMergeSlot(layout, slot))
        {
            if (outError)
                *outError = "shader parameter layout is full while reflecting GL uniforms";
            outLayout.Clear();
            return false;
        }
    }

    outLayout = layout;
    if (!outLayout.IsValid())
    {
        if (outError)
            *outError = "GL shader reflection found no active material-relevant slots";
        return false;
    }
    return true;
}
#endif

bool ReflectSingle(const assets::ShaderAsset& shader,
                   ShaderParameterLayout& outLayout,
                   std::string* outError)
{
#ifndef KROM_OPENGL_BACKEND
    (void)shader;
    outLayout.Clear();
    if (outError)
        *outError = "OpenGL reflection is unavailable because the OpenGL backend is not enabled";
    return false;
#else
    const std::string source = ResolveGLSource(shader);
    if (source.empty())
    {
        outLayout.Clear();
        if (outError)
            *outError = "shader has no GLSL source for OpenGL reflection";
        return false;
    }

    const ShaderStageMask stage = ToStageMask(shader.stage);
    GLuint program = 0u;
    bool built = false;
    if (shader.stage == assets::ShaderStage::Vertex)
        built = BuildProgram(source, MakeFallbackSource(kGLFragmentShader), program, outError);
    else if (shader.stage == assets::ShaderStage::Fragment)
        built = BuildProgram(MakeFallbackSource(kGLVertexShader), source, program, outError);
    else
    {
        if (outError)
            *outError = "OpenGL reflector only supports vertex+fragment materials";
        return false;
    }

    if (!built)
    {
        outLayout.Clear();
        return false;
    }

    const bool ok = ReflectProgramLayout(program, stage, outLayout, outError);
    glDeleteProgram(program);
    return ok;
#endif
}

} // namespace

bool GLShaderReflector::Reflect(const assets::ShaderAsset& shader,
                                ShaderParameterLayout& outLayout,
                                std::string* outError) const
{
    if (!ReflectSingle(shader, outLayout, outError))
        return false;
    ValidateShaderBindings(outLayout, shader.debugName);
    return true;
}

bool GLShaderReflector::ReflectProgram(const assets::ShaderAsset& vertexShader,
                                       const assets::ShaderAsset& fragmentShader,
                                       ShaderParameterLayout& outLayout,
                                       std::string* outError) const
{
#ifndef KROM_OPENGL_BACKEND
    (void)vertexShader;
    (void)fragmentShader;
    outLayout.Clear();
    if (outError)
        *outError = "OpenGL reflection is unavailable because the OpenGL backend is not enabled";
    return false;
#else
    const std::string vertexSource = ResolveGLSource(vertexShader);
    const std::string fragmentSource = ResolveGLSource(fragmentShader);
    if (vertexSource.empty() || fragmentSource.empty())
    {
        outLayout.Clear();
        if (outError)
            *outError = "vertex or fragment shader has no GLSL source for OpenGL reflection";
        return false;
    }

    GLuint program = 0u;
    if (!BuildProgram(vertexSource, fragmentSource, program, outError))
    {
        outLayout.Clear();
        return false;
    }

    const bool ok = ReflectProgramLayout(program,
                                         ToStageMask(vertexShader.stage) | ToStageMask(fragmentShader.stage),
                                         outLayout,
                                         outError);
    glDeleteProgram(program);
    if (ok)
        ValidateShaderBindings(outLayout, vertexShader.debugName);
    return ok;
#endif
}

} // namespace engine::renderer::opengl
