#include "addons/script/IScriptContext.hpp"
#include "core/Debug.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <string>
#include <vector>

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

EntityID IScriptContext::FindChild(std::string_view name, EntityID parent)
{
    ecs::World* world = GetWorld();
    if (!world)
        return NULL_ENTITY;
    // Breitensuche: direkte Kinder zuerst.
    std::vector<EntityID> pending{Resolve(parent)};
    for (size_t i = 0u; i < pending.size() && i < 4096u; ++i)
    {
        const auto* children = world->Get<ChildrenComponent>(pending[i]);
        if (!children)
            continue;
        for (const EntityID child : children->children)
        {
            if (!world->IsAlive(child))
                continue;
            if (const auto* n = world->Get<NameComponent>(child); n && n->name == name)
                return child;
            pending.push_back(child);
        }
    }
    return NULL_ENTITY;
}

EntityID IScriptContext::GetRoot(EntityID entity)
{
    ecs::World* world = GetWorld();
    EntityID cursor = Resolve(entity);
    if (!world)
        return cursor;

    for (uint32_t depth = 0; depth < 1024u && world->IsAlive(cursor); ++depth)
    {
        const auto* parent = world->Get<ParentComponent>(cursor);
        if (!parent || !world->IsAlive(parent->parent))
            break;
        cursor = parent->parent;
    }
    return cursor;
}

std::vector<EntityID> IScriptContext::FilterRoots(const std::vector<EntityID>& entities, bool ignoreSelf)
{
    const EntityID selfRoot = ignoreSelf ? GetRoot() : NULL_ENTITY;
    std::vector<EntityID> roots;
    for (const EntityID e : entities)
    {
        const EntityID root = GetRoot(e);
        if (ignoreSelf && root == selfRoot)
            continue;
        if (std::find(roots.begin(), roots.end(), root) == roots.end())
            roots.push_back(root);
    }
    return roots;
}

// ── Kollisionsabfragen ───────────────────────────────────────────────────────

bool IScriptContext::Raycast(const math::Vec3& origin, const math::Vec3& direction, float maxDistance,
                             collision::RaycastHit& outHit, bool ignoreSelf)
{
    const collision::SceneQueries* queries = GetSceneQueries();
    if (!queries || direction.LengthSq() <= math::EPSILON)
        return false;

    const collision::Ray ray{origin, direction.Normalized()};
    const EntityID selfRoot = ignoreSelf ? GetRoot() : NULL_ENTITY;
    // Kandidaten sind nach Distanz sortiert — erster Treffer ausserhalb der eigenen Hierarchie gewinnt.
    for (const collision::RaycastCandidate& candidate : queries->CollectRaycastCandidates(ray, maxDistance))
    {
        const EntityID root = GetRoot(candidate.entity);
        if (ignoreSelf && root == selfRoot)
            continue;
        outHit.entity   = root;
        outHit.distance = candidate.boundsDistance;
        outHit.position = ray.origin + ray.direction * candidate.boundsDistance;
        outHit.normal   = (outHit.position - candidate.sphere.center).Normalized();
        return true;
    }
    return false;
}

std::vector<EntityID> IScriptContext::OverlapSphere(const math::Vec3& center, float radius, bool ignoreSelf)
{
    const collision::SceneQueries* queries = GetSceneQueries();
    if (!queries)
        return {};
    return FilterRoots(queries->OverlapSphere({center, radius}), ignoreSelf);
}

std::vector<EntityID> IScriptContext::OverlapBox(const math::Vec3& center, const math::Vec3& halfExtents,
                                                 bool ignoreSelf)
{
    const collision::SceneQueries* queries = GetSceneQueries();
    if (!queries)
        return {};
    return FilterRoots(queries->OverlapAABB({center - halfExtents, center + halfExtents}), ignoreSelf);
}

// ── Physik ───────────────────────────────────────────────────────────────────

void IScriptContext::SetTimeScale(float scale) noexcept
{
    if (ScriptClock* clock = GetClock())
        clock->timeScale = (scale > 0.f) ? std::min(scale, 100.f) : 0.f;   // auch NaN → 0
}

