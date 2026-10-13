#pragma once
#include "krom.h"

class Rotate final : public engine::script::ComponentScript
{
public:
    float speedDegPerSecond = 5.0f;
    int spawnCount = 0;

    void OnStart(engine::script::IScriptContext& ctx) override;
    void OnUpdate(engine::script::IScriptContext& ctx) override;
};

// KROM_SCRIPT(Rotate)
// KROM_FIELD(Rotate, speedDegPerSecond, Float)
// KROM_FIELD(Rotate, spawnCount, Int)