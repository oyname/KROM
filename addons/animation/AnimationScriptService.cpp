// =============================================================================
// KROM Engine - addons/animation/AnimationScriptService.cpp
// =============================================================================
#include "addons/animation/AnimationScriptService.hpp"

#include "core/Debug.hpp"
#include "ecs/Components.hpp"

#include <string>
#include <unordered_set>

namespace engine::addons::animation {

namespace {

// Script mistakes are logged once per kind and name, not every frame.
void WarnOnce(const std::string& key, const char* format, const char* a, const char* b = "")
{
    static std::unordered_set<std::string> reported;
    if (reported.insert(key).second)
        Debug::LogWarning(format, a, b);
}

std::string EntityLabel(const ecs::World& world, EntityID entity)
{
    const auto* name = world.Get<NameComponent>(entity);
    return name && !name->name.empty() ? name->name : "#" + std::to_string(entity.value);
}

} // namespace

AnimationPlayerComponent* AnimationScriptService::Bound(EntityID entity)
{
    auto* player = m_world.IsAlive(entity) ? m_world.Get<AnimationPlayerComponent>(entity) : nullptr;
    if (!player || !player->HasController())
    {
        const std::string label = m_world.IsAlive(entity) ? EntityLabel(m_world, entity) : std::string("?");
        WarnOnce("nocontroller|" + label, "AnimationScriptService: '%s' has no animator with a controller%s", label.c_str());
        return nullptr;
    }
    if (!m_system.EnsureControllerBound(m_world, entity, *player))
        return nullptr; // the library already reported the file
    return player;
}

int32_t AnimationScriptService::FindParameter(EntityID entity, std::string_view name)
{
    auto* player = Bound(entity);
    if (!player)
        return -1;
    const auto& parameters = player->controllerState.controller->parameters;
    for (size_t i = 0; i < parameters.size(); ++i)
        if (parameters[i].name == name)
            return static_cast<int32_t>(i);
    const std::string text(name);
    WarnOnce("param|" + player->controllerAssetPath + "|" + text,
             "AnimationScriptService: controller '%s' has no parameter '%s'", player->controllerAssetPath.c_str(), text.c_str());
    return -1;
}

bool AnimationScriptService::GetParameterKind(EntityID entity, int32_t index, script::AnimationParameterKind& out)
{
    auto* player = Bound(entity);
    if (!player)
        return false;
    const auto& parameters = player->controllerState.controller->parameters;
    if (index < 0 || static_cast<size_t>(index) >= parameters.size())
        return false;
    out = static_cast<script::AnimationParameterKind>(parameters[static_cast<size_t>(index)].type);
    return true;
}

bool AnimationScriptService::SetParameter(EntityID entity, int32_t index, float value)
{
    auto* player = Bound(entity);
    return player && player->SetParameter(index, value);
}

bool AnimationScriptService::GetParameter(EntityID entity, int32_t index, float& out)
{
    auto* player = Bound(entity);
    if (!player)
        return false;
    const auto& values = player->controllerState.instance.parameters;
    if (index < 0 || static_cast<size_t>(index) >= values.size())
        return false;
    out = values[static_cast<size_t>(index)];
    return true;
}

std::string_view AnimationScriptService::GetCurrentState(EntityID entity)
{
    auto* player = Bound(entity);
    if (!player)
        return {};
    const auto& rt = player->controllerState;
    const int32_t current = rt.instance.current;
    if (current < 0 || static_cast<size_t>(current) >= rt.controller->states.size())
        return {};
    return rt.controller->states[static_cast<size_t>(current)].name;
}

float AnimationScriptService::GetNormalizedTime(EntityID entity)
{
    auto* player = Bound(entity);
    if (!player)
        return 0.f;
    const auto& rt = player->controllerState;
    return NormalizedStateTime(*rt.controller, rt.instance.current, rt.instance.currentTime);
}

bool AnimationScriptService::IsInTransition(EntityID entity)
{
    auto* player = Bound(entity);
    return player && player->controllerState.instance.Blending();
}

bool AnimationScriptService::CrossFade(EntityID entity, std::string_view state, float blendSeconds)
{
    auto* player = Bound(entity);
    if (!player)
        return false;
    auto& rt = player->controllerState;
    const auto& states = rt.controller->states;
    for (size_t i = 0; i < states.size(); ++i)
        if (states[i].name == state)
        {
            CrossFadeToState(*rt.controller, rt.instance, static_cast<int32_t>(i), blendSeconds);
            return true;
        }
    const std::string text(state);
    WarnOnce("state|" + player->controllerAssetPath + "|" + text,
             "AnimationScriptService: controller '%s' has no state '%s'", player->controllerAssetPath.c_str(), text.c_str());
    return false;
}

} // namespace engine::addons::animation
