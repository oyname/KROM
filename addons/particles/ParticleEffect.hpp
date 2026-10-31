#pragma once
// =============================================================================
// KROM Engine - addons/particles/ParticleEffect.hpp
// .particle-Assets: Effekt-Einstellungen eines Emitters als eigene Datei.
//
// Format (JSON): { "type": "ParticleEffect", "version": 1, <Emitter-Felder> }
// mit denselben Feldern wie ParticleEmitterComponent in der Szene, aber ohne
// effectPath und playing (die gehoeren zum einzelnen Emitter).
//
// ParticleEffectLibrary laedt Assets bei Bedarf relativ zum Asset-Root und
// haelt sie im Speicher. Fehlende oder kaputte Dateien werden einmal gemeldet
// und bis Reload/Clear nicht erneut versucht.
// =============================================================================

#include "addons/particles/ParticleComponents.hpp"

#include <filesystem>
#include <string>
#include <unordered_map>

namespace engine::particles {

inline constexpr const char* kParticleEffectExtension = ".particle";

[[nodiscard]] std::string ParticleEffectToJson(const ParticleEmitterComponent& effect);
// Liest die Effekt-Felder; effectPath und playing von out bleiben unberuehrt.
bool ParticleEffectFromJson(const std::string& json, ParticleEmitterComponent& out,
                            std::string* error = nullptr);

bool SaveParticleEffectFile(const std::filesystem::path& file, const ParticleEmitterComponent& effect,
                            std::string* error = nullptr);
bool LoadParticleEffectFile(const std::filesystem::path& file, ParticleEmitterComponent& out,
                            std::string* error = nullptr);

// Uebernimmt die Effekt-Einstellungen von effect nach target; effectPath und
// playing von target bleiben erhalten.
void ApplyParticleEffect(const ParticleEmitterComponent& effect, ParticleEmitterComponent& target);

class ParticleEffectLibrary
{
public:
    void SetAssetRoot(const std::filesystem::path& root);
    [[nodiscard]] const std::filesystem::path& AssetRoot() const noexcept { return m_root; }

    // path relativ zum Asset-Root (UTF-8, wie im Emitter gespeichert).
    // nullptr, wenn die Datei fehlt oder nicht lesbar ist.
    [[nodiscard]] const ParticleEmitterComponent* Find(const std::string& path);
    // Editor: Einstellungen im Speicher aendern (wirkt sofort auf alle
    // Emitter mit diesem Asset), Save schreibt sie in die Datei.
    [[nodiscard]] ParticleEmitterComponent* FindMutable(const std::string& path);
    bool Save(const std::string& path, std::string* error = nullptr);
    // Schreibt effect als neues bzw. ueberschriebenes Asset und uebernimmt es.
    bool Store(const std::string& path, const ParticleEmitterComponent& effect,
               std::string* error = nullptr);
    // Verwirft den Speicherstand; der naechste Find liest die Datei neu.
    void Reload(const std::string& path);
    void Clear() noexcept { m_entries.clear(); }

    [[nodiscard]] std::filesystem::path ResolvePath(const std::string& path) const;

private:
    struct Entry
    {
        ParticleEmitterComponent effect{};
        bool valid = false;
    };

    std::filesystem::path m_root;
    std::unordered_map<std::string, Entry> m_entries;
};

} // namespace engine::particles
