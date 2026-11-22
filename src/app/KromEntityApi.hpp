#pragma once
#include "krom.h"

#include "ecs/World.hpp"
#include "scene/BoundsSystem.hpp"
#include "scene/TransformSystem.hpp"

#include <cstdint>

// Entity functions of the krom.h wrapper: lifetime and identity, hierarchy,
// visibility/activity, transform, layers, tags, and the camera/light creators.
// Entities live in the ECS world, so the module owns no state. Most functions
// only need the world (null before Graphics()); SetParent and Detach also
// update the transform and bounds systems and take an EntityContext.
namespace Krom::internal {

// References into the wrapper State; built per call, never stored.
struct EntityContext
{
    engine::ecs::World* world = nullptr;    // null before Graphics()
    engine::TransformSystem& transformSystem;
    engine::BoundsSystem& boundsSystem;
};

// --- Hierarchy helpers (also used by the scene manager, DestroyEntity and the
// script destroy hook in Run()) -----------------------------------------------

// Destroys entity and all its children; detaches it from its parent first.
void DestroyEntityRecursive(engine::ecs::World& world, LPENTITY entity);
// Removes the ParentComponent of entity and the entity from the parent's children.
void DetachEntityFromParent(engine::ecs::World& world, LPENTITY entity);

// --- krom.h functions --------------------------------------------------------
// Same behaviour as the krom.h functions of the same name; world may be null.

[[nodiscard]] LPENTITY CreateEntity(engine::ecs::World* world, const char* name);
[[nodiscard]] bool IsAlive(engine::ecs::World* world, LPENTITY entity);
bool SetName(engine::ecs::World* world, LPENTITY entity, const char* name);
[[nodiscard]] const char* GetName(engine::ecs::World* world, LPENTITY entity);
[[nodiscard]] LPENTITY FindEntity(engine::ecs::World* world, const char* name);
// Main camera; falls back to the first CameraComponent.
[[nodiscard]] LPENTITY GetMainCamera(engine::ecs::World* world);

bool SetLayer(engine::ecs::World* world, LPENTITY entity, uint32_t layerMask);
[[nodiscard]] uint32_t GetLayer(engine::ecs::World* world, LPENTITY entity);
bool SetCameraLayers(engine::ecs::World* world, LPENTITY camera, uint32_t cullingMask);
[[nodiscard]] uint32_t GetCameraLayers(engine::ecs::World* world, LPENTITY camera);
bool SetTag(engine::ecs::World* world, LPENTITY entity, const char* tag);
[[nodiscard]] const char* GetTag(engine::ecs::World* world, LPENTITY entity);
[[nodiscard]] LPENTITY FindEntityByTag(engine::ecs::World* world, const char* tag);

[[nodiscard]] LPENTITY FindChild(engine::ecs::World* world, LPENTITY rootEntity, const char* name);
[[nodiscard]] int CountChildren(engine::ecs::World* world, LPENTITY entity);
[[nodiscard]] LPENTITY GetChild(engine::ecs::World* world, LPENTITY entity, int index);
bool SetVisible(engine::ecs::World* world, LPENTITY entity, bool visible, bool recursive);
bool SetActive(engine::ecs::World* world, LPENTITY entity, bool active, bool recursive);
// Both update the transform and bounds systems afterwards.
bool SetParent(const EntityContext& ctx, LPENTITY child, LPENTITY parent, bool keepWorldTransform);
bool Detach(const EntityContext& ctx, LPENTITY child, bool keepWorldTransform);

bool SetPosition(engine::ecs::World* world, LPENTITY entity, const engine::math::Vec3& position);
bool SetRotationEulerDeg(engine::ecs::World* world, LPENTITY entity, float pitch, float yaw, float roll);
[[nodiscard]] engine::math::Vec3 GetRotationEulerDeg(engine::ecs::World* world, LPENTITY entity);
bool SetScale(engine::ecs::World* world, LPENTITY entity, const engine::math::Vec3& scale);
bool MoveEntity(engine::ecs::World* world, LPENTITY entity, float x, float y, float z);
bool TurnEntity(engine::ecs::World* world, LPENTITY entity, float pitch, float yaw, float roll);
bool GetWorldBounds(engine::ecs::World* world,
                    LPENTITY entity,
                    engine::math::Vec3& outCenter,
                    engine::math::Vec3& outExtents,
                    float& outBoundingSphere);
bool LookAt(engine::ecs::World* world, LPENTITY entity, const engine::math::Vec3& target, const engine::math::Vec3& up);
bool LookAt(engine::ecs::World* world, LPENTITY entity, LPENTITY targetEntity, const engine::math::Vec3& up);

[[nodiscard]] LPENTITY CreateCamera(engine::ecs::World* world,
                                    const char* name,
                                    const engine::math::Vec3& position,
                                    float fovYDeg,
                                    bool isMainCamera);
[[nodiscard]] LPENTITY CreateDirectionalLight(engine::ecs::World* world,
                                              const char* name,
                                              const engine::math::Vec3& position,
                                              const engine::math::Vec3& eulerDeg,
                                              const engine::math::Vec3& color,
                                              float intensity);
[[nodiscard]] LPENTITY CreatePointLight(engine::ecs::World* world,
                                        const char* name,
                                        const engine::math::Vec3& position,
                                        const engine::math::Vec3& color,
                                        float intensity,
                                        float range);
[[nodiscard]] LPENTITY CreateSpotLight(engine::ecs::World* world,
                                       const char* name,
                                       const engine::math::Vec3& position,
                                       const engine::math::Vec3& eulerDeg,
                                       const engine::math::Vec3& color,
                                       float intensity,
                                       float range,
                                       float innerDeg,
                                       float outerDeg);

} // namespace Krom::internal
