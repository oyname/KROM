#pragma once
#include "krom.h"

// Verfolgerkamera: kreist um das Ziel. Maus mit gedrueckter rechter Taste
// oder Pfeiltasten drehen, Mausrad zoomt.
class ThirdPersonCamera final : public engine::script::ComponentScript
{
public:
    engine::EntityID target{};
    float distance = 5.0f;
    float pivotHeight = 1.5f;
    float mouseSensitivity = 0.15f;
    float turnSpeedDegPerSecond = 90.0f;

    void OnStart(engine::script::IScriptContext& ctx) override;
    void OnUpdate(engine::script::IScriptContext& ctx) override;

private:
    float yawDeg = 0.0f;
    float pitchDeg = -15.0f;
};

// KROM_SCRIPT(ThirdPersonCamera)
// KROM_FIELD(ThirdPersonCamera, target, Entity)
// KROM_FIELD(ThirdPersonCamera, distance, Float)
// KROM_FIELD(ThirdPersonCamera, pivotHeight, Float)
// KROM_FIELD(ThirdPersonCamera, mouseSensitivity, Float)
// KROM_FIELD(ThirdPersonCamera, turnSpeedDegPerSecond, Float)
