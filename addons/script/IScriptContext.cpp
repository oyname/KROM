#include "addons/script/IScriptContext.hpp"

namespace engine::script {

TransformComponent* IScriptContext::EditTransform(EntityID entity)
{
    TransformComponent* transform = Get<TransformComponent>(entity);
    if (transform)
    {
        transform->dirty = true;
        ++transform->localVersion;
    }
    return transform;
}

EntityID IScriptContext::FindEntity(std::string_view name)
{
    ecs::World* world = GetWorld();
    if (!world)
        return NULL_ENTITY;

    EntityID found = NULL_ENTITY;
    world->ForEachAlive([&](EntityID id)
    {
        if (found.IsValid())
            return;
        if (const auto* n = world->Get<NameComponent>(id); n && n->name == name)
            found = id;
    });
    return found;
}

// ── Transform ────────────────────────────────────────────────────────────────

math::Vec3 IScriptContext::GetPosition(EntityID entity)
{
    const TransformComponent* t = Get<TransformComponent>(entity);
    return t ? t->localPosition : math::Vec3::Zero();
}

void IScriptContext::SetPosition(const math::Vec3& position, EntityID entity)
{
    if (TransformComponent* t = EditTransform(entity))
        t->localPosition = position;
}

void IScriptContext::Translate(const math::Vec3& delta, Space space, EntityID entity)
{
    if (TransformComponent* t = EditTransform(entity))
        t->localPosition += (space == Space::Local) ? t->localRotation.Rotate(delta) : delta;
}

math::Quat IScriptContext::GetRotation(EntityID entity)
{
    const TransformComponent* t = Get<TransformComponent>(entity);
    return t ? t->localRotation : math::Quat::Identity();
}

void IScriptContext::SetRotation(const math::Quat& rotation, EntityID entity)
{
    if (TransformComponent* t = EditTransform(entity))
        t->localRotation = rotation.Normalized();
}

void IScriptContext::SetRotationEulerDeg(float pitch, float yaw, float roll, EntityID entity)
{
    if (TransformComponent* t = EditTransform(entity))
        t->SetEulerDeg(pitch, yaw, roll);
}

void IScriptContext::RotateEulerDeg(float pitch, float yaw, float roll, Space space, EntityID entity)
{
    TransformComponent* t = EditTransform(entity);
    if (!t)
        return;
    if (space == Space::Local)
        t->RotateLocalEulerDeg(pitch, yaw, roll);
    else
        t->RotateWorldEulerDeg(pitch, yaw, roll);
}

math::Vec3 IScriptContext::GetScale(EntityID entity)
{
    const TransformComponent* t = Get<TransformComponent>(entity);
    return t ? t->localScale : math::Vec3::One();
}

void IScriptContext::SetScale(const math::Vec3& scale, EntityID entity)
{
    if (TransformComponent* t = EditTransform(entity))
        t->localScale = scale;
}

math::Vec3 IScriptContext::Forward(EntityID entity)
{
    return GetRotation(entity).Rotate(math::Vec3::Forward());
}

math::Vec3 IScriptContext::Right(EntityID entity)
{
    return GetRotation(entity).Rotate(math::Vec3::Right());
}

math::Vec3 IScriptContext::Up(EntityID entity)
{
    return GetRotation(entity).Rotate(math::Vec3::Up());
}

math::Vec3 IScriptContext::GetWorldPosition(EntityID entity)
{
    if (const WorldTransformComponent* w = Get<WorldTransformComponent>(entity))
        return w->position;
    return GetPosition(entity);
}

// ── Input ────────────────────────────────────────────────────────────────────

bool IScriptContext::KeyDown(platform::Key key) const
{
    const platform::IInput* input = GetInput();
    return input && input->KeyDown(key);
}

bool IScriptContext::KeyHit(platform::Key key) const
{
    const platform::IInput* input = GetInput();
    return input && input->KeyHit(key);
}

bool IScriptContext::KeyReleased(platform::Key key) const
{
    const platform::IInput* input = GetInput();
    return input && input->KeyReleased(key);
}

bool IScriptContext::MouseButtonDown(platform::MouseButton button) const
{
    const platform::IInput* input = GetInput();
    return input && input->MouseButtonDown(button);
}

bool IScriptContext::MouseButtonHit(platform::MouseButton button) const
{
    const platform::IInput* input = GetInput();
    return input && input->MouseButtonHit(button);
}

math::Vec2 IScriptContext::MouseDelta() const
{
    const platform::IInput* input = GetInput();
    if (!input)
        return math::Vec2::Zero();
    return {static_cast<float>(input->MouseDeltaX()), static_cast<float>(input->MouseDeltaY())};
}

float IScriptContext::MouseScroll() const
{
    const platform::IInput* input = GetInput();
    return input ? input->MouseScrollDelta() : 0.f;
}

float IScriptContext::Axis(platform::Key negative, platform::Key positive) const
{
    float value = 0.f;
    if (KeyDown(negative)) value -= 1.f;
    if (KeyDown(positive)) value += 1.f;
    return value;
}

} // namespace engine::script
