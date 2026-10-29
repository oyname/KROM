#include "TestFramework.hpp"
#include "addons/script/ScriptList.hpp"
#include "addons/text/TextRenderer.hpp"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

using namespace engine;
using namespace engine::addons::text;

namespace {

// Systemschrift fuer die Backtests; ohne sie werden diese Tests uebersprungen.
std::filesystem::path FindTestFont()
{
#if defined(_WIN32)
    std::filesystem::path dir = "C:/Windows/Fonts";
    if (const char* windir = std::getenv("WINDIR"); windir && *windir)
        dir = std::filesystem::path(windir) / "Fonts";
    for (const char* name : { "arial.ttf", "segoeui.ttf" })
    {
        std::error_code ec;
        if (std::filesystem::is_regular_file(dir / name, ec))
            return dir / name;
    }
#endif
    return {};
}

void TestDecodeUtf8(test::TestContext& ctx)
{
    const std::string text = "A\xC3\xA4\xE2\x82\xAC\xF0\x9F\x98\x80";   // A ä € 😀
    size_t i = 0u;
    CHECK_EQ(ctx, DecodeUtf8(text, i), 0x41u);
    CHECK_EQ(ctx, DecodeUtf8(text, i), 0xE4u);
    CHECK_EQ(ctx, DecodeUtf8(text, i), 0x20ACu);
    CHECK_EQ(ctx, DecodeUtf8(text, i), 0x1F600u);
    CHECK_EQ(ctx, i, text.size());

    // Abgeschnittene und ungueltige Folgen: Ersatzzeichen, Fortschritt garantiert.
    const std::string broken = "\xC3" "x\xE2\x82";
    i = 0u;
    CHECK_EQ(ctx, DecodeUtf8(broken, i), 0xFFFDu);
    CHECK_EQ(ctx, i, size_t{1});
    CHECK_EQ(ctx, DecodeUtf8(broken, i), 0x78u);
    CHECK_EQ(ctx, DecodeUtf8(broken, i), 0xFFFDu);
    CHECK_EQ(ctx, i, broken.size());
}

void TestFontAtlasRejectsGarbage(test::TestContext& ctx)
{
    FontAtlas atlas;
    const std::vector<uint8_t> garbage(64u, 0x5Au);
    CHECK(ctx, !atlas.LoadFromMemory(garbage.data(), garbage.size(), 32.f));
    CHECK(ctx, !atlas.IsLoaded());
    CHECK_NULL(ctx, atlas.FindGlyph('A'));
    CHECK(ctx, !atlas.LoadFromFile("does/not/exist.ttf"));
}

void TestFontAtlasBakesLatin1(test::TestContext& ctx)
{
    const std::filesystem::path font = FindTestFont();
    if (font.empty())
    {
        std::fprintf(stdout, "  # skip: keine Systemschrift gefunden\n");
        return;
    }
    FontAtlas atlas;
    CHECK(ctx, atlas.LoadFromFile(font, 48.f));
    CHECK(ctx, atlas.IsLoaded());
    CHECK_EQ(ctx, atlas.Pixels().size(), static_cast<size_t>(atlas.Width()) * atlas.Height() * 4u);
    CHECK_GT(ctx, atlas.Ascent(), 0.f);
    CHECK_GE(ctx, atlas.LineHeight(), 48.f);

    const FontGlyph* a = atlas.FindGlyph('A');
    const FontGlyph* ae = atlas.FindGlyph(0xC4u);     // Ä
    const FontGlyph* sz = atlas.FindGlyph(0xDFu);     // ß
    CHECK(ctx, a && ae && sz);
    CHECK(ctx, a && a->advance > 0.f && a->x1 > a->x0 && a->y0 < 0.f);
    CHECK(ctx, a && a->u1 > a->u0 && a->v1 > a->v0 && a->u1 <= 1.f && a->v1 <= 1.f);
    // Ä reicht ueber das A hinaus (Punkte).
    CHECK(ctx, a && ae && ae->y0 < a->y0);
    // Nicht gebacken → Fallback '?'.
    CHECK(ctx, atlas.FindGlyph(0x4E2Du) == atlas.FindGlyph('?'));
}

void TestRendererLayout(test::TestContext& ctx)
{
    const std::filesystem::path font = FindTestFont();
    if (font.empty())
    {
        std::fprintf(stdout, "  # skip: keine Systemschrift gefunden\n");
        return;
    }
    TextRenderer text;
    CHECK(ctx, text.LoadFont(font, 48.f));

    // Leerzeichen erzeugen keine Quads, sichtbare Zeichen je 6 Vertices.
    text.Draw("A B", 10.f, 20.f, 24.f);
    CHECK_EQ(ctx, text.PendingVertexCount(), 12u);
    text.Draw("\xC3\x84\n\xC3\x9F", 0.f, 0.f, 24.f);           // Ä \n ß
    CHECK_EQ(ctx, text.PendingVertexCount(), 24u);
    text.Draw("unsichtbar", 0.f, 0.f, 24.f, math::Vec4{1.f, 1.f, 1.f, 0.f});
    text.Draw("", 0.f, 0.f, 24.f);
    CHECK_EQ(ctx, text.PendingVertexCount(), 24u);
    text.Clear();
    CHECK_EQ(ctx, text.PendingVertexCount(), 0u);

    // Masse skalieren linear mit der Groesse; zwei Zeilen sind hoeher als eine.
    const math::Vec2 small = text.Measure("0/3", 20.f);
    const math::Vec2 large = text.Measure("0/3", 40.f);
    CHECK_GT(ctx, small.x, 0.f);
    CHECK(ctx, std::abs(large.x - 2.f * small.x) < 0.01f);
    CHECK(ctx, std::abs(small.y - 20.f) < 0.01f);
    const math::Vec2 twoLines = text.Measure("WWW\nW", 20.f);
    CHECK(ctx, twoLines.y > small.y + 15.f);
    CHECK(ctx, std::abs(twoLines.x - text.Measure("WWW", 20.f).x) < 0.01f);
}

void TestRendererWithoutFontIsSilent(test::TestContext& ctx)
{
    TextRenderer text;
    text.SetFontCandidates({ "does/not/exist.ttf" });
    text.Draw("Hallo", 0.f, 0.f, 24.f);
    CHECK_EQ(ctx, text.PendingVertexCount(), 0u);
    const math::Vec2 size = text.Measure("Hallo", 24.f);
    CHECK_EQ(ctx, size.x, 0.f);
    text.SetViewport(800u, 600u);
    CHECK_EQ(ctx, text.ViewportSize().x, 800.f);
    CHECK_EQ(ctx, text.ViewportSize().y, 600.f);
}

class FakeTextService final : public script::ITextService
{
public:
    void Draw(std::string_view text, float x, float y, float size, const math::Vec4& color) override
    {
        lastText = std::string(text);
        lastX = x; lastY = y; lastSize = size; lastColor = color;
        ++draws;
    }
    math::Vec2 Measure(std::string_view text, float size) override
    {
        return { static_cast<float>(text.size()) * size, size };
    }
    math::Vec2 ScreenSize() const override { return { 1280.f, 720.f }; }

