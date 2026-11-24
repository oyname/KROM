#pragma once
#include "krom.h"

// Lets the entity bob gently up and down, like a helicopter holding a hover.
// Two sine waves with different periods are combined so the motion does not
// look like a perfect pendulum. The bobbing is added on top of whatever else
// moves the entity, so it works together with HelicopterControl.
//
// With a dynamic Rigidbody the bobbing is a small up-and-down force (the
// acceleration of the wave times the mass) instead of moving the entity, so
// the physics keeps control of the height. HelicopterControl's lift then
// evens part of it out, so the swing is a little smaller there.
class HelicopterHover final : public engine::script::ComponentScript
{
public:
    float height = 0.6f;          // maximum offset above/below the resting height
    float cyclesPerSecond = 0.25f; // one full up-and-down takes 4 seconds

    void OnUpdate(engine::script::IScriptContext& ctx) override;
    void OnFixedUpdate(engine::script::IScriptContext& ctx) override;

private:
    float phase = 0.0f;
    float lastOffset = 0.0f;
};
