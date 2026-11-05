#include "addons/particles/ParticleSystem.hpp"

#include <algorithm>
#include <cmath>
#include <atomic>
#include <cstdlib>
#include <limits>
#include <utility>

namespace engine::particles {
namespace {

// Laengere Frames (Haltepunkt, Ladezeit) nicht als einen Riesenschritt simulieren.
constexpr float kMaxStep = 0.1f;

// Kuerzester Durchgang bei Schleife (sonst Burst in jedem Frame).
constexpr float kMinLoopDuration = 0.05f;

[[nodiscard]] float Lerp(float a, float b, float t) noexcept { return a + (b - a) * t; }

// Unterhalb davon gilt ein Aufprall als Ruhe (kein endloses Mini-Huepfen).
constexpr float kRestSpeed = 0.15f;

uint32_t NextSystemID() noexcept
{
    static std::atomic<uint64_t> next{1u};
    const uint64_t id = next.fetch_add(1u);
    if (id > std::numeric_limits<uint32_t>::max())
        std::abort();
    return static_cast<uint32_t>(id);
}

bool Finite(const math::Vec3& value) noexcept
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

[[nodiscard]] float FloorHeight(const ParticleEmitterComponent& p, const math::Vec3& emitterPosition) noexcept
{
    return p.floorRelative ? emitterPosition.y + p.floorHeight : p.floorHeight;
}

} // namespace

float SrgbToLinear(float c) noexcept
{
    if (c <= 0.04045f)
        return std::max(c, 0.f) / 12.92f;
    return std::pow((c + 0.055f) / 1.055f, 2.4f);
}

ParticleSystem::ParticleSystem(uint32_t seed) noexcept
    : m_system(NextSystemID()), m_rng(seed != 0u ? seed : 1u)
{}

uint32_t ParticleLook::FrameCount() const noexcept
{
    if (!frames.empty())
        return static_cast<uint32_t>(frames.size());
    return static_cast<uint32_t>(std::min(uint64_t(std::max(columns, 1u)) * std::max(rows, 1u),
        uint64_t(std::numeric_limits<uint32_t>::max())));
}

const ParticleSystem::Slot* ParticleSystem::FindSlot(ParticleID id) const
{
    if (id.system != m_system || id.generation == 0u || id.slot >= m_slots.size())
        return nullptr;
    const Slot& slot = m_slots[id.slot];
    return slot.pool && slot.generation == id.generation ? &slot : nullptr;
}

ParticleSystem::Slot* ParticleSystem::FindSlot(ParticleID id)
{
    return const_cast<Slot*>(std::as_const(*this).FindSlot(id));
}

void ParticleSystem::RemoveAt(EmitterState& state, size_t index)
{
    const ParticleID id = state.particles[index].id;
    Slot& slot = m_slots[id.slot];
    slot.pool = nullptr;
    if (slot.generation < std::numeric_limits<uint32_t>::max())
    {
        ++slot.generation;
        m_freeSlots.push_back(id.slot);
    }
    if (index + 1u != state.particles.size())
    {
        state.particles[index] = std::move(state.particles.back());
        m_slots[state.particles[index].id.slot].index = index;
    }
    state.particles.pop_back();
}

void ParticleSystem::ClearPool(EmitterState& state)
{
    while (!state.particles.empty())
        RemoveAt(state, state.particles.size() - 1u);
}

void ParticleSystem::RefreshLook(EmitterState& state)
{
    const auto& p = state.params;
    if (state.look && state.look->texture == p.texturePath && state.look->columns == p.flipbookColumns &&
        state.look->rows == p.flipbookRows && state.look->frames == p.frames &&
        state.look->animation == p.FrameSettings() && state.look->clips == p.clips)
        return;
    auto look = std::make_shared<ParticleLook>();
    look->texture = p.texturePath;
    look->columns = std::max(p.flipbookColumns, 1u);
    look->rows = std::max(p.flipbookRows, 1u);
    look->animation = p.FrameSettings();
    look->clips = p.clips;
    if (std::all_of(p.frames.begin(), p.frames.end(), [](const ParticleFrame& frame) { return frame.IsValid(); }))
        look->frames = p.frames;
    state.look = std::move(look);
}

ParticleID ParticleSystem::SpawnParticle(const ecs::World& world, EntityID emitter, const math::Vec3& position)
{
    if (!Finite(position) || !world.IsAlive(emitter))
        return NULL_PARTICLE;
    const auto* params = world.Get<ParticleEmitterComponent>(emitter);
    if (!params || params->maxParticles == 0u)
        return NULL_PARTICLE;
    if (const auto existing = m_emitters.find(emitter); existing != m_emitters.end() &&
        existing->second.particles.size() >= params->maxParticles)
        return NULL_PARTICLE;
    auto& state = m_emitters[emitter];
    state.entity = emitter;
    state.params = *params;
    RefreshLook(state);
    WorldTransformComponent transform{};
    if (const auto* source = world.Get<WorldTransformComponent>(emitter))
        transform = *source;
    state.floorY = FloorHeight(*params, transform.position);
    transform.position = position;
    return Spawn(state, transform, true);
}

bool ParticleSystem::IsParticleAlive(ParticleID id) const { return FindSlot(id) != nullptr; }

EntityID ParticleSystem::ParticleEmitter(ParticleID id) const
{
    const auto* slot = FindSlot(id);
    return slot ? slot->pool->entity : NULL_ENTITY;
}

bool ParticleSystem::RemoveParticle(ParticleID id)
{
    auto* slot = FindSlot(id);
    if (!slot) return false;
    RemoveAt(*slot->pool, slot->index);
    return true;
}

bool ParticleSystem::SetParticlePosition(ParticleID id, const math::Vec3& position)
{
    auto* slot = FindSlot(id);
    if (!slot || !Finite(position)) return false;
    slot->pool->particles[slot->index].position = position;
    return true;
}

bool ParticleSystem::SetParticleVelocity(ParticleID id, const math::Vec3& velocity)
{
    auto* slot = FindSlot(id);
    if (!slot || !Finite(velocity)) return false;
    slot->pool->particles[slot->index].velocity = velocity;
    return true;
}

bool ParticleSystem::SetParticleMotionEnabled(ParticleID id, bool enabled)
{
    auto* slot = FindSlot(id);
    if (!slot) return false;
    slot->pool->particles[slot->index].motion = enabled;
    return true;
}

void ParticleSystem::LiftFromFloor(Slot& slot)
{
    auto& p = slot.pool->particles[slot.index];
    if (p.motion && slot.pool->params.collideWithFloor)
        p.position.y = std::max(p.position.y, slot.pool->floorY + FloorOffset(p, slot.pool->params));
}

bool ParticleSystem::SetParticleSize(ParticleID id, const math::Vec2& size)
{
    auto* slot = FindSlot(id);
    if (!slot || !std::isfinite(size.x) || !std::isfinite(size.y) || size.x <= 0.f || size.y <= 0.f)
        return false;
    slot->pool->particles[slot->index].sizeOverride = size;
    LiftFromFloor(*slot);
    return true;
}

bool ParticleSystem::SetParticleFrame(ParticleID id, uint32_t frame)
{
    auto* slot = FindSlot(id);
    if (!slot || frame >= slot->pool->particles[slot->index].look->FrameCount()) return false;
    auto& p = slot->pool->particles[slot->index];
    p.frame = frame;
    p.frameMode = FrameMode::Fixed;
    LiftFromFloor(*slot);
    return true;
}

bool ParticleSystem::PlayParticleFrames(ParticleID id, uint32_t first, uint32_t last, float fps, bool loop)
{
    auto* slot = FindSlot(id);
    if (!slot || !std::isfinite(fps) || fps <= 0.f) return false;
    auto& p = slot->pool->particles[slot->index];
    if (first >= p.look->FrameCount() || last >= p.look->FrameCount()) return false;
    p.firstFrame = first;
    p.lastFrame = last;
    p.fps = fps;
    p.loop = loop;
    p.animTime = 0.;
    p.paused = false;
    p.frameMode = FrameMode::Rate;
    p.frame = first;
    LiftFromFloor(*slot);
    return true;
}

bool ParticleSystem::PlayParticleFramesOverLifetime(ParticleID id, uint32_t first, uint32_t last)
{
    if (!PlayParticleFrames(id, first, last, 1.f, false)) return false;
    auto* slot = FindSlot(id);
    auto& p = slot->pool->particles[slot->index];
    p.animLength = double(p.lifetime) - p.age;
    p.frameMode = FrameMode::OverLifetime;
    return true;
}

bool ParticleSystem::PlayParticleClip(ParticleID id, std::string_view name)
{
    const auto* slot = FindSlot(id);
    if (!slot || !IsParticleClipNameValid(name)) return false;
    const auto& particle = slot->pool->particles[slot->index];
    const auto& clips = particle.look->clips;
    const auto clip = std::find_if(clips.begin(), clips.end(), [&](const ParticleClip& value) { return value.name == name; });
    if (clip == clips.end()) return false;
    const auto range = ResolveParticleFrameRange(clip->first, clip->last, particle.look->FrameCount());
    if (clip->timing == ParticleClipTiming::OverLifetime)
        return PlayParticleFramesOverLifetime(id, range.first, range.last);
    if (clip->timing == ParticleClipTiming::Rate)
        return PlayParticleFrames(id, range.first, range.last, clip->fps, clip->loop);
    return false;
}

bool ParticleSystem::SetParticleFrameProgress(ParticleID id, uint32_t first, uint32_t last, float progress)
{
    if (std::isnan(progress)) return false;
    auto* slot = FindSlot(id);
    if (!slot) return false;
    auto& p = slot->pool->particles[slot->index];
    if (first >= p.look->FrameCount() || last >= p.look->FrameCount()) return false;
    p.frame = ResolveParticleFrame(ParticleFrameAnimation::OverLifetime, first, last, 0., false,
        progress, 1., p.look->FrameCount());
    p.frameMode = FrameMode::Manual;
    LiftFromFloor(*slot);
    return true;
}

bool ParticleSystem::SetParticleAnimationPaused(ParticleID id, bool paused)
{
    auto* slot = FindSlot(id);
    if (!slot) return false;
    slot->pool->particles[slot->index].paused = paused;
    return true;
}

std::optional<uint32_t> ParticleSystem::GetParticleFrame(ParticleID id) const
{
    const auto* slot = FindSlot(id);
    return slot ? std::optional<uint32_t>(slot->pool->particles[slot->index].frame) : std::nullopt;
}

void ParticleSystem::UpdateFrame(Particle& p)
{
    switch (p.frameMode)
    {
    case FrameMode::Emitter:
    {
        const auto& settings = p.look->animation;
        p.frame = ResolveParticleFrame(settings.mode, settings.first, settings.last, settings.fps,
            settings.loop, p.animTime, p.lifetime, p.look->FrameCount(), p.randomFrameIndex);
        break;
    }
    case FrameMode::Rate:
        p.frame = ResolveParticleFrame(ParticleFrameAnimation::Rate, p.firstFrame, p.lastFrame, p.fps,
            p.loop, p.animTime, p.animLength, p.look->FrameCount());
        break;
    case FrameMode::OverLifetime:
        p.frame = ResolveParticleFrame(ParticleFrameAnimation::OverLifetime, p.firstFrame, p.lastFrame, 0.,
            false, p.animTime, p.animLength, p.look->FrameCount());
        break;
    default: break;
    }
}

math::Vec2 ParticleSystem::ParticleSize(const Particle& particle, const ParticleEmitterComponent& params)
{
    const float s = Lerp(params.startSize, params.endSize, std::clamp(particle.age / particle.lifetime, 0.f, 1.f));
    math::Vec2 size = particle.sizeOverride.x > 0.f ? particle.sizeOverride : math::Vec2{s, s};
    if (!particle.look->frames.empty())
    {
        const auto& ref = particle.look->frames.front();
        const auto& frame = particle.look->frames[particle.frame];
        if (particle.sizeOverride.x <= 0.f)
            size.x *= float(ref.width) / float(ref.height);
        size.x *= float(frame.width) / float(ref.width);
        size.y *= float(frame.height) / float(ref.height);
    }
    return size;
}

float ParticleSystem::FloorOffset(const Particle& particle, const ParticleEmitterComponent& params)
{
    const float pivotY = particle.look->frames.empty() ? 0.5f : particle.look->frames[particle.frame].pivotY;
    return ParticleSize(particle, params).y * (1.f - pivotY);
}

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
    for (auto& [id, state] : m_emitters) ClearPool(state);
    for (auto& state : m_detached) ClearPool(*state);
    m_emitters.clear();
    m_pendingBursts.clear();
    m_detached.clear();
}

