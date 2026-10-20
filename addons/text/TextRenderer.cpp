#include "addons/text/TextRenderer.hpp"

#include "core/Debug.hpp"
#include "core/PathUtf8.hpp"
#include "renderer/ShaderBindingModel.hpp"
#include "renderer/ShaderCompiler.hpp"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <utility>

// stb_truetype aus dem imgui-Vendor-Ordner, statisch: kollidiert nicht mit der
// Implementierung in imgui_draw.cpp, wenn beide in eine EXE gelinkt werden.
#define STBRP_STATIC
#define STB_RECT_PACK_IMPLEMENTATION
#include "imstb_rectpack.h"
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "imstb_truetype.h"

namespace engine::addons::text {
namespace {

// Gebackene Bereiche: ASCII und Latin-1 (deutsche Umlaute, ß, °, ©, ...).
constexpr uint32_t kFirstCodepoint = 32u;
constexpr uint32_t kAsciiCount     = 95u;    // 32..126
constexpr uint32_t kLatin1First    = 160u;
constexpr uint32_t kLatin1Count    = 96u;    // 160..255
constexpr uint32_t kGlyphSlots     = 256u - kFirstCodepoint;

constexpr const char* kTextVsHlsl = R"(
// Vulkan: Locations = VertexSemantic-Wert (Position 0, TexCoord0 4, Color0 6).
#ifdef __spirv__
#define VK_LOC(n) [[vk::location(n)]]
#else
#define VK_LOC(n)
#endif

struct VSIn
{
    VK_LOC(0) float2 position : POSITION;
    VK_LOC(4) float2 uv       : TEXCOORD0;
    VK_LOC(6) float4 color    : COLOR0;
};

struct VSOut
{
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD0;
    float4 color    : COLOR0;
};

VSOut main(VSIn input)
{
    VSOut output;
    output.position = float4(input.position, 0.0, 1.0);
    output.uv = input.uv;
    output.color = input.color;
    return output;
}
)";

constexpr const char* kTextPsHlsl = R"(
Texture2D    uAtlas   : register(t8);
SamplerState uSampler : register(s1);

struct PSIn
{
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD0;
    float4 color    : COLOR0;
};

float4 main(PSIn input) : SV_TARGET
{
    const float coverage = uAtlas.Sample(uSampler, input.uv).a;
    return float4(input.color.rgb, input.color.a * coverage);
}
)";

constexpr const char* kTextVsGlsl = R"(#version 450 core
layout(location = 0) in vec2 inPosition;
layout(location = 4) in vec2 inUv;
layout(location = 6) in vec4 inColor;
layout(location = 0) out vec2 vUv;
layout(location = 1) out vec4 vColor;

void main()
{
    gl_Position = vec4(inPosition, 0.0, 1.0);
    vUv = inUv;
    vColor = inColor;
}
)";

constexpr const char* kTextFsGlsl = R"(#version 450 core
layout(binding = 8) uniform sampler2D uAtlas;
layout(location = 0) in vec2 vUv;
layout(location = 1) in vec4 vColor;
layout(location = 0) out vec4 fragColor;

void main()
{
    float coverage = texture(uAtlas, vUv).a;
    fragColor = vec4(vColor.rgb, vColor.a * coverage);
}
)";

[[nodiscard]] uint32_t NextCapacity(uint32_t required) noexcept
{
    uint32_t capacity = 256u;
    while (capacity < required)
        capacity <<= 1u;
    return capacity;
}

[[nodiscard]] bool IsSrgbFormat(renderer::Format format) noexcept
{
    return format == renderer::Format::RGBA8_UNORM_SRGB
        || format == renderer::Format::BGRA8_UNORM_SRGB;
}