bool IScriptContext::AddForce(const math::Vec3& force, EntityID entity)
{
    IPhysicsService* physics = GetPhysics();
    if (!physics)
        return false;
    if (IsFixedStep())
        return physics->AddForce(Resolve(entity), force);
    // Frame-Takt: Jolt sammelt Kraefte bis zum naechsten Schritt — bei mehreren
    // Frames pro Schritt summierten sie sich. Als Impuls F*dt bleibt der
    // Gesamtimpuls unabhaengig von der Framerate.
    const ScriptClock* clock = GetClock();
    const float dt = clock ? clock->deltaTime : DeltaTime();
    return physics->AddImpulse(Resolve(entity), force * dt);
}

bool IScriptContext::AddImpulse(const math::Vec3& impulse, EntityID entity)
{
    IPhysicsService* physics = GetPhysics();
    return physics && physics->AddImpulse(Resolve(entity), impulse);
}

bool IScriptContext::SetVelocity(const math::Vec3& velocity, EntityID entity)
{
    IPhysicsService* physics = GetPhysics();
    return physics && physics->SetLinearVelocity(Resolve(entity), velocity);
}

math::Vec3 IScriptContext::GetVelocity(EntityID entity)
{
    math::Vec3 velocity{0.f, 0.f, 0.f};
    if (IPhysicsService* physics = GetPhysics())
        (void)physics->GetLinearVelocity(Resolve(entity), velocity);
    return velocity;
}

bool IScriptContext::SetMoveVelocity(const math::Vec3& velocity, EntityID entity)
{
    IPhysicsService* physics = GetPhysics();
    return physics && physics->SetCharacterMove(Resolve(entity), velocity);
}

bool IScriptContext::Jump(float speed, EntityID entity)
{
    IPhysicsService* physics = GetPhysics();
    return physics && physics->CharacterJump(Resolve(entity), speed);
}

bool IScriptContext::IsGrounded(EntityID entity)
{
    IPhysicsService* physics = GetPhysics();
    return physics && physics->IsCharacterGrounded(Resolve(entity));
}

// ── Audio ────────────────────────────────────────────────────────────────────

bool IScriptContext::PlayAudio(EntityID entity)
{
    IAudioService* audio = GetAudio();
    return audio && audio->Play(Resolve(entity));
}

bool IScriptContext::StopAudio(EntityID entity)
{
    IAudioService* audio = GetAudio();
    return audio && audio->Stop(Resolve(entity));
}

bool IScriptContext::IsAudioPlaying(EntityID entity)
{
    IAudioService* audio = GetAudio();
    return audio && audio->IsPlaying(Resolve(entity));
}

bool IScriptContext::PlayOneShot(std::string_view clipPath, float volume)
{
    IAudioService* audio = GetAudio();
    return audio && audio->PlayOneShot(clipPath, volume);
}

bool IScriptContext::PlayOneShotAt(std::string_view clipPath, const math::Vec3& position, float volume)
{
    IAudioService* audio = GetAudio();
    return audio && audio->PlayOneShotAt(clipPath, position, volume);
}

// ── Animation ────────────────────────────────────────────────────────────────

namespace {

// Parameter by name with the expected kind; -1 when missing or of another kind.
int32_t AnimationParameterOfKind(IAnimationService* animation, EntityID entity, std::string_view name,
                                 std::initializer_list<AnimationParameterKind> kinds)
{
    if (!animation)
        return -1;
    const int32_t index = animation->FindParameter(entity, name);
    AnimationParameterKind kind{};
    if (index < 0 || !animation->GetParameterKind(entity, index, kind))
        return -1;
    for (const AnimationParameterKind allowed : kinds)
        if (kind == allowed)
            return index;
    static const char* const kNames[] = {"Float", "Int", "Bool", "Trigger"};
    const std::string text(name);
    Debug::LogWarning("IScriptContext: animation parameter '%s' is a %s parameter", text.c_str(),
                      kNames[static_cast<int>(kind)]);
    return -1;
}

} // namespace

