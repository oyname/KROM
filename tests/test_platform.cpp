// =============================================================================
// KROM Engine - tests/test_platform.cpp
// Tests: IInput (StandardInput/NullInput), IFilesystem (NullFilesystem/Std),
//        IPlatformTiming (FixedTiming/NullTiming)
// =============================================================================
#include "TestFramework.hpp"
#include "core/Debug.hpp"
#include "platform/IInput.hpp"
#include "platform/PlatformInput.hpp"
#include "platform/IFilesystem.hpp"
#include "platform/IPlatformTiming.hpp"
#include "platform/NullPlatform.hpp"

using namespace engine::platform;

// ==========================================================================
// IInput - NullInput gibt immer false/0
// ==========================================================================
static void TestNullInput(test::TestContext& ctx)
{
    NullInput input;
    input.BeginFrame();
    CHECK(ctx, !input.KeyDown(Key::Space));
    CHECK(ctx, !input.KeyHit(Key::A));
    CHECK(ctx, !input.KeyReleased(Key::Escape));
    CHECK(ctx, !input.MouseButtonDown(MouseButton::Left));
    CHECK_EQ(ctx, input.MouseX(), 0);
    CHECK_EQ(ctx, input.MouseDeltaX(), 0);
    CHECK_EQ(ctx, input.MouseScrollDelta(), 0.f);
}

// ==========================================================================
// IInput - StandardInput Key-State-Machine
// ==========================================================================
static void TestStandardInputKeyboard(test::TestContext& ctx)
{
    StandardInput input;

    // Frame 1: Space gedrückt
    input.BeginFrame();
    input.OnKeyEvent({ Key::Space, true, false });
    CHECK(ctx,  input.KeyDown(Key::Space));
    CHECK(ctx,  input.KeyHit(Key::Space));     // erstmals diesen Frame
    CHECK(ctx, !input.KeyReleased(Key::Space));

    // Frame 2: Space weiterhin gehalten (repeat)
    input.BeginFrame();
    input.OnKeyEvent({ Key::Space, true, true }); // repeat=true
    CHECK(ctx,  input.KeyDown(Key::Space));
    CHECK(ctx, !input.KeyHit(Key::Space));     // kein erneutes Hit bei Repeat
    CHECK(ctx, !input.KeyReleased(Key::Space));

    // Frame 3: Space losgelassen
    input.BeginFrame();
    input.OnKeyEvent({ Key::Space, false, false });
    CHECK(ctx, !input.KeyDown(Key::Space));
    CHECK(ctx, !input.KeyHit(Key::Space));
    CHECK(ctx,  input.KeyReleased(Key::Space));

    // Frame 4: Released-Flag gelöscht
    input.BeginFrame();
    CHECK(ctx, !input.KeyDown(Key::Space));
    CHECK(ctx, !input.KeyReleased(Key::Space));
}

// ==========================================================================
// IInput - StandardInput Mouse
// ==========================================================================
static void TestStandardInputMouse(test::TestContext& ctx)
{
    StandardInput input;

    input.BeginFrame();
    input.OnMouseMoveEvent({ 100, 200, 5, -3 });
    CHECK_EQ(ctx, input.MouseX(), 100);
    CHECK_EQ(ctx, input.MouseY(), 200);
    CHECK_EQ(ctx, input.MouseDeltaX(), 5);
    CHECK_EQ(ctx, input.MouseDeltaY(), -3);

    input.OnMouseButtonEvent({ MouseButton::Left, true, 100, 200 });
    CHECK(ctx,  input.MouseButtonDown(MouseButton::Left));
    CHECK(ctx,  input.MouseButtonHit(MouseButton::Left));
    CHECK(ctx, !input.MouseButtonDown(MouseButton::Right));

    input.OnMouseScrollEvent({ 1.5f });
    CHECK_EQ(ctx, input.MouseScrollDelta(), 1.5f);

    // Delta wird in BeginFrame() zurückgesetzt
    input.BeginFrame();
    CHECK_EQ(ctx, input.MouseDeltaX(), 0);
    CHECK_EQ(ctx, input.MouseDeltaY(), 0);
    CHECK_EQ(ctx, input.MouseScrollDelta(), 0.f);
    // Position und Button-Down bleiben
    CHECK_EQ(ctx, input.MouseX(), 100);
    CHECK(ctx, input.MouseButtonDown(MouseButton::Left));
}