[[nodiscard]] float SrgbToLinear(float c) noexcept
{
    c = std::clamp(c, 0.f, 1.f);
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

[[nodiscard]] ShaderHandle CreateTextShader(renderer::IDevice& device,
                                            assets::ShaderTargetProfile target,
                                            assets::ShaderStage stage,
                                            const char* debugName)
{
    const bool openGl = target == assets::ShaderTargetProfile::OpenGL_GLSL450;
    const bool vertex = stage == assets::ShaderStage::Vertex;
    const char* source = openGl ? (vertex ? kTextVsGlsl : kTextFsGlsl)
                                : (vertex ? kTextVsHlsl : kTextPsHlsl);
    const renderer::ShaderStageMask stageMask = vertex
        ? renderer::ShaderStageMask::Vertex
        : renderer::ShaderStageMask::Fragment;

    if (openGl)
        return device.CreateShaderFromSource(source, stageMask, "main", debugName);

    assets::ShaderAsset shader{};
    shader.debugName = debugName;
    shader.stage = stage;
    shader.sourceLanguage = assets::ShaderSourceLanguage::HLSL;
    shader.entryPoint = "main";
    shader.sourceCode = source;

    assets::CompiledShaderArtifact compiled{};
    std::string error;
    if (!renderer::ShaderCompiler::CompileForTarget(shader, target, compiled, &error))
    {
        Debug::LogError("Text: failed to compile shader '%s': %s", debugName, error.c_str());
        return ShaderHandle::Invalid();
    }
    if (!compiled.bytecode.empty())
        return device.CreateShaderFromBytecode(compiled.bytecode.data(), compiled.bytecode.size(),
                                               stageMask, debugName);
    if (target == assets::ShaderTargetProfile::Vulkan_SPIRV)
    {
        Debug::LogError("Text: shader '%s' produced no SPIR-V bytecode", debugName);
        return ShaderHandle::Invalid();
    }
    return device.CreateShaderFromSource(compiled.sourceText.empty() ? source : compiled.sourceText,
                                         stageMask,
                                         compiled.entryPoint.empty() ? "main" : compiled.entryPoint,
                                         debugName);
}

} // namespace

// ─── UTF-8 ────────────────────────────────────────────────────────────────────

uint32_t DecodeUtf8(std::string_view text, size_t& index) noexcept
{
    constexpr uint32_t kReplacement = 0xFFFDu;
    if (index >= text.size())
        return 0u;

    const auto byte = [&](size_t i) { return static_cast<uint8_t>(text[i]); };
    const uint8_t lead = byte(index);
    if (lead < 0x80u)
    {
        ++index;
        return lead;
    }

    uint32_t length = 0u;
    uint32_t codepoint = 0u;
    if ((lead & 0xE0u) == 0xC0u)      { length = 2u; codepoint = lead & 0x1Fu; }
    else if ((lead & 0xF0u) == 0xE0u) { length = 3u; codepoint = lead & 0x0Fu; }
    else if ((lead & 0xF8u) == 0xF0u) { length = 4u; codepoint = lead & 0x07u; }
    else
    {
        ++index;
        return kReplacement;
    }

    if (index + length > text.size())
    {
        index = text.size();
        return kReplacement;
    }
    for (uint32_t i = 1u; i < length; ++i)
    {
        const uint8_t next = byte(index + i);
        if ((next & 0xC0u) != 0x80u)
        {
            index += i;
            return kReplacement;
        }
        codepoint = (codepoint << 6u) | (next & 0x3Fu);
    }
    index += length;
    return codepoint;
}

// ─── FontAtlas ────────────────────────────────────────────────────────────────

bool FontAtlas::LoadFromFile(const std::filesystem::path& ttfPath, float bakePixelHeight)
{
    std::ifstream file(ttfPath, std::ios::binary);
    if (!file)
        return false;
    const std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)),
                                    std::istreambuf_iterator<char>());
    if (!LoadFromMemory(data.data(), data.size(), bakePixelHeight))
    {
        Debug::LogWarning("Text: '%s' is not a usable TrueType font", PathToUtf8(ttfPath).c_str());
        return false;
    }
    return true;
}

