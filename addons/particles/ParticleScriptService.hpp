#pragma once
// =============================================================================
// KROM Engine - addons/particles/ParticleScriptService.hpp
// Adapter von IParticleService (Script-Addon) auf ParticleSystem + World.
// Play/Stop schalten ParticleEmitterComponent::playing, Burst/EmitAt gehen an
// die Simulation (wirksam im naechsten ParticleSystem::Update desselben Frames,
// da Scripts davor laufen).
// =============================================================================

#include "addons/particles/ParticleSystem.hpp"
#include "addons/script/IParticleService.hpp"
#include "ecs/World.hpp"

namespace engine::particles {

class ParticleScriptService final : public script::IParticleService
{
public:
    ParticleScriptService(ParticleSystem& system, ecs::World& world) noexcept
        : m_system(system), m_world(world) {}

    bool Play(EntityID entity) override;
    bool Stop(EntityID entity, bool clear) override;
    [[nodiscard]] bool IsPlaying(EntityID entity) const override;
    bool Burst(EntityID entity, uint32_t count) override;
    bool EmitAt(EntityID templateEntity, const math::Vec3& position, uint32_t count) override;
    [[nodiscard]] uint32_t ParticleCount(EntityID entity) const override;

private:
    ParticleSystem& m_system;
    ecs::World&     m_world;
};

} // namespace engine::particles
