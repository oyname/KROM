#pragma once
// =============================================================================
// KROM Engine - addons/text/TextRenderer.hpp
// Optionales Text-Addon fuer die Runtime. Keine Core-/ECS-Abhaengigkeit.
//
// FontAtlas backt eine TTF per stb_truetype (CPU, ohne GPU testbar) in einen
// Glyphen-Atlas: ASCII 32..126 und Latin-1 160..255 (Umlaute, ß).
// TextRenderer sammelt pro Frame Text-Quads in Bildschirm-Pixeln und zeichnet
// sie im UI-Pass ("frame.ui") alpha-geblendet direkt auf den Backbuffer, also
// nach dem Tonemapping. Kein Layout-, Button- oder Fokussystem: nur Text an
// einer Position, Zeilenumbruch per '\n'.
// =============================================================================

#include "core/Math.hpp"
#include "renderer/IDevice.hpp"
#include "renderer/RenderPipelineTypes.hpp"
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string_view>
#include <vector>

namespace engine::addons::text {

// Ein Glyph relativ zum Stift auf der Grundlinie, in Pixeln der Backgroesse
// (y nach unten). uv im Atlas [0..1].
struct FontGlyph
{
    float x0 = 0.f, y0 = 0.f, x1 = 0.f, y1 = 0.f;
    float u0 = 0.f, v0 = 0.f, u1 = 0.f, v1 = 0.f;
    float advance = 0.f;
    bool  visible = false;   // false: kein Umriss (Leerzeichen) → kein Quad
};

class FontAtlas
{
public:
    // bakePixelHeight: Zeilenhoehe (Ascent - Descent) der gebackenen Glyphen.
    bool LoadFromFile(const std::filesystem::path& ttfPath, float bakePixelHeight = 48.f);
    bool LoadFromMemory(const uint8_t* data, size_t size, float bakePixelHeight = 48.f);

    [[nodiscard]] bool IsLoaded() const noexcept { return !m_pixels.empty(); }

    // Unbekannte Zeichen fallen auf '?' zurueck; nullptr nur ohne Font.
    [[nodiscard]] const FontGlyph* FindGlyph(uint32_t codepoint) const noexcept;

    [[nodiscard]] float BakePixelHeight() const noexcept { return m_bakePixelHeight; }
    [[nodiscard]] float Ascent() const noexcept { return m_ascent; }
    [[nodiscard]] float LineHeight() const noexcept { return m_lineHeight; }

    // RGBA8, rgb = weiss, a = Abdeckung.
    [[nodiscard]] uint32_t Width() const noexcept { return m_width; }
    [[nodiscard]] uint32_t Height() const noexcept { return m_height; }
    [[nodiscard]] const std::vector<uint8_t>& Pixels() const noexcept { return m_pixels; }

private:
    std::vector<FontGlyph> m_glyphs;      // Index = Codepoint - kFirstCodepoint
    std::vector<uint8_t>   m_pixels;
    uint32_t m_width = 0u;
    uint32_t m_height = 0u;
    float m_bakePixelHeight = 0.f;
    float m_ascent = 0.f;
    float m_lineHeight = 0.f;
};

// Naechster Codepoint aus UTF-8 ab index (index wird weitergeschoben).
// Ungueltige Bytes ergeben U+FFFD.
[[nodiscard]] uint32_t DecodeUtf8(std::string_view text, size_t& index) noexcept;

struct TextVertex
{
    float x = 0.f, y = 0.f;      // Pixel; Flush rechnet in Clip-Space um
    float u = 0.f, v = 0.f;
    math::Vec4 color{1.f, 1.f, 1.f, 1.f};
};

class TextRenderer
{
public:
    TextRenderer() = default;
    ~TextRenderer();

    TextRenderer(const TextRenderer&) = delete;
    TextRenderer& operator=(const TextRenderer&) = delete;

    // Schriftquellen in Prioritaet; die erste ladbare wird beim ersten
    // Draw gebacken (ohne Text kostet das Addon nichts).
    void SetFontCandidates(std::vector<std::filesystem::path> candidates);
    bool LoadFont(const std::filesystem::path& ttfPath, float bakePixelHeight = 48.f);
    [[nodiscard]] bool EnsureFont();
    [[nodiscard]] const FontAtlas& Font() const noexcept { return m_font; }

    // Pro Frame vor Draw setzen: Backbuffergroesse und -format.
    void SetViewport(uint32_t width, uint32_t height) noexcept;
    void SetTargetFormat(renderer::Format colorFormat) noexcept;
    [[nodiscard]] math::Vec2 ViewportSize() const noexcept;

    // x/y: linke obere Ecke in Pixeln (0,0 = oben links), size = Zeilenhoehe
    // in Pixeln, color in sRGB (Alpha linear). Gilt nur fuer diesen Frame.
    void Draw(std::string_view utf8, float x, float y, float size,
              const math::Vec4& color = math::Vec4{1.f, 1.f, 1.f, 1.f});
    // Breite der laengsten Zeile und Gesamthoehe in Pixeln.
    [[nodiscard]] math::Vec2 Measure(std::string_view utf8, float size);

    void Clear() noexcept;
    [[nodiscard]] uint32_t PendingVertexCount() const noexcept;

    // Vor dem Rendern aufrufen (ausserhalb eines Render-Passes): legt Atlas,
    // Pipeline und Vertexbuffer fuer den anstehenden Text an.
    void Prepare(renderer::IDevice& device);
    void Flush(const renderer::RGExecContext& execCtx);
    void OnDeviceShutdown() noexcept;
    [[nodiscard]] renderer::FramePipelinePassCallback MakeFrameCallback();

private:
    struct GpuResources
    {
        ShaderHandle vertexShader = ShaderHandle::Invalid();
        ShaderHandle fragmentShader = ShaderHandle::Invalid();
        PipelineHandle pipeline = PipelineHandle::Invalid();
        renderer::Format pipelineFormat = renderer::Format::Unknown;
        BufferHandle vertexBuffer = BufferHandle::Invalid();
        uint32_t vertexCapacity = 0u;
        TextureHandle atlas = TextureHandle::Invalid();
        bool shadersFailed = false;
    };

    bool EnsureGpuResources(renderer::IDevice& device, uint32_t requiredVertices);
    void DestroyGpuResources(renderer::IDevice& device) noexcept;
    bool EnsureFontLocked();

    FontAtlas m_font;
    std::vector<std::filesystem::path> m_fontCandidates;
    bool m_fontAttempted = false;
    bool m_atlasDirty = false;

    uint32_t m_viewportWidth = 1u;
    uint32_t m_viewportHeight = 1u;
    renderer::Format m_targetFormat = renderer::Format::BGRA8_UNORM_SRGB;

    std::vector<TextVertex> m_vertices;
    GpuResources m_gpu{};
    renderer::IDevice* m_device = nullptr;
    mutable std::mutex m_mutex;
};

// Registriert den Text-Overlay als UI-Pass-Callback.
inline void AddTextCallback(renderer::FramePipelineCallbacks& callbacks, TextRenderer& text)
{
    callbacks.Register("frame.ui", text.MakeFrameCallback());
}

} // namespace engine::addons::text
