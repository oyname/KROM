#include "addons/audio/AudioSystem.hpp"
#include "core/PathUtf8.hpp"
#include "addons/audio/AudioComponents.hpp"
#include "core/Debug.hpp"
#include "ecs/Components.hpp"

#include "miniaudio.h"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace engine::audio {
namespace {

struct SoundDeleter
{
    void operator()(ma_sound* sound) const noexcept
    {
        if (sound)
        {
            ma_sound_uninit(sound);
            delete sound;
        }
    }
};
using SoundPtr = std::unique_ptr<ma_sound, SoundDeleter>;

math::Vec3 WorldPositionOf(const ecs::World& world, EntityID entity)
{
    if (const auto* wt = world.Get<WorldTransformComponent>(entity))
        return wt->position;
    if (const auto* tc = world.Get<TransformComponent>(entity))
        return tc->localPosition;
    return { 0.f, 0.f, 0.f };
}

} // namespace

struct AudioSystem::Impl
{
    struct Source
    {
        SoundPtr    sound;
        std::string loadedPath;
        bool        startedOnce = false;
        bool        pendingPlay = false;
        bool        loadFailed  = false;
    };

    ma_engine                              engine{};
    bool                                   initialized = false;
    std::filesystem::path                  assetRoot;
    std::unordered_map<uint32_t, Source>   sources;     // Entity → Quelle
    std::vector<SoundPtr>                  oneShots;
    std::unordered_set<std::string>        warnedPaths;

    std::filesystem::path Resolve(std::string_view clipPath) const
    {
        std::filesystem::path path = PathFromUtf8(clipPath);
        if (path.is_relative() && !assetRoot.empty())
            path = assetRoot / path;
        return path;
    }

    SoundPtr Load(std::string_view clipPath, bool spatial)
    {
        const std::filesystem::path path = Resolve(clipPath);
        SoundPtr sound(new ma_sound{});
        const ma_uint32 flags = MA_SOUND_FLAG_DECODE;
#ifdef _WIN32
        const ma_result result = ma_sound_init_from_file_w(&engine, path.wstring().c_str(), flags,
                                                           nullptr, nullptr, sound.get());
#else
        const ma_result result = ma_sound_init_from_file(&engine, PathToUtf8(path).c_str(), flags,
                                                         nullptr, nullptr, sound.get());
#endif
        if (result != MA_SUCCESS)
        {
            delete sound.release();   // nie initialisiert → kein ma_sound_uninit
            if (warnedPaths.insert(PathToUtf8(path)).second)
                Debug::LogWarning("AudioSystem: '%s' konnte nicht geladen werden (Fehler %d).",
                                  PathToUtf8(path).c_str(), static_cast<int>(result));
            return nullptr;
        }
        ma_sound_set_spatialization_enabled(sound.get(), spatial ? MA_TRUE : MA_FALSE);
        return sound;
    }

    void UpdateListener(const ecs::World& world)
    {
        bool found = false;
        world.View<AudioListenerComponent>([&](EntityID id, const AudioListenerComponent& listener) {
            if (found)
                return;
            found = true;
            const auto* wt = world.Get<WorldTransformComponent>(id);
            const math::Vec3 position = wt ? wt->position : WorldPositionOf(world, id);
            const math::Quat rotation = wt ? wt->rotation : math::Quat::Identity();
            const math::Vec3 forward  = rotation.Rotate({ 0.f, 0.f, -1.f });
            const math::Vec3 up       = rotation.Rotate({ 0.f, 1.f, 0.f });
            ma_engine_listener_set_position(&engine, 0, position.x, position.y, position.z);
            ma_engine_listener_set_direction(&engine, 0, forward.x, forward.y, forward.z);
            ma_engine_listener_set_world_up(&engine, 0, up.x, up.y, up.z);
            ma_engine_set_volume(&engine, std::max(0.f, listener.volume));
        });
        if (!found)
        {
            ma_engine_listener_set_position(&engine, 0, 0.f, 0.f, 0.f);
            ma_engine_listener_set_direction(&engine, 0, 0.f, 0.f, -1.f);
            ma_engine_listener_set_world_up(&engine, 0, 0.f, 1.f, 0.f);
        }
    }
};

AudioSystem::AudioSystem() : m_impl(std::make_unique<Impl>()) {}

AudioSystem::~AudioSystem()
{
    Shutdown();
}

bool AudioSystem::Initialize(const AudioSettings& settings)
{
    Impl& impl = *m_impl;
    if (impl.initialized)
        return true;

    ma_engine_config config = ma_engine_config_init();
    if (settings.noDevice)
    {
        config.noDevice   = MA_TRUE;
        config.channels   = 2;
        config.sampleRate = 48000;
    }
    const ma_result result = ma_engine_init(&config, &impl.engine);
    if (result != MA_SUCCESS)
    {
        Debug::LogWarning("AudioSystem: Audio konnte nicht initialisiert werden (Fehler %d) - Spiel laeuft ohne Ton.",
                          static_cast<int>(result));
        return false;
    }
    ma_engine_set_volume(&impl.engine, std::max(0.f, settings.masterVolume));
    impl.initialized = true;
    Debug::Log("AudioSystem: miniaudio initialisiert%s", settings.noDevice ? " (ohne Geraet)" : "");
    return true;
}

void AudioSystem::Shutdown()
{
    if (!m_impl || !m_impl->initialized)
        return;
    Impl& impl = *m_impl;
    impl.sources.clear();
    impl.oneShots.clear();
    ma_engine_uninit(&impl.engine);
    impl.initialized = false;
}

