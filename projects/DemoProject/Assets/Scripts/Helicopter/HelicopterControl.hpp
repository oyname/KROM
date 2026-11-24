#pragma once
#include "krom.h"

// Keyboard flight for a helicopter:
//   Up / Down (W / S)  forward / back        (nose dips or rises)
//   Left / Right       turn left / right     (banks into the turn)
//   A / D              slide left / right    (banks to that side)
//   Q / E              up / down
// The helicopter leans into the movement and swings back level when the keys
// are released. It coasts a little instead of stopping dead.
//
// Moves the entity with Translate, so it can be combined with HelicopterHover
// on the same entity. Put it on the root object of the helicopter; that
// object must stand upright in the scene (only turned around Y).
class HelicopterControl final : public engine::script::ComponentScript
{
public:
    float speed = 60.0f;                  // units per second, forward and sideways
    float climbSpeed = 30.0f;             // units per second, up and down
    float turnSpeedDegPerSecond = 120.0f;
    float maxTiltDeg = 15.0f;             // lean at full speed
    float acceleration = 1.5f;            // higher = reaches speed and stops sooner
    // Direction the nose points in the model's own axes. apache15 looks along -X.
    engine::math::Vec3 noseAxis{ -1.f, 0.f, 0.f };

    void OnStart(engine::script::IScriptContext& ctx) override;
    void OnUpdate(engine::script::IScriptContext& ctx) override;

private:
    engine::math::Quat startRotation{};
    engine::math::Vec3 velocity{};
    float yawDeg = 0.0f;        // turned since start
    float turnRate = 0.0f;      // degrees per second
    float pitchDeg = 0.0f;      // > 0: nose down
    float pitchRate = 0.0f;
    float bankDeg = 0.0f;       // > 0: right side down
    float bankRate = 0.0f;
};