void ParticleSystem::Restart(EntityID entity)
{
    const auto it = m_emitters.find(entity);
    if (it == m_emitters.end())
        return;
    EmitterState& state = it->second;
    ClearPool(state);
    state.emitAccumulator = 0.f;
    state.time = 0.f;
    state.burstDone = false;
}

void ParticleSystem::ClearParticles(EntityID entity)
{
    if (const auto it = m_emitters.find(entity); it != m_emitters.end())
        ClearPool(it->second);
}

void ParticleSystem::Burst(EntityID entity, uint32_t count)
{
    if (entity.IsValid() && count > 0u)
        m_pendingBursts[entity] += count;
}

void ParticleSystem::EmitAt(const ParticleEmitterComponent& params, const math::Vec3& position,
                            uint32_t count, const math::Quat& rotation)
{
    if (count == 0u)
        return;
    auto state = std::make_unique<EmitterState>();
    state->params = params;
    state->seen = true;
    state->floorY = FloorHeight(params, position);
    RefreshLook(*state);
    WorldTransformComponent transform{};
    transform.position = position;
    transform.rotation = rotation;
    for (uint32_t i = 0u; i < count; ++i)
        Spawn(*state, transform);
    if (!state->particles.empty())
        m_detached.push_back(std::move(state));
}

