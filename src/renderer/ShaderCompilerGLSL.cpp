#include "ShaderCompilerInternal.hpp"

namespace engine::renderer::internal {


GlslSourceSections SplitGlslSourceSections(std::string_view source)
{
    GlslSourceSections sections{};
    size_t cursor = 0u;
    bool foundVersion = false;
    bool inBlockComment = false;

    while (cursor < source.size())
    {
        const size_t lineStart = cursor;
        size_t lineEnd = source.find('\n', cursor);
        if (lineEnd == std::string_view::npos)
            lineEnd = source.size();
        cursor = (lineEnd < source.size()) ? (lineEnd + 1u) : lineEnd;

        std::string_view line = source.substr(lineStart, lineEnd - lineStart);
        const size_t first = line.find_first_not_of(" \t\r");
        const bool isBlank = (first == std::string_view::npos);

        bool triviaBeforeVersion = false;
        if (!foundVersion)
        {
            if (inBlockComment)
            {
                triviaBeforeVersion = true;
                if (line.find("*/") != std::string_view::npos)
                    inBlockComment = false;
            }
            else if (isBlank)
            {
                triviaBeforeVersion = true;
            }
            else if (line.compare(first, 2u, "//") == 0)
            {
                triviaBeforeVersion = true;
            }
            else if (line.compare(first, 2u, "/*") == 0)
            {
                triviaBeforeVersion = true;
                if (line.find("*/", first + 2u) == std::string_view::npos)
                    inBlockComment = true;
            }
        }

        const bool isVersion = !isBlank && !inBlockComment && line.compare(first, 8u, "#version") == 0;

        if (!foundVersion)
        {
            if (triviaBeforeVersion)
            {
                sections.preVersionTrivia.append(source.substr(lineStart, cursor - lineStart));
                continue;
            }
            if (isVersion)
            {
                sections.versionLine.assign(source.substr(lineStart, cursor - lineStart));
                foundVersion = true;
                continue;
            }

            sections.body.assign(source.substr(lineStart));
            return sections;
        }

        if (isVersion)
            continue;

        sections.body.append(source.substr(lineStart, cursor - lineStart));
    }

    return sections;
}

std::string BuildShaderSource(const SourceBundle& bundle, const std::vector<std::string>& defines)
{
    const GlslSourceSections sections = SplitGlslSourceSections(bundle.preprocessedSource);

    std::string defineBlock;
    defineBlock.reserve(defines.size() * 32u);
    for (const auto& d : defines)
    {
        defineBlock += "#define ";
        defineBlock += d;
        defineBlock += " 1\n";
    }

    if (sections.versionLine.empty())
        return defineBlock + sections.body;

    std::string source;
    source.reserve(sections.versionLine.size() + sections.preVersionTrivia.size() + defineBlock.size() + sections.body.size() + 8u);
    source += sections.versionLine;
    if (!source.empty() && source.back() != '\n')
        source.push_back('\n');
    source += sections.preVersionTrivia;
    source += defineBlock;
    source += sections.body;
    return source;
}

} // namespace engine::renderer::internal
