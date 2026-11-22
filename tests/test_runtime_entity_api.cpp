// Opt-in integration test of the krom.h entity API against the real runtime:
// lifetime, names, hierarchy, transform, visibility/activity, layers, tags,
// cameras and lights. Start from the repository directory; the DX11 window
// stays hidden. The checks document the current behaviour of the wrapper.
#include "krom.h"
#include "addons/camera/CameraComponents.hpp"
#include "addons/lighting/LightingComponents.hpp"
#include "addons/mesh_renderer/MeshRendererComponents.hpp"
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

using engine::math::Quat;
using engine::math::Vec3;

bool Near(float a, float b, float tolerance = 1.0e-3f)
{
    return std::fabs(a - b) <= tolerance;
}

bool NearVec(const Vec3& a, const Vec3& b, float tolerance = 1.0e-3f)
{
    return Near(a.x, b.x, tolerance) && Near(a.y, b.y, tolerance) && Near(a.z, b.z, tolerance);
}

// Same rotation (q and -q are equal).
bool SameRotation(const Quat& a, const Quat& b)
{
    const float dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    return std::fabs(std::fabs(dot) - 1.0f) <= 1.0e-3f;
}

Vec3 WorldPosition(LPENTITY entity)
{
    const auto* wt = Krom::GetComponent<engine::WorldTransformComponent>(entity);
    return wt ? wt->position : Vec3{-9999.f, -9999.f, -9999.f};
}

Vec3 LocalPosition(LPENTITY entity)
{
    const auto* tr = Krom::GetComponent<engine::TransformComponent>(entity);
    return tr ? tr->localPosition : Vec3{-9999.f, -9999.f, -9999.f};
}

// Engine convention: local -Z is forward.
Vec3 Forward(LPENTITY entity)
{
    const auto* tr = Krom::GetComponent<engine::TransformComponent>(entity);
    return tr ? tr->localRotation.Rotate(Vec3{0.f, 0.f, -1.f}) : Vec3{};
}

LPENTITY CreateTriangleEntity(const char* name)
{
    // Bounds [-1, 1] on every axis.
    static const Krom::MeshVertex vertices[3] = {
        {{-1.f, -1.f, -1.f}, {}, {0.f, 0.f}, {1.f, 1.f, 1.f, 1.f}},
        {{ 1.f, -1.f,  1.f}, {}, {1.f, 0.f}, {1.f, 1.f, 1.f, 1.f}},
        {{ 0.f,  1.f,  0.f}, {}, {0.5f, 1.f}, {1.f, 1.f, 1.f, 1.f}},
    };
    Krom::MeshData data{};
    data.name = name;
    data.vertices = vertices;
    data.vertexCount = 3u;
    return Krom::CreateMeshEntity(data);
}

} // namespace