// ==========================================================================
// NullFilesystem - InjectFile + Read/Write
// ==========================================================================
static void TestNullFilesystem(test::TestContext& ctx)
{
    NullFilesystem fs;

    // Nicht vorhandene Datei
    CHECK(ctx, !fs.FileExists("missing.txt"));
    std::vector<uint8_t> out;
    CHECK(ctx, !fs.ReadFile("missing.txt", out));

    // Datei injizieren
    fs.InjectText("hello.txt", "KROM Engine");
    CHECK(ctx,  fs.FileExists("hello.txt"));

    std::string text;
    CHECK(ctx, fs.ReadText("hello.txt", text));
    CHECK_EQ(ctx, text, std::string("KROM Engine"));

    FileStats s = fs.GetFileStats("hello.txt");
    CHECK(ctx,  s.exists);
    CHECK_EQ(ctx, s.sizeBytes, static_cast<uint64_t>(11u));

    // WriteFile + ReadFile Round-Trip
    const uint8_t data[] = { 0x01, 0x02, 0x03 };
    CHECK(ctx, fs.WriteFile("bin.bin", data, sizeof(data)));
    std::vector<uint8_t> read;
    CHECK(ctx, fs.ReadFile("bin.bin", read));
    CHECK_EQ(ctx, read.size(), 3u);
    CHECK_EQ(ctx, read[0], uint8_t(0x01));
    CHECK_EQ(ctx, read[2], uint8_t(0x03));
}

// ==========================================================================
// NullFilesystem - AssetRoot + ResolveAssetPath
// ==========================================================================
static void TestNullFilesystemAssetRoot(test::TestContext& ctx)
{
    NullFilesystem fs;
    fs.SetAssetRoot("assets/");
    fs.InjectFile("assets/texture.png", { 0xAB });

    const std::string resolved = fs.ResolveAssetPath("texture.png");
    CHECK_EQ(ctx, resolved, std::string("assets/texture.png"));
    CHECK(ctx, fs.FileExists("assets/texture.png"));
}

// ==========================================================================
// NullTiming - alles 0
// ==========================================================================
static void TestNullTiming(test::TestContext& ctx)
{
    NullTiming t;
    t.BeginFrame();
    CHECK_EQ(ctx, t.GetDeltaSeconds(), 0.0);
    CHECK_EQ(ctx, t.GetTimeSeconds(),  0.0);
    CHECK_EQ(ctx, t.GetFrameCount(),   uint64_t(0));
}

// ==========================================================================
// FixedTiming - deterministisch
// ==========================================================================
static void TestFixedTiming(test::TestContext& ctx)
{
    FixedTiming t(1.0 / 60.0);

    // Vor erstem BeginFrame: alles 0
    CHECK_EQ(ctx, t.GetFrameCount(), uint64_t(0));
    CHECK_EQ(ctx, t.GetTimeSeconds(), 0.0);

    // 3 Frames simulieren
    t.BeginFrame();
    CHECK_EQ(ctx, t.GetFrameCount(), uint64_t(1));
    CHECK(ctx, t.GetDeltaSecondsF() > 0.f);

    t.BeginFrame();
    t.BeginFrame();
    CHECK_EQ(ctx, t.GetFrameCount(), uint64_t(3));

    // Zeit = 3 * (1/60)
    const double expected = 3.0 / 60.0;
    const double diff = t.GetTimeSeconds() - expected;
    CHECK(ctx, diff > -1e-9 && diff < 1e-9);

    // FPS = 60
    CHECK_EQ(ctx, t.GetSmoothedFPS(), 60.f);

    // GetDeltaSecondsF konsistent mit GetDeltaSeconds
    CHECK(ctx, std::abs(t.GetDeltaSecondsF() - static_cast<float>(t.GetDeltaSeconds())) < 1e-6f);
}

// ==========================================================================
// StdTiming - sanity checks (kein deterministischer Wert, nur Plausibilität)
// ==========================================================================
static void TestStdTiming(test::TestContext& ctx)
{
    StdTiming t;
    t.BeginFrame();
    t.BeginFrame();

    CHECK_EQ(ctx, t.GetFrameCount(), uint64_t(2));
    CHECK(ctx, t.GetTimeSeconds() >= 0.0);
    CHECK(ctx, t.GetDeltaSeconds() >= 0.0);
    CHECK(ctx, t.GetRawTimestampSeconds() >= t.GetTimeSeconds());
}

// ==========================================================================
// Run all platform tests
// ==========================================================================
int RunPlatformTests()
{
    engine::Debug::MinLevel = engine::LogLevel::Fatal;

    test::TestSuite suite("Platform");
    suite
        .Add("NullInput",                    TestNullInput)
        .Add("StandardInput keyboard",       TestStandardInputKeyboard)
        .Add("StandardInput mouse",          TestStandardInputMouse)
        .Add("NullFilesystem CRUD",          TestNullFilesystem)
        .Add("NullFilesystem asset root",    TestNullFilesystemAssetRoot)
        .Add("NullTiming",                   TestNullTiming)
        .Add("FixedTiming deterministic",    TestFixedTiming)
        .Add("StdTiming sanity",             TestStdTiming);

    return suite.Run();
}
