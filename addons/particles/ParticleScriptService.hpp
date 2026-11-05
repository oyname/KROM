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
    void DestroyWhenFinished(EntityID root) override;

    bool SetEmitterTexture(EntityID emitter, const std::string& texturePath) override;
    bool SetEmitterAtlasGrid(EntityID emitter, uint32_t columns, uint32_t rows) override;
    bool SetEmitterFrames(EntityID emitter, std::span<const ParticleFrame> frames) override;
    uint32_t GetEmitterFrameCount(EntityID emitter) override;
    ParticleID SpawnParticle(EntityID emitter, const math::Vec3& position) override;
    bool SetParticlePosition(ParticleID particle, const math::Vec3& position) override;
    bool SetParticleVelocity(ParticleID particle, const math::Vec3& velocity) override;
    bool SetParticleMotionEnabled(ParticleID particle, bool enabled) override;
    bool SetParticleSize(ParticleID particle, const math::Vec2& size) override;
    bool SetParticleFrame(ParticleID particle, uint32_t frame) override;
    bool PlayParticleFrames(ParticleID particle, uint32_t firstFrame, uint32_t lastFrame, float fps, bool loop = false) override;
    bool PlayParticleFramesOverLifetime(ParticleID particle, uint32_t firstFrame, uint32_t lastFrame) override;
    bool SetParticleFrameProgress(ParticleID particle, uint32_t firstFrame, uint32_t lastFrame, float progress) override;
    bool SetParticleAnimationPaused(ParticleID particle, bool paused) override;
    std::optional<uint32_t> GetParticleFrame(ParticleID particle) override;
    bool IsParticleAlive(ParticleID particle) override;
    bool RemoveParticle(ParticleID particle) override;
    bool PlayParticleClip(ParticleID particle, std::string_view name) override;
    std::optional<ParticleClip> FindEmitterClip(EntityID emitter, std::string_view name) override;

private:
    [[nodiscard]] bool HasParticle(ParticleID particle) const;
    ParticleSystem& m_system;
    ecs::World&     m_world;
};

} // namespace engine::particles
