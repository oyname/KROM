// =============================================================================
// KROM Engine - tools/shader_cross/krom_shader_cross.cpp
// Experiment (Auftrag 13): SPIR-V (from HLSL via DXC) -> GLSL 4.10 for the
// OpenGL backend, using the SPIRV-Cross C API from the Vulkan SDK.
//
// This is a build-time tool. The engine and exported games never load it.
//
// Naming rules, so the output fits the name-based binding of the OpenGL
// backend (addons/opengl/OpenGLResources.cpp):
//   - Uniform blocks: DXC names the block type "type.<cbuffer>". The block is
//     renamed to "<cbuffer>" (the name the backend binds), the instance to
//     "cb_<cbuffer>" (GLSL forbids block and instance sharing one name).
//   - Combined image samplers: named after the HLSL texture ("uHDRInput"), so
//     the backend's sampler-name table applies unchanged.
//   - Stage inputs/outputs: DXC names them "in.var.<SEMANTIC>" and
//     "out.var.<SEMANTIC>". Both sides become "krom_<SEMANTIC>", so vertex
//     outputs and fragment inputs match by name as well as by location.
//     Vertex inputs get the attribute names the backend binds (aPosition, ...).
//
// Usage:
//   krom_shader_cross <input.spv> <output.glsl> [--version 410] [--source-name name]
// =============================================================================
#include <spirv_cross/spirv_cross_c.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace {

bool ReadSpirv(const char* path, std::vector<SpvId>& outWords)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
        return false;
    const std::streamsize size = file.tellg();
    if (size <= 0 || (size % 4) != 0)
        return false;
    outWords.resize(static_cast<size_t>(size) / 4u);
    file.seekg(0);
    return static_cast<bool>(file.read(reinterpret_cast<char*>(outWords.data()), size));
}

// DXC writes "type.PerPass", "in.var.TEXCOORD0", ...; SPIRV-Cross only turns
// the dots into underscores when it prints. Accept both spellings.
bool StripPrefix(std::string& value, const char* dotted)
{
    std::string underscored = dotted;
    for (char& c : underscored)
        if (c == '.')
            c = '_';
    for (const std::string& prefix : { std::string(dotted), underscored })
    {
        if (value.rfind(prefix, 0u) == 0u)
        {
            value.erase(0u, prefix.size());
            return true;
        }
    }
    return false;
}

// Vertex attribute names bound by the OpenGL backend (glBindAttribLocation).
std::string VertexAttributeName(const std::string& semantic)
{
    static const std::map<std::string, std::string> kNames = {
        { "POSITION", "aPosition" },   { "POSITION0", "aPosition" },
        { "NORMAL", "aNormal" },       { "NORMAL0", "aNormal" },
        { "TANGENT", "aTangent" },     { "TANGENT0", "aTangent" },
        { "BINORMAL", "aBitangent" },  { "BINORMAL0", "aBitangent" },
        { "TEXCOORD", "aTexCoord" },   { "TEXCOORD0", "aTexCoord" },
        { "TEXCOORD1", "aTexCoord1" },
        { "COLOR", "aColor" },         { "COLOR0", "aColor" },
    };
    const auto it = kNames.find(semantic);
    return it != kNames.end() ? it->second : ("krom_" + semantic);
}

struct Options
{
    const char* input = nullptr;
    const char* output = nullptr;
    unsigned    version = 410u;
    std::string sourceName;
};

bool ParseArgs(int argc, char** argv, Options& out)
{
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--version") == 0 && i + 1 < argc)
            out.version = static_cast<unsigned>(std::strtoul(argv[++i], nullptr, 10));
        else if (std::strcmp(argv[i], "--source-name") == 0 && i + 1 < argc)
            out.sourceName = argv[++i];
        else if (!out.input)
            out.input = argv[i];
        else if (!out.output)
            out.output = argv[i];
        else
            return false;
    }
    return out.input && out.output;
}

} // namespace