int32_t IScriptContext::FindAnimationParameter(std::string_view name, EntityID entity)
{
    IAnimationService* animation = GetAnimation();
    return animation ? animation->FindParameter(Resolve(entity), name) : -1;
}

bool IScriptContext::SetAnimationFloat(std::string_view name, float value, EntityID entity)
{
    const EntityID id = Resolve(entity);
    const int32_t index = AnimationParameterOfKind(GetAnimation(), id, name, {AnimationParameterKind::Float, AnimationParameterKind::Int});
    return index >= 0 && GetAnimation()->SetParameter(id, index, value);
}

bool IScriptContext::SetAnimationInt(std::string_view name, int value, EntityID entity)
{
    const EntityID id = Resolve(entity);
    const int32_t index = AnimationParameterOfKind(GetAnimation(), id, name, {AnimationParameterKind::Int, AnimationParameterKind::Float});
    return index >= 0 && GetAnimation()->SetParameter(id, index, static_cast<float>(value));
}

bool IScriptContext::SetAnimationBool(std::string_view name, bool value, EntityID entity)
{
    const EntityID id = Resolve(entity);
    const int32_t index = AnimationParameterOfKind(GetAnimation(), id, name, {AnimationParameterKind::Bool});
    return index >= 0 && GetAnimation()->SetParameter(id, index, value ? 1.f : 0.f);
}

bool IScriptContext::SetAnimationTrigger(std::string_view name, EntityID entity)
{
    const EntityID id = Resolve(entity);
    const int32_t index = AnimationParameterOfKind(GetAnimation(), id, name, {AnimationParameterKind::Trigger});
    return index >= 0 && GetAnimation()->SetParameter(id, index, 1.f);
}

bool IScriptContext::ResetAnimationTrigger(std::string_view name, EntityID entity)
{
    const EntityID id = Resolve(entity);
    const int32_t index = AnimationParameterOfKind(GetAnimation(), id, name, {AnimationParameterKind::Trigger});
    return index >= 0 && GetAnimation()->SetParameter(id, index, 0.f);
}

bool IScriptContext::SetAnimationParameter(int32_t index, float value, EntityID entity)
{
    IAnimationService* animation = GetAnimation();
    return animation && animation->SetParameter(Resolve(entity), index, value);
}

float IScriptContext::GetAnimationFloat(std::string_view name, EntityID entity)
{
    const EntityID id = Resolve(entity);
    float value = 0.f;
    const int32_t index = AnimationParameterOfKind(GetAnimation(), id, name, {AnimationParameterKind::Float, AnimationParameterKind::Int});
    return index >= 0 && GetAnimation()->GetParameter(id, index, value) ? value : 0.f;
}

int IScriptContext::GetAnimationInt(std::string_view name, EntityID entity)
{
    const EntityID id = Resolve(entity);
    float value = 0.f;
    const int32_t index = AnimationParameterOfKind(GetAnimation(), id, name, {AnimationParameterKind::Int, AnimationParameterKind::Float});
    return index >= 0 && GetAnimation()->GetParameter(id, index, value) ? static_cast<int>(std::lround(value)) : 0;
}

bool IScriptContext::GetAnimationBool(std::string_view name, EntityID entity)
{
    const EntityID id = Resolve(entity);
    float value = 0.f;
    const int32_t index = AnimationParameterOfKind(GetAnimation(), id, name,
                                                   {AnimationParameterKind::Bool, AnimationParameterKind::Trigger});
    return index >= 0 && GetAnimation()->GetParameter(id, index, value) && value != 0.f;
}

std::string_view IScriptContext::GetAnimationState(EntityID entity)
{
    IAnimationService* animation = GetAnimation();
    return animation ? animation->GetCurrentState(Resolve(entity)) : std::string_view{};
}

bool IScriptContext::IsAnimationState(std::string_view state, EntityID entity)
{
    return GetAnimationState(entity) == state;
}

float IScriptContext::GetAnimationNormalizedTime(EntityID entity)
{
    IAnimationService* animation = GetAnimation();
    return animation ? animation->GetNormalizedTime(Resolve(entity)) : 0.f;
}

