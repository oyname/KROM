#pragma once

#include "core/Math.hpp"
#include "core/Types.hpp"
#include "ecs/ComponentMeta.hpp"

namespace engine {

struct ParticleEmitterComponent
{
    TextureHandle atlasTexture;
    uint32_t      maxParticles     = 1000u;
    float         emitRate         = 50.f;
    float         particleLifetime = 2.f;
    math::Vec3    initialVelocity  { 0.f, 1.f, 0.f };
    math::Vec3    velocityRandom   { 0.5f, 0.5f, 0.5f };
    math::Vec3    gravity          { 0.f, -9.81f, 0.f };
    float         startSize        = 0.1f;
    float         endSize          = 0.0f;
    math::Vec4    startColor       { 1.f, 1.f, 1.f, 1.f };
    math::Vec4    endColor         { 1.f, 1.f, 1.f, 0.f };
    bool          looping          = true;
    bool          playing          = true;
};

inline void RegisterParticleComponents(ecs::ComponentMetaRegistry& registry)
{
    using namespace ecs;
    RegisterComponent<ParticleEmitterComponent>(registry, "ParticleEmitterComponent");
}

} // namespace engine
