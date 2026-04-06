#pragma once
#include "assets/AssetRegistry.hpp"
#include "renderer/IDevice.hpp"
#include <string>

namespace engine::renderer {

class ShaderCompiler
{
public:
    [[nodiscard]] static assets::ShaderTargetProfile ResolveTargetProfile(const IDevice& device);
    [[nodiscard]] static const char* ToString(assets::ShaderTargetProfile profile) noexcept;
    [[nodiscard]] static bool IsRuntimeConsumable(const assets::CompiledShaderArtifact& shader) noexcept;
    [[nodiscard]] static bool CompileForTarget(const assets::ShaderAsset& asset,
                                               assets::ShaderTargetProfile target,
                                               assets::CompiledShaderArtifact& outCompiled,
                                               std::string* outError = nullptr);
    [[nodiscard]] static std::vector<std::string> VariantFlagsToDefines(ShaderVariantFlag flags) noexcept;
    [[nodiscard]] static bool CompileVariant(const assets::ShaderAsset& asset,
                                             assets::ShaderTargetProfile target,
                                             ShaderVariantFlag flags,
                                             assets::CompiledShaderArtifact& outCompiled,
                                             std::string* outError = nullptr);
};

} // namespace engine::renderer