bool ParticleSystem::IsFinished(EntityID entity) const noexcept
{
    const auto it = m_emitters.find(entity);
    if (it == m_emitters.end())
        return false;
    const EmitterState& state = it->second;
    if (!state.particles.empty())
        return false;
    const ParticleEmitterComponent& p = state.params;
    return !p.playing || (!p.looping && state.burstDone && state.time >= p.duration);
}

void ParticleSystem::CollectFinishedEffects(const ecs::World& world, std::vector<EntityID>& out) const
{
    world.View<ParticleEffectAutoDestroyComponent>(
        [&](EntityID root, const ParticleEffectAutoDestroyComponent&)
    {
        bool finished = true;
        std::vector<EntityID> pending{root};
        for (size_t i = 0u; i < pending.size() && finished; ++i)
        {
            const EntityID id = pending[i];
            if (world.Has<ParticleEmitterComponent>(id) && !IsFinished(id))
                finished = false;
            if (const auto* children = world.Get<ChildrenComponent>(id))
                pending.insert(pending.end(), children->children.begin(), children->children.end());
        }
        if (finished)
            out.push_back(root);
    });
}

uint32_t ParticleSystem::ParticleCount() const noexcept
{
    size_t count = 0u;
    for (const auto& [id, state] : m_emitters)
        count += state.particles.size();
    for (const auto& state : m_detached)
        count += state->particles.size();
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

ParticleID ParticleSystem::Spawn(EmitterState& state, const WorldTransformComponent& transform, bool exactPosition)
{
    const ParticleEmitterComponent& p = state.params;
    if (state.particles.size() >= p.maxParticles)
        return NULL_PARTICLE;

    Particle particle{};
    particle.lifetime = std::max(0.01f,
        p.particleLifetime * (1.f + std::clamp(p.lifetimeRandom, 0.f, 1.f) * RandomSigned()));

    math::Vec3 offset{};
    if (!exactPosition && p.emitterRadius > 0.f)
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

    particle.look = state.look;
    const uint32_t frames = particle.look->FrameCount();
    const auto& animation = particle.look->animation;
    const auto range = ResolveParticleFrameRange(animation.first, animation.last, frames);
    const double random = Random01();
    particle.randomFrameIndex = static_cast<uint32_t>(random * range.Count());
    if (animation.mode == ParticleFrameAnimation::Rate && animation.randomStart &&
        std::isfinite(animation.fps) && animation.fps > 0.f)
        particle.animTime = random * range.Count() / animation.fps;
    UpdateFrame(particle);
    uint32_t slotIndex;
    if (m_freeSlots.empty())
    {
        if (m_slots.size() >= std::numeric_limits<uint32_t>::max()) return NULL_PARTICLE;
        slotIndex = static_cast<uint32_t>(m_slots.size());
        m_slots.emplace_back();
    }
    else
    {
        slotIndex = m_freeSlots.back();
        m_freeSlots.pop_back();
    }
    auto& slot = m_slots[slotIndex];
    slot.pool = &state;
    slot.index = state.particles.size();
    particle.id = {m_system, slotIndex, slot.generation};
    state.particles.push_back(particle);
    return particle.id;
}

void ParticleSystem::Simulate(EmitterState& state, float dt)
{
    const ParticleEmitterComponent& p = state.params;
    const float dragFactor = std::max(0.f, 1.f - p.drag * dt);
    auto& particles = state.particles;
    for (size_t i = 0u; i < particles.size();)
    {
        Particle& particle = particles[i];
        const float oldContact = state.floorY + FloorOffset(particle, p);
        particle.age += dt;
        if (particle.age >= particle.lifetime)
        {
            RemoveAt(state, i);
            continue;
        }
        if (!particle.paused)
            particle.animTime += dt;
        UpdateFrame(particle);
        if (!particle.motion)
        {
            ++i;
            continue;
        }
        particle.velocity = (particle.velocity + p.gravity * dt) * dragFactor;
        particle.position = particle.position + particle.velocity * dt;

        if (p.collideWithFloor)
        {
            const float floor = state.floorY + FloorOffset(particle, p);
            if (particle.position.y < floor)
            {
                const bool impact = particle.velocity.y < 0.f && particle.position.y < oldContact;
                if (impact && p.dieOnFloor)
                {
                    RemoveAt(state, i);
                    continue;
                }
                particle.position.y = floor;
                if (impact)
                {
                    const float keep = std::clamp(1.f - p.floorFriction, 0.f, 1.f);
                    particle.velocity.x *= keep;
                    particle.velocity.z *= keep;
                    particle.velocity.y = -particle.velocity.y * std::clamp(p.bounce, 0.f, 1.f);
                    if (particle.velocity.y < kRestSpeed)
                        particle.velocity.y = 0.f;
                }
            }
        }
        ++i;
    }
}

void ParticleSystem::Update(const ecs::World& world, float dt,
                            const std::unordered_set<EntityID>* only)
{
    dt = std::isfinite(dt) ? std::clamp(dt, 0.f, kMaxStep) : 0.f;

    for (auto& [id, state] : m_emitters)
        state.seen = false;

    world.View<ParticleEmitterComponent, WorldTransformComponent>(
        [&](EntityID id, const ParticleEmitterComponent& emitter, const WorldTransformComponent& transform)
    {
        if (only && only->count(id) == 0u)
            return;
        EmitterState& state = m_emitters[id];
        state.params = emitter;
        state.entity = id;
        RefreshLook(state);
        state.seen = true;
        state.floorY = FloorHeight(emitter, transform.position);

        // Bestehende Partikel altern lassen und bewegen.
        Simulate(state, dt);

        // Script-Burst: auch bei pausiertem Emitter.
        if (const auto burst = m_pendingBursts.find(id); burst != m_pendingBursts.end())
        {
            for (uint32_t i = 0u; i < burst->second; ++i)
                Spawn(state, transform);
            m_pendingBursts.erase(burst);
        }

        if (!emitter.playing)
        {
            state.emitAccumulator = 0.f;
            return;
        }

        // Schleife wie in Unity: nach "duration" beginnt ein neuer Durchgang,
        // der Burst kommt erneut.
        if (emitter.looping)
        {
            const float cycle = std::max(emitter.duration, kMinLoopDuration);
            if (state.time >= cycle)
            {
                state.time = std::fmod(state.time, cycle);
                state.burstDone = false;
            }
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
        {
            ClearPool(it->second);
            it = m_emitters.erase(it);
        }
        else
            ++it;
    }
    // Bursts fuer Entities ohne (simulierten) Emitter verfallen.
    m_pendingBursts.clear();

    for (auto& state : m_detached)
        Simulate(*state, dt);
    m_detached.erase(std::remove_if(m_detached.begin(), m_detached.end(),
                                    [](const auto& state) { return state->particles.empty(); }),
                     m_detached.end());
}

void ParticleSystem::CollectBillboards(const math::Vec3& cameraPosition,
                                       std::vector<ParticleBillboard>& alpha,
                                       std::vector<ParticleBillboard>& additive) const
{
    alpha.clear();
    additive.clear();

    const auto collect = [&](const EmitterState& state)
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
            billboard.size = ParticleSize(particle, p);
            billboard.color = math::Vec4{
                Lerp(startLinear.x, endLinear.x, t) * p.intensity,
                Lerp(startLinear.y, endLinear.y, t) * p.intensity,
                Lerp(startLinear.z, endLinear.z, t) * p.intensity,
                std::clamp(Lerp(p.startColor.w, p.endColor.w, t), 0.f, 1.f),
            };
            if (billboard.size.x <= 0.f || billboard.size.y <= 0.f || billboard.color.w <= 0.f)
                continue;
            billboard.look = particle.look;
            billboard.frame = particle.frame;
            billboard.texture = particle.look->texture.empty() ? nullptr : &particle.look->texture;
            if (particle.look->frames.empty())
            {
                const uint32_t columns = particle.look->columns;
                const float frameW = 1.f / float(columns);
                const float frameH = 1.f / float(particle.look->rows);
                const float u0 = float(particle.frame % columns) * frameW;
                const float v0 = float(particle.frame / columns) * frameH;
                billboard.uvRect = math::Vec4{u0, v0, u0 + frameW, v0 + frameH};
                billboard.gridAspect = particle.sizeOverride.x <= 0.f;
            }
            else
            {
                const auto& frame = particle.look->frames[particle.frame];
                billboard.pivot = {frame.pivotX, frame.pivotY};
            }
            target.push_back(billboard);
        }
    };

    for (const auto& [id, state] : m_emitters)
        collect(state);
    for (const auto& state : m_detached)
        collect(*state);

    std::sort(alpha.begin(), alpha.end(),
        [&](const ParticleBillboard& a, const ParticleBillboard& b) {
            return (a.position - cameraPosition).LengthSq() > (b.position - cameraPosition).LengthSq();
        });
}

} // namespace engine::particles
