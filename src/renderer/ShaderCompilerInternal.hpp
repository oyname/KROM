#pragma once
#include "renderer/ShaderCompiler.hpp"
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// Forward declaration of the namespace-safe API resolver defined in
// ShaderCompiler.cpp (outer engine::renderer namespace).
// Visible to internal:: sub-namespace via enclosing namespace lookup.
namespace engine::renderer {
    ShaderTargetApi ResolveTargetApiNameSpaceSafe(assets::ShaderTargetProfile profile) noexcept;
}

namespace engine::renderer::internal {

struct SourceBundle
{
    std::filesystem::path canonicalSourcePath;
    std::string preprocessedSource;
    std::vector<assets::ShaderDependencyRecord> dependencies;
};

struct GlslSourceSections
{
    std::string preVersionTrivia;
    std::string versionLine;
    std::string body;
};

void SetError(std::string* outError, const std::string& msg);
uint64_t HashBytes(const void* data, size_t size) noexcept;
uint64_t HashString(std::string_view value) noexcept;
uint64_t HashCombine(uint64_t seed, uint64_t value) noexcept;
std::string Hex64(uint64_t value);
std::string ToLower(std::string s);
std::string StageToGlslangSuffix(assets::ShaderStage stage);
std::string StageToToolExtension(assets::ShaderStage stage, assets::ShaderSourceLanguage language);
bool ReadBinaryFile(const std::filesystem::path& path, std::vector<uint8_t>& outBytes);
GlslSourceSections SplitGlslSourceSections(std::string_view source);
std::string BuildShaderSource(const SourceBundle& bundle, const std::vector<std::string>& defines);
#ifdef _WIN32
std::string GetHlslTargetProfile(assets::ShaderStage stage, assets::ShaderTargetProfile target);
#endif
bool CompileToD3DBytecode(const assets::ShaderAsset& asset,
                          assets::ShaderTargetProfile target,
                          const SourceBundle& bundle,
                          const std::vector<std::string>& defines,
                          assets::CompiledShaderArtifact& outCompiled,
                          std::string* outError);
bool CompileToDxilWithTool(const assets::ShaderAsset& asset,
                           const SourceBundle& bundle,
                           const std::vector<std::string>& defines,
                           assets::CompiledShaderArtifact& outCompiled,
                           std::string* outError);
bool CompileToSpirvWithTool(const assets::ShaderAsset& asset,
                            const SourceBundle& bundle,
                            const std::vector<std::string>& defines,
                            assets::CompiledShaderArtifact& outCompiled,
                            std::string* outError);
bool CompileToSpirv(const assets::ShaderAsset& asset,
                    const SourceBundle& bundle,
                    const std::vector<std::string>& defines,
                    assets::CompiledShaderArtifact& outCompiled,
                    std::string* outError);

} // namespace engine::renderer::internal