int main()
{
    int failures = 0;
    const auto check = [&](bool ok, const char* message) {
        std::printf("%s: %s\n", ok ? "PASS" : "FAIL", message);
        if (!ok) ++failures;
    };

    // --- Before Graphics(): no world -------------------------------------------
    check(!Krom::CreateEntity("Early").IsValid(), "CreateEntity before Graphics returns null");
    check(!Krom::IsAlive(NULL_LPENTITY), "IsAlive before Graphics is false");
    check(!Krom::FindEntity("Early").IsValid() && !Krom::FindEntityByTag("x").IsValid(),
          "FindEntity / FindEntityByTag before Graphics return null");
    check(!Krom::GetMainCamera().IsValid(), "GetMainCamera before Graphics returns null");
    check(!Krom::CreateCamera("EarlyCam").IsValid() && !Krom::CreatePointLight("EarlyLight").IsValid(),
          "camera / light creators before Graphics return null");
    check(std::strcmp(Krom::GetName(NULL_LPENTITY), "") == 0 && std::strcmp(Krom::GetTag(NULL_LPENTITY), "") == 0,
          "GetName / GetTag before Graphics return empty");
    check(Krom::GetLayer(NULL_LPENTITY) == engine::renderer::LAYER_DEFAULT &&
          Krom::GetCameraLayers(NULL_LPENTITY) == engine::renderer::LAYER_ALL,
          "GetLayer / GetCameraLayers before Graphics return defaults");

    const auto sourceRoot = std::filesystem::path(__FILE__).parent_path().parent_path();
    Krom::SetAssetRoot(sourceRoot / "projects/DemoProject/Assets");
    Krom::GraphicsConfig config{};
    config.backend = Krom::Renderer::DX11;
    config.visible = false;
    config.audioDevice = false;
    config.enableForwardPlus = false;
    config.enableGtao = false;
    config.width = 64;
    config.height = 64;
    if (!Krom::Graphics(config))
        return 2;

    // --- Create / IsAlive / components of a new entity -------------------------
    const LPENTITY plain = Krom::CreateEntity("Plain");
    check(plain.IsValid() && Krom::IsAlive(plain), "CreateEntity returns a live entity");
    check(Krom::HasComponent<engine::TransformComponent>(plain) &&
          Krom::HasComponent<engine::WorldTransformComponent>(plain) &&
          Krom::HasComponent<engine::ActiveComponent>(plain) &&
          Krom::HasComponent<engine::NameComponent>(plain),
          "new entity has transform, world transform, active and name components");
    check(!Krom::HasComponent<engine::VisibilityComponent>(plain), "new entity has no VisibilityComponent");
    const LPENTITY unnamed = Krom::CreateEntity(nullptr);
    check(unnamed.IsValid() && !Krom::HasComponent<engine::NameComponent>(unnamed) &&
          std::strcmp(Krom::GetName(unnamed), "") == 0,
          "CreateEntity(nullptr) has no NameComponent and an empty name");
    const LPENTITY emptyName = Krom::CreateEntity("");
    check(emptyName.IsValid() && !Krom::HasComponent<engine::NameComponent>(emptyName),
          "CreateEntity(\"\") has no NameComponent");
    check(!Krom::IsAlive(NULL_LPENTITY), "IsAlive(null) is false");

    // --- Names and FindEntity ---------------------------------------------------
    check(std::strcmp(Krom::GetName(plain), "Plain") == 0, "GetName returns the creation name");
    check(Krom::SetName(plain, "Renamed") && std::strcmp(Krom::GetName(plain), "Renamed") == 0, "SetName renames");
    check(Krom::SetName(unnamed, "LateName") && Krom::HasComponent<engine::NameComponent>(unnamed) &&
          std::strcmp(Krom::GetName(unnamed), "LateName") == 0,
          "SetName adds a NameComponent when missing");
    check(Krom::SetName(emptyName, nullptr) && Krom::HasComponent<engine::NameComponent>(emptyName) &&
          std::strcmp(Krom::GetName(emptyName), "") == 0,
          "SetName(nullptr) stores an empty name");
    check(Krom::FindEntity("Renamed") == plain, "FindEntity finds by name");
    check(!Krom::FindEntity("Plain").IsValid(), "FindEntity does not find the old name");
    check(!Krom::FindEntity(nullptr).IsValid() && !Krom::FindEntity("NoSuchEntity").IsValid(),
          "FindEntity(null / unknown) returns null");
    check(!Krom::SetName(NULL_LPENTITY, "x"), "SetName on null entity fails");

    // --- Hierarchy: SetParent, FindChild, CountChildren, GetChild --------------
    const LPENTITY root = Krom::CreateEntity("Root");
    const LPENTITY childA = Krom::CreateEntity("ChildA");
    const LPENTITY childB = Krom::CreateEntity("ChildB");
    const LPENTITY grandChild = Krom::CreateEntity("GrandChild");
    check(Krom::CountChildren(root) == 0 && !Krom::GetChild(root, 0).IsValid(), "root starts without children");
    check(Krom::SetParent(childA, root, false) && Krom::SetParent(childB, root, false) &&
          Krom::SetParent(grandChild, childA, false),
          "SetParent builds a three-level hierarchy");
    check(Krom::CountChildren(root) == 2 && Krom::CountChildren(childA) == 1 && Krom::CountChildren(childB) == 0,
          "CountChildren counts direct children only");
    check(Krom::GetChild(root, 0) == childA && Krom::GetChild(root, 1) == childB, "GetChild in insertion order");
    check(!Krom::GetChild(root, 2).IsValid() && !Krom::GetChild(root, -1).IsValid(),
          "GetChild out of range / negative returns null");
    check(Krom::FindChild(root, "GrandChild") == grandChild, "FindChild searches recursively");
    check(Krom::FindChild(root, "Root") == root, "FindChild also matches the root itself");
    check(!Krom::FindChild(root, "Missing").IsValid() && !Krom::FindChild(root, nullptr).IsValid(),
          "FindChild(missing / null name) returns null");
    check(!Krom::FindChild(childB, "GrandChild").IsValid(), "FindChild does not search siblings");
    check(!Krom::SetParent(root, grandChild, true), "SetParent rejects a cycle (parent under own descendant)");
    check(!Krom::SetParent(root, root, true), "SetParent rejects self-parenting");
    check(!Krom::SetParent(childA, NULL_LPENTITY, true) && !Krom::SetParent(NULL_LPENTITY, root, true),
          "SetParent rejects null entities");
    check(Krom::CountChildren(root) == 2 && Krom::CountChildren(grandChild) == 0,
          "rejected SetParent leaves the hierarchy unchanged");

    // Re-parenting moves the child between parents.
    check(Krom::SetParent(grandChild, childB, false), "re-parent grand child to ChildB");
    check(Krom::CountChildren(childA) == 0 && Krom::CountChildren(childB) == 1 && Krom::GetChild(childB, 0) == grandChild,
          "re-parenting removes the child from the old parent");
    check(Krom::Detach(grandChild, false) && Krom::CountChildren(childB) == 0 &&
          !Krom::HasComponent<engine::ParentComponent>(grandChild),
          "Detach removes the parent link");
    check(Krom::Detach(grandChild, true), "Detach of an entity without parent succeeds");
    check(!Krom::Detach(NULL_LPENTITY, true), "Detach of null entity fails");

    // --- SetParent / Detach with and without keepWorldTransform -----------------
    const LPENTITY tParent = Krom::CreateEntity("TParent");
    const LPENTITY tChild = Krom::CreateEntity("TChild");
    Krom::SetPosition(tParent, 10.f, 0.f, 0.f);
    Krom::SetScale(tParent, 2.f);
    Krom::SetPosition(tChild, 1.f, 0.f, 0.f);
    check(Krom::SetParent(tChild, tParent, false), "SetParent without keepWorldTransform");
    check(NearVec(LocalPosition(tChild), {1.f, 0.f, 0.f}), "without keepWorldTransform the local position stays");
    check(NearVec(WorldPosition(tChild), {12.f, 0.f, 0.f}), "without keepWorldTransform the world position follows the parent");
    check(Krom::Detach(tChild, true), "Detach with keepWorldTransform");
    check(NearVec(WorldPosition(tChild), {12.f, 0.f, 0.f}) && NearVec(LocalPosition(tChild), {12.f, 0.f, 0.f}),
          "Detach with keepWorldTransform keeps the world position");
    {
        const auto* tr = Krom::GetComponent<engine::TransformComponent>(tChild);
        check(tr && NearVec(tr->localScale, {2.f, 2.f, 2.f}), "Detach with keepWorldTransform keeps the inherited world scale");
    }

    Krom::SetRotationEulerDeg(tParent, 0.f, 90.f, 0.f);
    Krom::SetPosition(tChild, 13.f, 2.f, -3.f);
    check(Krom::SetParent(tChild, tParent, true), "SetParent with keepWorldTransform (rotated, scaled parent)");
    check(NearVec(WorldPosition(tChild), {13.f, 2.f, -3.f}), "keepWorldTransform keeps the world position");
    {
        const auto* tr = Krom::GetComponent<engine::TransformComponent>(tChild);
        // World scale 2 (kept from the Detach above) divided by the parent scale 2.
        check(tr && NearVec(tr->localScale, {1.f, 1.f, 1.f}), "keepWorldTransform divides the scale by the parent scale");
    }
    check(Krom::Detach(tChild, false), "Detach without keepWorldTransform");
    check(NearVec(WorldPosition(tChild), LocalPosition(tChild)),
          "after Detach without keepWorldTransform the world position equals the local one");

    // --- Destroy ------------------------------------------------------------------
    check(Krom::DestroyEntity(root), "DestroyEntity of a parent");
    check(!Krom::IsAlive(root) && !Krom::IsAlive(childA) && !Krom::IsAlive(childB),
          "destroying a parent destroys its children");
    check(Krom::IsAlive(grandChild), "detached former grand child survives");
    check(!Krom::DestroyEntity(root), "DestroyEntity of a dead entity fails");
    check(!Krom::DestroyEntity(NULL_LPENTITY), "DestroyEntity of null fails");
    check(!Krom::SetPosition(childA, 1.f, 2.f, 3.f) && !Krom::SetName(childA, "x") &&
          !Krom::SetTag(childA, "x") && !Krom::SetVisible(childA, false, true) &&
          !Krom::SetActive(childA, false, true) && !Krom::SetScale(childA, 2.f) &&
          !Krom::SetRotationEulerDeg(childA, 1.f, 2.f, 3.f) && !Krom::MoveEntity(childA, 1.f, 0.f, 0.f) &&
          !Krom::TurnEntity(childA, 1.f, 0.f, 0.f) && !Krom::LookAt(childA, Vec3{0.f, 0.f, -1.f}) &&
          !Krom::SetParent(childA, grandChild, true) && !Krom::SetParent(grandChild, childA, true) &&
          !Krom::Detach(childA, true) && !Krom::SetLayer(childA, 2u) && !Krom::SetCameraLayers(childA, 2u),
          "setters reject a destroyed entity");
    check(std::strcmp(Krom::GetName(childA), "") == 0 && Krom::CountChildren(root) == 0 &&
          !Krom::GetChild(root, 0).IsValid() && !Krom::FindChild(root, "ChildA").IsValid() &&
          !Krom::FindEntity("ChildA").IsValid(),
          "getters return empty values for a destroyed entity");
    {
        Vec3 c{}, e{};
        float s = 0.f;
        check(!Krom::GetWorldBounds(childA, c, e, s), "GetWorldBounds rejects a destroyed entity");
    }

    // --- Transform setters ----------------------------------------------------------
    const LPENTITY mover = Krom::CreateEntity("Mover");
    check(Krom::SetPosition(mover, Vec3{1.f, 2.f, 3.f}) && NearVec(LocalPosition(mover), {1.f, 2.f, 3.f}),
          "SetPosition(Vec3)");
    check(Krom::SetPosition(mover, 4.f, 5.f, 6.f) && NearVec(LocalPosition(mover), {4.f, 5.f, 6.f}),
          "SetPosition(x, y, z)");
    check(Krom::SetRotationEulerDeg(mover, 0.f, 90.f, 0.f), "SetRotationEulerDeg yaw 90");
    {
        const Vec3 expectedOffset = Quat::FromEulerDeg(0.f, 90.f, 0.f).Rotate(Vec3{0.f, 0.f, -2.f});
        check(Krom::MoveEntity(mover, 0.f, 0.f, -2.f) &&
              NearVec(LocalPosition(mover), Vec3{4.f, 5.f, 6.f} + expectedOffset),
              "MoveEntity moves along the local rotation");
        check(!NearVec(expectedOffset, {0.f, 0.f, -2.f}), "MoveEntity offset differs from the unrotated one");
    }
    check(Krom::TurnEntity(mover, 10.f, 0.f, 0.f), "TurnEntity");
    {
        const auto* tr = Krom::GetComponent<engine::TransformComponent>(mover);
        const Quat expected = (Quat::FromEulerDeg(0.f, 90.f, 0.f) * Quat::FromEulerDeg(10.f, 0.f, 0.f)).Normalized();
        check(tr && SameRotation(tr->localRotation, expected), "TurnEntity multiplies the local rotation on the right");
    }
    {
        const auto* tr = Krom::GetComponent<engine::TransformComponent>(mover);
        check(Krom::SetScale(mover, Vec3{1.f, 2.f, 3.f}) && tr && NearVec(tr->localScale, {1.f, 2.f, 3.f}), "SetScale(Vec3)");
        check(Krom::SetScale(mover, 4.f, 5.f, 6.f) && tr && NearVec(tr->localScale, {4.f, 5.f, 6.f}), "SetScale(x, y, z)");
        check(Krom::SetScale(mover, 7.f) && tr && NearVec(tr->localScale, {7.f, 7.f, 7.f}), "SetScale(uniform)");
    }

    // GetRotationEulerDeg reads the world transform, which only changes with a
    // transform update (SetParent/Detach/DestroyEntity or a frame).
    const LPENTITY rot = Krom::CreateEntity("Rot");
    Krom::SetRotationEulerDeg(rot, 20.f, 30.f, 0.f);
    check(NearVec(Krom::GetRotationEulerDeg(rot), {0.f, 0.f, 0.f}),
          "GetRotationEulerDeg right after SetRotationEulerDeg still returns the old world rotation");
    check(NearVec(Krom::GetRotationEulerDeg(NULL_LPENTITY), {0.f, 0.f, 0.f}), "GetRotationEulerDeg(null) is zero");

    // Round trip entities, checked after the first frame.
    const Vec3 eulerCases[] = {
        {0.f, 45.f, 0.f}, {20.f, 30.f, 0.f}, {-30.f, 0.f, 10.f}, {10.f, -60.f, 25.f}, {0.f, 0.f, 0.f},
    };
    LPENTITY eulerEntities[5]{};
    for (int i = 0; i < 5; ++i)
    {
        eulerEntities[i] = Krom::CreateEntity("Euler");
        Krom::SetRotationEulerDeg(eulerEntities[i], eulerCases[i].x, eulerCases[i].y, eulerCases[i].z);
    }

    // LookAt towards a point (entity at the origin, world transform current).
    const LPENTITY looker = Krom::CreateEntity("Looker");
    check(Krom::LookAt(looker, Vec3{3.f, 0.f, -4.f}), "LookAt(point)");
    check(NearVec(Forward(looker), Vec3{0.6f, 0.f, -0.8f}), "LookAt(point) points local -Z at the target");
    check(Krom::LookAt(looker, Vec3{0.f, 5.f, -5.f}) && NearVec(Forward(looker), Vec3{0.f, 0.70710678f, -0.70710678f}),
          "LookAt(point) with pitch");
    check(Krom::LookAt(looker, Vec3{0.f, 10.f, 0.f}) && NearVec(Forward(looker), Vec3{0.f, 1.f, 0.f}),
          "LookAt(point) straight up falls back to another right vector");
    check(!Krom::LookAt(looker, Vec3{0.f, 0.f, 0.f}), "LookAt at its own position fails");
    // Targets for LookAt(entity), checked after the first frame.
    const LPENTITY lookTarget = Krom::CreateEntity("LookTarget");
    Krom::SetPosition(lookTarget, -4.f, 0.f, 3.f);
    const LPENTITY entityLooker = Krom::CreateEntity("EntityLooker");
    Krom::SetPosition(entityLooker, 0.f, 0.f, 3.f);

    // --- Visibility and activity ---------------------------------------------------------
    const LPENTITY visRoot = CreateTriangleEntity("VisRoot");
    const LPENTITY visChild = CreateTriangleEntity("VisChild");
    check(visRoot.IsValid() && visChild.IsValid() && Krom::SetParent(visChild, visRoot, false), "mesh hierarchy");
    const auto visibleOf = [](LPENTITY e) -> int {   // -1: no VisibilityComponent
        const auto* v = Krom::GetComponent<engine::VisibilityComponent>(e);
        return v ? (v->visible ? 1 : 0) : -1;
    };
    const auto meshVisibleOf = [](LPENTITY e) {
        const auto* m = Krom::GetComponent<engine::MeshComponent>(e);
        return m && m->visible;
    };
    check(Krom::SetVisible(visRoot, false, false), "SetVisible non-recursive");
    check(visibleOf(visRoot) == 0 && !meshVisibleOf(visRoot), "SetVisible sets VisibilityComponent and MeshComponent::visible");
    check(visibleOf(visChild) == -1 && meshVisibleOf(visChild), "SetVisible non-recursive leaves the child alone");
    check(Krom::SetVisible(visRoot, false, true), "SetVisible recursive");
    check(visibleOf(visChild) == 0 && !meshVisibleOf(visChild), "SetVisible recursive hides the child");
    check(Krom::SetVisible(visRoot, true), "SetVisible default is recursive");
    check(visibleOf(visRoot) == 1 && visibleOf(visChild) == 1 && meshVisibleOf(visRoot) && meshVisibleOf(visChild),
          "SetVisible(true) shows root and child");

    const auto activeOf = [](LPENTITY e) -> int {
        const auto* a = Krom::GetComponent<engine::ActiveComponent>(e);
        return a ? (a->active ? 1 : 0) : -1;
    };
    check(Krom::SetActive(visRoot, false, false) && activeOf(visRoot) == 0 && activeOf(visChild) == 1,
          "SetActive non-recursive");
    check(Krom::SetActive(visRoot, false, true) && activeOf(visRoot) == 0 && activeOf(visChild) == 0,
          "SetActive recursive");
    check(Krom::SetActive(visRoot, true) && activeOf(visRoot) == 1 && activeOf(visChild) == 1,
          "SetActive default is recursive");
    Krom::RemoveComponent<engine::ActiveComponent>(visChild);
    check(Krom::SetActive(visChild, false, false) && activeOf(visChild) == 0,
          "SetActive adds a missing ActiveComponent");
    Krom::SetActive(visChild, true, false);
    check(visibleOf(visRoot) == 1 && meshVisibleOf(visRoot), "SetActive does not touch visibility");

    // Bounds hierarchy, checked after the first frame.
    const LPENTITY boundsRoot = CreateTriangleEntity("BoundsRoot");
    const LPENTITY boundsChild = CreateTriangleEntity("BoundsChild");
    Krom::SetPosition(boundsRoot, 0.f, 10.f, 0.f);
    Krom::SetPosition(boundsChild, 4.f, 0.f, 0.f);
    check(Krom::SetParent(boundsChild, boundsRoot, false), "bounds hierarchy");
    const LPENTITY noBounds = Krom::CreateEntity("NoBounds");

    // --- Layers and tags ---------------------------------------------------------------------
    check(!Krom::SetLayer(plain, Krom::Layer::User0), "SetLayer without MeshComponent fails");
    check(Krom::GetLayer(plain) == engine::renderer::LAYER_DEFAULT, "GetLayer without MeshComponent is LAYER_DEFAULT");
    check(Krom::GetLayer(visRoot) == engine::renderer::LAYER_DEFAULT, "mesh entity starts on LAYER_DEFAULT");
    check(Krom::SetLayer(visRoot, Krom::Layer::User1 | Krom::Layer::User2) &&
          Krom::GetLayer(visRoot) == (Krom::Layer::User1 | Krom::Layer::User2),
          "SetLayer / GetLayer round trip");
    check(Krom::GetLayer(visChild) == engine::renderer::LAYER_DEFAULT, "SetLayer is not recursive");

    check(!Krom::SetCameraLayers(plain, 3u) && Krom::GetCameraLayers(plain) == engine::renderer::LAYER_ALL,
          "camera layers on a non-camera: set fails, get returns LAYER_ALL");

    check(std::strcmp(Krom::GetTag(plain), "") == 0, "GetTag without TagComponent is empty");
    check(Krom::SetTag(plain, "Enemy") && std::strcmp(Krom::GetTag(plain), "Enemy") == 0, "SetTag / GetTag");
    check(Krom::SetTag(plain, "Boss") && std::strcmp(Krom::GetTag(plain), "Boss") == 0, "SetTag replaces the tag");
    check(Krom::SetTag(mover, nullptr) && Krom::HasComponent<engine::TagComponent>(mover) &&
          std::strcmp(Krom::GetTag(mover), "") == 0,
          "SetTag(nullptr) stores an empty tag");
    check(Krom::FindEntityByTag("Boss") == plain, "FindEntityByTag finds the entity");
    check(!Krom::FindEntityByTag("Enemy").IsValid(), "FindEntityByTag does not find the old tag");
    check(!Krom::FindEntityByTag(nullptr).IsValid() && !Krom::FindEntityByTag("Nobody").IsValid(),
          "FindEntityByTag(null / unknown) returns null");
    Krom::SetTag(visRoot, "Boss");
    {
        const LPENTITY found = Krom::FindEntityByTag("Boss");
        check(found == plain || found == visRoot, "FindEntityByTag with two matches returns one of them");
    }

    // --- Cameras -------------------------------------------------------------------------------
    check(!Krom::GetMainCamera().IsValid(), "GetMainCamera without cameras returns null");
    const LPENTITY sideCam = Krom::CreateCamera("SideCam", Vec3{1.f, 2.f, 3.f}, 75.f, false);
    check(sideCam.IsValid() && std::strcmp(Krom::GetName(sideCam), "SideCam") == 0, "CreateCamera");
    check(Krom::GetMainCamera() == sideCam, "GetMainCamera falls back to the only camera");
    {
        const auto* cam = Krom::GetComponent<engine::CameraComponent>(sideCam);
        const auto* tr = Krom::GetComponent<engine::TransformComponent>(sideCam);
        check(cam && cam->projection == engine::ProjectionType::Perspective && Near(cam->fovYDeg, 75.f) &&
              Near(cam->nearPlane, 0.05f) && Near(cam->farPlane, 1000.f) && !cam->isMainCamera,
              "CreateCamera sets projection, fov, near/far and the main flag");
        check(tr && NearVec(tr->localPosition, {1.f, 2.f, 3.f}) &&
              SameRotation(tr->localRotation, Quat::FromEulerDeg(-10.f, 0.f, 0.f)),
              "CreateCamera sets position and pitch -10");
    }
    const LPENTITY mainCam = Krom::CreateCamera();
    check(mainCam.IsValid() && std::strcmp(Krom::GetName(mainCam), "MainCamera") == 0, "CreateCamera with defaults");
    check(Krom::GetMainCamera() == mainCam, "GetMainCamera prefers the main camera");
    {
        const auto* cam = Krom::GetComponent<engine::CameraComponent>(mainCam);
        check(cam && cam->isMainCamera && Near(cam->fovYDeg, 60.f) && NearVec(LocalPosition(mainCam), {0.f, 2.f, 10.f}),
              "default camera: main, fov 60, position (0, 2, 10)");
        check(cam && Krom::GetCameraLayers(mainCam) == cam->cullingMask, "GetCameraLayers returns the culling mask");
    }
    check(Krom::SetCameraLayers(mainCam, Krom::Layer::Default | Krom::Layer::User3) &&
          Krom::GetCameraLayers(mainCam) == (Krom::Layer::Default | Krom::Layer::User3),
          "SetCameraLayers / GetCameraLayers round trip");

    // --- Lights --------------------------------------------------------------------------------
    const LPENTITY sun = Krom::CreateDirectionalLight("TestSun", Vec3{1.f, 2.f, 3.f}, Vec3{-40.f, 20.f, 0.f},
                                                      Vec3{1.f, 0.9f, 0.8f}, 3.f);
    {
        const auto* light = Krom::GetComponent<engine::LightComponent>(sun);
        const auto* tr = Krom::GetComponent<engine::TransformComponent>(sun);
        check(light && light->type == engine::LightType::Directional && NearVec(light->color, {1.f, 0.9f, 0.8f}) &&
              Near(light->intensity, 3.f) && light->castShadows,
              "CreateDirectionalLight: type, color, intensity, shadows");
        check(light && light->shadowSettings.enabled && light->shadowSettings.resolution == 2048u &&
              Near(light->shadowSettings.bias, 0.0015f, 1.0e-6f) && light->shadowSettings.cascadeCount == 4u &&
              Near(light->shadowSettings.cascadeLambda, 0.5f) && Near(light->shadowSettings.maxDistance, 50.f),
              "CreateDirectionalLight: shadow settings with directional defaults");
        check(tr && NearVec(tr->localPosition, {1.f, 2.f, 3.f}) &&
              SameRotation(tr->localRotation, Quat::FromEulerDeg(-40.f, 20.f, 0.f)),
              "CreateDirectionalLight: position and rotation");
    }
    const LPENTITY point = Krom::CreatePointLight("TestPoint", Vec3{0.f, 1.f, 0.f}, Vec3{0.5f, 0.5f, 1.f}, 2.f, 7.f);
    {
        const auto* light = Krom::GetComponent<engine::LightComponent>(point);
        const auto* tr = Krom::GetComponent<engine::TransformComponent>(point);
        check(light && light->type == engine::LightType::Point && NearVec(light->color, {0.5f, 0.5f, 1.f}) &&
              Near(light->intensity, 2.f) && Near(light->range, 7.f) && light->castShadows,
              "CreatePointLight: type, color, intensity, range");
        check(light && light->shadowSettings.enabled && light->shadowSettings.resolution == 1024u &&
              Near(light->shadowSettings.bias, 0.0015f, 1.0e-6f) && Near(light->shadowSettings.maxDistance, 7.f) &&
              light->shadowSettings.cascadeCount == 1u,
              "CreatePointLight: shadow settings, max distance = range");
        check(tr && NearVec(tr->localPosition, {0.f, 1.f, 0.f}) && SameRotation(tr->localRotation, Quat::Identity()),
              "CreatePointLight: position, no rotation");
    }
    const LPENTITY spot = Krom::CreateSpotLight("TestSpot", Vec3{0.f, 4.f, 0.f}, Vec3{-70.f, 10.f, 0.f},
                                                Vec3{1.f, 1.f, 0.5f}, 5.f, 12.f, 12.f, 25.f);
    {
        const auto* light = Krom::GetComponent<engine::LightComponent>(spot);
        const auto* tr = Krom::GetComponent<engine::TransformComponent>(spot);
        check(light && light->type == engine::LightType::Spot && NearVec(light->color, {1.f, 1.f, 0.5f}) &&
              Near(light->intensity, 5.f) && Near(light->range, 12.f) &&
              Near(light->spotInnerDeg, 12.f) && Near(light->spotOuterDeg, 25.f) && light->castShadows,
              "CreateSpotLight: type, color, intensity, range, cone");
        check(light && light->shadowSettings.enabled && light->shadowSettings.resolution == 1024u &&
              Near(light->shadowSettings.bias, 0.0015f, 1.0e-6f) && Near(light->shadowSettings.maxDistance, 12.f),
              "CreateSpotLight: shadow settings, max distance = range");
        check(tr && NearVec(tr->localPosition, {0.f, 4.f, 0.f}) &&
              SameRotation(tr->localRotation, Quat::FromEulerDeg(-70.f, 10.f, 0.f)),
              "CreateSpotLight: position and rotation");
    }
    check(Krom::GetMainCamera() == mainCam, "lights do not change the main camera");

    // --- Frames ------------------------------------------------------------------------------
    int frame = 0;
    const int runResult = Krom::Run([&](float) {
        ++frame;
        if (frame == 2)
        {
            for (int i = 0; i < 5; ++i)
            {
                char message[128];
                const Vec3 e = Krom::GetRotationEulerDeg(eulerEntities[i]);
                std::snprintf(message, sizeof(message), "GetRotationEulerDeg round trip (%g, %g, %g)",
                              eulerCases[i].x, eulerCases[i].y, eulerCases[i].z);
                check(NearVec(e, eulerCases[i], 0.05f), message);
            }

            check(NearVec(WorldPosition(lookTarget), {-4.f, 0.f, 3.f}), "world position updated by the frame");
            check(Krom::LookAt(entityLooker, lookTarget), "LookAt(entity)");
            check(NearVec(Forward(entityLooker), {-1.f, 0.f, 0.f}), "LookAt(entity) points local -Z at the target");
            check(!Krom::LookAt(entityLooker, entityLooker), "LookAt at itself fails");

            Vec3 center{}, extents{};
            float sphere = 0.f;
            check(Krom::GetWorldBounds(boundsRoot, center, extents, sphere), "GetWorldBounds of a mesh hierarchy");
            check(NearVec(center, {2.f, 10.f, 0.f}) && NearVec(extents, {3.f, 1.f, 1.f}) &&
                  Near(sphere, std::sqrt(11.f)),
                  "GetWorldBounds covers root and child");
            check(Krom::GetWorldBounds(boundsChild, center, extents, sphere) &&
                  NearVec(center, {4.f, 10.f, 0.f}) && NearVec(extents, {1.f, 1.f, 1.f}),
                  "GetWorldBounds of the child alone");
            check(!Krom::GetWorldBounds(noBounds, center, extents, sphere), "GetWorldBounds without bounds fails");
        }
        else if (frame == 5)
        {
            check(Krom::IsAlive(mainCam) && Krom::IsAlive(sun) && Krom::IsAlive(visRoot), "entities alive after 5 frames");
            check(Krom::DestroyEntity(visRoot) && !Krom::IsAlive(visChild), "DestroyEntity during Run");
            Krom::Quit();
        }
    });
    check(frame == 5, "five frames rendered");
    Krom::Shutdown();
    return failures == 0 && runResult == 0 ? 0 : 1;
}