int main(int argc, char** argv)
{
    Options options;
    if (!ParseArgs(argc, argv, options))
    {
        std::fprintf(stderr, "krom_shader_cross.cpp: usage: krom_shader_cross <input.spv> <output.glsl> "
                             "[--version 410] [--source-name name]\n");
        return 2;
    }

    std::vector<SpvId> words;
    if (!ReadSpirv(options.input, words))
    {
        std::fprintf(stderr, "krom_shader_cross.cpp: cannot read SPIR-V '%s'\n", options.input);
        return 1;
    }

    const auto start = std::chrono::steady_clock::now();

    spvc_context context = nullptr;
    if (spvc_context_create(&context) != SPVC_SUCCESS)
    {
        std::fprintf(stderr, "krom_shader_cross.cpp: spvc_context_create failed\n");
        return 1;
    }

    auto fail = [&](const char* what) {
        std::fprintf(stderr, "krom_shader_cross.cpp: %s: %s\n", what, spvc_context_get_last_error_string(context));
        spvc_context_destroy(context);
        return 1;
    };

    spvc_parsed_ir ir = nullptr;
    if (spvc_context_parse_spirv(context, words.data(), words.size(), &ir) != SPVC_SUCCESS)
        return fail("parse SPIR-V");

    spvc_compiler compiler = nullptr;
    if (spvc_context_create_compiler(context, SPVC_BACKEND_GLSL, ir, SPVC_CAPTURE_MODE_TAKE_OWNERSHIP, &compiler) != SPVC_SUCCESS)
        return fail("create GLSL compiler");

    spvc_compiler_options compilerOptions = nullptr;
    if (spvc_compiler_create_compiler_options(compiler, &compilerOptions) != SPVC_SUCCESS)
        return fail("create options");
    spvc_compiler_options_set_uint(compilerOptions, SPVC_COMPILER_OPTION_GLSL_VERSION, options.version);
    spvc_compiler_options_set_bool(compilerOptions, SPVC_COMPILER_OPTION_GLSL_ES, SPVC_FALSE);
    // GL 4.1 has no layout(binding = N); the backend binds by name after linking.
    spvc_compiler_options_set_bool(compilerOptions, SPVC_COMPILER_OPTION_GLSL_ENABLE_420PACK_EXTENSION, SPVC_FALSE);
    if (spvc_compiler_install_compiler_options(compiler, compilerOptions) != SPVC_SUCCESS)
        return fail("install options");

    const SpvExecutionModel model = spvc_compiler_get_execution_model(compiler);

    // HLSL keeps texture and sampler apart, GLSL 4.1 only has combined samplers.
    if (spvc_compiler_build_combined_image_samplers(compiler) != SPVC_SUCCESS)
        return fail("build combined image samplers");

    const spvc_combined_image_sampler* combined = nullptr;
    size_t combinedCount = 0u;
    if (spvc_compiler_get_combined_image_samplers(compiler, &combined, &combinedCount) != SPVC_SUCCESS)
        return fail("get combined image samplers");

    std::map<spvc_variable_id, int> uses;
    for (size_t i = 0u; i < combinedCount; ++i)
        ++uses[combined[i].image_id];
    for (size_t i = 0u; i < combinedCount; ++i)
    {
        std::string name = spvc_compiler_get_name(compiler, combined[i].image_id);
        if (uses[combined[i].image_id] > 1)
        {
            // One texture read through several samplers: the backend's
            // name table cannot bind these; keep them apart and say so.
            name += "_";
            name += spvc_compiler_get_name(compiler, combined[i].sampler_id);
            std::fprintf(stderr, "krom_shader_cross.cpp: warning: texture used with several samplers, "
                                 "combined sampler '%s' has no backend binding\n", name.c_str());
        }
        spvc_compiler_set_name(compiler, combined[i].combined_id, name.c_str());
    }

    spvc_resources resources = nullptr;
    if (spvc_compiler_create_shader_resources(compiler, &resources) != SPVC_SUCCESS)
        return fail("create shader resources");

    const spvc_reflected_resource* list = nullptr;
    size_t count = 0u;

    spvc_resources_get_resource_list_for_type(resources, SPVC_RESOURCE_TYPE_UNIFORM_BUFFER, &list, &count);
    for (size_t i = 0u; i < count; ++i)
    {
        std::string blockName = spvc_compiler_get_name(compiler, list[i].base_type_id);
        StripPrefix(blockName, "type.");
        spvc_compiler_set_name(compiler, list[i].base_type_id, blockName.c_str());
        spvc_compiler_set_name(compiler, list[i].id, ("cb_" + blockName).c_str());
    }

    spvc_resources_get_resource_list_for_type(resources, SPVC_RESOURCE_TYPE_STAGE_INPUT, &list, &count);
    for (size_t i = 0u; i < count; ++i)
    {
        std::string semantic = list[i].name ? list[i].name : "";
        if (!StripPrefix(semantic, "in.var."))
            continue;
        const std::string newName = (model == SpvExecutionModelVertex)
            ? VertexAttributeName(semantic)
            : ("krom_" + semantic);
        spvc_compiler_set_name(compiler, list[i].id, newName.c_str());
    }

    spvc_resources_get_resource_list_for_type(resources, SPVC_RESOURCE_TYPE_STAGE_OUTPUT, &list, &count);
    for (size_t i = 0u; i < count; ++i)
    {
        std::string semantic = list[i].name ? list[i].name : "";
        if (!StripPrefix(semantic, "out.var."))
            continue;
        spvc_compiler_set_name(compiler, list[i].id, ("krom_" + semantic).c_str());
    }

    const char* glsl = nullptr;
    if (spvc_compiler_compile(compiler, &glsl) != SPVC_SUCCESS)
        return fail("compile GLSL");

    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();

    // Keep #version first; the provenance note goes right after it.
    std::string text = glsl;
    const size_t versionEnd = text.find('\n');
    std::string header = "// Generated by tools/shader_cross (krom_shader_cross) from ";
    header += options.sourceName.empty() ? options.input : options.sourceName;
    header += ". Do not edit; regenerate with the CMake target krom_generate_glsl_experiment.\n";
    if (versionEnd != std::string::npos)
        text.insert(versionEnd + 1u, header);
    else
        text = header + text;

    std::ofstream out(options.output, std::ios::binary | std::ios::trunc);
    if (!out || !(out << text))
    {
        std::fprintf(stderr, "krom_shader_cross.cpp: cannot write '%s'\n", options.output);
        spvc_context_destroy(context);
        return 1;
    }

    std::printf("krom_shader_cross.cpp: %s -> %s (%.2f ms)\n", options.input, options.output, ms);
    spvc_context_destroy(context);
    return 0;
}
