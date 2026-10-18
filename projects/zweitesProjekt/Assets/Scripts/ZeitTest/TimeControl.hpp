#pragma once
#include "krom.h"

// Tasten fuer die Zeit-Testszene und eine Log-Zeile pro Sekunde:
//   P  Pause an/aus            T  Zeitlupe (0.25) an/aus
//   I  Interpolation an/aus    F  kuenstlich ~30 FPS an/aus
// Leertaste (Thruster) hebt beide Raketen an; im Log sollten A und B gleich hoch sein.
class TimeControl final : public engine::script::ComponentScript
{
public:
    void OnUpdate(engine::script::IScriptContext& ctx) override;

private:
    float logTimer  = 0.f;
    int   logFrames = 0;
    bool  lowFps    = false;
};

// KROM_SCRIPT(TimeControl)
