#include "renderer/ShaderCompiler.hpp"
#include "core/Debug.hpp"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <vector>

namespace engine::renderer {
namespace {
std::string ToLower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

uint64_t HashBytes(const void* data, size_t size) noexcept
{
    const auto* bytes = static_cast<const uint8_t*>(data);
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < size; ++i)
    {
        h ^= static_cast<uint64_t>(bytes[i]);
        h *= 1099511628211ull;
    }
    return h;
}

void SetError(std::string* outError, const std::string& msg)
{
    if (outError)
        *outError = msg;
}
}

assets::ShaderTargetProfile ShaderCompiler::ResolveTargetProfile(const IDevice& device)
{
    const std::string backend = ToLower(device.GetBackendName() ? device.GetBackendName() : "");
    if (backend.find("dx12") != std::string::npos || backend.find("directx12") != std::string::npos)
        return assets::ShaderTargetProfile::DirectX12_SM6;
    if (backend.find("dx11") != std::string::npos || backend.find("directx11") != std::string::npos)
        return assets::ShaderTargetProfile::DirectX11_SM5;
    if (backend.find("vulkan") != std::string::npos)
        return assets::ShaderTargetProfile::Vulkan_SPIRV;
    if (backend.find("opengl") != std::string::npos || backend.find("gl") != std::string::npos)
        return assets::ShaderTargetProfile::OpenGL_GLSL450;
    if (backend.find("null") != std::string::npos)
        return assets::ShaderTargetProfile::Null;
    return assets::ShaderTargetProfile::Generic;
}

const char* ShaderCompiler::ToString(assets::ShaderTargetProfile profile) noexcept
{
    switch (profile)
    {
    case assets::ShaderTargetProfile::Null: return "Null";
    case assets::ShaderTargetProfile::DirectX11_SM5: return "DirectX11_SM5";
    case assets::ShaderTargetProfile::DirectX12_SM6: return "DirectX12_SM6";
    case assets::ShaderTargetProfile::Vulkan_SPIRV: return "Vulkan_SPIRV";
    case assets::ShaderTargetProfile::OpenGL_GLSL450: return "OpenGL_GLSL450";
    default: return "Generic";
    }
}

bool ShaderCompiler::IsRuntimeConsumable(const assets::CompiledShaderArtifact& shader) noexcept
{
    return !shader.bytecode.empty() || !shader.sourceText.empty();
}

bool ShaderCompiler::CompileForTarget(const assets::ShaderAsset& asset,
                                      assets::ShaderTargetProfile target,
                                      assets::CompiledShaderArtifact& outCompiled,
                                      std::string* outError)
{
    outCompiled = {};
    outCompiled.target = target;
    outCompiled.stage = asset.stage;
    outCompiled.entryPoint = asset.entryPoint.empty() ? "main" : asset.entryPoint;
    outCompiled.debugName = asset.debugName;

    if (!asset.bytecode.empty())
    {
        outCompiled.bytecode = asset.bytecode;
        outCompiled.sourceHash = HashBytes(outCompiled.bytecode.data(), outCompiled.bytecode.size());
        return true;
    }

    if (asset.sourceCode.empty())
    {
        SetError(outError, "shader asset has neither source nor bytecode");
        return false;
    }

    switch (target)
    {
    case assets::ShaderTargetProfile::DirectX11_SM5:
    case assets::ShaderTargetProfile::DirectX12_SM6:
    case assets::ShaderTargetProfile::Vulkan_SPIRV:
        // KROM's current neutral strategy: keep backend-targeted source as the compiled payload
        // until a platform compiler toolchain is present. Runtime still consumes a cached artifact,
        // not the raw asset directly.
        outCompiled.sourceText = asset.sourceCode;
        break;
    case assets::ShaderTargetProfile::OpenGL_GLSL450:
    case assets::ShaderTargetProfile::Null:
    case assets::ShaderTargetProfile::Generic:
    default:
        outCompiled.sourceText = asset.sourceCode;
        break;
    }

    outCompiled.sourceHash = HashBytes(outCompiled.sourceText.data(), outCompiled.sourceText.size());
    if (!IsRuntimeConsumable(outCompiled))
    {
        SetError(outError, "compiled shader artifact is empty");
        return false;
    }
    return true;
}


std::vector<std::string> ShaderCompiler::VariantFlagsToDefines(ShaderVariantFlag flags) noexcept
{
    std::vector<std::string> defines;
    if (HasFlag(flags, ShaderVariantFlag::Skinned))     defines.emplace_back("KROM_SKINNING");
    if (HasFlag(flags, ShaderVariantFlag::VertexColor)) defines.emplace_back("KROM_VERTEX_COLOR");
    if (HasFlag(flags, ShaderVariantFlag::AlphaTest))   defines.emplace_back("KROM_ALPHA_TEST");
    if (HasFlag(flags, ShaderVariantFlag::NormalMap))   defines.emplace_back("KROM_NORMAL_MAP");
    if (HasFlag(flags, ShaderVariantFlag::Unlit))       defines.emplace_back("KROM_UNLIT");
    if (HasFlag(flags, ShaderVariantFlag::ShadowPass))  defines.emplace_back("KROM_SHADOW_PASS");
    if (HasFlag(flags, ShaderVariantFlag::Instanced))   defines.emplace_back("KROM_INSTANCED");
    return defines;
}

bool ShaderCompiler::CompileVariant(const assets::ShaderAsset& asset,
                                    assets::ShaderTargetProfile target,
                                    ShaderVariantFlag flags,
                                    assets::CompiledShaderArtifact& outCompiled,
                                    std::string* outError)
{
    if (!CompileForTarget(asset, target, outCompiled, outError))
        return false;

    outCompiled.defines = VariantFlagsToDefines(flags);

    uint64_t h = outCompiled.sourceHash;
    for (const auto& d : outCompiled.defines)
    {
        h ^= HashBytes(d.data(), d.size());
        h *= 1099511628211ull;
    }
    outCompiled.sourceHash = h;
    return true;
}

} // namespace engine::renderer
