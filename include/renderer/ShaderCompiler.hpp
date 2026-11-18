#pragma once
#include "assets/AssetRegistry.hpp"
#include "renderer/IDevice.hpp"
#include "renderer/ShaderContract.hpp"
#include <filesystem>
#include <string>

namespace engine::renderer {

class ShaderCompiler
{
public:
    // Überschreibt den Shader-Cache-Pfad prozessweit.
    // Muss vor dem ersten CompileForTarget-Aufruf gesetzt werden.
    // Wenn nicht gesetzt: KROM_SHADER_CACHE_DIR env-var, dann <exe-dir>/shader_artifacts.
    static void SetCacheDirectory(const std::filesystem::path& dir) noexcept;
    // Resolved cache directory (the one set above, else the env-var/exe-dir fallback).
    // Other per-project caches (e.g. the Vulkan pipeline cache) live beside the shaders.
    [[nodiscard]] static std::filesystem::path GetCacheDirectory();
    // Override the engine include directory for file-backed and embedded HLSL.
    // By default, look for assets beside the executable or in the working directory.
    static void SetEngineAssetDirectory(const std::filesystem::path& dir) noexcept;

    [[nodiscard]] static assets::ShaderTargetProfile ResolveTargetProfile(const IDevice& device);
    [[nodiscard]] static ShaderTargetApi ResolveTargetApi(const IDevice& device);
    [[nodiscard]] static ShaderBinaryFormat ResolveBinaryFormat(assets::ShaderTargetProfile profile) noexcept;
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
    // Same artifact as CompileVariant, but only from the on-disk cache: returns
    // false on a cache miss and never runs the backend compiler. Lets callers
    // decide cheaply whether a variant needs a (slow) compile.
    // ShaderVariantFlag::None gives the artifact of CompileForTarget.
    [[nodiscard]] static bool LoadCachedVariant(const assets::ShaderAsset& asset,
                                                assets::ShaderTargetProfile target,
                                                ShaderVariantFlag flags,
                                                assets::CompiledShaderArtifact& outCompiled);
};

} // namespace engine::renderer
