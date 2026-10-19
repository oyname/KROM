// =============================================================================
// KROM Engine - addons/editor/EditorUIViewport.cpp
// Viewport-Werkzeuge: Gizmos (Verschieben/Drehen/Skalieren, Dpad),
// Snap-Raster, Licht-Gizmos, OBB-Handles und Collider-Drahtgitter.
// =============================================================================
#include "addons/editor/EditorUIInternal.hpp"
#include "core/PathUtf8.hpp"

#ifdef KROM_EDITOR_HAS_IMGUI

namespace engine::renderer::addons::editor {

using namespace detail;

namespace {

// =============================================================================
// OBB – Oriented Bounding Box
// =============================================================================

static void BuildBasis(const math::Vec3& n, math::Vec3& outRight, math::Vec3& outUp);

static math::Mat4 BuildEditorViewMatrix(const EditorCameraState& cam)
{
    const math::Quat rot     = math::Quat::FromEulerDeg(cam.pitchDeg, cam.yawDeg, 0.f);
    const math::Vec3 forward = rot.Rotate({0.f, 0.f, -1.f});
    const math::Vec3 up      = rot.Rotate({0.f, 1.f,  0.f});
    return math::Mat4::LookAtRH(cam.position, cam.position + forward, up);
}

static float EditorProjectionAspect(const EditorFrameContext& ctx) noexcept
{
    if (ctx.renderViewWidth > 1.f && ctx.renderViewHeight > 1.f)
        return ctx.renderViewWidth / ctx.renderViewHeight;

    const ImGuiIO& io = ImGui::GetIO();
    return io.DisplaySize.y > 0.f ? io.DisplaySize.x / io.DisplaySize.y : 1.f;
}

static math::Mat4 BuildEditorViewProjection(const EditorFrameContext& ctx)
{
    const EditorCameraState& cam = ctx.state.editorCamera;
    return math::Mat4::PerspectiveFovRH(
        cam.fovDeg * math::DEG_TO_RAD,
        EditorProjectionAspect(ctx),
        cam.nearPlane,
        cam.farPlane) * BuildEditorViewMatrix(cam);
}

// Projiziert einen Weltpunkt auf Bildschirm-Pixel. Gibt false zurück wenn hinter der Kamera.
static bool WorldToScreenPx(const math::Vec3& worldPos,
                             const math::Mat4& vp,
                             const ImVec2& displaySize,
                             ImVec2& outScreen)
{
    const math::Vec4 clip = vp * math::Vec4{worldPos, 1.f};
    if (clip.w < 0.01f)
        return false;
    outScreen = {
        (clip.x / clip.w * 0.5f + 0.5f)       * displaySize.x,
        (1.f - (clip.y / clip.w * 0.5f + 0.5f)) * displaySize.y
    };
    return true;
}

// 8 World-Space-Ecken der OBB (Reihenfolge passend zu DebugDrawRenderer::Box).
static std::array<math::Vec3, 8> ComputeOBBCorners(const math::Mat4& entityWorld,
                                                    const OBBComponent& obb)
{
    const math::Mat4 obbMat = entityWorld *
        math::Mat4::TRS(obb.centerOffset, obb.orientation, {1.f, 1.f, 1.f});
    const float hx = obb.halfExtents.x, hy = obb.halfExtents.y, hz = obb.halfExtents.z;
    const math::Vec3 lc[8] = {
        {-hx,-hy,-hz}, { hx,-hy,-hz}, { hx, hy,-hz}, {-hx, hy,-hz},
        {-hx,-hy, hz}, { hx,-hy, hz}, { hx, hy, hz}, {-hx, hy, hz}
    };
    std::array<math::Vec3, 8> r;
    for (int i = 0; i < 8; ++i)
        r[i] = obbMat.TransformPoint(lc[i]);
    return r;
}

struct OBBFace
{
    math::Vec3 worldCenter;
    math::Vec3 worldNormal;
    int        axis;  // 0=X, 1=Y, 2=Z
    float      sign;  // +1 oder -1
};

static std::array<OBBFace, 6> ComputeOBBFaces(const math::Mat4& entityWorld,
                                               const OBBComponent& obb)
{
    const math::Mat4 obbMat = entityWorld *
        math::Mat4::TRS(obb.centerOffset, obb.orientation, {1.f, 1.f, 1.f});
    const math::Vec3 worldCenter = obbMat.TransformPoint({0.f, 0.f, 0.f});
    const float h[3] = { obb.halfExtents.x, obb.halfExtents.y, obb.halfExtents.z };

    std::array<OBBFace, 6> faces;
    for (int ax = 0; ax < 3; ++ax)
    {
        math::Vec3 localAxis{};
        (&localAxis.x)[ax] = 1.f;
        for (int s = 0; s < 2; ++s)
        {
            const float sg = (s == 0) ? 1.f : -1.f;
            // Face-Center: Offset in World-Space unter Berücksichtigung der Entity-Scale.
            // TransformDirection(axis * halfExtent) skaliert korrekt mit; normalize erst
            // danach für die Normale verwenden.
            const math::Vec3 faceOffset = obbMat.TransformDirection(localAxis * (sg * h[ax]));
            const math::Vec3 worldNormal = obbMat.TransformDirection(localAxis * sg).Normalized();
            faces[ax * 2 + s] = { worldCenter + faceOffset, worldNormal, ax, sg };
        }
    }
    return faces;
}

// Ray vs World-Space AABB (BoundsComponent). Gibt Eintrittsdistanz t zurück.
// Nur t > 0 ist ein Vorwärts-Treffer; -1 = kein Treffer.
static float RayVsWorldAABB(const math::Vec3& rayOrigin,
                             const math::Vec3& rayDir,
                             const BoundsComponent& bounds)
{
    const float cx = bounds.centerWorld.x, cy = bounds.centerWorld.y, cz = bounds.centerWorld.z;
    const float ex = bounds.extentsWorld.x, ey = bounds.extentsWorld.y, ez = bounds.extentsWorld.z;
    const float o[3] = { rayOrigin.x, rayOrigin.y, rayOrigin.z };
    const float d[3] = { rayDir.x,    rayDir.y,    rayDir.z    };
    const float c[3] = { cx, cy, cz };
    const float e[3] = { ex, ey, ez };

    float tMin = -1e30f, tMax = 1e30f;
    for (int i = 0; i < 3; ++i)
    {
        if (std::abs(d[i]) < 1e-8f)
        {
            if (o[i] < c[i] - e[i] || o[i] > c[i] + e[i]) return -1.f;
        }
        else
        {
            float t1 = (c[i] - e[i] - o[i]) / d[i];
            float t2 = (c[i] + e[i] - o[i]) / d[i];
            if (t1 > t2) std::swap(t1, t2);
            tMin = std::max(tMin, t1);
            tMax = std::min(tMax, t2);
            if (tMin > tMax) return -1.f;
        }
    }
    return (tMin > 0.f) ? tMin : -1.f;
}

// ── Physik-Collider: Drahtgitter ─────────────────────────────────────────────
// Zeigt alle Formen des Koerpers, zu dem die Selektion gehoert (Rigidbody-
// Entity bzw. Selektion selbst + Kinder). Die eigenen Formen der Selektion
// folgen showInEditor, die uebrigen werden immer (gedimmt) gezeichnet.

static void DrawWireSphere(engine::addons::debug_draw::DebugDrawRenderer& dd, const math::Vec3& center,
                           const math::Quat& rotation, float radius, const math::Vec4& color)
{
    dd.Circle(center, rotation.Rotate({1.f, 0.f, 0.f}), radius, 32, color);
    dd.Circle(center, rotation.Rotate({0.f, 1.f, 0.f}), radius, 32, color);
    dd.Circle(center, rotation.Rotate({0.f, 0.f, 1.f}), radius, 32, color);
}

static void DrawWireCapsule(engine::addons::debug_draw::DebugDrawRenderer& dd, const math::Vec3& center,
                            const math::Vec3& axis, const math::Vec3& u, const math::Vec3& v,
                            float radius, float halfCylinder, const math::Vec4& color)
{
    const math::Vec3 top    = center + axis * halfCylinder;
    const math::Vec3 bottom = center - axis * halfCylinder;
    dd.Circle(top, axis, radius, 32, color);
    dd.Circle(bottom, axis, radius, 32, color);

    constexpr int kArcSegments = 12;
    for (const math::Vec3& side : { u, v })
    {
        dd.Line(top + side * radius,    bottom + side * radius,    color);
        dd.Line(top - side * radius,    bottom - side * radius,    color);
        // Halbkreise ueber die Kappen (von -side ueber +/-axis nach +side)
        for (int i = 0; i < kArcSegments; ++i)
        {
            const float t0 = math::PI * static_cast<float>(i)     / kArcSegments;
            const float t1 = math::PI * static_cast<float>(i + 1) / kArcSegments;
            const math::Vec3 d0 = side * -std::cos(t0), d1 = side * -std::cos(t1);
            dd.Line(top + (d0 + axis * std::sin(t0)) * radius,
                    top + (d1 + axis * std::sin(t1)) * radius, color);
            dd.Line(bottom + (d0 - axis * std::sin(t0)) * radius,
                    bottom + (d1 - axis * std::sin(t1)) * radius, color);
        }
    }
}

static void DrawEntityColliders(EditorFrameContext& ctx, EntityID entity, bool isSelected,
                                const math::Vec4& color)
{
    const auto* wtc = ctx.world.Get<WorldTransformComponent>(entity);
    if (!wtc)
        return;
    engine::addons::debug_draw::DebugDrawRenderer& dd = *ctx.debugDraw;
    const math::Vec3 s{ std::fabs(wtc->scale.x), std::fabs(wtc->scale.y), std::fabs(wtc->scale.z) };

    // Jede Form folgt ihrem eigenen "Sichtbar"-Haken. Die Box der Selektion
    // zeichnet DrawOBBDebugLines (inkl. Handles).
    if (const auto* obb = ctx.world.Get<OBBComponent>(entity);
        obb && !obb->isTrigger && !isSelected && obb->showInEditor)
        dd.Box(ComputeOBBCorners(wtc->matrix, *obb), color);

    if (const auto* sphere = ctx.world.Get<SphereColliderComponent>(entity);
        sphere && sphere->showInEditor)
    {
        DrawWireSphere(dd, wtc->matrix.TransformPoint(sphere->centerOffset), wtc->rotation,
                       sphere->radius * std::max({ s.x, s.y, s.z }), color);
    }

    if (const auto* capsule = ctx.world.Get<CapsuleColliderComponent>(entity);
        capsule && capsule->showInEditor)
    {
        math::Vec3 axisLocal{0.f, 1.f, 0.f}, uLocal{1.f, 0.f, 0.f}, vLocal{0.f, 0.f, 1.f};
        float axisScale = s.y, radialScale = std::max(s.x, s.z);
        if (capsule->axis == CapsuleAxis::X)
        {
            axisLocal = {1.f, 0.f, 0.f}; uLocal = {0.f, 1.f, 0.f};
            axisScale = s.x; radialScale = std::max(s.y, s.z);
        }
        else if (capsule->axis == CapsuleAxis::Z)
        {
            axisLocal = {0.f, 0.f, 1.f}; vLocal = {0.f, 1.f, 0.f};
            axisScale = s.z; radialScale = std::max(s.x, s.y);
        }
        const float radius = capsule->radius * radialScale;
        const float halfCylinder = std::max(0.f, capsule->height * axisScale * 0.5f - radius);
        DrawWireCapsule(dd, wtc->matrix.TransformPoint(capsule->centerOffset),
                        wtc->rotation.Rotate(axisLocal), wtc->rotation.Rotate(uLocal),
                        wtc->rotation.Rotate(vLocal), radius, halfCylinder, color);
    }

    // Spielfigur: immer aufrecht (ignoriert Rotation/Skalierung), Ursprung an den Fuessen.
    if (const auto* cc = ctx.world.Get<CharacterControllerComponent>(entity);
        cc && cc->showInEditor)
    {
        const float radius = std::max(0.05f, cc->radius);
        const float halfCylinder = std::max(0.f, cc->height * 0.5f - radius);
        const math::Vec4 characterColor{ 0.3f, 0.8f, 1.f, color.w };
        DrawWireCapsule(dd, wtc->position + math::Vec3{ 0.f, halfCylinder + radius, 0.f },
                        { 0.f, 1.f, 0.f }, { 1.f, 0.f, 0.f }, { 0.f, 0.f, 1.f },
                        radius, halfCylinder, characterColor);
    }
}

static void DrawColliderSubtree(EditorFrameContext& ctx, EntityID entity, EntityID selected,
                                uint32_t depth = 0u)
{
    if (depth > 1024u || !ctx.world.IsAlive(entity))
        return;
    const bool isSelected = entity == selected;
    const math::Vec4 color = isSelected ? math::Vec4{0.f, 1.f, 0.2f, 1.f}
                                        : math::Vec4{0.2f, 0.8f, 0.4f, 0.7f};
    DrawEntityColliders(ctx, entity, isSelected, color);
    if (const auto* children = ctx.world.Get<ChildrenComponent>(entity))
    {
        for (const EntityID child : children->children)
        {
            // Kind mit eigenem Rigidbody waere ein eigener Koerper (wird ignoriert).
            if (!ctx.world.Has<RigidBodyComponent>(child))
                DrawColliderSubtree(ctx, child, selected, depth + 1u);
        }
    }
}

static void BuildBasis(const math::Vec3& n, math::Vec3& outRight, math::Vec3& outUp)
{
    const math::Vec3 ref = (std::abs(n.x) <= std::abs(n.y) && std::abs(n.x) <= std::abs(n.z))
        ? math::Vec3{1.f, 0.f, 0.f}
        : (std::abs(n.y) <= std::abs(n.z) ? math::Vec3{0.f, 1.f, 0.f} : math::Vec3{0.f, 0.f, 1.f});
    outRight = math::Vec3::Cross(n, ref).Normalized();
    outUp    = math::Vec3::Cross(n, outRight);
}


// =============================================================================
// Transform-Gizmo – 3D-Pfeile zum Verschieben der selektierten Entity
// =============================================================================

// 2D-Abstand von Punkt P zum Liniensegment AB (Pixel)
static float DistPointToSegment2D(ImVec2 p, ImVec2 a, ImVec2 b)
{
    const float dx = b.x - a.x, dy = b.y - a.y;
    const float lenSq = dx * dx + dy * dy;
    if (lenSq < 1e-6f)
        return std::sqrt((p.x-a.x)*(p.x-a.x) + (p.y-a.y)*(p.y-a.y));
    const float t  = std::clamp(((p.x-a.x)*dx + (p.y-a.y)*dy) / lenSq, 0.f, 1.f);
    const float cx = a.x + t*dx, cy = a.y + t*dy;
    return std::sqrt((p.x-cx)*(p.x-cx) + (p.y-cy)*(p.y-cy));
}

// ── Phase 1: DebugDraw ────────────────────────────────────────────────────────
struct DpadGizmoRuntime
{
    MeshHandle arrowMesh;
    MeshHandle sphereMesh;
    EntityID arrows[3] = {NULL_ENTITY, NULL_ENTITY, NULL_ENTITY};
    EntityID center = NULL_ENTITY;
    bool attemptedLoad = false;
};

static DpadGizmoRuntime& GetDpadGizmoRuntime()
{
    static DpadGizmoRuntime runtime;
    return runtime;
}

static std::filesystem::path EditorDpadAssetPath(const EditorFrameContext& ctx, const char* filename)
{
    if (!ctx.engineEditorDir.empty())
        return PathFromUtf8(ctx.engineEditorDir) / PathFromUtf8(filename);
    return std::filesystem::path("editor") / PathFromUtf8(filename);
}

static MeshHandle LoadEditorDpadMesh(EditorFrameContext& ctx, const std::filesystem::path& path)
{
    if (!ctx.assetPipeline)
        return MeshHandle::Invalid();

    const std::string meshKey = PathToGenericUtf8(path) + "#mesh/0";
    assets::ImportedAssetBundle bundle = ctx.assetPipeline->ImportBundle(PathToUtf8(path));
    if (!bundle.Ok() || bundle.meshes.empty())
    {
        Debug::LogWarning("Transform-DPad: Mesh konnte nicht geladen werden: %s", PathToUtf8(path).c_str());
        return MeshHandle::Invalid();
    }

    for (assets::SubMeshData& sub : bundle.meshes[0].submeshes)
        assets::EnsureTangents(sub);

    auto meshAsset = std::make_unique<assets::MeshAsset>(std::move(bundle.meshes[0]));
    meshAsset->path = meshKey;
    meshAsset->state = assets::AssetState::Loaded;
    meshAsset->gpuStatus.dirty = true;
    meshAsset->gpuStatus.uploaded = false;
    meshAsset->materialHandles.clear();
    return ctx.registry.GetOrAddMesh(meshKey, std::move(meshAsset));
}

static EntityID CreateDpadGizmoEntity(EditorFrameContext& ctx,
                                      const char* name,
                                      MeshHandle mesh,
                                      int axis)
{
    const EntityID entity = ctx.world.CreateEntity();
    ctx.world.Add<ActiveComponent>(entity, ActiveComponent{false});
    ctx.world.Add<NameComponent>(entity, name);
    ctx.world.Add<TransformComponent>(entity);
    ctx.world.Add<WorldTransformComponent>(entity);
    ctx.world.Add<EditorRuntimeGizmoTag>(entity);
    ctx.world.Add<EditorDpadGizmoComponent>(entity, EditorDpadGizmoComponent{axis});

    MeshComponent meshComp{};
    meshComp.mesh = mesh;
    meshComp.castShadows = false;
    meshComp.receiveShadows = false;
    meshComp.layerMask = renderer::LAYER_EDITOR_GIZMO;
    if (const assets::MeshAsset* meshAsset = ctx.registry.meshes.Get(mesh))
        meshComp.meshAssetPath = meshAsset->path;
    ctx.world.Add<MeshComponent>(entity, meshComp);
    ctx.world.Add<MaterialComponent>(entity);
    return entity;
}

static void SetDpadGizmoActive(EditorFrameContext& ctx, bool active)
{
    DpadGizmoRuntime& runtime = GetDpadGizmoRuntime();
    auto setOne = [&](EntityID entity)
    {
        if (!entity.IsValid() || !ctx.world.IsAlive(entity))
            return;
        if (auto* activeComp = ctx.world.Get<ActiveComponent>(entity))
            activeComp->active = active;
    };
    for (EntityID arrow : runtime.arrows)
        setOne(arrow);
    setOne(runtime.center);
}

static bool EnsureDpadGizmo(EditorFrameContext& ctx)
{
    DpadGizmoRuntime& runtime = GetDpadGizmoRuntime();
    const bool alive =
        runtime.arrows[0].IsValid() && ctx.world.IsAlive(runtime.arrows[0]) &&
        runtime.arrows[1].IsValid() && ctx.world.IsAlive(runtime.arrows[1]) &&
        runtime.arrows[2].IsValid() && ctx.world.IsAlive(runtime.arrows[2]) &&
        runtime.center.IsValid() && ctx.world.IsAlive(runtime.center);
    if (alive)
        return true;

    if (!ctx.assetPipeline)
        return false;
    if (runtime.attemptedLoad && (!runtime.arrowMesh.IsValid() || !runtime.sphereMesh.IsValid()))
        return false;

    runtime.attemptedLoad = true;
    if (!runtime.arrowMesh.IsValid())
        runtime.arrowMesh = LoadEditorDpadMesh(ctx, EditorDpadAssetPath(ctx, "arrow.glb"));
    if (!runtime.sphereMesh.IsValid())
        runtime.sphereMesh = LoadEditorDpadMesh(ctx, EditorDpadAssetPath(ctx, "dpad_sphere.glb"));
    if (!runtime.arrowMesh.IsValid() || !runtime.sphereMesh.IsValid())
        return false;

    runtime.arrows[0] = CreateDpadGizmoEntity(ctx, "__editor_dpad_x", runtime.arrowMesh, 0);
    runtime.arrows[1] = CreateDpadGizmoEntity(ctx, "__editor_dpad_y", runtime.arrowMesh, 1);
    runtime.arrows[2] = CreateDpadGizmoEntity(ctx, "__editor_dpad_z", runtime.arrowMesh, 2);
    runtime.center = CreateDpadGizmoEntity(ctx, "__editor_dpad_center", runtime.sphereMesh, -1);
    ctx.assetPipeline->UploadPendingGpuAssets();
    return true;
}

static bool UpdateDpadGizmo(EditorFrameContext& ctx,
                            const math::Vec3& center,
                            const math::Quat& basisRotation,
                            float size)
{
    if (!EnsureDpadGizmo(ctx))
        return false;

    SetDpadGizmoActive(ctx, true);
    DpadGizmoRuntime& runtime = GetDpadGizmoRuntime();

    const std::string normalMat[3] = {
        PathToGenericUtf8(EditorDpadAssetPath(ctx, "dpad_rot.mat")),
        PathToGenericUtf8(EditorDpadAssetPath(ctx, "dpad_gruen.mat")),
        PathToGenericUtf8(EditorDpadAssetPath(ctx, "dpad_blau.mat"))
    };
    const std::string yellowMat = PathToGenericUtf8(EditorDpadAssetPath(ctx, "dpad_gelb.mat"));
    const std::string centerMat = PathToGenericUtf8(EditorDpadAssetPath(ctx, "dpad_weiss.mat"));

    const math::Vec3 axes[3] = {
        basisRotation.Rotate({1.f, 0.f, 0.f}),
        basisRotation.Rotate({0.f, 1.f, 0.f}),
        basisRotation.Rotate({0.f, 0.f, 1.f})
    };
    const math::Quat rotations[3] = {
        basisRotation * math::Quat::FromAxisAngleDeg({0.f, 1.f, 0.f}, 90.f),
        basisRotation * math::Quat::FromAxisAngleDeg({1.f, 0.f, 0.f}, -90.f),
        basisRotation
    };
    const float scale = std::max(0.001f, size / 8.0f);
    const float offsets[3] = {2.50218f, 2.11085f, 2.21877f};
    bool materialChanged = false;

    auto applyTransform = [&](EntityID entity,
                              const math::Vec3& position,
                              const math::Quat& rotation,
                              const math::Vec3& localScale)
    {
        if (auto* transform = ctx.world.Get<TransformComponent>(entity))
        {
            transform->localPosition = position;
            transform->localRotation = rotation;
            transform->localScale = localScale;
            transform->dirty = false;
            ++transform->localVersion;
            ++transform->worldVersion;
            transform->parentWorldVersion = 0u;
        }
        if (auto* worldTransform = ctx.world.Get<WorldTransformComponent>(entity))
        {
            worldTransform->position = position;
            worldTransform->rotation = rotation;
            worldTransform->scale = localScale;
            worldTransform->matrix = math::Mat4::TRS(position, rotation, localScale);
            worldTransform->inverse = worldTransform->matrix.InverseAffine();
        }
    };

    for (int i = 0; i < 3; ++i)
    {
        const EntityID entity = runtime.arrows[i];
        if (!entity.IsValid() || !ctx.world.IsAlive(entity))
            continue;
        applyTransform(entity,
                       center + axes[i] * (offsets[i] * scale),
                       rotations[i],
                       {scale, scale, scale});
        const bool highlighted = ctx.state.gizmoHoveredAxis == i || ctx.state.gizmoDragAxis == i;
        const std::string& materialPath = highlighted ? yellowMat : normalMat[i];
        if (auto* material = ctx.world.Get<MaterialComponent>(entity))
        {
            if (material->materialAssetPath != materialPath)
            {
                material->slotOverrides.clear();
                material->baseColorTexturePath.clear();
                material->materialAssetPath = materialPath;
                material->material = MaterialHandle::Invalid();
                materialChanged = true;
            }
        }
    }

    if (runtime.center.IsValid() && ctx.world.IsAlive(runtime.center))
    {
        applyTransform(runtime.center, center, basisRotation, {scale, scale, scale});
        const bool centerHov = (ctx.state.gizmoHoveredAxis == 3 ||
                                ctx.state.gizmoDragAxis    == 3);
        const std::string& cMat = centerHov ? yellowMat : centerMat;
        if (auto* material = ctx.world.Get<MaterialComponent>(runtime.center))
        {
            if (material->materialAssetPath != cMat)
            {
                material->slotOverrides.clear();
                material->baseColorTexturePath.clear();
                material->materialAssetPath = cMat;
                material->material = MaterialHandle::Invalid();
                materialChanged = true;
            }
        }
    }

    if (materialChanged)
        ResolveMaterialAssetBindings(ctx);
    return true;
}

struct RotateGizmoRuntime
{
    MeshHandle mesh;
    MeshHandle sphereMesh;
    EntityID rings[3]    = {NULL_ENTITY, NULL_ENTITY, NULL_ENTITY};
    EntityID center      = NULL_ENTITY;
    bool attemptedLoad   = false;
};

static RotateGizmoRuntime& GetRotateGizmoRuntime()
{
    static RotateGizmoRuntime runtime;
    return runtime;
}

static EntityID CreateRotateGizmoEntity(EditorFrameContext& ctx,
                                        const char* name,
                                        MeshHandle mesh,
                                        int axis)
{
    const EntityID entity = ctx.world.CreateEntity();
    ctx.world.Add<ActiveComponent>(entity, ActiveComponent{false});
    ctx.world.Add<NameComponent>(entity, name);
    ctx.world.Add<TransformComponent>(entity);
    ctx.world.Add<WorldTransformComponent>(entity);
    ctx.world.Add<EditorRuntimeGizmoTag>(entity);
    ctx.world.Add<EditorRotateGizmoComponent>(entity, EditorRotateGizmoComponent{axis});

    MeshComponent meshComp{};
    meshComp.mesh = mesh;
    meshComp.castShadows = false;
    meshComp.receiveShadows = false;
    meshComp.layerMask = renderer::LAYER_EDITOR_GIZMO;
    if (const assets::MeshAsset* meshAsset = ctx.registry.meshes.Get(mesh))
        meshComp.meshAssetPath = meshAsset->path;
    ctx.world.Add<MeshComponent>(entity, meshComp);
    ctx.world.Add<MaterialComponent>(entity);
    return entity;
}

static void SetRotateGizmoActive(EditorFrameContext& ctx, bool active)
{
    RotateGizmoRuntime& runtime = GetRotateGizmoRuntime();
    auto setOne = [&](EntityID entity)
    {
        if (!entity.IsValid() || !ctx.world.IsAlive(entity)) return;
        if (auto* ac = ctx.world.Get<ActiveComponent>(entity))
            ac->active = active;
    };
    for (EntityID ring : runtime.rings)
        setOne(ring);
    setOne(runtime.center);
}

static bool EnsureRotateGizmo(EditorFrameContext& ctx)
{
    RotateGizmoRuntime& runtime = GetRotateGizmoRuntime();
    const bool alive =
        runtime.rings[0].IsValid() && ctx.world.IsAlive(runtime.rings[0]) &&
        runtime.rings[1].IsValid() && ctx.world.IsAlive(runtime.rings[1]) &&
        runtime.rings[2].IsValid() && ctx.world.IsAlive(runtime.rings[2]) &&
        runtime.center.IsValid()   && ctx.world.IsAlive(runtime.center);
    if (alive)
        return true;

    if (!ctx.assetPipeline)
        return false;
    if (runtime.attemptedLoad && (!runtime.mesh.IsValid() || !runtime.sphereMesh.IsValid()))
        return false;

    runtime.attemptedLoad = true;
    if (!runtime.mesh.IsValid())
        runtime.mesh = LoadEditorDpadMesh(ctx, EditorDpadAssetPath(ctx, "rotate.glb"));
    if (!runtime.sphereMesh.IsValid())
        runtime.sphereMesh = LoadEditorDpadMesh(ctx, EditorDpadAssetPath(ctx, "dpad_sphere.glb"));
    if (!runtime.mesh.IsValid() || !runtime.sphereMesh.IsValid())
        return false;

    runtime.rings[0] = CreateRotateGizmoEntity(ctx, "__editor_rotate_x",      runtime.mesh,       0);
    runtime.rings[1] = CreateRotateGizmoEntity(ctx, "__editor_rotate_y",      runtime.mesh,       1);
    runtime.rings[2] = CreateRotateGizmoEntity(ctx, "__editor_rotate_z",      runtime.mesh,       2);
    runtime.center   = CreateRotateGizmoEntity(ctx, "__editor_rotate_center", runtime.sphereMesh, -1);
    ctx.assetPipeline->UploadPendingGpuAssets();
    return true;
}

static void BuildRotateGizmoVisualRotations(const math::Quat& basisRotation,
                                            math::Quat (&rotations)[3])
{
    // rotate.glb braucht eine feste Y-Korrektur. In World bleibt basisRotation
    // Identity; in Local kommt die Objektrotation aus DrawRotationGizmo dazu.
    const math::Quat visualBasis =
        basisRotation * math::Quat::FromAxisAngleDeg({0.f, 1.f, 0.f}, 90.f);
    rotations[0] = visualBasis * math::Quat::FromAxisAngleDeg({1.f, 0.f, 0.f}, -90.f);
    rotations[1] = visualBasis;
    rotations[2] = visualBasis * math::Quat::FromAxisAngleDeg({0.f, 0.f, 1.f}, -90.f);
}

static bool UpdateRotateGizmo(EditorFrameContext& ctx,
                              const math::Vec3& center,
                              const math::Quat& basisRotation,
                              float radius)
{
    if (!EnsureRotateGizmo(ctx))
        return false;

    SetRotateGizmoActive(ctx, true);
    RotateGizmoRuntime& runtime = GetRotateGizmoRuntime();

    const std::string normalMat[3] = {
        PathToGenericUtf8(EditorDpadAssetPath(ctx, "dpad_rot.mat")),
        PathToGenericUtf8(EditorDpadAssetPath(ctx, "dpad_gruen.mat")),
        PathToGenericUtf8(EditorDpadAssetPath(ctx, "dpad_blau.mat"))
    };
    const std::string yellowMat = PathToGenericUtf8(EditorDpadAssetPath(ctx, "dpad_gelb.mat"));

    math::Quat rotations[3];
    BuildRotateGizmoVisualRotations(basisRotation, rotations);
    constexpr float kRotateMeshRadius = 1.3088281f;
    const float scale = std::max(0.001f, radius / kRotateMeshRadius);
    bool materialChanged = false;

    auto applyTransform = [&](EntityID entity,
                              const math::Quat& rotation,
                              const math::Vec3& localScale)
    {
        if (auto* transform = ctx.world.Get<TransformComponent>(entity))
        {
            transform->localPosition = center;
            transform->localRotation = rotation;
            transform->localScale = localScale;
            transform->dirty = false;
            ++transform->localVersion;
            ++transform->worldVersion;
            transform->parentWorldVersion = 0u;
        }
        if (auto* worldTransform = ctx.world.Get<WorldTransformComponent>(entity))
        {
            worldTransform->position = center;
            worldTransform->rotation = rotation;
            worldTransform->scale = localScale;
            worldTransform->matrix = math::Mat4::TRS(center, rotation, localScale);
            worldTransform->inverse = worldTransform->matrix.InverseAffine();
        }
    };

    for (int i = 0; i < 3; ++i)
    {
        const EntityID entity = runtime.rings[i];
        if (!entity.IsValid() || !ctx.world.IsAlive(entity))
            continue;

        applyTransform(entity, rotations[i], {scale, scale, scale});
        const bool highlighted = ctx.state.rotGizmoHoveredAxis == i ||
                                 ctx.state.rotGizmoDragAxis == i;
        const std::string& materialPath = highlighted ? yellowMat : normalMat[i];
        if (auto* material = ctx.world.Get<MaterialComponent>(entity))
        {
            if (material->materialAssetPath != materialPath)
            {
                material->slotOverrides.clear();
                material->baseColorTexturePath.clear();
                material->materialAssetPath = materialPath;
                material->material = MaterialHandle::Invalid();
                materialChanged = true;
            }
        }
    }

    // Center-Sphere (Trackball, axis 3)
    if (runtime.center.IsValid() && ctx.world.IsAlive(runtime.center))
    {
        const float sphereScale = scale * 0.16f;  // gleiche Sichtgröße wie Positions-Sphere
        if (auto* transform = ctx.world.Get<TransformComponent>(runtime.center))
        {
            transform->localPosition = center;
            transform->localRotation = math::Quat::Identity();
            transform->localScale    = {sphereScale, sphereScale, sphereScale};
            transform->dirty = false;
            ++transform->localVersion; ++transform->worldVersion;
            transform->parentWorldVersion = 0u;
        }
        if (auto* wt = ctx.world.Get<WorldTransformComponent>(runtime.center))
        {
            const math::Vec3 ls{sphereScale, sphereScale, sphereScale};
            wt->position = center;
            wt->rotation = math::Quat::Identity();
            wt->scale    = ls;
            wt->matrix   = math::Mat4::TRS(center, math::Quat::Identity(), ls);
            wt->inverse  = wt->matrix.InverseAffine();
        }
        const bool hov = (ctx.state.rotGizmoHoveredAxis == 3 ||
                          ctx.state.rotGizmoDragAxis    == 3);
        const std::string matPath = hov
            ? yellowMat
            : PathToGenericUtf8(EditorDpadAssetPath(ctx, "dpad_weiss.mat"));
        if (auto* material = ctx.world.Get<MaterialComponent>(runtime.center))
        {
            if (material->materialAssetPath != matPath)
            {
                material->slotOverrides.clear();
                material->baseColorTexturePath.clear();
                material->materialAssetPath = matPath;
                material->material          = MaterialHandle::Invalid();
                materialChanged             = true;
            }
        }
    }

    if (materialChanged)
        ResolveMaterialAssetBindings(ctx);
    return true;
}

// =============================================================================
// Scale-Gizmo — drei farbige Pfeile (scale.glb) entlang X / Y / Z
// =============================================================================

struct ScaleGizmoRuntime
{
    MeshHandle scaleMesh;
    MeshHandle sphereMesh;
    EntityID   arrows[3]     = {NULL_ENTITY, NULL_ENTITY, NULL_ENTITY};
    EntityID   center        = NULL_ENTITY;
    bool       attemptedLoad = false;
};

static ScaleGizmoRuntime& GetScaleGizmoRuntime()
{
    static ScaleGizmoRuntime runtime;
    return runtime;
}

static EntityID CreateScaleGizmoEntity(EditorFrameContext& ctx,
                                       const char* name,
                                       MeshHandle mesh,
                                       int axis)
{
    const EntityID entity = ctx.world.CreateEntity();
    ctx.world.Add<ActiveComponent>(entity, ActiveComponent{false});
    ctx.world.Add<NameComponent>(entity, name);
    ctx.world.Add<TransformComponent>(entity);
    ctx.world.Add<WorldTransformComponent>(entity);
    ctx.world.Add<EditorRuntimeGizmoTag>(entity);
    ctx.world.Add<EditorScaleGizmoComponent>(entity, EditorScaleGizmoComponent{axis});

    MeshComponent meshComp{};
    meshComp.mesh         = mesh;
    meshComp.castShadows  = false;
    meshComp.receiveShadows = false;
    meshComp.layerMask    = renderer::LAYER_EDITOR_GIZMO;
    if (const assets::MeshAsset* meshAsset = ctx.registry.meshes.Get(mesh))
        meshComp.meshAssetPath = meshAsset->path;
    ctx.world.Add<MeshComponent>(entity, meshComp);
    ctx.world.Add<MaterialComponent>(entity);
    return entity;
}

static void SetScaleGizmoActive(EditorFrameContext& ctx, bool active)
{
    ScaleGizmoRuntime& runtime = GetScaleGizmoRuntime();
    auto setOne = [&](EntityID entity)
    {
        if (!entity.IsValid() || !ctx.world.IsAlive(entity)) return;
        if (auto* ac = ctx.world.Get<ActiveComponent>(entity))
            ac->active = active;
    };
    for (EntityID arrow : runtime.arrows)
        setOne(arrow);
    setOne(runtime.center);
}

static bool EnsureScaleGizmo(EditorFrameContext& ctx)
{
    ScaleGizmoRuntime& runtime = GetScaleGizmoRuntime();
    const bool alive =
        runtime.arrows[0].IsValid() && ctx.world.IsAlive(runtime.arrows[0]) &&
        runtime.arrows[1].IsValid() && ctx.world.IsAlive(runtime.arrows[1]) &&
        runtime.arrows[2].IsValid() && ctx.world.IsAlive(runtime.arrows[2]) &&
        runtime.center.IsValid()   && ctx.world.IsAlive(runtime.center);
    if (alive)
        return true;

    if (!ctx.assetPipeline)
        return false;
    if (runtime.attemptedLoad && (!runtime.scaleMesh.IsValid() || !runtime.sphereMesh.IsValid()))
        return false;

    runtime.attemptedLoad = true;
    if (!runtime.scaleMesh.IsValid())
        runtime.scaleMesh  = LoadEditorDpadMesh(ctx, EditorDpadAssetPath(ctx, "scale.glb"));
    if (!runtime.sphereMesh.IsValid())
        runtime.sphereMesh = LoadEditorDpadMesh(ctx, EditorDpadAssetPath(ctx, "dpad_sphere.glb"));
    if (!runtime.scaleMesh.IsValid() || !runtime.sphereMesh.IsValid())
        return false;

    runtime.arrows[0] = CreateScaleGizmoEntity(ctx, "__editor_scale_x",      runtime.scaleMesh,  0);
    runtime.arrows[1] = CreateScaleGizmoEntity(ctx, "__editor_scale_y",      runtime.scaleMesh,  1);
    runtime.arrows[2] = CreateScaleGizmoEntity(ctx, "__editor_scale_z",      runtime.scaleMesh,  2);
    runtime.center    = CreateScaleGizmoEntity(ctx, "__editor_scale_center", runtime.sphereMesh, -1);
    ctx.assetPipeline->UploadPendingGpuAssets();
    return true;
}

static bool UpdateScaleGizmo(EditorFrameContext& ctx,
                             const math::Vec3& center,
                             const math::Quat& basisRotation,
                             float size)
{
    if (!EnsureScaleGizmo(ctx))
        return false;

    SetScaleGizmoActive(ctx, true);
    ScaleGizmoRuntime& runtime = GetScaleGizmoRuntime();

    const std::string normalMat[3] = {
        PathToGenericUtf8(EditorDpadAssetPath(ctx, "dpad_rot.mat")),
        PathToGenericUtf8(EditorDpadAssetPath(ctx, "dpad_gruen.mat")),
        PathToGenericUtf8(EditorDpadAssetPath(ctx, "dpad_blau.mat"))
    };
    const std::string yellowMat = PathToGenericUtf8(EditorDpadAssetPath(ctx, "dpad_gelb.mat"));
    const std::string centerMat = PathToGenericUtf8(EditorDpadAssetPath(ctx, "dpad_weiss.mat"));

    const math::Vec3 axes[3] = {
        basisRotation.Rotate({1.f, 0.f, 0.f}),
        basisRotation.Rotate({0.f, 1.f, 0.f}),
        basisRotation.Rotate({0.f, 0.f, 1.f})
    };
    const math::Quat rotations[3] = {
        basisRotation * math::Quat::FromAxisAngleDeg({0.f, 1.f, 0.f},  90.f),
        basisRotation * math::Quat::FromAxisAngleDeg({1.f, 0.f, 0.f}, -90.f),
        basisRotation
    };
    const float scale = std::max(0.001f, size / 8.0f);
    const float offsets[3] = {2.50218f, 2.11085f, 2.21877f};
    bool materialChanged = false;

    auto applyTransform = [&](EntityID entity,
                              const math::Vec3& position,
                              const math::Quat& rotation,
                              const math::Vec3& localScale)
    {
        if (auto* transform = ctx.world.Get<TransformComponent>(entity))
        {
            transform->localPosition = position;
            transform->localRotation = rotation;
            transform->localScale    = localScale;
            transform->dirty         = false;
            ++transform->localVersion;
            ++transform->worldVersion;
            transform->parentWorldVersion = 0u;
        }
        if (auto* worldTransform = ctx.world.Get<WorldTransformComponent>(entity))
        {
            worldTransform->position = position;
            worldTransform->rotation = rotation;
            worldTransform->scale    = localScale;
            worldTransform->matrix   = math::Mat4::TRS(position, rotation, localScale);
            worldTransform->inverse  = worldTransform->matrix.InverseAffine();
        }
    };

    for (int i = 0; i < 3; ++i)
    {
        const EntityID entity = runtime.arrows[i];
        if (!entity.IsValid() || !ctx.world.IsAlive(entity))
            continue;
        applyTransform(entity,
                       center + axes[i] * (offsets[i] * scale),
                       rotations[i],
                       {scale, scale, scale});
        const bool highlighted = ctx.state.sclGizmoHoveredAxis == i ||
                                 ctx.state.sclGizmoDragAxis    == i;
        const std::string& materialPath = highlighted ? yellowMat : normalMat[i];
        if (auto* material = ctx.world.Get<MaterialComponent>(entity))
        {
            if (material->materialAssetPath != materialPath)
            {
                material->slotOverrides.clear();
                material->baseColorTexturePath.clear();
                material->materialAssetPath = materialPath;
                material->material          = MaterialHandle::Invalid();
                materialChanged             = true;
            }
        }
    }

    // Center-Sphere (axis 3 = uniform)
    if (runtime.center.IsValid() && ctx.world.IsAlive(runtime.center))
    {
        applyTransform(runtime.center, center, basisRotation, {scale, scale, scale});
        const bool highlighted = ctx.state.sclGizmoHoveredAxis == 3 ||
                                 ctx.state.sclGizmoDragAxis    == 3;
        const std::string& matPath = highlighted ? yellowMat : centerMat;
        if (auto* material = ctx.world.Get<MaterialComponent>(runtime.center))
        {
            if (material->materialAssetPath != matPath)
            {
                material->slotOverrides.clear();
                material->baseColorTexturePath.clear();
                material->materialAssetPath = matPath;
                material->material          = MaterialHandle::Invalid();
                materialChanged             = true;
            }
        }
    }

    if (materialChanged)
        ResolveMaterialAssetBindings(ctx);
    return true;
}

// ── Raster-Snapping ──────────────────────────────────────────────────────────
// Strg waehrend des Ziehens kehrt den Schalter um (an → frei, aus → gerastert),
// wie Unitys Increment-Snap; die Schalter selbst sitzen in der Menueleiste (Unreal).
static bool IsSnapActive(bool enabled)
{
    return enabled != ImGui::GetIO().KeyCtrl;
}

static float SnapTo(float value, float step)
{
    return step > 0.f ? std::round(value / step) * step : value;
}

} // namespace

namespace detail {

// Trackt die Bewegungsachse des selektierten Entitys anhand des Weltpositions-Deltas.
// Muss jeden Frame vor SnapEntityToSurface aufgerufen werden.
void UpdateSnapAxis(EditorFrameContext& ctx)
{
    EditorState& st = ctx.state;
    const EntityID selected = st.selectedEntity;

    if (!selected.IsValid())
    {
        st.snapAxis = -1;
        return;
    }

    const auto* wtc = ctx.world.Get<WorldTransformComponent>(selected);
    if (!wtc) { st.snapAxis = -1; return; }

    const math::Vec3 delta = {
        std::abs(wtc->position.x - st.snapLastPos.x),
        std::abs(wtc->position.y - st.snapLastPos.y),
        std::abs(wtc->position.z - st.snapLastPos.z)
    };
    st.snapLastPos = wtc->position;

    constexpr float kThreshold = 0.0005f; // unter diesem Wert: keine Bewegung erkannt
    const float maxDelta = std::max({delta.x, delta.y, delta.z});
    if (maxDelta < kThreshold)
        return; // Achse beibehalten wenn keine Bewegung

    if (delta.x >= delta.y && delta.x >= delta.z)      st.snapAxis = 0;
    else if (delta.y >= delta.x && delta.y >= delta.z) st.snapAxis = 1;
    else                                                st.snapAxis = 2;
}

// Rastet das selektierte Entity an der nächsten Oberfläche entlang der erkannten Achse ein.
// snapAxis == -1: alle 6 Richtungen; 0/1/2: nur ±X/±Y/±Z.
void SnapEntityToSurface(EditorFrameContext& ctx)
{
    const EntityID selected = ctx.state.selectedEntity;
    if (!selected.IsValid()) return;

    const auto* bounds = ctx.world.Get<BoundsComponent>(selected);
    const auto* wtc    = ctx.world.Get<WorldTransformComponent>(selected);
    auto*       tc     = ctx.world.Get<TransformComponent>(selected);
    if (!bounds || !wtc || !tc) return;

    const math::Vec3 cx = bounds->centerWorld;
    const math::Vec3 ex = bounds->extentsWorld;

    struct SnapCandidate { math::Vec3 origin; math::Vec3 dir; };
    const SnapCandidate kAll[6] = {
        { {cx.x - ex.x, cx.y, cx.z}, {-1.f,  0.f,  0.f} },
        { {cx.x + ex.x, cx.y, cx.z}, { 1.f,  0.f,  0.f} },
        { {cx.x, cx.y - ex.y, cx.z}, { 0.f, -1.f,  0.f} },
        { {cx.x, cx.y + ex.y, cx.z}, { 0.f,  1.f,  0.f} },
        { {cx.x, cx.y, cx.z - ex.z}, { 0.f,  0.f, -1.f} },
        { {cx.x, cx.y, cx.z + ex.z}, { 0.f,  0.f,  1.f} },
    };

    // Kandidaten nach erkannter Achse filtern
    const int axis = ctx.state.snapAxis;
    int first = 0, last = 6;
    if (axis == 0) { first = 0; last = 2; } // ±X
    if (axis == 1) { first = 2; last = 4; } // ±Y
    if (axis == 2) { first = 4; last = 6; } // ±Z

    constexpr float kMaxDist = 50.f;
    float      bestT   = kMaxDist;
    math::Vec3 bestDir = {0.f, -1.f, 0.f};
    bool       found   = false;

    for (int ci = first; ci < last; ++ci)
    {
        const auto& cand = kAll[ci];
        ctx.world.ForEachAlive([&](EntityID e)
        {
            if (e == selected) return;
            const auto* ob = ctx.world.Get<BoundsComponent>(e);
            if (!ob) return;
            const float t = RayVsWorldAABB(cand.origin, cand.dir, *ob);
            if (t > 0.f && t < bestT)
            {
                bestT   = t;
                bestDir = cand.dir;
                found   = true;
            }
        });
    }

    if (!found) return;

    const math::Vec3 newWorldPos = wtc->position + bestDir * bestT;

    SetWorldPosition(ctx, selected, *tc, newWorldPos);
}

// ── Phase 1: DebugDraw-Rendering ─────────────────────────────────────────────
// Wird aus DrawLightDebugLines (Pre-Render-Phase, vor ImGui::NewFrame) aufgerufen.
// Liest obbHoveredFace aus dem letzten ImGui-Frame für Hover-Highlighting.
void DrawOBBDebugLines(EditorFrameContext& ctx)
{
    if (!ctx.debugDraw) return;
    const EntityID selected = ctx.state.selectedEntity;
    if (!selected.IsValid()) return;
    const auto* obb = ctx.world.Get<OBBComponent>(selected);
    if (!obb || !obb->showInEditor) return;
    const auto* wtc = ctx.world.Get<WorldTransformComponent>(selected);
    if (!wtc) return;

    using namespace engine::addons::debug_draw;
    DebugDrawRenderer& dd = *ctx.debugDraw;

    // Trigger orange, feste Kollision gruen
    const math::Vec4 boxColor = obb->isTrigger ? math::Vec4{1.f, 0.6f, 0.f, 1.f}
                                               : math::Vec4{0.f, 1.f, 0.2f, 1.f};
    dd.Box(ComputeOBBCorners(wtc->matrix, *obb), boxColor);

    const auto faces = ComputeOBBFaces(wtc->matrix, *obb);

    // Handle-Kreise: screen-space konstant (~2% Kamera-Distanz), in der Flächen-Ebene.
    constexpr float kScreenFraction = 0.02f;

    for (int i = 0; i < 6; ++i)
    {
        const auto& f = faces[i];

        const float camDist = (f.worldCenter - ctx.state.editorCamera.position).Length();
        const float radius  = std::max(0.03f, camDist * kScreenFraction);

        const bool       hovered = (ctx.state.obbHoveredFace == i);
        const float      drawR   = hovered ? radius * 1.6f : radius;
        const math::Vec4 col     = hovered
            ? math::Vec4{1.f, 0.4f, 0.f, 1.f}
            : math::Vec4{1.f, 1.f, 0.f, 1.f};

        dd.Circle(f.worldCenter, f.worldNormal, drawR, 16, col);
    }
}

void DrawPhysicsColliderLines(EditorFrameContext& ctx)
{
    if (!ctx.debugDraw) return;
    const EntityID selected = ctx.state.selectedEntity;
    if (!selected.IsValid() || !ctx.world.IsAlive(selected)) return;

    // Hat die Selektion selbst eine ausgeblendete Kollisionsform, schaltet das die
    // Anzeige des ganzen Koerpers (inkl. Kollisions-Kinder) ab.
    const auto hidden = [&](bool hasShape, bool visible) { return hasShape && !visible; };
    const auto* obb     = ctx.world.Get<OBBComponent>(selected);
    const auto* sphere  = ctx.world.Get<SphereColliderComponent>(selected);
    const auto* capsule = ctx.world.Get<CapsuleColliderComponent>(selected);
    const auto* cc      = ctx.world.Get<CharacterControllerComponent>(selected);
    if (hidden(obb != nullptr, obb && obb->showInEditor) ||
        hidden(sphere != nullptr, sphere && sphere->showInEditor) ||
        hidden(capsule != nullptr, capsule && capsule->showInEditor) ||
        hidden(cc != nullptr, cc && cc->showInEditor))
        return;

    EntityID root = FindRigidBodyAncestor(ctx.world, selected);
    if (!root.IsValid())
        root = selected;
    DrawColliderSubtree(ctx, root, selected);
}

// ── Phase 2: ImGui-Input-Handling ────────────────────────────────────────────
// Wird aus DrawEditorPanels aufgerufen (nach ImGui::NewFrame, vor SelectEntityUnderMouse).
// Schreibt obbHoveredFace / obbDragFaceIdx in EditorState.
void UpdateOBBHandles(EditorFrameContext& ctx)
{
    EditorState& st = ctx.state;

    // Drag beenden (immer zuerst prüfen)
    if (st.obbDragFaceIdx >= 0 && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
    {
        st.obbDragFaceIdx = -1;
        st.obbHoveredFace = -1;
    }

    // Aktiver Gizmo-Drag (Position oder Rotation) oder Snap-Lock → OBB-Handles sperren
    if (st.gizmoDragAxis >= 0 || st.rotGizmoDragAxis >= 0 || st.sclGizmoDragAxis >= 0 || st.snapMoveLocked)
    {
        st.obbHoveredFace = -1;
        return;
    }

    const EntityID selected = st.selectedEntity;
    if (!selected.IsValid()) { st.obbHoveredFace = -1; return; }
    auto* obb = ctx.world.Get<OBBComponent>(selected);
    if (!obb || !obb->showInEditor) { st.obbHoveredFace = -1; st.obbDragFaceIdx = -1; return; }
    const auto* wtc = ctx.world.Get<WorldTransformComponent>(selected);
    if (!wtc) { st.obbHoveredFace = -1; return; }

    ImGuiIO& io = ImGui::GetIO();
    if (io.WantCaptureMouse) { st.obbHoveredFace = -1; return; }

    const auto faces = ComputeOBBFaces(wtc->matrix, *obb);

    const math::Mat4 vp = BuildEditorViewProjection(ctx);

    // ── Hover-Erkennung (nur wenn kein Drag läuft) ──
    st.obbHoveredFace = -1;
    if (st.obbDragFaceIdx < 0)
    {
        constexpr float kPickRadiusSq = 10.f * 10.f;
        for (int i = 0; i < 6; ++i)
        {
            ImVec2 sp;
            if (!WorldToScreenPx(faces[i].worldCenter, vp, io.DisplaySize, sp))
                continue;
            const float dx = io.MousePos.x - sp.x;
            const float dy = io.MousePos.y - sp.y;
            if (dx * dx + dy * dy < kPickRadiusSq)
            {
                st.obbHoveredFace = i;
                break;
            }
        }
    }

    // ── Drag starten ──
    if (st.obbHoveredFace >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
    {
        const OBBFace& f  = faces[st.obbHoveredFace];
        st.obbDragFaceIdx     = st.obbHoveredFace;
        st.obbDragDepth       = (f.worldCenter - st.editorCamera.position).Length();
        st.obbDragStartMouse  = { io.MousePos.x, io.MousePos.y };
        st.obbDragStartExtent = (&obb->halfExtents.x)[f.axis];
        st.obbDragStartCenter = (&obb->centerOffset.x)[f.axis];
    }

    // ── Drag fortführen ──
    if (st.obbDragFaceIdx >= 0 && ImGui::IsMouseDown(ImGuiMouseButton_Left))
    {
        const OBBFace& f = faces[st.obbDragFaceIdx];

        const float tanHalfFov    = std::tan(30.f * math::DEG_TO_RAD);
        const float unitsPerPixel = 2.f * st.obbDragDepth * tanHalfFov /
                                    std::max(1.f, io.DisplaySize.y);

        // Face-Normale in Kamera-Raum → Bildschirm-Richtung
        const math::Mat4 view = BuildEditorViewMatrix(st.editorCamera);
        const math::Vec3 camNormal = view.TransformDirection(f.worldNormal);
        const float snx =  camNormal.x;
        const float sny = -camNormal.y; // Kamera-Y nach oben, Screen-Y nach unten
        const float len = std::sqrt(snx * snx + sny * sny);

        if (len > 1e-4f)
        {
            const float nx = snx / len;
            const float ny = sny / len;
            const float dx = io.MousePos.x - st.obbDragStartMouse.x;
            const float dy = io.MousePos.y - st.obbDragStartMouse.y;
            const float worldDelta = (nx * dx + ny * dy) * unitsPerPixel;

            float& he = (&obb->halfExtents.x)[f.axis];
            float& co = (&obb->centerOffset.x)[f.axis];

            const bool ctrl = ImGui::IsKeyDown(ImGuiKey_LeftCtrl) ||
                              ImGui::IsKeyDown(ImGuiKey_RightCtrl);
            if (ctrl)
            {
                // Symmetrisch: beide Flächen wachsen, Zentrum bleibt
                he = std::max(0.001f, st.obbDragStartExtent + worldDelta);
                co = st.obbDragStartCenter;
            }
            else
            {
                // Einseitig: nur die gezogene Fläche bewegt sich
                he = std::max(0.001f, st.obbDragStartExtent + worldDelta * 0.5f);
                co = st.obbDragStartCenter + f.sign * worldDelta * 0.5f;
            }
        }
    }
}

void DrawLightGizmos(EditorFrameContext& ctx)
{
    if (!ctx.debugDraw)
        return;
    const EntityID selected = ctx.state.selectedEntity;
    if (!selected.IsValid())
        return;
    const auto* light = ctx.world.Get<LightComponent>(selected);
    if (!light)
        return;

    using namespace engine::addons::debug_draw;
    DebugDrawRenderer& dd = *ctx.debugDraw;

    const math::Vec3 pos     = WorldPosition(ctx, selected);
    const math::Quat rot     = WorldRotation(ctx, selected);
    const math::Vec4 col     = {light->color.x, light->color.y, light->color.z, 1.f};
    const math::Vec3 forward = rot.Rotate({0.f, 0.f, -1.f});

    switch (light->type)
    {
    case LightType::Spot:
    {
        const float outerR = light->range * std::tan(light->spotOuterDeg * math::DEG_TO_RAD);
        const float innerR = light->range * std::tan(light->spotInnerDeg * math::DEG_TO_RAD);
        const math::Vec3 tip = pos + forward * light->range;

        dd.Circle(tip, forward, outerR, 32, col);
        dd.Circle(tip, forward, innerR, 32, {col.x, col.y, col.z, 0.5f});

        math::Vec3 right, up;
        BuildBasis(forward, right, up);
        constexpr int kRays = 6;
        for (int i = 0; i < kRays; ++i)
        {
            const float a   = (static_cast<float>(i) / kRays) * math::TWO_PI;
            const math::Vec3 rim = tip + (right * std::cos(a) + up * std::sin(a)) * outerR;
            dd.Line(pos, rim, col);
        }
        break;
    }
    case LightType::Directional:
    {
        constexpr float kLen       = 2.f;
        constexpr float kHeadLen   = 0.35f;
        constexpr float kHeadWidth = 0.12f;
        constexpr float kSpacing   = 0.25f;

        const math::Vec3 end = pos + forward * kLen;
        dd.Line(pos, end, col);

        math::Vec3 right, up;
        BuildBasis(forward, right, up);

        const math::Vec3 headBase = end - forward * kHeadLen;
        dd.Line(end, headBase + right * kHeadWidth, col);
        dd.Line(end, headBase - right * kHeadWidth, col);
        dd.Line(end, headBase + up    * kHeadWidth, col);
        dd.Line(end, headBase - up    * kHeadWidth, col);

        dd.Line(pos + right * kSpacing, end + right * kSpacing, col);
        dd.Line(pos - right * kSpacing, end - right * kSpacing, col);
        dd.Line(pos + up    * kSpacing, end + up    * kSpacing, col);
        dd.Line(pos - up    * kSpacing, end - up    * kSpacing, col);
        break;
    }
    case LightType::Point:
    {
        dd.Circle(pos, {0.f, 1.f, 0.f}, light->range, 32, col);
        dd.Circle(pos, {1.f, 0.f, 0.f}, light->range, 32, col);
        dd.Circle(pos, {0.f, 0.f, 1.f}, light->range, 32, col);
        break;
    }
    }
}

void DrawScaleGizmo(EditorFrameContext& ctx)
{
    if (ctx.state.gizmoMode != GizmoMode::Scale)
    {
        SetScaleGizmoActive(ctx, false);
        // Hover/Drag aus dem Scale-Modus nicht stehen lassen - sonst blockiert
        // IsEditorHandleActiveOrHovered nach einem Moduswechsel jede Selektion.
        if (ctx.state.sclGizmoDragAxis >= 0)
            CommitPendingSceneEdit(ctx, "gizmo.scale");
        ctx.state.sclGizmoHoveredAxis = -1;
        ctx.state.sclGizmoDragAxis    = -1;
        return;
    }

    EditorState& st = ctx.state;
    const bool isMulti = st.multiSelection.size() >= 2;

    math::Vec3 center;
    math::Quat basisRotation = math::Quat::Identity();
    math::Vec3 axes[3] = {{1,0,0},{0,1,0},{0,0,1}};

    if (isMulti)
    {
        center = st.multiSelectionPivot;
        // Multi-Selektion: immer World-Space-Achsen
    }
    else
    {
        const EntityID selected = st.selectedEntity;
        if (!selected.IsValid())
        {
            SetScaleGizmoActive(ctx, false);
            st.sclGizmoHoveredAxis = -1;
            return;
        }
        const auto* wtc = ctx.world.Get<WorldTransformComponent>(selected);
        if (!wtc)
        {
            SetScaleGizmoActive(ctx, false);
            st.sclGizmoHoveredAxis = -1;
            return;
        }
        center = wtc->position;
        basisRotation = wtc->rotation;
        axes[0] = wtc->rotation.Rotate({1.f, 0.f, 0.f});
        axes[1] = wtc->rotation.Rotate({0.f, 1.f, 0.f});
        axes[2] = wtc->rotation.Rotate({0.f, 0.f, 1.f});
    }

    const float camDist = (center - st.editorCamera.position).Length();
    const float sz      = std::max(0.15f, camDist * 0.10f);
    const bool meshOk   = UpdateScaleGizmo(ctx, center, basisRotation, sz);

    // Drag beenden
    if (st.sclGizmoDragAxis >= 0 && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
    {
        CommitPendingSceneEdit(ctx, "gizmo.scale");
        st.sclGizmoDragAxis = -1;
    }

    ImGuiIO& io = ImGui::GetIO();
    if (io.WantCaptureMouse) { st.sclGizmoHoveredAxis = -1; return; }

    const math::Mat4 vp = BuildEditorViewProjection(ctx);

    ImVec2 screenCenter;
    if (!WorldToScreenPx(center, vp, io.DisplaySize, screenCenter))
    {
        st.sclGizmoHoveredAxis = -1;
        return;
    }

    if (!meshOk) { st.sclGizmoHoveredAxis = -1; return; }

    // Hover-Erkennung
    st.sclGizmoHoveredAxis = -1;
    if (st.sclGizmoDragAxis < 0)
    {
        constexpr float kPickDist       = 22.f;
        constexpr float kVisualTipScale = 1.45f;
        constexpr float kCenterRadius   = 20.f;  // Pixel-Radius für Center-Sphere
        float bestDist = kPickDist;

        // Center-Sphere zuerst prüfen (hat Vorrang)
        const float dcx = io.MousePos.x - screenCenter.x;
        const float dcy = io.MousePos.y - screenCenter.y;
        const float distCenter = std::sqrt(dcx * dcx + dcy * dcy);
        if (distCenter < kCenterRadius)
        {
            st.sclGizmoHoveredAxis = 3;
        }
        else
        {
            for (int i = 0; i < 3; ++i)
            {
                ImVec2 screenTip;
                if (!WorldToScreenPx(center + axes[i] * (sz * kVisualTipScale), vp, io.DisplaySize, screenTip))
                    continue;
                const float d = DistPointToSegment2D(io.MousePos, screenCenter, screenTip);
                if (d < bestDist) { bestDist = d; st.sclGizmoHoveredAxis = i; }
            }
        }
    }
    else
    {
        st.sclGizmoHoveredAxis = st.sclGizmoDragAxis;
    }

    // Drag starten
    if (st.sclGizmoHoveredAxis >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
    {
        // Für Einzel-Achsen: Screen-Space-Richtung einfrieren
        // Für Center (axis 3): diagonal nach rechts-oben als Drag-Richtung
        float snx = 1.f, sny = -1.f;
        if (st.sclGizmoHoveredAxis < 3)
        {
            const math::Vec3& axisDir = axes[st.sclGizmoHoveredAxis];
            ImVec2 screenTip;
            if (WorldToScreenPx(center + axisDir, vp, io.DisplaySize, screenTip))
            {
                const float sdx  = screenTip.x - screenCenter.x;
                const float sdy  = screenTip.y - screenCenter.y;
                const float sLen = std::sqrt(sdx * sdx + sdy * sdy);
                if (sLen > 1e-4f) { snx = sdx / sLen; sny = sdy / sLen; }
            }
        }
        else
        {
            // uniform: diagonal (rechts = größer, links = kleiner)
            const float diag = std::sqrt(2.f);
            snx = 1.f / diag;
            sny = -1.f / diag;
        }

        BeginPendingSceneEdit(ctx, "gizmo.scale", "Objekt skaliert");
        st.sclGizmoDragAxis       = st.sclGizmoHoveredAxis;
        st.sclGizmoDragStartMouse = {io.MousePos.x, io.MousePos.y};
        st.sclGizmoDragSnx        = snx;
        st.sclGizmoDragSny        = sny;

        // Start-Scales sichern
        if (isMulti)
        {
            st.multiSelectionDragStartScales.clear();
            for (EntityID e : st.multiSelection)
            {
                math::Vec3 s{1.f, 1.f, 1.f};
                if (e.IsValid() && ctx.world.IsAlive(e))
                    if (const auto* tc = ctx.world.Get<TransformComponent>(e))
                        s = tc->localScale;
                st.multiSelectionDragStartScales.push_back(s);
            }
            st.sclGizmoDragStartScale = {1.f, 1.f, 1.f};
        }
        else
        {
            if (const auto* tc = ctx.world.Get<TransformComponent>(st.selectedEntity))
                st.sclGizmoDragStartScale = tc->localScale;
        }
    }

    // Drag fortführen
    if (st.sclGizmoDragAxis >= 0 && ImGui::IsMouseDown(ImGuiMouseButton_Left))
    {
        const float mouseDx    = io.MousePos.x - st.sclGizmoDragStartMouse.x;
        const float mouseDy    = io.MousePos.y - st.sclGizmoDragStartMouse.y;
        const float projPixels = st.sclGizmoDragSnx * mouseDx + st.sclGizmoDragSny * mouseDy;

        constexpr float kSensitivity = 1.f / 200.f;
        const bool  snapScale   = IsSnapActive(st.snap.scaleEnabled) && st.snap.scaleStep > 0.f;
        // Uniform: Faktor relativ rastern (Proportionen bleiben); Achse: Endwert absolut rastern.
        const float scaleFactor = (snapScale && st.sclGizmoDragAxis == 3)
            ? 1.f + SnapTo(projPixels * kSensitivity, st.snap.scaleStep)
            : 1.f + projPixels * kSensitivity;

        auto applyScale = [&](math::Vec3 s0, TransformComponent* tc)
        {
            if (!tc) return;
            const bool snapAxisValue = snapScale && st.sclGizmoDragAxis < 3;
            auto clampV = [&](float v, float f) -> float
            {
                float r = v * f;
                if (snapAxisValue)
                {
                    r = SnapTo(r, st.snap.scaleStep);
                    if (std::abs(r) < st.snap.scaleStep)          // nie auf 0 rasten
                        r = v >= 0.f ? st.snap.scaleStep : -st.snap.scaleStep;
                }
                return v >= 0.f ? std::max(0.0001f, r) : std::min(-0.0001f, r);
            };
            math::Vec3 newScale = s0;
            switch (st.sclGizmoDragAxis)
            {
                case 0: newScale.x = clampV(s0.x, scaleFactor); break;
                case 1: newScale.y = clampV(s0.y, scaleFactor); break;
                case 2: newScale.z = clampV(s0.z, scaleFactor); break;
                case 3:
                    newScale.x = clampV(s0.x, scaleFactor);
                    newScale.y = clampV(s0.y, scaleFactor);
                    newScale.z = clampV(s0.z, scaleFactor);
                    break;
                default: break;
            }
            tc->localScale = newScale;
            tc->dirty      = true;
        };

        if (isMulti)
        {
            for (size_t i = 0; i < st.multiSelection.size(); ++i)
            {
                const EntityID e = st.multiSelection[i];
                if (!e.IsValid() || !ctx.world.IsAlive(e)) continue;
                const math::Vec3 s0 = (i < st.multiSelectionDragStartScales.size())
                    ? st.multiSelectionDragStartScales[i] : math::Vec3{1.f, 1.f, 1.f};
                applyScale(s0, ctx.world.Get<TransformComponent>(e));
            }
        }
        else
        {
            applyScale(st.sclGizmoDragStartScale,
                       ctx.world.Get<TransformComponent>(st.selectedEntity));
        }
    }
}

// =============================================================================

void DrawTransformGizmo(EditorFrameContext& ctx)
{
    if (ctx.state.gizmoMode != GizmoMode::Position)
    {
        SetDpadGizmoActive(ctx, false);
        return;
    }

    math::Vec3 center;
    math::Vec3 axes[3];
    math::Quat basisRotation = math::Quat::Identity();

    const bool isMulti = ctx.state.multiSelection.size() >= 2;
    if (isMulti)
    {
        center  = ctx.state.multiSelectionPivot;
        axes[0] = {1.f, 0.f, 0.f};
        axes[1] = {0.f, 1.f, 0.f};
        axes[2] = {0.f, 0.f, 1.f};
    }
    else
    {
        const EntityID selected = ctx.state.selectedEntity;
        if (!selected.IsValid())
        {
            SetDpadGizmoActive(ctx, false);
            return;
        }
        const auto* wtc = ctx.world.Get<WorldTransformComponent>(selected);
        if (!wtc)
        {
            SetDpadGizmoActive(ctx, false);
            return;
        }
        center = wtc->position;
        if (ctx.state.transformSpace == EditorTransformSpace::Local)
        {
            basisRotation = wtc->rotation;
            axes[0] = wtc->rotation.Rotate({1.f, 0.f, 0.f});
            axes[1] = wtc->rotation.Rotate({0.f, 1.f, 0.f});
            axes[2] = wtc->rotation.Rotate({0.f, 0.f, 1.f});
        }
        else
        {
            axes[0] = {1.f, 0.f, 0.f};
            axes[1] = {0.f, 1.f, 0.f};
            axes[2] = {0.f, 0.f, 1.f};
        }
    }

    const float camDist = (center - ctx.state.editorCamera.position).Length();
    const float sz      = std::max(0.15f, camDist * 0.10f);
    if (UpdateDpadGizmo(ctx, center, basisRotation, sz))
        return;

    if (!ctx.debugDraw) return;
    using namespace engine::addons::debug_draw;
    DebugDrawRenderer& dd = *ctx.debugDraw;

    const math::Vec4 kAxisCol[3] = {
        {1.0f, 0.20f, 0.20f, 1.f},   // X – Rot
        {0.2f, 1.00f, 0.20f, 1.f},   // Y – Grün
        {0.2f, 0.50f, 1.00f, 1.f},   // Z – Blau
    };
    const math::Vec4 kHover{1.f, 1.f, 0.f, 1.f};  // Gelb beim Hover

    const float shaftLen = sz * 0.82f;
    const float headW    = sz * 0.07f;

    for (int i = 0; i < 3; ++i)
    {
        const bool       hovered  = (ctx.state.gizmoHoveredAxis == i);
        const bool       dragging = (ctx.state.gizmoDragAxis    == i);
        const math::Vec4 col      = (hovered || dragging) ? kHover : kAxisCol[i];

        const math::Vec3 tip      = center + axes[i] * sz;
        const math::Vec3 headBase = center + axes[i] * shaftLen;

        // Schaft
        dd.Line(center, headBase, col);

        // Pfeilkopf: 4 Linien von Spitze zur Basis
        math::Vec3 t, bu;
        BuildBasis(axes[i], t, bu);
        dd.Line(tip, headBase + t  * headW, col);
        dd.Line(tip, headBase - t  * headW, col);
        dd.Line(tip, headBase + bu * headW, col);
        dd.Line(tip, headBase - bu * headW, col);
    }

    // Mittelpunkt-Marker (kleines weißes Kreuz)
    const float  cSz = sz * 0.06f;
    const math::Vec4 cCol{1.f, 1.f, 1.f, 0.7f};
    dd.Line(center - math::Vec3{cSz, 0.f, 0.f}, center + math::Vec3{cSz, 0.f, 0.f}, cCol);
    dd.Line(center - math::Vec3{0.f, cSz, 0.f}, center + math::Vec3{0.f, cSz, 0.f}, cCol);
    dd.Line(center - math::Vec3{0.f, 0.f, cSz}, center + math::Vec3{0.f, 0.f, cSz}, cCol);
}

// ── Phase 2: ImGui-Input ──────────────────────────────────────────────────────
void UpdateTransformGizmoInput(EditorFrameContext& ctx)
{
    EditorState& st = ctx.state;

    // Drag beenden
    if (st.gizmoDragAxis >= 0 && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
    {
        CommitPendingSceneEdit(ctx, "gizmo.translate");
        st.gizmoDragAxis = -1;
    }

    if (st.gizmoMode != GizmoMode::Position) { st.gizmoHoveredAxis = -1; return; }

    // Snap-Lock: nach G-Snap keine Bewegung bis Maus vollständig losgelassen
    if (st.snapMoveLocked)
    {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
            st.snapMoveLocked = false;  // freigeben sobald Maus los
        else
        {
            st.gizmoHoveredAxis = -1;
            return;                     // noch gedrückt → alles sperren
        }
    }

    const bool isMulti = st.multiSelection.size() >= 2;

    math::Vec3 center;
    math::Vec3 axes[3];

    if (isMulti)
    {
        center  = st.multiSelectionPivot;
        axes[0] = {1.f, 0.f, 0.f};
        axes[1] = {0.f, 1.f, 0.f};
        axes[2] = {0.f, 0.f, 1.f};
    }
    else
    {
        const EntityID selected = st.selectedEntity;
        if (!selected.IsValid()) { st.gizmoHoveredAxis = -1; return; }
        const auto* wtc = ctx.world.Get<WorldTransformComponent>(selected);
        if (!wtc) { st.gizmoHoveredAxis = -1; return; }
        center = wtc->position;
        if (st.transformSpace == EditorTransformSpace::Local)
        {
            axes[0] = wtc->rotation.Rotate({1.f, 0.f, 0.f});
            axes[1] = wtc->rotation.Rotate({0.f, 1.f, 0.f});
            axes[2] = wtc->rotation.Rotate({0.f, 0.f, 1.f});
        }
        else
        {
            axes[0] = {1.f, 0.f, 0.f};
            axes[1] = {0.f, 1.f, 0.f};
            axes[2] = {0.f, 0.f, 1.f};
        }
    }

    ImGuiIO& io = ImGui::GetIO();
    if (io.WantCaptureMouse) { st.gizmoHoveredAxis = -1; return; }

    const float camDist = (center - st.editorCamera.position).Length();
    const float sz      = std::max(0.15f, camDist * 0.10f);

    // VP-Matrix
    const math::Mat4 vp = BuildEditorViewProjection(ctx);

    ImVec2 screenCenter;
    if (!WorldToScreenPx(center, vp, io.DisplaySize, screenCenter))
    {
        st.gizmoHoveredAxis = -1;
        return;
    }

    // Hover-Erkennung (nur wenn kein Drag läuft)
    st.gizmoHoveredAxis = -1;
    if (st.gizmoDragAxis < 0)
    {
        // Center-Sphere zuerst (hat Vorrang)
        constexpr float kCenterRadius = 18.f;
        const float dcx = io.MousePos.x - screenCenter.x;
        const float dcy = io.MousePos.y - screenCenter.y;
        if (std::sqrt(dcx * dcx + dcy * dcy) < kCenterRadius)
        {
            st.gizmoHoveredAxis = 3;
        }
        else
        {
            constexpr float kPickDist = 22.f;
            constexpr float kVisualTipScale = 1.45f;
            float bestDist = kPickDist;
            for (int i = 0; i < 3; ++i)
            {
                ImVec2 screenTip;
                if (!WorldToScreenPx(center + axes[i] * (sz * kVisualTipScale), vp, io.DisplaySize, screenTip))
                    continue;
                const float d = DistPointToSegment2D(io.MousePos, screenCenter, screenTip);
                if (d < bestDist) { bestDist = d; st.gizmoHoveredAxis = i; }
            }
        }
    }
    else
    {
        st.gizmoHoveredAxis = st.gizmoDragAxis;
    }

    // Drag starten
    if (st.gizmoHoveredAxis >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
    {
        float snx = 1.f, sny = 0.f;
        if (st.gizmoHoveredAxis < 3)
        {
            const math::Vec3& axisDir = axes[st.gizmoHoveredAxis];
            ImVec2 screenTip;
            if (WorldToScreenPx(center + axisDir, vp, io.DisplaySize, screenTip))
            {
                const float sdx = screenTip.x - screenCenter.x;
                const float sdy = screenTip.y - screenCenter.y;
                const float sLen = std::sqrt(sdx * sdx + sdy * sdy);
                if (sLen > 1e-4f) { snx = sdx / sLen; sny = sdy / sLen; }
            }
        }
        // Achse 3 (Screen-Plane): snx/sny werden beim Drag nicht genutzt

        BeginPendingSceneEdit(ctx, "gizmo.translate", "Objekt bewegt");
        st.gizmoDragAxis       = st.gizmoHoveredAxis;
        st.gizmoDragStartPos   = center;
        st.gizmoDragStartMouse = {io.MousePos.x, io.MousePos.y};
        st.gizmoDragDepth      = camDist;
        st.gizmoDragSnx        = snx;
        st.gizmoDragSny        = sny;

        // Start-Positionen aller Entities sichern (für stabilen Multi-Selektion-Drag)
        if (isMulti)
        {
            st.multiSelectionDragStartPositions.clear();
            for (EntityID e : st.multiSelection)
            {
                math::Vec3 pos{0.f, 0.f, 0.f};
                if (e.IsValid() && ctx.world.IsAlive(e))
                    if (const auto* wtc = ctx.world.Get<WorldTransformComponent>(e))
                        pos = wtc->position;
                st.multiSelectionDragStartPositions.push_back(pos);
            }
        }
    }

    // Drag fortführen
    if (st.gizmoDragAxis >= 0 && ImGui::IsMouseDown(ImGuiMouseButton_Left))
    {
        const float mouseDx    = io.MousePos.x - st.gizmoDragStartMouse.x;
        const float mouseDy    = io.MousePos.y - st.gizmoDragStartMouse.y;

        const float tanHalfFov = std::tan(30.f * math::DEG_TO_RAD);
        const float unitsPerPx = 2.f * st.gizmoDragDepth * tanHalfFov /
                                 std::max(1.f, io.DisplaySize.y);

        // Achse 3: freies Bewegen in der Kamera-Ebene (Screen-Plane)
        if (st.gizmoDragAxis == 3)
        {
            const math::Mat4 viewMat = BuildEditorViewMatrix(st.editorCamera);
            const math::Vec3 camRight = { viewMat.m[0][0], viewMat.m[1][0], viewMat.m[2][0] };
            const math::Vec3 camUp    = { viewMat.m[0][1], viewMat.m[1][1], viewMat.m[2][1] };
            math::Vec3 newPos   = st.gizmoDragStartPos
                + camRight * ( mouseDx * unitsPerPx)
                + camUp    * (-mouseDy * unitsPerPx);  // Y invertiert (Screen nach unten)
            if (IsSnapActive(st.snap.moveEnabled) && st.snap.moveStep > 0.f)
            {
                newPos.x = SnapTo(newPos.x, st.snap.moveStep);
                newPos.y = SnapTo(newPos.y, st.snap.moveStep);
                newPos.z = SnapTo(newPos.z, st.snap.moveStep);
            }

            if (isMulti)
            {
                for (size_t i = 0; i < st.multiSelection.size(); ++i)
                {
                    const EntityID e = st.multiSelection[i];
                    if (!e.IsValid() || !ctx.world.IsAlive(e)) continue;
                    auto* tc = ctx.world.Get<TransformComponent>(e);
                    if (!tc) continue;
                    const math::Vec3 startPos = (i < st.multiSelectionDragStartPositions.size())
                        ? st.multiSelectionDragStartPositions[i] : math::Vec3{0.f, 0.f, 0.f};
                    const math::Vec3 delta = newPos - st.gizmoDragStartPos;
                    SetWorldPosition(ctx, e, *tc, startPos + delta);
                }
                st.multiSelectionPivot = newPos;
            }
            else
            {
                const EntityID selected = st.selectedEntity;
                auto* tc = ctx.world.Get<TransformComponent>(selected);
                if (tc)
                {
                    SetWorldPosition(ctx, selected, *tc, newPos);
                }
            }
            return;  // nicht in Achsen-Drag-Code fallen
        }

        const math::Vec3& axisDir = axes[st.gizmoDragAxis];
        const float projPixels = st.gizmoDragSnx * mouseDx + st.gizmoDragSny * mouseDy;
        float displacement = projPixels * unitsPerPx;
        if (IsSnapActive(st.snap.moveEnabled) && st.snap.moveStep > 0.f)
        {
            const bool worldAxis = isMulti || st.transformSpace == EditorTransformSpace::World;
            if (worldAxis)
            {
                // Weltachse: absolute Koordinate auf das Raster setzen (wie Unreal)
                const float start = (&st.gizmoDragStartPos.x)[st.gizmoDragAxis];
                displacement = SnapTo(start + displacement, st.snap.moveStep) - start;
            }
            else
            {
                // Lokale (gedrehte) Achse: in Raster-Schritten entlang der Achse
                displacement = SnapTo(displacement, st.snap.moveStep);
            }
        }

        if (isMulti)
        {
            for (size_t i = 0; i < st.multiSelection.size(); ++i)
            {
                const EntityID e = st.multiSelection[i];
                if (!e.IsValid() || !ctx.world.IsAlive(e)) continue;
                auto* tc = ctx.world.Get<TransformComponent>(e);
                if (!tc) continue;
                const math::Vec3 startPos = (i < st.multiSelectionDragStartPositions.size())
                    ? st.multiSelectionDragStartPositions[i]
                    : math::Vec3{0.f, 0.f, 0.f};
                SetWorldPosition(ctx, e, *tc, startPos + axisDir * displacement);
            }
            // Pivot mitbewegen
            st.multiSelectionPivot = st.gizmoDragStartPos + axisDir * displacement;
        }
        else
        {
            const EntityID   selected     = st.selectedEntity;
            auto*            tc           = ctx.world.Get<TransformComponent>(selected);
            const math::Vec3 newParentPos = st.gizmoDragStartPos + axisDir * displacement;
            if (tc)
            {
                SetWorldPosition(ctx, selected, *tc, newParentPos);
            }
        }
    }
}

// =============================================================================
// Rotations-Gizmo – drei farbige Ringe um alle drei Achsen
// =============================================================================

void DrawRotationGizmo(EditorFrameContext& ctx)
{
    if (ctx.state.gizmoMode != GizmoMode::Rotation)
    {
        SetRotateGizmoActive(ctx, false);
        return;
    }

    math::Vec3 center;
    math::Vec3 axes[3];
    math::Quat basisRotation = math::Quat::Identity();
    const bool isMulti = ctx.state.multiSelection.size() >= 2;

    if (isMulti)
    {
        center  = ctx.state.multiSelectionPivot;
        axes[0] = {1.f, 0.f, 0.f};
        axes[1] = {0.f, 1.f, 0.f};
        axes[2] = {0.f, 0.f, 1.f};
    }
    else
    {
        const EntityID selected = ctx.state.selectedEntity;
        if (!selected.IsValid())
        {
            SetRotateGizmoActive(ctx, false);
            return;
        }
        const auto* wtc = ctx.world.Get<WorldTransformComponent>(selected);
        if (!wtc)
        {
            SetRotateGizmoActive(ctx, false);
            return;
        }
        center = wtc->position;
        if (ctx.state.transformSpace == EditorTransformSpace::Local)
        {
            basisRotation = wtc->rotation;
            axes[0] = wtc->rotation.Rotate({1.f, 0.f, 0.f});
            axes[1] = wtc->rotation.Rotate({0.f, 1.f, 0.f});
            axes[2] = wtc->rotation.Rotate({0.f, 0.f, 1.f});
        }
        else
        {
            axes[0] = {1.f, 0.f, 0.f};
            axes[1] = {0.f, 1.f, 0.f};
            axes[2] = {0.f, 0.f, 1.f};
        }
    }

    const float camDist = (center - ctx.state.editorCamera.position).Length();
    const float radius  = std::max(0.15f, camDist * 0.10f);

    if (UpdateRotateGizmo(ctx, center, basisRotation, radius))
        return;

    if (!ctx.debugDraw) return;
    using namespace engine::addons::debug_draw;
    DebugDrawRenderer& dd = *ctx.debugDraw;

    const math::Vec4 kColors[3] = {
        {1.0f, 0.20f, 0.20f, 1.f},
        {0.2f, 1.00f, 0.20f, 1.f},
        {0.2f, 0.50f, 1.00f, 1.f},
    };
    const math::Vec4 kHover{1.f, 1.f, 0.f, 1.f};

    for (int i = 0; i < 3; ++i)
    {
        const bool hov = (ctx.state.rotGizmoHoveredAxis == i ||
                          ctx.state.rotGizmoDragAxis    == i);
        dd.Circle(center, axes[i], radius, 48, hov ? kHover : kColors[i]);
    }
}

void UpdateRotationGizmoInput(EditorFrameContext& ctx)
{
    EditorState& st = ctx.state;
    if (st.gizmoMode != GizmoMode::Rotation) { st.rotGizmoHoveredAxis = -1; return; }

    // Drag beenden
    if (st.rotGizmoDragAxis >= 0 && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
    {
        CommitPendingSceneEdit(ctx, "gizmo.rotate");
        st.rotGizmoDragAxis = -1;
    }

    // Snap-Lock
    if (st.snapMoveLocked) { st.rotGizmoHoveredAxis = -1; return; }

    const bool isMulti = st.multiSelection.size() >= 2;

    math::Vec3 center;
    if (isMulti)
    {
        center = st.multiSelectionPivot;
    }
    else
    {
        const EntityID selected = st.selectedEntity;
        if (!selected.IsValid()) { st.rotGizmoHoveredAxis = -1; return; }
        const auto* wtc = ctx.world.Get<WorldTransformComponent>(selected);
        const auto* tc  = ctx.world.Get<TransformComponent>(selected);
        if (!wtc || !tc) { st.rotGizmoHoveredAxis = -1; return; }
        center = wtc->position;
    }

    ImGuiIO& io = ImGui::GetIO();
    if (io.WantCaptureMouse) { st.rotGizmoHoveredAxis = -1; return; }

    // center bereits gesetzt oben
    const float      camDist = (center - st.editorCamera.position).Length();
    const float      radius  = std::max(0.15f, camDist * 0.10f);

    // Bei Single-Selection: Local-Space-Achsen aus Entity-Rotation
    math::Vec3 axes[3];
    math::Quat basisRotation = math::Quat::Identity();
    if (!isMulti && st.transformSpace == EditorTransformSpace::Local)
    {
        const auto* wtcSingle = ctx.world.Get<WorldTransformComponent>(st.selectedEntity);
        if (wtcSingle)
        {
            basisRotation = wtcSingle->rotation;
            axes[0] = wtcSingle->rotation.Rotate({1.f, 0.f, 0.f});
            axes[1] = wtcSingle->rotation.Rotate({0.f, 1.f, 0.f});
            axes[2] = wtcSingle->rotation.Rotate({0.f, 0.f, 1.f});
        }
        else
        {
            axes[0] = {1.f, 0.f, 0.f};
            axes[1] = {0.f, 1.f, 0.f};
            axes[2] = {0.f, 0.f, 1.f};
        }
    }
    else
    {
        axes[0] = {1.f, 0.f, 0.f};
        axes[1] = {0.f, 1.f, 0.f};
        axes[2] = {0.f, 0.f, 1.f};
    }

    const math::Mat4 vp = BuildEditorViewProjection(ctx);

    ImVec2 screenCenter;
    const bool screenCenterValid = WorldToScreenPx(center, vp, io.DisplaySize, screenCenter);

    // Hover: zuerst Center-Sphere prüfen, dann Ringe
    st.rotGizmoHoveredAxis = -1;
    if (st.rotGizmoDragAxis < 0)
    {
        constexpr float kCenterRadius = 18.f;
        if (screenCenterValid)
        {
            const float dcx = io.MousePos.x - screenCenter.x;
            const float dcy = io.MousePos.y - screenCenter.y;
            if (std::sqrt(dcx * dcx + dcy * dcy) < kCenterRadius)
                st.rotGizmoHoveredAxis = 3;
        }

        if (st.rotGizmoHoveredAxis < 0)  // Ringe nur prüfen wenn Sphere nicht getroffen
        {
        constexpr int   kSamples  = 18;
        constexpr float kPickDist = 18.f;
        float bestDist = kPickDist;
        math::Quat visualRotations[3];
        BuildRotateGizmoVisualRotations(basisRotation, visualRotations);

        for (int i = 0; i < 3; ++i)
        {
            ImVec2 prev{};
            bool prevValid = false;
            for (int s = 0; s <= kSamples; ++s)
            {
                const float a = (static_cast<float>(s) / kSamples) * math::TWO_PI;
                const math::Vec3 localPoint{
                    -std::cos(a) * radius,
                    0.f,
                    std::sin(a) * radius
                };
                const math::Vec3 wp = center + visualRotations[i].Rotate(localPoint);
                ImVec2 sp;
                const bool valid = WorldToScreenPx(wp, vp, io.DisplaySize, sp);
                if (valid && prevValid)
                {
                    const float d = DistPointToSegment2D(io.MousePos, prev, sp);
                    if (d < bestDist) { bestDist = d; st.rotGizmoHoveredAxis = i; }
                }
                prev = sp;
                prevValid = valid;
            }
        }
        } // end if (rotGizmoHoveredAxis < 0)
    }
    else
    {
        st.rotGizmoHoveredAxis = st.rotGizmoDragAxis;
    }

    // Drag starten — Achse + Startwinkel einfrieren (oder Trackball)
    if (st.rotGizmoHoveredAxis >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
    {
        ImVec2 sc;
        if (!WorldToScreenPx(center, vp, io.DisplaySize, sc)) return;

        BeginPendingSceneEdit(ctx, "gizmo.rotate", "Objekt rotiert");
        st.rotGizmoDragAxis         = st.rotGizmoHoveredAxis;
        st.rotGizmoDragStartMouse   = {io.MousePos.x, io.MousePos.y};
        st.rotGizmoDragScreenCenter = {sc.x, sc.y};

        if (st.rotGizmoHoveredAxis < 3)
        {
            // Normaler Ring-Drag: Winkel einfrieren
            st.rotGizmoDragWorldAxis  = axes[st.rotGizmoHoveredAxis];
            st.rotGizmoDragStartAngle = std::atan2(-(io.MousePos.y - sc.y), io.MousePos.x - sc.x);
        }
        else
        {
            // Trackball: Kamera-Right/Up beim Drag-Start einfrieren
            const math::Mat4 viewMat = BuildEditorViewMatrix(st.editorCamera);
            // Right = erste Zeile (X), Up = zweite Zeile (Y) der View-Matrix (transponiert = Kamera-Weltachsen)
            st.rotGizmoDragCamRight = { viewMat.m[0][0], viewMat.m[1][0], viewMat.m[2][0] };
            st.rotGizmoDragCamUp    = { viewMat.m[0][1], viewMat.m[1][1], viewMat.m[2][1] };
        }

        if (isMulti)
        {
            // Start-Quats + Positionen aller Entities sichern
            st.multiSelectionDragStartPositions.clear();
            st.multiSelectionDragStartRotations.clear();
            for (EntityID e : st.multiSelection)
            {
                math::Vec3 pos{0.f, 0.f, 0.f};
                math::Quat rot = math::Quat::Identity();
                if (e.IsValid() && ctx.world.IsAlive(e))
                    if (const auto* wtc = ctx.world.Get<WorldTransformComponent>(e))
                    {
                        pos = wtc->position;
                        rot = wtc->rotation;
                    }
                st.multiSelectionDragStartPositions.push_back(pos);
                st.multiSelectionDragStartRotations.push_back(rot);
            }
        }
        else
        {
            st.rotGizmoDragStartQuat = WorldRotation(ctx, st.selectedEntity);
        }
    }

    // Drag fortführen
    if (st.rotGizmoDragAxis >= 0 && ImGui::IsMouseDown(ImGuiMouseButton_Left))
    {
        math::Quat deltaRot;

        if (st.rotGizmoDragAxis < 3)
        {
            // Ring-Drag: Winkel-Delta um eingefrorene Weltachse
            float currentAngle = std::atan2(
                -(io.MousePos.y - st.rotGizmoDragScreenCenter.y),
                  (io.MousePos.x - st.rotGizmoDragScreenCenter.x));
            float deltaAngle = currentAngle - st.rotGizmoDragStartAngle;
            while (deltaAngle >  math::PI)  deltaAngle -= math::TWO_PI;
            while (deltaAngle < -math::PI)  deltaAngle += math::TWO_PI;
            float deltaDeg = deltaAngle * math::RAD_TO_DEG;
            if (IsSnapActive(st.snap.rotateEnabled))
                deltaDeg = SnapTo(deltaDeg, st.snap.rotateStepDeg);
            deltaRot = math::Quat::FromAxisAngleDeg(st.rotGizmoDragWorldAxis, deltaDeg);
        }
        else
        {
            // Trackball: Maus-Delta → Rotation um senkrechte Kamera-Achse
            const float mouseDx = io.MousePos.x - st.rotGizmoDragStartMouse.x;
            const float mouseDy = io.MousePos.y - st.rotGizmoDragStartMouse.y;
            constexpr float kSensitivity = 0.4f;  // Grad pro Pixel
            // Rechts ziehen = um Kamera-Up drehen (Yaw),
            // Hoch ziehen   = um Kamera-Right drehen (Pitch, negiert weil Y nach unten)
            float angleYaw   = mouseDx * kSensitivity;
            float anglePitch = -mouseDy * kSensitivity;
            if (IsSnapActive(st.snap.rotateEnabled))
            {
                angleYaw   = SnapTo(angleYaw,   st.snap.rotateStepDeg);
                anglePitch = SnapTo(anglePitch, st.snap.rotateStepDeg);
            }
            const math::Quat yawRot   = math::Quat::FromAxisAngleDeg(st.rotGizmoDragCamUp,    angleYaw);
            const math::Quat pitchRot = math::Quat::FromAxisAngleDeg(st.rotGizmoDragCamRight, anglePitch);
            deltaRot = (yawRot * pitchRot).Normalized();
        }

        if (isMulti)
        {
            // Jede Entity: Rotation um Pivot
            // newPos  = pivot + deltaRot * (startPos - pivot)
            // newQuat = deltaRot * startQuat
            for (size_t i = 0; i < st.multiSelection.size(); ++i)
            {
                const EntityID e = st.multiSelection[i];
                if (!e.IsValid() || !ctx.world.IsAlive(e)) continue;
                auto* tc = ctx.world.Get<TransformComponent>(e);
                if (!tc) continue;

                const math::Vec3 startPos = (i < st.multiSelectionDragStartPositions.size())
                    ? st.multiSelectionDragStartPositions[i] : math::Vec3{0.f, 0.f, 0.f};
                const math::Quat startQuat = (i < st.multiSelectionDragStartRotations.size())
                    ? st.multiSelectionDragStartRotations[i] : math::Quat::Identity();

                const math::Vec3 offset    = startPos - st.multiSelectionPivot;
                const math::Vec3 newOffset = deltaRot.Rotate(offset);
                const math::Vec3 newPos    = st.multiSelectionPivot + newOffset;
                const math::Quat newQuat   = (deltaRot * startQuat).Normalized();

                const math::Mat4 desiredWorld = math::Mat4::TRS(newPos, newQuat,
                    WorldScale(ctx, e));
                SetWorldTransform(ctx, e, *tc, desiredWorld);
            }
        }
        else
        {
            auto* tc = ctx.world.Get<TransformComponent>(st.selectedEntity);
            if (tc)
            {
                const math::Quat newParentRot = (deltaRot * st.rotGizmoDragStartQuat).Normalized();
                SetWorldRotation(ctx, st.selectedEntity, *tc, newParentRot);
            }
        }
    }
}

// Bodenraster auf y = 0 um die Kamera. Gezeichnet ueber DebugDraw::LineDepthTested,
// also gegen die Szenentiefe getestet: Objekte verdecken das Raster wie in Unreal/Unity.
// Die Linien landen im HDR-Puffer vor dem Tonemapping — daher dunkle Farbwerte.
// Linienabstand = Positions-Rasterweite, bei grossem Abstand vergroebert (x10).
// Jede 10. Linie heller, Welt-X rot, Welt-Z blau, zum Rand hin ausgeblendet.
void DrawSnapGrid(EditorFrameContext& ctx)
{
    const EditorSnapSettings& snap = ctx.state.snap;
    if (!ctx.debugDraw || !snap.showGrid || ctx.currentProjectName.empty())
        return;

    using namespace engine::addons::debug_draw;
    DebugDrawRenderer& dd = *ctx.debugDraw;

    const math::Vec3 cam = ctx.state.editorCamera.position;
    const float extent = std::clamp(std::abs(cam.y) * 6.f + 20.f, 20.f, 2000.f);
    float spacing = snap.moveStep > 0.f ? snap.moveStep : 1.f;
    while (extent / spacing > 100.f)
        spacing *= 10.f;

    const float majorSpacing = spacing * 10.f;
    const float cx = std::floor(cam.x / spacing) * spacing;
    const float cz = std::floor(cam.z / spacing) * spacing;
    const int   halfCount = static_cast<int>(std::ceil(extent / spacing));

    const math::Vec4 minorColor{0.08f, 0.08f, 0.08f, 0.45f};
    const math::Vec4 majorColor{0.16f, 0.16f, 0.16f, 0.65f};
    const math::Vec4 xAxisColor{0.45f, 0.06f, 0.06f, 0.90f};
    const math::Vec4 zAxisColor{0.06f, 0.12f, 0.50f, 0.90f};

    auto isMajor = [&](float v) {
        return std::abs(v / majorSpacing - std::round(v / majorSpacing)) < 1e-3f;
    };

    // Gegen Z-Fighting mit Boeden auf y = 0: jeden Punkt minimal entlang des
    // Sichtstrahls zur Kamera ziehen. Die Bildschirmposition bleibt exakt gleich,
    // nur die Tiefe sinkt um einen konstanten Bruchteil des Abstands.
    constexpr float kTowardCamera = 0.002f;
    auto lift = [&](const math::Vec3& p) { return p + (cam - p) * kTowardCamera; };

    // Jede Linie in Segmente teilen, Alpha nach Abstand zur Kamera (Ausblenden zum Rand).
    constexpr int kSegments = 16;
    auto fadeLine = [&](const math::Vec3& from, const math::Vec3& to, const math::Vec4& color)
    {
        for (int s = 0; s < kSegments; ++s)
        {
            const math::Vec3 p0 = from + (to - from) * (static_cast<float>(s) / kSegments);
            const math::Vec3 p1 = from + (to - from) * (static_cast<float>(s + 1) / kSegments);
            const math::Vec3 mid = (p0 + p1) * 0.5f;
            const float dist = std::sqrt((mid.x - cam.x) * (mid.x - cam.x) + (mid.z - cam.z) * (mid.z - cam.z));
            const float fade = std::clamp(1.f - (dist - extent * 0.4f) / (extent * 0.6f), 0.f, 1.f);
            if (fade > 0.f)
                dd.LineDepthTested(lift(p0), lift(p1), {color.x, color.y, color.z, color.w * fade});
        }
    };

    for (int i = -halfCount; i <= halfCount; ++i)
    {
        // Linien parallel zu Z (konstantes x)
        const float x = cx + static_cast<float>(i) * spacing;
        fadeLine({x, 0.f, cz - extent}, {x, 0.f, cz + extent},
                 std::abs(x) < spacing * 0.01f ? zAxisColor : (isMajor(x) ? majorColor : minorColor));

        // Linien parallel zu X (konstantes z)
        const float z = cz + static_cast<float>(i) * spacing;
        fadeLine({cx - extent, 0.f, z}, {cx + extent, 0.f, z},
                 std::abs(z) < spacing * 0.01f ? xAxisColor : (isMajor(z) ? majorColor : minorColor));
    }
}

} // namespace detail

} // namespace engine::renderer::addons::editor

#endif // KROM_EDITOR_HAS_IMGUI