bool FontAtlas::LoadFromMemory(const uint8_t* data, size_t size, float bakePixelHeight)
{
    if (!data || size < 12u || bakePixelHeight < 4.f)
        return false;

    stbtt_fontinfo info{};
    const int offset = stbtt_GetFontOffsetForIndex(data, 0);
    if (offset < 0 || !stbtt_InitFont(&info, data, offset))
        return false;

    std::vector<stbtt_packedchar> packed(kAsciiCount + kLatin1Count);
    stbtt_pack_range ranges[2]{};
    ranges[0].font_size = bakePixelHeight;
    ranges[0].first_unicode_codepoint_in_range = static_cast<int>(kFirstCodepoint);
    ranges[0].num_chars = static_cast<int>(kAsciiCount);
    ranges[0].chardata_for_range = packed.data();
    ranges[1].font_size = bakePixelHeight;
    ranges[1].first_unicode_codepoint_in_range = static_cast<int>(kLatin1First);
    ranges[1].num_chars = static_cast<int>(kLatin1Count);
    ranges[1].chardata_for_range = packed.data() + kAsciiCount;

    // 2x2-Oversampling fuer saubere Kanten bei Skalierung; wachsen, bis alles passt.
    const uint32_t sizes[][2] = { {1024u, 1024u}, {2048u, 1024u}, {2048u, 2048u} };
    std::vector<uint8_t> alpha;
    uint32_t width = 0u;
    uint32_t height = 0u;
    bool packedOk = false;
    for (const auto& s : sizes)
    {
        width = s[0];
        height = s[1];
        alpha.assign(static_cast<size_t>(width) * height, 0u);
        stbtt_pack_context pc{};
        if (!stbtt_PackBegin(&pc, alpha.data(), static_cast<int>(width), static_cast<int>(height), 0, 1, nullptr))
            return false;
        stbtt_PackSetOversampling(&pc, 2u, 2u);
        packedOk = stbtt_PackFontRanges(&pc, data, 0, ranges, 2) != 0;
        stbtt_PackEnd(&pc);
        if (packedOk)
            break;
    }
    if (!packedOk)
        return false;

    int ascent = 0, descent = 0, lineGap = 0;
    stbtt_GetFontVMetrics(&info, &ascent, &descent, &lineGap);
    const float scale = stbtt_ScaleForPixelHeight(&info, bakePixelHeight);

    m_glyphs.assign(kGlyphSlots, FontGlyph{});
    const auto storeRange = [&](uint32_t firstCodepoint, uint32_t count, uint32_t packedOffset) {
        for (uint32_t i = 0u; i < count; ++i)
        {
            float penX = 0.f, penY = 0.f;
            stbtt_aligned_quad q{};
            stbtt_GetPackedQuad(packed.data(), static_cast<int>(width), static_cast<int>(height),
                                static_cast<int>(packedOffset + i), &penX, &penY, &q, 0);
            FontGlyph& g = m_glyphs[firstCodepoint + i - kFirstCodepoint];
            g.x0 = q.x0; g.y0 = q.y0; g.x1 = q.x1; g.y1 = q.y1;
            g.u0 = q.s0; g.v0 = q.t0; g.u1 = q.s1; g.v1 = q.t1;
            g.advance = penX;
            const int glyphIndex = stbtt_FindGlyphIndex(&info, static_cast<int>(firstCodepoint + i));
            g.visible = glyphIndex != 0 && !stbtt_IsGlyphEmpty(&info, glyphIndex) && g.x1 > g.x0 && g.y1 > g.y0;
        }
    };
    storeRange(kFirstCodepoint, kAsciiCount, 0u);
    storeRange(kLatin1First, kLatin1Count, kAsciiCount);

    m_pixels.assign(static_cast<size_t>(width) * height * 4u, 255u);
    for (size_t i = 0u; i < alpha.size(); ++i)
        m_pixels[i * 4u + 3u] = alpha[i];

    m_width = width;
    m_height = height;
    m_bakePixelHeight = bakePixelHeight;
    m_ascent = static_cast<float>(ascent) * scale;
    m_lineHeight = static_cast<float>(ascent - descent + lineGap) * scale;
    return true;
}

const FontGlyph* FontAtlas::FindGlyph(uint32_t codepoint) const noexcept
{
    if (m_glyphs.empty())
        return nullptr;
    const bool baked = (codepoint >= kFirstCodepoint && codepoint < kFirstCodepoint + kAsciiCount)
        || (codepoint >= kLatin1First && codepoint < kLatin1First + kLatin1Count);
    if (!baked)
        codepoint = '?';
    return &m_glyphs[codepoint - kFirstCodepoint];
}

// ─── TextRenderer ─────────────────────────────────────────────────────────────

TextRenderer::~TextRenderer()
{
    OnDeviceShutdown();
}

void TextRenderer::SetFontCandidates(std::vector<std::filesystem::path> candidates)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_fontCandidates = std::move(candidates);
    m_fontAttempted = false;
}

bool TextRenderer::LoadFont(const std::filesystem::path& ttfPath, float bakePixelHeight)
{
    FontAtlas font;
    if (!font.LoadFromFile(ttfPath, bakePixelHeight))
        return false;
    std::lock_guard<std::mutex> lock(m_mutex);
    m_font = std::move(font);
    m_fontAttempted = true;
    m_atlasDirty = true;
    return true;
}

bool TextRenderer::EnsureFont()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return EnsureFontLocked();
}

