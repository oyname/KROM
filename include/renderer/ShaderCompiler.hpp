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
    // Engine-Asset-Root als Fallback-Include-Suchpfad setzen.
    // Shader koennen dann #include "per_object_binding.hlsl" nutzen
    // unabhaengig vom Projekt-Asset-Root.
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
};

} // namespace engine::renderer
