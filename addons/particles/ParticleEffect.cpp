#include "addons/particles/ParticleEffect.hpp"
#include "addons/particles/ParticleSerialization.hpp"

#include "core/Debug.hpp"
#include "core/PathUtf8.hpp"

#include <fstream>
#include <iterator>
#include <utility>

namespace engine::particles {

std::string ParticleEffectToJson(const ParticleEmitterComponent& effect)
{
    serialization::JsonWriter w;
    w.BeginObject();
    w.WriteString("type", "ParticleEffect");
    w.WriteUint("version", 1u);
    WriteParticleEffectFields(w, effect);
    w.EndObject();
    return w.GetString();
}

bool ParticleEffectFromJson(const std::string& json, ParticleEmitterComponent& out, std::string* error)
{
    std::string parseError;
    const serialization::JsonValue root = serialization::JsonParser::Parse(json, parseError);
    if (!parseError.empty() || !root.IsObject())
    {
        if (error)
            *error = parseError.empty() ? "kein JSON-Objekt" : parseError;
        return false;
    }
    const auto* type = root.Get("type");
    if (!type || !type->IsString() || type->AsString() != "ParticleEffect")
    {
        if (error)
            *error = "type ist nicht \"ParticleEffect\"";
        return false;
    }
    ReadParticleEffectFields(root, out);
    return true;
}

bool SaveParticleEffectFile(const std::filesystem::path& file, const ParticleEmitterComponent& effect,
                            std::string* error)
{
    std::error_code ec;
    if (file.has_parent_path())
        std::filesystem::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out)
    {
        if (error)
            *error = "Datei nicht schreibbar: " + PathToUtf8(file);
        return false;
    }
    out << ParticleEffectToJson(effect) << '\n';
    return static_cast<bool>(out);
}

bool LoadParticleEffectFile(const std::filesystem::path& file, ParticleEmitterComponent& out,
                            std::string* error)
{
    std::ifstream in(file, std::ios::binary);
    if (!in)
    {
        if (error)
            *error = "Datei nicht gefunden: " + PathToUtf8(file);
        return false;
    }
    const std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return ParticleEffectFromJson(json, out, error);
}

void ApplyParticleEffect(const ParticleEmitterComponent& effect, ParticleEmitterComponent& target)
{
    std::string effectPath = std::move(target.effectPath);
    const bool playing = target.playing;
    target = effect;
    target.effectPath = std::move(effectPath);
    target.playing = playing;
}

void ParticleEffectLibrary::SetAssetRoot(const std::filesystem::path& root)
{
    if (root == m_root)
        return;
    m_root = root;
    m_entries.clear();
}

std::filesystem::path ParticleEffectLibrary::ResolvePath(const std::string& path) const
{
    std::filesystem::path file = PathFromUtf8(path);
    if (file.is_relative() && !m_root.empty())
        file = m_root / file;
    return file;
}

ParticleEmitterComponent* ParticleEffectLibrary::FindMutable(const std::string& path)
{
    if (path.empty())
        return nullptr;
    auto [it, inserted] = m_entries.try_emplace(path);
    Entry& entry = it->second;
    if (inserted)
    {
        std::string error;
        entry.valid = LoadParticleEffectFile(ResolvePath(path), entry.effect, &error);
        if (entry.valid)
        {
            entry.effect.effectPath.clear();
            entry.effect.playing = true;
        }
        else
        {
            Debug::LogWarning("Particles: Effekt '%s' nicht ladbar (%s) - Emitter nutzt eigene Werte",
                              path.c_str(), error.c_str());
        }
    }
    return entry.valid ? &entry.effect : nullptr;
}

const ParticleEmitterComponent* ParticleEffectLibrary::Find(const std::string& path)
{
    return FindMutable(path);
}

bool ParticleEffectLibrary::Save(const std::string& path, std::string* error)
{
    const auto it = m_entries.find(path);
    if (it == m_entries.end() || !it->second.valid)
    {
        if (error)
            *error = "Effekt nicht geladen: " + path;
        return false;
    }
    return SaveParticleEffectFile(ResolvePath(path), it->second.effect, error);
}

bool ParticleEffectLibrary::Store(const std::string& path, const ParticleEmitterComponent& effect,
                                  std::string* error)
{
    if (path.empty())
    {
        if (error)
            *error = "leerer Pfad";
        return false;
    }
    Entry entry{};
    entry.effect = effect;
    entry.effect.effectPath.clear();
    entry.effect.playing = true;
    entry.valid = true;
    if (!SaveParticleEffectFile(ResolvePath(path), entry.effect, error))
        return false;
    m_entries[path] = std::move(entry);
    return true;
}

void ParticleEffectLibrary::Reload(const std::string& path)
{
    m_entries.erase(path);
}

} // namespace engine::particles