bool TextRenderer::EnsureFontLocked()
{
    if (m_font.IsLoaded())
        return true;
    if (m_fontAttempted)
        return false;
    m_fontAttempted = true;

    std::error_code ec;
    for (const std::filesystem::path& candidate : m_fontCandidates)
    {
        if (candidate.empty() || !std::filesystem::is_regular_file(candidate, ec))
            continue;
        if (m_font.LoadFromFile(candidate))
        {
            m_atlasDirty = true;
            Debug::Log("Text: font '%s' baked into %ux%u atlas",
                       PathToUtf8(candidate).c_str(), m_font.Width(), m_font.Height());
            return true;
        }
    }
    Debug::LogWarning("Text: no TrueType font found, screen text is disabled");
    return false;
}

void TextRenderer::SetViewport(uint32_t width, uint32_t height) noexcept
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_viewportWidth = std::max(1u, width);
    m_viewportHeight = std::max(1u, height);
}

void TextRenderer::SetTargetFormat(renderer::Format colorFormat) noexcept
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_targetFormat = colorFormat;
}

math::Vec2 TextRenderer::ViewportSize() const noexcept
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return { static_cast<float>(m_viewportWidth), static_cast<float>(m_viewportHeight) };
}

void TextRenderer::Draw(std::string_view utf8, float x, float y, float size, const math::Vec4& color)
{
    if (utf8.empty() || size <= 0.f || color.w <= 0.f)
        return;

    std::lock_guard<std::mutex> lock(m_mutex);
    if (!EnsureFontLocked())
        return;

    const float scale = size / m_font.BakePixelHeight();
    const float lineAdvance = m_font.LineHeight() * scale;
    float penX = x;
    float baseline = y + m_font.Ascent() * scale;

    size_t index = 0u;
    while (index < utf8.size())
    {
        const uint32_t codepoint = DecodeUtf8(utf8, index);
        if (codepoint == '\n')
        {
            penX = x;
            baseline += lineAdvance;
            continue;
        }
        if (codepoint == '\r')
            continue;
        if (codepoint == '\t')
        {
            if (const FontGlyph* space = m_font.FindGlyph(' '))
                penX += space->advance * scale * 4.f;
            continue;
        }

        const FontGlyph* g = m_font.FindGlyph(codepoint);
        if (!g)
            continue;
        if (g->visible)
        {
            const float x0 = penX + g->x0 * scale;
            const float x1 = penX + g->x1 * scale;
            const float y0 = baseline + g->y0 * scale;
            const float y1 = baseline + g->y1 * scale;
            const TextVertex tl{ x0, y0, g->u0, g->v0, color };
            const TextVertex tr{ x1, y0, g->u1, g->v0, color };
            const TextVertex bl{ x0, y1, g->u0, g->v1, color };
            const TextVertex br{ x1, y1, g->u1, g->v1, color };
            m_vertices.insert(m_vertices.end(), { tl, tr, bl, tr, br, bl });
        }
        penX += g->advance * scale;
    }
}

math::Vec2 TextRenderer::Measure(std::string_view utf8, float size)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (utf8.empty() || size <= 0.f || !EnsureFontLocked())
        return { 0.f, 0.f };

    const float scale = size / m_font.BakePixelHeight();
    float lineWidth = 0.f;
    float maxWidth = 0.f;
    uint32_t lines = 1u;
    size_t index = 0u;
    while (index < utf8.size())
    {
        const uint32_t codepoint = DecodeUtf8(utf8, index);
        if (codepoint == '\n')
        {
            maxWidth = std::max(maxWidth, lineWidth);
            lineWidth = 0.f;
            ++lines;
            continue;
        }
        if (codepoint == '\r')
            continue;
        const FontGlyph* g = m_font.FindGlyph(codepoint == '\t' ? ' ' : codepoint);
        if (g)
            lineWidth += g->advance * scale * (codepoint == '\t' ? 4.f : 1.f);
    }
    maxWidth = std::max(maxWidth, lineWidth);
    const float height = size + static_cast<float>(lines - 1u) * m_font.LineHeight() * scale;
    return { maxWidth, height };
}

void TextRenderer::Clear() noexcept
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_vertices.clear();
}

uint32_t TextRenderer::PendingVertexCount() const noexcept
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return static_cast<uint32_t>(m_vertices.size());
}

renderer::FramePipelinePassCallback TextRenderer::MakeFrameCallback()
{
    return [this](const renderer::RGExecContext& execCtx) {
        Flush(execCtx);
    };
}

