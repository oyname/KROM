#pragma once
#include "krom.h"
#include "StressSpin.hpp"

#include <string>
#include <vector>

// ECS stress test: spawns `count` copies of a prefab at random places inside
// `areaSize` (centred on this entity) and shows frame times on screen.
// Keys while playing: 1 = every cube turns itself (one script per cube),
// 2 = this script turns all cubes in one loop, 3 = nothing turns.
class StressSpawner final : public engine::script::ComponentScript
{
public:
    KROM_PREFAB std::string cubePrefab = "Prefabs/StressCube.prefab";
    int count = 5000;
    engine::math::Vec3 areaSize{ 120.f, 40.f, 120.f };
    int seed = 1;

    void OnStart(engine::script::IScriptContext& ctx) override;
    void OnUpdate(engine::script::IScriptContext& ctx) override;

private:
    std::vector<engine::EntityID> cubes;
    std::vector<stresstest::Spin> spins;
    float spawnMs = 0.f;
    float loopMs = 0.f;
    float statTimer = 0.f;
    int statFrames = 0;
    float shownFps = 0.f;
    float shownFrameMs = 0.f;
};
