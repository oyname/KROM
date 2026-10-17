// =============================================================================
// KROM Engine - tests/test_audio.cpp
// Tests fuer das Audio-Addon (AudioSystem ohne Geraet, Serialisierung).
// =============================================================================
#include "TestFramework.hpp"

#include "addons/audio/AudioComponents.hpp"
#include "addons/audio/AudioSerialization.hpp"
#include "addons/audio/AudioSystem.hpp"
#include "ecs/Components.hpp"
#include "ecs/World.hpp"
#include "serialization/SceneSerializer.hpp"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

using namespace engine;
using namespace engine::audio;

namespace {

ecs::ComponentMetaRegistry MakeAudioRegistry()
{
    ecs::ComponentMetaRegistry reg;
    RegisterCoreComponents(reg);
    RegisterAudioComponents(reg);
    return reg;
}

// Schreibt eine kurze Mono-WAV (16 Bit, 22050 Hz, Sinus) und liefert den Pfad.
std::filesystem::path WriteTestWav(const char* name, float seconds)
{
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "krom_audio_test";
    std::filesystem::create_directories(dir);
    const std::filesystem::path path = dir / name;

    const uint32_t sampleRate = 22050u;
    const uint32_t frames = static_cast<uint32_t>(seconds * sampleRate);
    std::vector<int16_t> samples(frames);
    for (uint32_t i = 0; i < frames; ++i)
        samples[i] = static_cast<int16_t>(8000.f * std::sin(6.2831853f * 440.f * static_cast<float>(i) / sampleRate));

    const uint32_t dataBytes = frames * 2u;
    std::ofstream f(path, std::ios::binary);
    auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    f.write("RIFF", 4); u32(36u + dataBytes); f.write("WAVE", 4);
    f.write("fmt ", 4); u32(16u); u16(1u); u16(1u); u32(sampleRate); u32(sampleRate * 2u); u16(2u); u16(16u);
    f.write("data", 4); u32(dataBytes);
    f.write(reinterpret_cast<const char*>(samples.data()), dataBytes);
    return path;
}

void TestSourcePlaysOnStartAndStops(test::TestContext& ctx)
{
    const std::filesystem::path wav = WriteTestWav("beep.wav", 0.5f);
    auto reg = MakeAudioRegistry();
    ecs::World world(reg);
    const EntityID speaker = world.CreateEntity();
    world.Add<WorldTransformComponent>(speaker).position = { 3.f, 0.f, 0.f };
    AudioSourceComponent source{};
    source.clipPath = "beep.wav";            // relativ zum Asset-Root
    source.loop = true;
    world.Add<AudioSourceComponent>(speaker, source);

    AudioSystem audio;
    AudioSettings settings{};
    settings.noDevice = true;
    CHECK(ctx, audio.Initialize(settings));
    audio.SetAssetRoot(wav.parent_path());
    audio.Update(world);

    CHECK_EQ(ctx, audio.SourceCount(), size_t{1});
    CHECK(ctx, audio.IsPlaying(speaker));    // playOnStart
    CHECK(ctx, audio.Stop(speaker));
    CHECK(ctx, !audio.IsPlaying(speaker));
    CHECK(ctx, audio.Play(speaker));
    CHECK(ctx, audio.IsPlaying(speaker));

    world.DestroyEntity(speaker);
    audio.Update(world);
    CHECK_EQ(ctx, audio.SourceCount(), size_t{0});
}

void TestMissingFileAndOneShots(test::TestContext& ctx)
{
    const std::filesystem::path wav = WriteTestWav("shot.wav", 0.2f);
    auto reg = MakeAudioRegistry();
    ecs::World world(reg);
    const EntityID broken = world.CreateEntity();
    AudioSourceComponent missing{};
    missing.clipPath = "gibt_es_nicht.wav";
    world.Add<AudioSourceComponent>(broken, missing);

    AudioSystem audio;
    AudioSettings settings{};
    settings.noDevice = true;
    audio.Initialize(settings);
    audio.SetAssetRoot(wav.parent_path());
    audio.Update(world);
    audio.Update(world);                      // kein erneuter Ladeversuch, kein Absturz
    CHECK_EQ(ctx, audio.SourceCount(), size_t{0});
    CHECK(ctx, !audio.IsPlaying(broken));

    CHECK(ctx, audio.PlayOneShot("shot.wav", 0.5f));
    CHECK(ctx, audio.PlayOneShotAt("shot.wav", { 1.f, 2.f, 3.f }));
    CHECK(ctx, !audio.PlayOneShot("gibt_es_nicht.wav"));
    CHECK_EQ(ctx, audio.OneShotCount(), size_t{2});
    audio.StopAll();
    CHECK_EQ(ctx, audio.OneShotCount(), size_t{0});
}

void TestAudioSerializationRoundtrip(test::TestContext& ctx)
{
    auto reg = MakeAudioRegistry();
    ecs::World world(reg);
    const EntityID e = world.CreateEntity();
    AudioSourceComponent source{};
    source.clipPath = "Sounds/music.ogg";
    source.volume = 0.4f;
    source.loop = true;
    source.spatial = false;
    world.Add<AudioSourceComponent>(e, source);
    world.Add<AudioListenerComponent>(e).volume = 0.7f;

    serialization::SceneSerializer ser(world);
    ser.RegisterDefaultHandlers();
    RegisterAudioSerializationHandlers(ser);
    const std::string json = ser.SerializeToJson("Test");

    auto reg2 = MakeAudioRegistry();
    ecs::World world2(reg2);
    serialization::SceneDeserializer deser(world2);
    deser.RegisterDefaultHandlers();
    RegisterAudioDeserializationHandlers(deser);
    CHECK(ctx, deser.DeserializeFromJson(json).success);

    int found = 0;
    world2.ForEachAlive([&](EntityID id) {
        const auto* s = world2.Get<AudioSourceComponent>(id);
        if (!s) return;
        ++found;
        CHECK_EQ(ctx, s->clipPath, std::string("Sounds/music.ogg"));
        CHECK(ctx, std::fabs(s->volume - 0.4f) < 1e-5f && s->loop && !s->spatial);
        const auto* l = world2.Get<AudioListenerComponent>(id);
        CHECK(ctx, l && std::fabs(l->volume - 0.7f) < 1e-5f);
    });
    CHECK_EQ(ctx, found, 1);
}

} // namespace

int RunAudioTests()
{
    test::TestSuite suite("Audio");
    suite
        .Add("Source plays on start and stops", TestSourcePlaysOnStartAndStops)
        .Add("Missing file and one-shots",      TestMissingFileAndOneShots)
        .Add("Serialization roundtrip",         TestAudioSerializationRoundtrip);
    return suite.Run();
}
