#pragma once
#include "krom.h"

class Move final : public engine::script::ComponentScript
{
public:
    float speedPerSecond = 5.0f;
    LPENTITY target = NULL_LPENTITY;

    void OnStart(engine::script::IScriptContext& ctx) override;
    void OnUpdate(engine::script::IScriptContext& ctx) override;
};

// KROM_SCRIPT(Move)
// KROM_FIELD(Move, speedPerSecond, Float)
// KROM_FIELD(Move, target, Entity)