bool AudioSystem::IsInitialized() const noexcept
{
    return m_impl->initialized;
}

void AudioSystem::SetAssetRoot(const std::filesystem::path& assetRoot)
{
    m_impl->assetRoot = assetRoot;
}

void AudioSystem::Update(ecs::World& world)
{
    Impl& impl = *m_impl;
    if (!impl.initialized)
        return;

    impl.UpdateListener(world);

    std::unordered_set<uint32_t> seen;
    world.View<AudioSourceComponent>([&](EntityID id, const AudioSourceComponent& source) {
        if (const auto* active = world.Get<ActiveComponent>(id); active && !active->active)
            return;
        if (source.clipPath.empty())
            return;
        seen.insert(id.value);

        Impl::Source& state = impl.sources[id.value];
        if (!state.sound || state.loadedPath != source.clipPath)
        {
            if (state.loadFailed && state.loadedPath == source.clipPath)
                return;
            state.sound = impl.Load(source.clipPath, source.spatial);
            state.loadedPath = source.clipPath;
            state.loadFailed = !state.sound;
            state.startedOnce = false;
            if (!state.sound)
                return;
        }

        ma_sound* sound = state.sound.get();
        ma_sound_set_volume(sound, std::max(0.f, source.volume));
        ma_sound_set_pitch(sound, std::max(0.01f, source.pitch));
        ma_sound_set_looping(sound, source.loop ? MA_TRUE : MA_FALSE);
        ma_sound_set_spatialization_enabled(sound, source.spatial ? MA_TRUE : MA_FALSE);
        if (source.spatial)
        {
            const math::Vec3 p = WorldPositionOf(world, id);
            ma_sound_set_position(sound, p.x, p.y, p.z);
            ma_sound_set_min_distance(sound, std::max(0.01f, source.minDistance));
            ma_sound_set_max_distance(sound, std::max(source.minDistance, source.maxDistance));
        }

        if ((source.playOnStart && !state.startedOnce) || state.pendingPlay)
        {
            ma_sound_seek_to_pcm_frame(sound, 0);
            ma_sound_start(sound);
            state.startedOnce = true;
            state.pendingPlay = false;
        }
    });

    // Quellen entfernter/inaktiver Entities stoppen und freigeben.
    for (auto it = impl.sources.begin(); it != impl.sources.end();)
    {
        if (seen.count(it->first))
            ++it;
        else
            it = impl.sources.erase(it);
    }

    impl.oneShots.erase(std::remove_if(impl.oneShots.begin(), impl.oneShots.end(),
        [](const SoundPtr& s) { return !s || ma_sound_at_end(s.get()) || !ma_sound_is_playing(s.get()); }),
        impl.oneShots.end());
}

void AudioSystem::StopAll()
{
    Impl& impl = *m_impl;
    impl.sources.clear();
    impl.oneShots.clear();
}

bool AudioSystem::Play(EntityID entity)
{
    Impl& impl = *m_impl;
    if (!impl.initialized || !entity.IsValid())
        return false;
    Impl::Source& state = impl.sources[entity.value];
    if (state.sound)
    {
        ma_sound_seek_to_pcm_frame(state.sound.get(), 0);
        ma_sound_start(state.sound.get());
        state.startedOnce = true;
        return true;
    }
    state.pendingPlay = true;   // spielt nach dem Laden im naechsten Update
    return true;
}

bool AudioSystem::Stop(EntityID entity)
{
    Impl& impl = *m_impl;
    auto it = impl.sources.find(entity.value);
    if (!impl.initialized || it == impl.sources.end())
        return false;
    it->second.pendingPlay = false;
    if (it->second.sound)
        ma_sound_stop(it->second.sound.get());
    return true;
}

bool AudioSystem::IsPlaying(EntityID entity) const
{
    const Impl& impl = *m_impl;
    auto it = impl.sources.find(entity.value);
    return impl.initialized && it != impl.sources.end() && it->second.sound &&
           ma_sound_is_playing(it->second.sound.get()) == MA_TRUE;
}

bool AudioSystem::PlayOneShot(std::string_view clipPath, float volume)
{
    Impl& impl = *m_impl;
    if (!impl.initialized || clipPath.empty())
        return false;
    SoundPtr sound = impl.Load(clipPath, false);
    if (!sound)
        return false;
    ma_sound_set_volume(sound.get(), std::max(0.f, volume));
    ma_sound_start(sound.get());
    impl.oneShots.push_back(std::move(sound));
    return true;
}

bool AudioSystem::PlayOneShotAt(std::string_view clipPath, const math::Vec3& position, float volume)
{
    Impl& impl = *m_impl;
    if (!impl.initialized || clipPath.empty())
        return false;
    SoundPtr sound = impl.Load(clipPath, true);
    if (!sound)
        return false;
    ma_sound_set_position(sound.get(), position.x, position.y, position.z);
    ma_sound_set_volume(sound.get(), std::max(0.f, volume));
    ma_sound_start(sound.get());
    impl.oneShots.push_back(std::move(sound));
    return true;
}

void AudioSystem::SetMasterVolume(float volume)
{
    if (m_impl->initialized)
        ma_engine_set_volume(&m_impl->engine, std::max(0.f, volume));
}

size_t AudioSystem::SourceCount() const noexcept
{
    size_t count = 0;
    for (const auto& [id, source] : m_impl->sources)
        count += source.sound ? 1u : 0u;
    return count;
}

size_t AudioSystem::OneShotCount() const noexcept
{
    return m_impl->oneShots.size();
}

} // namespace engine::audio
