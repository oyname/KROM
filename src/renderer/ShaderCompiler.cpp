#include "renderer/ShaderCompiler.hpp"
#include "ShaderCompilerInternal.hpp"

#include <string>
#include <vector>

namespace engine::renderer {

namespace {

std::vector<std::string> BuildVariantDefines(assets::ShaderTargetProfile target,
                                             ShaderVariantFlag flags) noexcept
{
    std::vector<std::string> defines;
    if (target == assets::ShaderTargetProfile::Vulkan_SPIRV)
    {
        defines.emplace_back("KROM_VULKAN");
        defines.emplace_back("KROM_VULKAN_PUSH_PER_OBJECT");
    }
    if (HasFlag(flags, ShaderVariantFlag::Skinned))       defines.emplace_back("KROM_SKINNING");
    if (HasFlag(flags, ShaderVariantFlag::VertexColor))   defines.emplace_back("KROM_VERTEX_COLOR");
    if (HasFlag(flags, ShaderVariantFlag::AlphaTest))     defines.emplace_back("KROM_ALPHA_TEST");
    if (HasFlag(flags, ShaderVariantFlag::NormalMap))     defines.emplace_back("KROM_NORMAL_MAP");
    if (HasFlag(flags, ShaderVariantFlag::NormalMapBC5))  defines.emplace_back("KROM_NORMALMAP_BC5");
    if (HasFlag(flags, ShaderVariantFlag::Unlit))         defines.emplace_back("KROM_UNLIT");
    if (HasFlag(flags, ShaderVariantFlag::ShadowPass))    defines.emplace_back("KROM_SHADOW_PASS");
    if (HasFlag(flags, ShaderVariantFlag::Instanced))     defines.emplace_back("KROM_INSTANCED");
    if (HasFlag(flags, ShaderVariantFlag::BaseColorMap))  defines.emplace_back("KROM_BASECOLOR_MAP");
    if (HasFlag(flags, ShaderVariantFlag::MetallicMap))   defines.emplace_back("KROM_METALLIC_MAP");
    if (HasFlag(flags, ShaderVariantFlag::RoughnessMap))  defines.emplace_back("KROM_ROUGHNESS_MAP");
    if (HasFlag(flags, ShaderVariantFlag::OcclusionMap))  defines.emplace_back("KROM_OCCLUSION_MAP");
    if (HasFlag(flags, ShaderVariantFlag::EmissiveMap))   defines.emplace_back("KROM_EMISSIVE_MAP");
    if (HasFlag(flags, ShaderVariantFlag::OpacityMap))    defines.emplace_back("KROM_OPACITY_MAP");
    if (HasFlag(flags, ShaderVariantFlag::PBRMetalRough)) defines.emplace_back("KROM_PBR_METAL_ROUGH");
    if (HasFlag(flags, ShaderVariantFlag::DoubleSided))   defines.emplace_back("KROM_DOUBLE_SIDED");
    if (HasFlag(flags, ShaderVariantFlag::ORMMap))        defines.emplace_back("KROM_ORM_MAP");
    if (HasFlag(flags, ShaderVariantFlag::IBLMap))        defines.emplace_back("KROM_IBL");
    return defines;
}

} // namespace

ShaderTargetApi ResolveTargetApiNameSpaceSafe(assets::ShaderTargetProfile profile) noexcept
{
    switch (profile)
    {
    case assets::ShaderTargetProfile::Null: return ShaderTargetApi::Null;
    case assets::ShaderTargetProfile::DirectX11_SM5: return ShaderTargetApi::DirectX11;
    case assets::ShaderTargetProfile::DirectX12_SM6: return ShaderTargetApi::DirectX12;
    case assets::ShaderTargetProfile::Vulkan_SPIRV: return ShaderTargetApi::Vulkan;
    case assets::ShaderTargetProfile::OpenGL_GLSL450: return ShaderTargetApi::OpenGL;
    default: return ShaderTargetApi::Generic;
    }
}

assets::ShaderTargetProfile ShaderCompiler::ResolveTargetProfile(const IDevice& device)
{
    return device.GetShaderTargetProfile();
}

ShaderTargetApi ShaderCompiler::ResolveTargetApi(const IDevice& device)
{
    return ResolveTargetApiNameSpaceSafe(ResolveTargetProfile(device));
}

ShaderBinaryFormat ShaderCompiler::ResolveBinaryFormat(assets::ShaderTargetProfile profile) noexcept
{
    switch (profile)
    {
    case assets::ShaderTargetProfile::Vulkan_SPIRV: return ShaderBinaryFormat::Spirv;
    case assets::ShaderTargetProfile::DirectX11_SM5: return ShaderBinaryFormat::Dxbc;
    case assets::ShaderTargetProfile::DirectX12_SM6: return ShaderBinaryFormat::Dxil;
    case assets::ShaderTargetProfile::OpenGL_GLSL450: return ShaderBinaryFormat::GlslSource;
    case assets::ShaderTargetProfile::Null:
    case assets::ShaderTargetProfile::Generic:
    default:
        return ShaderBinaryFormat::SourceText;
    }
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
    return !shader.bytecode.empty() || !shader.sourceText.empty() || shader.contract.IsValid();
}

bool ShaderCompiler::CompileForTarget(const assets::ShaderAsset& asset,
                                      assets::ShaderTargetProfile target,
                                      assets::CompiledShaderArtifact& outCompiled,
                                      std::string* outError)
{
    return internal::CacheFirstCompile(asset, target, BuildVariantDefines(target, ShaderVariantFlag::None), outCompiled, outError);
}

std::vector<std::string> ShaderCompiler::VariantFlagsToDefines(ShaderVariantFlag flags) noexcept
{
    return BuildVariantDefines(assets::ShaderTargetProfile::Generic, flags);
}

bool ShaderCompiler::CompileVariant(const assets::ShaderAsset& asset,
                                    assets::ShaderTargetProfile target,
                                    ShaderVariantFlag flags,
                                    assets::CompiledShaderArtifact& outCompiled,
                                    std::string* outError)
{
    return internal::CacheFirstCompile(asset, target, BuildVariantDefines(target, flags), outCompiled, outError);
}

} // namespace engine::renderer
