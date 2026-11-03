#include "addons/script/ScriptModuleApi.hpp"
#include "addons/script/IScriptContext.hpp"
#include "ecs/Components.hpp"
#include "ecs/World.hpp"

#ifndef FIXTURE_VALUE
#define FIXTURE_VALUE 7
#endif
#ifdef FIXTURE_PREINIT
// Absichtlich unzulaessiger Zugriff vor KromScriptModuleInit.
const auto prematureComponentId = engine::ComponentTypeID<engine::TransformComponent>::Get();
#endif

class ModuleFixture final : public engine::script::ComponentScript
{
public:
    float speed = 2.f;
    void OnUpdate(engine::script::IScriptContext& ctx) override
    {
        using namespace engine;
        if (ctx.Has<TransformComponent>())
        {
            ctx.Get<TransformComponent>()->localPosition.x = FIXTURE_VALUE * speed;
            ctx.RotateEulerDeg(0.f, 30.f, 0.f);
            if (!ctx.Has<NameComponent>()) ctx.Add<NameComponent>(ctx.GetEntity());
            ctx.Get<NameComponent>()->name = "module updated";
            Debug::LogWarning("Script module fixture log");
        }
    }
};

class ModuleFollowCamera final : public engine::script::ComponentScript
{
public:
    engine::EntityID target{};
    void OnUpdate(engine::script::IScriptContext& ctx) override
    {
        if (target.IsValid()) ctx.SetPosition(ctx.GetPosition(target) + engine::math::Vec3{0.f, 2.f, 5.f});
    }
};

KROM_SCRIPT_EXPORT void KromRegisterGameScripts(engine::script::ScriptRegistry& registry)
{
    registry.Register<ModuleFixture>("ModuleFixture");
    registry.RegisterField<ModuleFixture>("ModuleFixture", "speed",
        engine::script::ScriptFieldType::Float, &ModuleFixture::speed);
    registry.Register<ModuleFollowCamera>("ModuleFollowCamera");
    registry.RegisterField<ModuleFollowCamera>("ModuleFollowCamera", "target",
        engine::script::ScriptFieldType::Entity, &ModuleFollowCamera::target);
}
