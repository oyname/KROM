#pragma once
// =============================================================================
// KROM Engine - addons/audio/AudioComponents.hpp
// Audio-Komponenten des Audio-Addons. Ohne Abhaengigkeit zu miniaudio, damit
// Editor, Prefabs und Szenen-Loader sie ohne die Wiedergabe nutzen koennen.
//
//   • AudioSourceComponent: spielt eine Datei (wav/mp3/flac/ogg) ab, 2D oder
//     raeumlich an der Position der Entity.
//   • AudioListenerComponent: "Ohr" der Szene, typischerweise an der Kamera.
//     Ohne Listener hoert die Szene vom Weltursprung aus.
// =============================================================================
#include "ecs/ComponentMeta.hpp"

#include <string>

namespace engine {

struct AudioSourceComponent
{
    std::string clipPath;            // relativ zum Asset-Root des Projekts
    float volume       = 1.f;
    float pitch        = 1.f;
    bool  loop         = false;
    bool  playOnStart  = true;       // startet, sobald die Entity in der Szene ist
    bool  spatial      = true;       // false: 2D (Musik, UI-Sounds)
    float minDistance  = 1.f;        // volle Lautstaerke bis hierhin
    float maxDistance  = 50.f;       // ab hier nicht mehr leiser
};

struct AudioListenerComponent
{
    float volume = 1.f;              // Gesamtlautstaerke
};

inline void RegisterAudioComponents(ecs::ComponentMetaRegistry& registry)
{
    using namespace ecs;
    RegisterComponent<AudioSourceComponent>(registry, "AudioSourceComponent");
    RegisterComponent<AudioListenerComponent>(registry, "AudioListenerComponent");
}

} // namespace engine
