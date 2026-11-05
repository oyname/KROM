#include "addons/particles/ParticleScriptService.hpp"
#include "core/PathUtf8.hpp"
#include <limits>
#include <algorithm>

namespace engine::particles {

bool ParticleScriptService::Play(EntityID entity)
{
    auto* emitter = m_world.Get<ParticleEmitterComponent>(entity);
    if (!emitter)
        return false;
    emitter->playing = true;
    m_system.Restart(entity);
    return true;
}

bool ParticleScriptService::Stop(EntityID entity, bool clear)
{
    auto* emitter = m_world.Get<ParticleEmitterComponent>(entity);
    if (!emitter)
        return false;
    emitter->playing = false;
    if (clear)
        m_system.ClearParticles(entity);
    return true;
}

bool ParticleScriptService::IsPlaying(EntityID entity) const
{
    const auto* emitter = m_world.Get<ParticleEmitterComponent>(entity);
    return emitter && emitter->playing;
}

bool ParticleScriptService::Burst(EntityID entity, uint32_t count)
{
    if (!m_world.Has<ParticleEmitterComponent>(entity))
        return false;
    m_system.Burst(entity, count);
    return true;
}

bool ParticleScriptService::EmitAt(EntityID templateEntity, const math::Vec3& position, uint32_t count)
{
    const auto* emitter = m_world.Get<ParticleEmitterComponent>(templateEntity);
    if (!emitter)
        return false;
    // Ausrichtung der Vorlage uebernehmen, damit die Startgeschwindigkeit wie
    // im Editor eingestellt wirkt.
    const auto* transform = m_world.Get<WorldTransformComponent>(templateEntity);
    m_system.EmitAt(*emitter, position, count, transform ? transform->rotation : math::Quat::Identity());
    return true;
}

uint32_t ParticleScriptService::ParticleCount(EntityID entity) const
{
    return m_system.ParticleCount(entity);
}

void ParticleScriptService::DestroyWhenFinished(EntityID root)
{
    if (m_world.IsAlive(root) && !m_world.Has<ParticleEffectAutoDestroyComponent>(root))
        m_world.Add<ParticleEffectAutoDestroyComponent>(root, ParticleEffectAutoDestroyComponent{});
}

bool ParticleScriptService::SetEmitterTexture(EntityID entity, const std::string& texturePath)
{
    auto* emitter = m_world.Get<ParticleEmitterComponent>(entity);
    const auto path = PathFromUtf8(texturePath).lexically_normal();
    if (!emitter || texturePath.find('\0') != std::string::npos || path.has_root_path() ||
        (!path.empty() && *path.begin() == ".."))
        return false;
    emitter->texturePath = texturePath;
    return true;
}

bool ParticleScriptService::SetEmitterAtlasGrid(EntityID entity, uint32_t columns, uint32_t rows)
{
    auto* emitter = m_world.Get<ParticleEmitterComponent>(entity);
    if (!emitter || columns == 0u || rows == 0u ||
        uint64_t(columns) * rows > std::numeric_limits<uint32_t>::max())
        return false;
    emitter->flipbookColumns = columns;
    emitter->flipbookRows = rows;
    emitter->frames.clear();
    return true;
}

bool ParticleScriptService::SetEmitterFrames(EntityID entity, std::span<const ParticleFrame> frames)
{
    auto* emitter = m_world.Get<ParticleEmitterComponent>(entity);
    if (!emitter || frames.empty() || frames.size() > std::numeric_limits<uint32_t>::max() ||
        !std::all_of(frames.begin(), frames.end(), [](const ParticleFrame& frame) { return frame.IsValid(); }))
        return false;
    std::vector<ParticleFrame> copy(frames.begin(), frames.end());
    emitter->frames = std::move(copy);
    return true;
}

uint32_t ParticleScriptService::GetEmitterFrameCount(EntityID entity)
{
    const auto* emitter = m_world.Get<ParticleEmitterComponent>(entity);
    if (!emitter) return 0u;
    if (!emitter->frames.empty()) return static_cast<uint32_t>(emitter->frames.size());
    return static_cast<uint32_t>(std::min(uint64_t(std::max(emitter->flipbookColumns, 1u)) *
        std::max(emitter->flipbookRows, 1u), uint64_t(std::numeric_limits<uint32_t>::max())));
}

ParticleID ParticleScriptService::SpawnParticle(EntityID emitter, const math::Vec3& position)
{
    return m_system.SpawnParticle(m_world, emitter, position);
}

bool ParticleScriptService::HasParticle(ParticleID particle) const
{
    const EntityID emitter = m_system.ParticleEmitter(particle);
    return m_world.IsAlive(emitter) && m_world.Has<ParticleEmitterComponent>(emitter);
}

bool ParticleScriptService::SetParticlePosition(ParticleID particle, const math::Vec3& position)
{
    return HasParticle(particle) && m_system.SetParticlePosition(particle, position);
}

bool ParticleScriptService::SetParticleVelocity(ParticleID particle, const math::Vec3& velocity)
{
    return HasParticle(particle) && m_system.SetParticleVelocity(particle, velocity);
}

bool ParticleScriptService::SetParticleMotionEnabled(ParticleID particle, bool enabled)
{
    return HasParticle(particle) && m_system.SetParticleMotionEnabled(particle, enabled);
}

bool ParticleScriptService::SetParticleSize(ParticleID particle, const math::Vec2& size)
{
    return HasParticle(particle) && m_system.SetParticleSize(particle, size);
}

bool ParticleScriptService::SetParticleFrame(ParticleID particle, uint32_t frame)
{
    return HasParticle(particle) && m_system.SetParticleFrame(particle, frame);
}

bool ParticleScriptService::PlayParticleFrames(ParticleID particle, uint32_t firstFrame, uint32_t lastFrame, float fps, bool loop)
{
    return HasParticle(particle) && m_system.PlayParticleFrames(particle, firstFrame, lastFrame, fps, loop);
}

bool ParticleScriptService::PlayParticleFramesOverLifetime(ParticleID particle, uint32_t firstFrame, uint32_t lastFrame)
{
    return HasParticle(particle) && m_system.PlayParticleFramesOverLifetime(particle, firstFrame, lastFrame);
}

bool ParticleScriptService::SetParticleFrameProgress(ParticleID particle, uint32_t firstFrame, uint32_t lastFrame, float progress)
{
    return HasParticle(particle) && m_system.SetParticleFrameProgress(particle, firstFrame, lastFrame, progress);
}

bool ParticleScriptService::SetParticleAnimationPaused(ParticleID particle, bool paused)
{
    return HasParticle(particle) && m_system.SetParticleAnimationPaused(particle, paused);
}

std::optional<uint32_t> ParticleScriptService::GetParticleFrame(ParticleID particle)
{
    return HasParticle(particle) ? m_system.GetParticleFrame(particle) : std::nullopt;
}

bool ParticleScriptService::IsParticleAlive(ParticleID particle)
{
    return HasParticle(particle);
}

bool ParticleScriptService::RemoveParticle(ParticleID particle)
{
    return HasParticle(particle) && m_system.RemoveParticle(particle);
}

bool ParticleScriptService::PlayParticleClip(ParticleID particle, std::string_view name)
{
    return HasParticle(particle) && m_system.PlayParticleClip(particle, name);
}

std::optional<ParticleClip> ParticleScriptService::FindEmitterClip(EntityID entity, std::string_view name)
{
    const auto* emitter = m_world.Get<ParticleEmitterComponent>(entity);
    if (!emitter || !IsParticleClipNameValid(name)) return std::nullopt;
    const auto clip = std::find_if(emitter->clips.begin(), emitter->clips.end(), [&](const ParticleClip& value) { return value.name == name; });
    if (clip == emitter->clips.end()) return std::nullopt;
    ParticleClip result = *clip;
    const auto range = ResolveParticleFrameRange(result.first, result.last, GetEmitterFrameCount(entity));
    result.first = range.first;
    result.last = range.last;
    return result;
}

} // namespace engine::particles
