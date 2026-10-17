#pragma once
// =============================================================================
// KROM Engine - addons/script/IAudioService.hpp
// Schnittstelle, ueber die Scripts Audio abspielen. Ohne Abhaengigkeit zum
// Audio-Addon: die Runtime stellt einen Adapter auf AudioSystem bereit.
// Ohne Audio ist ScriptServices::audio null und die Helfer sind neutral.
// =============================================================================
#include "core/Math.hpp"
#include "core/Types.hpp"

#include <string_view>

namespace engine::script {

class IAudioService
{
public:
    virtual ~IAudioService() = default;

    // AudioSourceComponent einer Entity von vorn starten / stoppen.
    virtual bool Play(EntityID entity) = 0;
    virtual bool Stop(EntityID entity) = 0;
    [[nodiscard]] virtual bool IsPlaying(EntityID entity) const = 0;

    // Einmaliger Sound ohne Entity (Pfad relativ zum Asset-Root).
    virtual bool PlayOneShot(std::string_view clipPath, float volume) = 0;
    virtual bool PlayOneShotAt(std::string_view clipPath, const math::Vec3& position, float volume) = 0;
};

} // namespace engine::script
