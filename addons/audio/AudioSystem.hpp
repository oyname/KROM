#pragma once
// =============================================================================
// KROM Engine - addons/audio/AudioSystem.hpp
// Audio-Wiedergabe des Audio-Addons (intern miniaudio, nach aussen frei davon).
//
// Pro Frame nach dem TransformSystem: Update(world) gleicht AudioSource- und
// AudioListener-Komponenten ab (Laden, Parameter, Position, playOnStart) und
// raeumt beendete One-Shots auf.
// =============================================================================
#include "core/Math.hpp"
#include "core/Types.hpp"
#include "ecs/World.hpp"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string_view>

namespace engine::audio {

struct AudioSettings
{
    // true: ohne Audiogeraet (Tests/Server). Sounds werden verwaltet, aber nicht hoerbar.
    bool  noDevice     = false;
    float masterVolume = 1.f;
};

class AudioSystem
{
public:
    AudioSystem();
    ~AudioSystem();
    AudioSystem(const AudioSystem&)            = delete;
    AudioSystem& operator=(const AudioSystem&) = delete;

    bool Initialize(const AudioSettings& settings = {});
    void Shutdown();
    [[nodiscard]] bool IsInitialized() const noexcept;

    // Basis fuer relative Clip-Pfade (Asset-Root des Projekts).
    void SetAssetRoot(const std::filesystem::path& assetRoot);

    void Update(ecs::World& world);

    // Stoppt alles und vergisst alle Quellen (z.B. bei Szenenwechsel).
    void StopAll();

    // Quelle einer Entity (AudioSourceComponent) von vorn starten / stoppen.
    // Play vor dem ersten Update wird vorgemerkt.
    bool Play(EntityID entity);
    bool Stop(EntityID entity);
    [[nodiscard]] bool IsPlaying(EntityID entity) const;

    // Einmaliger Sound ohne Entity, 2D bzw. an einer Weltposition.
    bool PlayOneShot(std::string_view clipPath, float volume = 1.f);
    bool PlayOneShotAt(std::string_view clipPath, const math::Vec3& position, float volume = 1.f);

    void SetMasterVolume(float volume);

    [[nodiscard]] size_t SourceCount() const noexcept;      // geladene Entity-Quellen
    [[nodiscard]] size_t OneShotCount() const noexcept;     // laufende One-Shots

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace engine::audio