bool TextRenderer::EnsureGpuResources(renderer::IDevice& device, uint32_t requiredVertices)
{
    if (m_device != nullptr && m_device != &device)
        DestroyGpuResources(*m_device);
    m_device = &device;

    if (m_atlasDirty && m_gpu.atlas.IsValid())
    {
        device.DestroyTexture(m_gpu.atlas);
        m_gpu.atlas = TextureHandle::Invalid();
    }
    if (!m_gpu.atlas.IsValid())
    {
        renderer::TextureDesc desc{};
        desc.width = m_font.Width();
        desc.height = m_font.Height();
        desc.format = renderer::Format::RGBA8_UNORM;
        desc.usage = renderer::ResourceUsage::ShaderResource | renderer::ResourceUsage::CopyDest;
        desc.initialState = renderer::ResourceState::ShaderRead;
        desc.debugName = "TextFontAtlas";
        m_gpu.atlas = device.CreateTexture(desc);
        if (!m_gpu.atlas.IsValid())
            return false;
        device.UploadTextureData(m_gpu.atlas, m_font.Pixels().data(), m_font.Pixels().size(), 0u, 0u);
        m_atlasDirty = false;
    }

    requiredVertices = std::max(requiredVertices, 6u);
    if (!m_gpu.vertexBuffer.IsValid() || m_gpu.vertexCapacity < requiredVertices)
    {
        if (m_gpu.vertexBuffer.IsValid())
            device.DestroyBuffer(m_gpu.vertexBuffer);
        const uint32_t capacity = NextCapacity(requiredVertices);
        renderer::BufferDesc desc{};
        desc.byteSize = static_cast<uint64_t>(capacity) * sizeof(TextVertex);
        desc.stride = sizeof(TextVertex);
        desc.type = renderer::BufferType::Vertex;
        desc.usage = renderer::ResourceUsage::VertexBuffer;
        desc.access = renderer::MemoryAccess::CpuWrite;
        desc.initialState = renderer::ResourceState::VertexBuffer;
        desc.debugName = "TextVB";
        m_gpu.vertexBuffer = device.CreateBuffer(desc);
        m_gpu.vertexCapacity = m_gpu.vertexBuffer.IsValid() ? capacity : 0u;
        if (!m_gpu.vertexBuffer.IsValid())
            return false;
    }

    if (m_gpu.pipeline.IsValid() && m_gpu.pipelineFormat == m_targetFormat)
        return true;
    if (m_gpu.shadersFailed)
        return false;

    if (!m_gpu.vertexShader.IsValid() || !m_gpu.fragmentShader.IsValid())
    {
        const assets::ShaderTargetProfile target = renderer::ShaderCompiler::ResolveTargetProfile(device);
        m_gpu.vertexShader = CreateTextShader(device, target, assets::ShaderStage::Vertex, "TextVS");
        m_gpu.fragmentShader = CreateTextShader(device, target, assets::ShaderStage::Fragment, "TextPS");
        if (!m_gpu.vertexShader.IsValid() || !m_gpu.fragmentShader.IsValid())
        {
            m_gpu.shadersFailed = true;
            return false;
        }
    }

    if (m_gpu.pipeline.IsValid())
        device.DestroyPipeline(m_gpu.pipeline);

    renderer::PipelineDesc pso{};
    pso.shaderStages.push_back(renderer::ShaderStageDesc{m_gpu.vertexShader, renderer::ShaderStageMask::Vertex, "main"});
    pso.shaderStages.push_back(renderer::ShaderStageDesc{m_gpu.fragmentShader, renderer::ShaderStageMask::Fragment, "main"});
    pso.vertexLayout.bindings.push_back(renderer::VertexBinding{0u, sizeof(TextVertex), renderer::VertexInputRate::PerVertex});
    pso.vertexLayout.attributes.push_back(renderer::VertexAttribute{renderer::VertexSemantic::Position, renderer::Format::RG32_FLOAT, 0u, 0u});
    pso.vertexLayout.attributes.push_back(renderer::VertexAttribute{renderer::VertexSemantic::TexCoord0, renderer::Format::RG32_FLOAT, 0u, static_cast<uint32_t>(offsetof(TextVertex, u))});
    pso.vertexLayout.attributes.push_back(renderer::VertexAttribute{renderer::VertexSemantic::Color0, renderer::Format::RGBA32_FLOAT, 0u, static_cast<uint32_t>(offsetof(TextVertex, color))});
    pso.rasterizer.cullMode = renderer::CullMode::None;
    pso.depthStencil.depthEnable = false;
    pso.depthStencil.depthWrite = false;
    pso.topology = renderer::PrimitiveTopology::TriangleList;
    // UI-Pass: direkt auf den Backbuffer, ohne Tiefe (wie das Tonemapping).
    pso.colorFormat = m_targetFormat;
    pso.depthFormat = renderer::Format::Unknown;
    pso.blendStates[0].blendEnable   = true;
    pso.blendStates[0].srcBlend      = renderer::BlendFactor::SrcAlpha;
    pso.blendStates[0].dstBlend      = renderer::BlendFactor::InvSrcAlpha;
    pso.blendStates[0].blendOp       = renderer::BlendOp::Add;
    pso.blendStates[0].srcBlendAlpha = renderer::BlendFactor::One;
    pso.blendStates[0].dstBlendAlpha = renderer::BlendFactor::InvSrcAlpha;
    pso.blendStates[0].blendOpAlpha  = renderer::BlendOp::Add;
    pso.debugName = "TextOverlayPipeline";
    m_gpu.pipeline = device.CreatePipeline(pso);
    m_gpu.pipelineFormat = m_targetFormat;
    return m_gpu.pipeline.IsValid();
}