bool IScriptContext::IsAnimationInTransition(EntityID entity)
{
    IAnimationService* animation = GetAnimation();
    return animation && animation->IsInTransition(Resolve(entity));
}

bool IScriptContext::CrossFadeAnimation(std::string_view state, float blendSeconds, EntityID entity)
{
    IAnimationService* animation = GetAnimation();
    return animation && animation->CrossFade(Resolve(entity), state, blendSeconds);
}

// ── Partikel ─────────────────────────────────────────────────────────────────

bool IScriptContext::SetMaterialParameter(std::string_view name, float value, EntityID entity)
{
    return SetMaterialParameter(name, math::Vec3{value, value, value}, entity);
}

bool IScriptContext::SetMaterialParameter(std::string_view name, const math::Vec2& value, EntityID entity)
{
    return SetMaterialParameter(name, math::Vec3{value.x, value.y, 0.f}, entity);
}

bool IScriptContext::SetMaterialParameter(std::string_view name, const math::Vec3& value, EntityID entity)
{
    IMaterialService* materials = GetMaterials();
    return materials && materials->SetParameter(Resolve(entity), name, value);
}

bool IScriptContext::SetMaterialUV(EntityID entity, const math::Vec2& scale, const math::Vec2& offset)
{
    IMaterialService* materials = GetMaterials();
    return materials && materials->SetUV(Resolve(entity), scale, offset);
}

bool IScriptContext::PlayParticles(EntityID entity)
{
    IParticleService* particles = GetParticles();
    return particles && particles->Play(Resolve(entity));
}

bool IScriptContext::StopParticles(bool clear, EntityID entity)
{
    IParticleService* particles = GetParticles();
    return particles && particles->Stop(Resolve(entity), clear);
}

bool IScriptContext::AreParticlesPlaying(EntityID entity)
{
    IParticleService* particles = GetParticles();
    return particles && particles->IsPlaying(Resolve(entity));
}

bool IScriptContext::BurstParticles(uint32_t count, EntityID entity)
{
    IParticleService* particles = GetParticles();
    return particles && particles->Burst(Resolve(entity), count);
}

bool IScriptContext::EmitParticlesAt(EntityID templateEntity, const math::Vec3& position, uint32_t count)
{
    IParticleService* particles = GetParticles();
    return particles && particles->EmitAt(templateEntity, position, count);
}

uint32_t IScriptContext::GetParticleCount(EntityID entity)
{
    IParticleService* particles = GetParticles();
    return particles ? particles->ParticleCount(Resolve(entity)) : 0u;
}

bool IScriptContext::SetEmitterTexture(EntityID emitter, const std::string& texturePath)
{
    auto* service = GetParticles();
    return service ? service->SetEmitterTexture(emitter, texturePath) : false;
}

bool IScriptContext::SetEmitterAtlasGrid(EntityID emitter, uint32_t columns, uint32_t rows)
{
    auto* service = GetParticles();
    return service ? service->SetEmitterAtlasGrid(emitter, columns, rows) : false;
}

bool IScriptContext::SetEmitterFrames(EntityID emitter, std::span<const ParticleFrame> frames)
{
    auto* service = GetParticles();
    return service ? service->SetEmitterFrames(emitter, frames) : false;
}

uint32_t IScriptContext::GetEmitterFrameCount(EntityID emitter)
{
    auto* service = GetParticles();
    return service ? service->GetEmitterFrameCount(emitter) : 0u;
}

ParticleID IScriptContext::SpawnParticle(EntityID emitter, const math::Vec3& position)
{
    auto* service = GetParticles();
    return service ? service->SpawnParticle(emitter, position) : NULL_PARTICLE;
}

bool IScriptContext::SetParticlePosition(ParticleID particle, const math::Vec3& position)
{
    auto* service = GetParticles();
    return service ? service->SetParticlePosition(particle, position) : false;
}

bool IScriptContext::SetParticleVelocity(ParticleID particle, const math::Vec3& velocity)
{
    auto* service = GetParticles();
    return service ? service->SetParticleVelocity(particle, velocity) : false;
}

