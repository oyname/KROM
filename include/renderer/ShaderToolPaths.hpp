#pragma once
#include <filesystem>
#include <string>

namespace engine::renderer {

// External shader compilers the runtime shader pipeline may launch.
enum class ShaderTool
{
    Dxc,     // dxc.exe: HLSL -> DXIL / SPIR-V
    Glslang, // glslangValidator (or glslc): SPIR-V fallback without shaderc
};

struct ShaderToolLocation
{
    std::filesystem::path path; // empty: not found
    // Where the path came from: "override", "DXC_PATH", "VULKAN_SDK" or "PATH".
    std::string source;
};

// A user-chosen executable that takes precedence over the environment search.
// An empty path removes the override. Thread-safe; shader compiles started
// afterwards use the new value.
void SetShaderToolOverride(ShaderTool tool, const std::filesystem::path& path);
std::filesystem::path GetShaderToolOverride(ShaderTool tool);

// Search order: override, DXC_PATH (dxc only), VULKAN_SDK/Bin, PATH. An override
// that does not name an existing file is reported as not found (no silent fallback),
// so a mistyped path shows up instead of quietly using another compiler.
ShaderToolLocation FindShaderTool(ShaderTool tool);
// Same search with an explicit override instead of the stored one (empty = none),
// e.g. to check values the user has not applied yet.
ShaderToolLocation FindShaderTool(ShaderTool tool, const std::filesystem::path& overridePath);

} // namespace engine::renderer
