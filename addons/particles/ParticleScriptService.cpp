#include "addons/particles/ParticleScriptService.hpp"

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

bool ParticleScriptService::SpawnEffect(std::string_view effectPath, const math::Vec3& position)
{
    return !effectPath.empty() && m_system.SpawnEffect(std::string(effectPath), position);
}

} // namespace engine::particles