bool IScriptContext::SetParticleMotionEnabled(ParticleID particle, bool enabled)
{
    auto* service = GetParticles();
    return service ? service->SetParticleMotionEnabled(particle, enabled) : false;
}

bool IScriptContext::SetParticleSize(ParticleID particle, const math::Vec2& size)
{
    auto* service = GetParticles();
    return service ? service->SetParticleSize(particle, size) : false;
}

bool IScriptContext::SetParticleFrame(ParticleID particle, uint32_t frame)
{
    auto* service = GetParticles();
    return service ? service->SetParticleFrame(particle, frame) : false;
}

bool IScriptContext::PlayParticleFrames(ParticleID particle, uint32_t firstFrame, uint32_t lastFrame, float fps, bool loop)
{
    auto* service = GetParticles();
    return service ? service->PlayParticleFrames(particle, firstFrame, lastFrame, fps, loop) : false;
}

bool IScriptContext::PlayParticleFramesOverLifetime(ParticleID particle, uint32_t firstFrame, uint32_t lastFrame)
{
    auto* service = GetParticles();
    return service ? service->PlayParticleFramesOverLifetime(particle, firstFrame, lastFrame) : false;
}

bool IScriptContext::SetParticleFrameProgress(ParticleID particle, uint32_t firstFrame, uint32_t lastFrame, float progress)
{
    auto* service = GetParticles();
    return service ? service->SetParticleFrameProgress(particle, firstFrame, lastFrame, progress) : false;
}

bool IScriptContext::SetParticleAnimationPaused(ParticleID particle, bool paused)
{
    auto* service = GetParticles();
    return service ? service->SetParticleAnimationPaused(particle, paused) : false;
}

std::optional<uint32_t> IScriptContext::GetParticleFrame(ParticleID particle)
{
    auto* service = GetParticles();
    return service ? service->GetParticleFrame(particle) : std::nullopt;
}

bool IScriptContext::IsParticleAlive(ParticleID particle)
{
    auto* service = GetParticles();
    return service ? service->IsParticleAlive(particle) : false;
}

bool IScriptContext::RemoveParticle(ParticleID particle)
{
    auto* service = GetParticles();
    return service ? service->RemoveParticle(particle) : false;
}

bool IScriptContext::PlayParticleClip(ParticleID particle, std::string_view name)
{
    auto* service = GetParticles();
    return service && service->PlayParticleClip(particle, name);
}

std::optional<ParticleClip> IScriptContext::FindEmitterClip(EntityID emitter, std::string_view name)
{
    auto* service = GetParticles();
    return service ? service->FindEmitterClip(emitter, name) : std::nullopt;
}

EntityID IScriptContext::SpawnEffect(std::string_view prefabPath, const math::Vec3& position,
                                     const math::Quat& rotation)
{
    const EntityID root = InstantiatePrefab(prefabPath, position, rotation);
    if (IParticleService* particles = GetParticles(); particles && root.IsValid())
        particles->DestroyWhenFinished(root);
    return root;
}

void IScriptContext::DrawScreenText(std::string_view text, float x, float y, float size,
                                    const math::Vec4& color)
{
    if (ITextService* service = GetText())
        service->Draw(text, x, y, size, color);
}

math::Vec2 IScriptContext::MeasureScreenText(std::string_view text, float size)
{
    ITextService* service = GetText();
    return service ? service->Measure(text, size) : math::Vec2{0.f, 0.f};
}

math::Vec2 IScriptContext::GetScreenSize()
{
    ITextService* service = GetText();
    return service ? service->ScreenSize() : math::Vec2{0.f, 0.f};
}

bool IScriptContext::PhysicsRaycast(const math::Vec3& origin, const math::Vec3& direction, float maxDistance,
                                    collision::RaycastHit& outHit, bool ignoreSelf)
{
    IPhysicsService* physics = GetPhysics();
    if (!physics)
        return false;
    const EntityID ignore = ignoreSelf ? GetRoot() : NULL_ENTITY;
    return physics->Raycast(origin, direction, maxDistance, outHit, ignore);
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
