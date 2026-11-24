#pragma once
#include "krom.h"

class MoveX final : public engine::script::ComponentScript
{
public:
    float speedPerSecond = 90.0f;

    void OnStart(engine::script::IScriptContext& ctx) override;
    void OnUpdate(engine::script::IScriptContext& ctx) override;
};