    std::string lastText;
    float lastX = 0.f, lastY = 0.f, lastSize = 0.f;
    math::Vec4 lastColor{};
    int draws = 0;
};

void TestScriptContextText(test::TestContext& ctx)
{
    // Ohne Dienst neutral.
    script::ScriptFrameContext neutral(0.016f, NULL_ENTITY);
    neutral.DrawScreenText("x", 1.f, 2.f);
    CHECK_EQ(ctx, neutral.GetScreenSize().x, 0.f);
    CHECK_EQ(ctx, neutral.MeasureScreenText("abc").x, 0.f);

    FakeTextService service;
    script::ScriptServices services{};
    services.text = &service;
    script::ScriptFrameContext context(0.016f, NULL_ENTITY, services);
    context.DrawScreenText("1/3", 5.f, 6.f, 32.f, math::Vec4{1.f, 0.5f, 0.f, 1.f});
    CHECK_EQ(ctx, service.draws, 1);
    CHECK_EQ(ctx, service.lastText, std::string("1/3"));
    CHECK_EQ(ctx, service.lastX, 5.f);
    CHECK_EQ(ctx, service.lastSize, 32.f);
    CHECK_EQ(ctx, service.lastColor.y, 0.5f);
    context.DrawScreenText("Standard", 0.f, 0.f);
    CHECK_EQ(ctx, service.lastSize, 24.f);
    CHECK_EQ(ctx, service.lastColor.w, 1.f);
    CHECK_EQ(ctx, context.MeasureScreenText("abc", 10.f).x, 30.f);
    CHECK_EQ(ctx, context.GetScreenSize().y, 720.f);
}

} // namespace

int RunTextTests()
{
    test::TestSuite suite("Text");
    suite
        .Add("UTF-8 decode",                     TestDecodeUtf8)
        .Add("FontAtlas rejects garbage",        TestFontAtlasRejectsGarbage)
        .Add("FontAtlas bakes ASCII + Latin-1",  TestFontAtlasBakesLatin1)
        .Add("Renderer layout and measure",      TestRendererLayout)
        .Add("Renderer without font is silent",  TestRendererWithoutFontIsSilent)
        .Add("Script context text helpers",      TestScriptContextText);
    return suite.Run();
}
