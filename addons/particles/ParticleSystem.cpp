#include "addons/particles/ParticleSystem.hpp"

#include <algorithm>
#include <cmath>

namespace engine::particles {
namespace {

// Laengere Frames (Haltepunkt, Ladezeit) nicht als einen Riesenschritt simulieren.
constexpr float kMaxStep = 0.1f;

[[nodiscard]] float Lerp(float a, float b, float t) noexcept { return a + (b - a) * t; }

} // namespace

float SrgbToLinear(float c) noexcept
{
    if (c <= 0.04045f)
        return std::max(c, 0.f) / 12.92f;
    return std::pow((c + 0.055f) / 1.055f, 2.4f);
}

ParticleSystem::ParticleSystem(uint32_t seed) noexcept
    : m_rng(seed != 0u ? seed : 1u)
{}

float ParticleSystem::Random01() noexcept
{
    // xorshift32: schnell, reproduzierbar, fuer Optik ausreichend.
    m_rng ^= m_rng << 13u;
    m_rng ^= m_rng >> 17u;
    m_rng ^= m_rng << 5u;
    return static_cast<float>(m_rng >> 8u) * (1.f / 16777216.f);
}

float ParticleSystem::RandomSigned() noexcept
{
    return Random01() * 2.f - 1.f;
}

void ParticleSystem::Clear() noexcept
{
    m_emitters.clear();
}

void ParticleSystem::Restart(EntityID entity)
{
    const auto it = m_emitters.find(entity);
    if (it == m_emitters.end())
        return;
    EmitterState& state = it->second;
    state.particles.clear();
    state.emitAccumulator = 0.f;
    state.time = 0.f;
    state.burstDone = false;
}

uint32_t ParticleSystem::ParticleCount() const noexcept
{
    size_t count = 0u;
    for (const auto& [id, state] : m_emitters)
        count += state.particles.size();
    return static_cast<uint32_t>(count);
}

uint32_t ParticleSystem::ParticleCount(EntityID entity) const noexcept
{
    const auto it = m_emitters.find(entity);
    return it != m_emitters.end() ? static_cast<uint32_t>(it->second.particles.size()) : 0u;
}

const std::vector<Particle>* ParticleSystem::Particles(EntityID entity) const noexcept
{
    const auto it = m_emitters.find(entity);
    return it != m_emitters.end() ? &it->second.particles : nullptr;
}

void ParticleSystem::Spawn(EmitterState& state, const WorldTransformComponent& transform)
{
    const ParticleEmitterComponent& p = state.params;
    if (state.particles.size() >= p.maxParticles)
        return;

    Particle particle{};
    particle.lifetime = std::max(0.01f,
        p.particleLifetime * (1.f + std::clamp(p.lifetimeRandom, 0.f, 1.f) * RandomSigned()));

    math::Vec3 offset{};
    if (p.emitterRadius > 0.f)
    {
        // Gleichverteilt in der Kugel: Verwerfen ausserhalb (im Mittel ~2 Versuche).
        for (int attempt = 0; attempt < 8; ++attempt)
        {
            offset = math::Vec3{RandomSigned(), RandomSigned(), RandomSigned()};
            if (offset.LengthSq() <= 1.f)
                break;
        }
        offset = offset * p.emitterRadius;
    }
    particle.position = transform.position + offset;

    const math::Vec3 localVelocity{
        p.initialVelocity.x + p.velocityRandom.x * RandomSigned(),
        p.initialVelocity.y + p.velocityRandom.y * RandomSigned(),
        p.initialVelocity.z + p.velocityRandom.z * RandomSigned(),
    };
    particle.velocity = transform.rotation.Rotate(localVelocity);
    state.particles.push_back(particle);
}

void ParticleSystem::Update(const ecs::World& world, float dt,
                            const std::unordered_set<EntityID>* only)
{
    dt = std::clamp(dt, 0.f, kMaxStep);

    for (auto& [id, state] : m_emitters)
        state.seen = false;

    world.View<ParticleEmitterComponent, WorldTransformComponent>(
        [&](EntityID id, const ParticleEmitterComponent& emitter, const WorldTransformComponent& transform)
    {
        if (only && only->count(id) == 0u)
            return;
        EmitterState& state = m_emitters[id];
        state.params = emitter;
        state.seen = true;

        // Bestehende Partikel altern lassen und bewegen.
        const float dragFactor = std::max(0.f, 1.f - emitter.drag * dt);
        auto& particles = state.particles;
        for (size_t i = 0u; i < particles.size();)
        {
            Particle& particle = particles[i];
            particle.age += dt;
            if (particle.age >= particle.lifetime)
            {
                particle = particles.back();
                particles.pop_back();
                continue;
            }
            particle.velocity = (particle.velocity + emitter.gravity * dt) * dragFactor;
            particle.position = particle.position + particle.velocity * dt;
            ++i;
        }

        if (!emitter.playing)
        {
            state.emitAccumulator = 0.f;
            return;
        }

        if (!state.burstDone)
        {
            for (uint32_t i = 0u; i < emitter.burstCount; ++i)
                Spawn(state, transform);
            state.burstDone = true;
        }

        const bool emitting = emitter.looping || state.time < emitter.duration;
        if (emitting && emitter.emitRate > 0.f)
        {
            state.emitAccumulator += emitter.emitRate * dt;
            const uint32_t count = static_cast<uint32_t>(state.emitAccumulator);
            state.emitAccumulator -= static_cast<float>(count);
            for (uint32_t i = 0u; i < count; ++i)
                Spawn(state, transform);
        }
        state.time += dt;
    });

    for (auto it = m_emitters.begin(); it != m_emitters.end();)
    {
        if (!it->second.seen)
            it = m_emitters.erase(it);
        else
            ++it;
    }
}

void ParticleSystem::CollectBillboards(const math::Vec3& cameraPosition,
                                       std::vector<ParticleBillboard>& alpha,
                                       std::vector<ParticleBillboard>& additive) const
{
    alpha.clear();
    additive.clear();

    for (const auto& [id, state] : m_emitters)
    {
        const ParticleEmitterComponent& p = state.params;
        const math::Vec3 startLinear{SrgbToLinear(p.startColor.x), SrgbToLinear(p.startColor.y), SrgbToLinear(p.startColor.z)};
        const math::Vec3 endLinear{SrgbToLinear(p.endColor.x), SrgbToLinear(p.endColor.y), SrgbToLinear(p.endColor.z)};
        auto& target = p.blendMode == ParticleBlendMode::Additive ? additive : alpha;

        for (const Particle& particle : state.particles)
        {
            const float t = std::clamp(particle.age / particle.lifetime, 0.f, 1.f);
            ParticleBillboard billboard{};
            billboard.position = particle.position;
            billboard.size = Lerp(p.startSize, p.endSize, t);
            billboard.color = math::Vec4{
                Lerp(startLinear.x, endLinear.x, t) * p.intensity,
                Lerp(startLinear.y, endLinear.y, t) * p.intensity,
                Lerp(startLinear.z, endLinear.z, t) * p.intensity,
                std::clamp(Lerp(p.startColor.w, p.endColor.w, t), 0.f, 1.f),
            };
            if (billboard.size <= 0.f || billboard.color.w <= 0.f)
                continue;
            target.push_back(billboard);
        }
    }

    std::sort(alpha.begin(), alpha.end(),
        [&](const ParticleBillboard& a, const ParticleBillboard& b) {
            return (a.position - cameraPosition).LengthSq() > (b.position - cameraPosition).LengthSq();
        });
}

} // namespace engine::particles