void TextRenderer::Prepare(renderer::IDevice& device)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_vertices.empty() || !m_font.IsLoaded())
        return;
    (void)EnsureGpuResources(device, static_cast<uint32_t>(m_vertices.size()));
}

void TextRenderer::Flush(const renderer::RGExecContext& execCtx)
{
    if (!execCtx.device || !execCtx.cmd)
        return;

    std::vector<TextVertex> vertices;
    float width = 1.f, height = 1.f;
    bool srgbTarget = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_vertices.empty() || !m_font.IsLoaded())
            return;
        vertices.swap(m_vertices);
        if (!EnsureGpuResources(*execCtx.device, static_cast<uint32_t>(vertices.size())))
            return;
        width = static_cast<float>(m_viewportWidth);
        height = static_cast<float>(m_viewportHeight);
        srgbTarget = IsSrgbFormat(m_targetFormat);
    }

    // Pixel (oben links) → Clip-Space mit y nach oben. Ohne
    // GetClipSpaceAdjustment: deren Y-Flip gilt unter OpenGL nur fuer die
    // Offscreen-Szenenziele; der UI-Pass schreibt direkt in den Backbuffer, in
    // dem diese Orientierung auf allen Backends stimmt (Vulkan per Viewport).
    for (TextVertex& v : vertices)
    {
        v.x = v.x / width * 2.f - 1.f;
        v.y = 1.f - v.y / height * 2.f;
        if (srgbTarget)
        {
            v.color.x = SrgbToLinear(v.color.x);
            v.color.y = SrgbToLinear(v.color.y);
            v.color.z = SrgbToLinear(v.color.z);
        }
    }

    renderer::IDevice& device = *execCtx.device;
    const uint32_t vertexCount = static_cast<uint32_t>(vertices.size());
    device.UploadBufferData(m_gpu.vertexBuffer, vertices.data(),
                            static_cast<size_t>(vertexCount) * sizeof(TextVertex));

    execCtx.cmd->SetPipeline(m_gpu.pipeline);
    execCtx.cmd->SetShaderResource(renderer::TexSlots::PassSRV0, m_gpu.atlas, renderer::ShaderStageMask::Fragment);
    execCtx.cmd->SetSampler(renderer::SamplerSlots::LinearClamp, renderer::SamplerSlots::LinearClamp,
                            renderer::ShaderStageMask::Fragment);
    execCtx.cmd->SetVertexBuffer(0u, m_gpu.vertexBuffer, 0u);
    execCtx.cmd->Draw(vertexCount, 1u, 0u, 0u);
}

void TextRenderer::DestroyGpuResources(renderer::IDevice& device) noexcept
{
    if (m_gpu.vertexBuffer.IsValid())
        device.DestroyBuffer(m_gpu.vertexBuffer);
    if (m_gpu.atlas.IsValid())
        device.DestroyTexture(m_gpu.atlas);
    if (m_gpu.pipeline.IsValid())
        device.DestroyPipeline(m_gpu.pipeline);
    if (m_gpu.fragmentShader.IsValid())
        device.DestroyShader(m_gpu.fragmentShader);
    if (m_gpu.vertexShader.IsValid())
        device.DestroyShader(m_gpu.vertexShader);
    m_gpu = {};
    m_atlasDirty = m_font.IsLoaded();
}

void TextRenderer::OnDeviceShutdown() noexcept
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_device)
    {
        DestroyGpuResources(*m_device);
        m_device = nullptr;
    }
}

} // namespace engine::addons::text
