#pragma once
#include "krom.h"

// Spins the entity around one of its own axes, like a helicopter rotor.
// The rotation is local, so the rotor stays aligned with the helicopter
// however the parent is turned or tilted.
// axis = (0, 1, 0) for the main rotor, (0, 0, 1) for the tail rotor.
class RotorSpin final : public engine::script::ComponentScript
{
public:
    float speedDegPerSecond = 1080.0f;   // 3 revolutions per second
    engine::math::Vec3 axis{ 0.f, 1.f, 0.f };

    void OnUpdate(engine::script::IScriptContext& ctx) override;
};
