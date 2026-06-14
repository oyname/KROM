#pragma once
#include "krom.h"

class CameraController final : public engine::script::ComponentScript
{
public:
    float moveSpeed = 4.0f;
    float fastMultiplier = 4.0f;
    float turnSpeedDegPerSecond = 55.0f;
    float mouseSensitivity = 0.12f;

    void OnStart(engine::script::IScriptContext& ctx) override;
    void OnUpdate(engine::script::IScriptContext& ctx) override;

private:
    float yawDeg = 0.0f;
    float pitchDeg = 0.0f;
};

// KROM_SCRIPT(CameraController)
// KROM_FIELD(CameraController, moveSpeed, Float)
// KROM_FIELD(CameraController, fastMultiplier, Float)
// KROM_FIELD(CameraController, turnSpeedDegPerSecond, Float)
// KROM_FIELD(CameraController, mouseSensitivity, Float)
