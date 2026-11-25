#pragma once
#include "krom.h"

// Shared by StressSpawner and StressSpin.
namespace stresstest {

// Who turns the cubes.
enum Mode : int
{
    PerScript = 0,   // every cube turns itself in its own OnUpdate
    SingleLoop = 1,  // the spawner turns all cubes in one loop
    Stopped = 2      // nobody: no transform changes, rendering only
};

inline int g_mode = PerScript;

struct Spin
{
    engine::math::Vec3 axis;
    float degPerSecond;
};

// Axis and speed of a cube, derived from its entity id, so that every cube
// turns differently without any per-instance setup.
inline Spin SpinFor(engine::EntityID entity) noexcept
{
    uint32_t h = entity.value * 2654435761u;
    const auto next = [&h]() noexcept
    {
        h ^= h << 13;
        h ^= h >> 17;
        h ^= h << 5;
        return static_cast<float>(h & 0xFFFFu) / 65535.0f;   // 0..1
    };
    engine::math::Vec3 axis{next() * 2.0f - 1.0f, next() * 2.0f - 1.0f, next() * 2.0f - 1.0f};
    if (axis.LengthSq() < 0.01f)
        axis = {0.0f, 1.0f, 0.0f};
    const float speed = 20.0f + next() * 160.0f;
    return {axis.Normalized(), next() < 0.5f ? speed : -speed};
}

inline void Turn(engine::TransformComponent& transform, const Spin& spin, float dt) noexcept
{
    transform.localRotation = (transform.localRotation *
        engine::math::Quat::FromAxisAngleDeg(spin.axis, spin.degPerSecond * dt)).Normalized();
    transform.dirty = true;
    ++transform.localVersion;
}

} // namespace stresstest

// Sits on the root of Prefabs/StressCube.prefab.
class StressSpin final : public engine::script::ComponentScript
{
public:
    void OnStart(engine::script::IScriptContext& ctx) override;
    void OnUpdate(engine::script::IScriptContext& ctx) override;

private:
    stresstest::Spin spin{};
};
