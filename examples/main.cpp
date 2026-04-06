// =============================================================================
// KROM Engine - examples/main.cpp  v0.3
// Demonstriert:
//   - Material-Registrierung + PipelineKey
//   - Extract-Phase: ECS → RenderWorld
//   - Frustum-Culling + DrawList-Aufbau
//   - Radix-Sort nach SortKey
//   - RenderGraph mit korrekter State-Planung (topo-sort first)
//   - Korrektes JSON-Serializer-Output
// =============================================================================
#include "core/Types.hpp"
#include "core/Math.hpp"
#include "core/Debug.hpp"
#include "ecs/World.hpp"
#include "ecs/Components.hpp"
#include "scene/Scene.hpp"
#include "assets/AssetRegistry.hpp"
#include "jobs/JobSystem.hpp"
#include "ecs/EntityCommandBuffer.hpp"
#include "ecs/QueryCache.hpp"
#include "jobs/TaskGraph.hpp"
#include "events/EventBus.hpp"
#include "serialization/SceneSerializer.hpp"
#include "renderer/IDevice.hpp"
#include "renderer/MaterialSystem.hpp"
#include "renderer/RenderWorld.hpp"
#include "renderer/ECSExtractor.hpp"
#include "renderer/PipelineCache.hpp"
#include "renderer/ShaderBindingModel.hpp"
#include "scene/TransformSystem.hpp"
#include "rendergraph/ResourceAliaser.hpp"
#include "NullDevice.hpp"
#include "rendergraph/RenderGraph.hpp"
#include "rendergraph/FramePipeline.hpp"
#include <cstdio>
#include <memory>

using namespace engine;
using namespace engine::math;
using namespace engine::ecs;
using namespace engine::renderer;
using namespace engine::rendergraph;

// =============================================================================
// Materialen registrieren
// =============================================================================
static void RegisterDemoMaterials(MaterialSystem& ms,
                                   ShaderHandle vs, ShaderHandle fs,
                                   MaterialHandle& outOpaque,
                                   MaterialHandle& outTransparent)
{
    // Opaque PBR-Material
    {
        MaterialDesc d;
        d.name           = "DefaultOpaque";
        d.passTag        = RenderPassTag::Opaque;
        d.vertexShader   = vs;
        d.fragmentShader = fs;
        d.colorFormat    = Format::RGBA16_FLOAT;
        d.depthFormat    = Format::D24_UNORM_S8_UINT;
        d.depthStencil.depthEnable = true;
        d.depthStencil.depthWrite  = true;
        d.depthStencil.depthFunc   = DepthFunc::Less;
        d.rasterizer.cullMode      = CullMode::Back;
        d.rasterizer.fillMode      = FillMode::Solid;

        // Parameter: albedo color, roughness, metallic
        MaterialParam p0; p0.name="albedo"; p0.type=MaterialParam::Type::Vec4;
        p0.value.f[0]=1.f; p0.value.f[1]=1.f; p0.value.f[2]=1.f; p0.value.f[3]=1.f;
        MaterialParam p1; p1.name="roughness"; p1.type=MaterialParam::Type::Float;
        p1.value.f[0]=0.5f;
        MaterialParam p2; p2.name="metallic"; p2.type=MaterialParam::Type::Float;
        p2.value.f[0]=0.0f;
        d.params = { p0, p1, p2 };

        outOpaque = ms.RegisterMaterial(std::move(d));
    }

    // Transparent Material
    {
        MaterialDesc d;
        d.name           = "DefaultTransparent";
        d.passTag        = RenderPassTag::Transparent;
        d.vertexShader   = vs;
        d.fragmentShader = fs;
        d.colorFormat    = Format::RGBA16_FLOAT;
        d.depthFormat    = Format::D24_UNORM_S8_UINT;
        d.depthStencil.depthEnable = true;
        d.depthStencil.depthWrite  = false;   // kein Depth-Write bei Transparenz
        d.depthStencil.depthFunc   = DepthFunc::Less;
        d.rasterizer.cullMode      = CullMode::None;    // beidseitig
        d.blend.blendEnable        = true;
        d.blend.srcBlend           = BlendFactor::SrcAlpha;
        d.blend.dstBlend           = BlendFactor::InvSrcAlpha;
        d.blend.blendOp            = BlendOp::Add;

        MaterialParam p0; p0.name="albedo"; p0.type=MaterialParam::Type::Vec4;
        p0.value.f[0]=0.2f; p0.value.f[1]=0.6f; p0.value.f[2]=1.f; p0.value.f[3]=0.5f;
        d.params = { p0 };

        outTransparent = ms.RegisterMaterial(std::move(d));
    }
}

// =============================================================================
// Bounds-Update (normalerweise ein ECS-System)
// =============================================================================
static void UpdateBounds(World& world)
{
    world.View<TransformComponent, MeshComponent, BoundsComponent>(
        [](EntityID, TransformComponent& tc, MeshComponent&, BoundsComponent& bc)
        {
            // Skaliere lokale Extents mit der absoluten Scale-Komponente
            const Vec3& s = tc.localScale;
            bc.extentsWorld = Vec3{ bc.extentsLocal.x * s.x,
                                    bc.extentsLocal.y * s.y,
                                    bc.extentsLocal.z * s.z };
            bc.centerWorld  = tc.localPosition;
            bc.boundingSphere = bc.extentsWorld.Length();
        });
}

// =============================================================================
// Szene aufbauen
// =============================================================================
static void BuildScene(Scene& scene, MaterialHandle opaqueMat, MaterialHandle transparentMat)
{
    // Boden
    EntityID ground = scene.CreateEntity("Ground");
    scene.GetWorld().Add<MeshComponent>(ground, MeshHandle::Make(1u,1u));
    scene.GetWorld().Add<MaterialComponent>(ground, opaqueMat);
    scene.GetWorld().Add<BoundsComponent>(ground, BoundsComponent{
        .centerLocal={0,0,0}, .extentsLocal={10,0.1f,10} });
    scene.SetLocalPosition(ground, Vec3{0,-0.5f,0});
    scene.SetLocalScale(ground, Vec3{20,0.1f,20});

    // 10 opaque Cubes
    for (int i = 0; i < 10; ++i)
    {
        char name[32]; std::snprintf(name,sizeof(name),"Cube_%d",i);
        EntityID e = scene.CreateEntity(name);
        scene.GetWorld().Add<MeshComponent>(e, MeshHandle::Make(1u,1u));
        scene.GetWorld().Add<MaterialComponent>(e, opaqueMat);
        scene.GetWorld().Add<BoundsComponent>(e, BoundsComponent{
            .centerLocal={0,0,0}, .extentsLocal={0.5f,0.5f,0.5f} });
        scene.SetLocalPosition(e, Vec3{static_cast<float>(i)*2.f - 9.f, 0.5f, 0.f});
    }

    // 3 transparente Objekte hinter den opaquen
    for (int i = 0; i < 3; ++i)
    {
        char name[32]; std::snprintf(name,sizeof(name),"Glass_%d",i);
        EntityID e = scene.CreateEntity(name);
        scene.GetWorld().Add<MeshComponent>(e, MeshHandle::Make(1u,1u));
        scene.GetWorld().Add<MaterialComponent>(e, transparentMat);
        scene.GetWorld().Add<BoundsComponent>(e, BoundsComponent{
            .centerLocal={0,0,0}, .extentsLocal={0.5f,1.f,0.05f} });
        scene.SetLocalPosition(e, Vec3{static_cast<float>(i)*3.f - 3.f, 1.f, 3.f});
    }

    // Licht
    EntityID light = scene.CreateEntity("SunLight");
    scene.GetWorld().Add<LightComponent>(light, LightComponent{
        .type=LightType::Directional, .color={1,0.95f,0.85f},
        .intensity=2.f, .castShadows=true });
    scene.SetLocalRotation(light, Quat::FromEulerDeg(45,30,0));
}

// =============================================================================
// Main
// =============================================================================
int main(int, char**)
{
    Debug::MinLevel = engine::LogLevel::Info;
    Debug::Log("main.cpp: KROM Engine v0.3 starting");

    RegisterAllComponents();

    // --- ECS + Scene ---
    World  world;
    Scene  scene(world);

    // --- Job-System ---
    jobs::JobSystem jobs;
    jobs.Initialize(0u);

    // --- Event-Bus ---
    events::EventBus bus;

    // --- Device (Null-Backend) ---
    auto device = DeviceFactory::Create(DeviceFactory::BackendType::Null);
    device->Initialize({ .enableDebugLayer=true, .appName="KROM v0.5" });

    IDevice::SwapchainDesc sc;
    sc.width=1280; sc.height=720; sc.bufferCount=2; sc.vsync=true;
    auto swapchain = device->CreateSwapchain(sc);

    // --- Dummy-Shader-Handles (im Null-Backend ohne echten Compile) ---
    const ShaderHandle vs = device->CreateShaderFromSource(
        "// vertex shader stub", ShaderStageMask::Vertex, "VSMain", "DefaultVS");
    const ShaderHandle fs = device->CreateShaderFromSource(
        "// fragment shader stub", ShaderStageMask::Fragment, "PSMain", "DefaultPS");

    // --- Materialsystem ---
    MaterialSystem matSys;
    MaterialHandle opaqueMat, transparentMat;
    RegisterDemoMaterials(matSys, vs, fs, opaqueMat, transparentMat);

    Debug::Log("main.cpp: Materials: %zu descs registered", matSys.DescCount());

    // PipelineKey validieren
    const PipelineKey opaqKey   = matSys.BuildPipelineKey(opaqueMat);
    const PipelineKey transKey  = matSys.BuildPipelineKey(transparentMat);
    Debug::Log("main.cpp: PipelineKey opaque hash=0x%llx transparent hash=0x%llx",
        static_cast<unsigned long long>(opaqKey.Hash()),
        static_cast<unsigned long long>(transKey.Hash()));
    assert(opaqKey.Hash() != transKey.Hash() && "PipelineKey: opaque != transparent");

    // Constant-Buffer-Daten
    const auto& cbData = matSys.GetCBData(opaqueMat);
    Debug::Log("main.cpp: Opaque CB: %zu bytes", cbData.size());

    // Material-Instance erstellen und Parameter überschreiben
    MaterialHandle redInstance = matSys.CreateInstance(opaqueMat, "RedVariant");
    matSys.SetVec4(redInstance, "albedo", Vec4{1,0.1f,0.1f,1});
    const auto& redCB = matSys.GetCBData(redInstance);
    Debug::Log("main.cpp: RedVariant CB: %zu bytes, albedo[0]=%.2f",
        redCB.size(),
        redCB.size() >= 4u ? *reinterpret_cast<const float*>(redCB.data()) : 0.f);

    // --- Szene ---
    BuildScene(scene, opaqueMat, transparentMat);
    scene.PropagateTransforms();
    UpdateBounds(world);
    Debug::Log("main.cpp: Scene: %zu entities", world.EntityCount());

    // --- RenderWorld ---
    RenderWorld renderWorld;

    // Kamera-Setup
    const Vec3  camPos{0, 3, -10};
    const Vec3  camTarget{0, 0, 0};
    const Mat4  viewMat  = Mat4::LookAtRH(camPos, camTarget, Vec3::Up());
    const Mat4  projMat  = Mat4::PerspectiveFovRH(60.f*math::DEG_TO_RAD, 16.f/9.f, 0.1f, 100.f);
    const Mat4  viewProj = projMat * viewMat;

    // --- 2 Frames simulieren ---
    auto cmdList = device->CreateCommandList(QueueType::Graphics);

    for (uint32_t frame = 0; frame < 2; ++frame)
    {
        Debug::Log("main.cpp: ===== Frame %u =====", frame);
        device->BeginFrame();

        // Extract: ECS → SceneSnapshot → RenderWorld
        renderer::SceneSnapshot sceneSnap;
        renderer::ECSExtractor::Extract(world, sceneSnap);
        renderWorld.Extract(sceneSnap, matSys);
        Debug::Log("main.cpp: Extracted %u proxies, %u lights",
            renderWorld.TotalProxyCount(),
            static_cast<uint32_t>(renderWorld.GetLights().size()));

        // Cull + DrawList
        renderWorld.BuildDrawLists(viewMat, viewProj, 0.1f, 100.f, matSys);
        const RenderQueue& q = renderWorld.GetQueue();
        Debug::Log("main.cpp: DrawLists - opaque=%zu transparent=%zu shadow=%zu particles=%zu",
            q.opaque.Size(), q.transparent.Size(),
            q.shadow.Size(), q.particles.Size());

        // SortKey-Verifikation: Opaque-Liste muss sortiert sein
        {
            bool sortOk = true;
            for (size_t i = 1; i < q.opaque.items.size(); ++i)
                if (q.opaque.items[i].sortKey < q.opaque.items[i-1].sortKey)
                    { sortOk = false; break; }
            Debug::Log("main.cpp: Opaque sort order: %s", sortOk ? "OK" : "FAIL");

            bool transSortOk = true;
            for (size_t i = 1; i < q.transparent.items.size(); ++i)
                if (q.transparent.items[i].sortKey < q.transparent.items[i-1].sortKey)
                    { transSortOk = false; break; }
            Debug::Log("main.cpp: Transparent sort order (back-to-front): %s",
                transSortOk ? "OK" : "FAIL");
        }

        // RenderGraph aufbauen
        RenderGraph rg;
        FramePipelineBuilder::BuildParams p;
        p.viewportWidth  = 1280; p.viewportHeight = 720;
        p.shadowMapSize  = 2048;
        p.bloomWidth     = 640; p.bloomHeight = 360;
        p.backbufferRT   = swapchain->GetBackbufferRenderTarget(frame % 2);
        p.backbufferTex  = swapchain->GetBackbufferTexture(frame % 2);

        // Echte Execute-Callbacks mit DrawList-Zugriff
        FramePipelineCallbacks cb;
        cb.onShadowPass = [&](const RGExecContext& ctx) {
            Debug::Log("main.cpp: ShadowPass execute - %zu items", q.shadow.Size());
            for (size_t i = 0; i < q.shadow.items.size(); ++i)
                ctx.cmd->DrawIndexed(36u, 1u, 0u, 0, 0u);
        };
        cb.onOpaquePass = [&](const RGExecContext& ctx) {
            Debug::Log("main.cpp: OpaquePass execute - %zu items", q.opaque.Size());
            for (const auto& item : q.opaque.items)
            {
                ctx.cmd->SetConstantBuffer(0u, BufferHandle::Make(item.cbOffset+1u,1u),
                    ShaderStageMask::Vertex);
                ctx.cmd->DrawIndexed(36u, item.instanceCount, 0u, 0, item.firstInstance);
            }
        };
        cb.onTransparentPass = [&](const RGExecContext& ctx) {
            Debug::Log("main.cpp: TransparentPass execute - %zu items", q.transparent.Size());
            for (size_t i = 0; i < q.transparent.items.size(); ++i)
                ctx.cmd->DrawIndexed(36u, 1u, 0u, 0, 0u);
        };
        cb.onParticlesPass = [&](const RGExecContext&) {
            Debug::Log("main.cpp: ParticlesPass execute - %zu items", q.particles.Size());
        };
        cb.onPresent = [&](const RGExecContext&) {
            Debug::Log("main.cpp: PresentPass execute");
        };

        FramePipelineResources res = FramePipelineBuilder::Build(rg, p, cb);

        // Transiente Ressourcen allozieren
        auto alloc = [&](RGResourceID id, const char* nm, uint32_t w, uint32_t h, Format fmt) {
            if (id==RG_INVALID_RESOURCE) return;
            if (rg.GetResources()[id].lifetime!=RGResourceLifetime::Transient) return;
            RenderTargetDesc d; d.width=w; d.height=h; d.colorFormat=fmt; d.debugName=nm;
            RenderTargetHandle rt = device->CreateRenderTarget(d);
            rg.SetTransientRenderTarget(id, rt, device->GetRenderTargetColorTexture(rt));
        };
        alloc(res.shadowMap,      "ShadowMap",     2048,2048,Format::D32_FLOAT);
        alloc(res.depthBuffer,    "MainDepth",     1280,720, Format::D24_UNORM_S8_UINT);
        alloc(res.hdrSceneColor,  "HDRSceneColor", 1280,720, Format::RGBA16_FLOAT);
        alloc(res.bloomExtracted, "BloomExtracted",640, 360, Format::RGBA16_FLOAT);
        alloc(res.bloomBlurH,     "BloomBlurH",    640, 360, Format::RGBA16_FLOAT);
        alloc(res.bloomBlurV,     "BloomBlurV",    640, 360, Format::RGBA16_FLOAT);
        alloc(res.tonemapped,     "Tonemapped",    1280,720, Format::RGBA8_UNORM_SRGB);
        alloc(res.uiOverlay,      "UIOverlay",     1280,720, Format::RGBA8_UNORM_SRGB);

        if (!rg.Compile())
        { Debug::LogError("main.cpp: RenderGraph compile failed"); break; }

        // Topologische Pass-Reihenfolge ausgeben
        Debug::Log("main.cpp: Pass order:");
        for (RGPassID pid : rg.GetSortedPasses())
            Debug::Log("main.cpp:   [%u] %s", pid, rg.GetPasses()[pid].debugName.c_str());

        // Execute
        cmdList->Begin();
        RGExecContext ctx; ctx.device=device.get(); ctx.cmd=cmdList.get();
        rg.Execute(ctx);
        cmdList->End();
        cmdList->Submit();

        device->EndFrame();
        swapchain->Present(true);
    }

    // --- JSON-Serializer (korrektes Format) ---
    {
        serialization::SceneSerializer ser(world);
        ser.RegisterDefaultHandlers();
        std::string json = ser.SerializeToJson("DemoScene");
        Debug::Log("main.cpp: JSON output (%zu chars):", json.size());
        // Nur ersten 800 Zeichen ausgeben
        if (json.size() > 800) json.resize(800);
        std::puts(json.c_str());
    }

    // =========================================================================
    // PipelineCache - deterministisches Caching von Pipeline-Objekten
    // =========================================================================
    {
        Debug::Log("main.cpp: ===== PipelineCache demo =====");
        renderer::PipelineCache pipelineCache;

        // Simuliert Backend-seitiges Pipeline-Erstellen
        auto factory = [&](const renderer::PipelineKey& k) -> PipelineHandle {
            Debug::Log("main.cpp: PipelineCache MISS - creating pipeline hash=0x%llx",
                static_cast<unsigned long long>(k.Hash()));
            return device->CreatePipeline(renderer::PipelineDesc{ .debugName="CachedPipeline" });
        };

        // Zwei Materialien teilen denselben PipelineKey wenn alle State-Felder gleich sind
        const renderer::PipelineKey key1 = matSys.BuildPipelineKey(opaqueMat);
        const renderer::PipelineKey key2 = matSys.BuildPipelineKey(opaqueMat); // identisch

        PipelineHandle p1 = pipelineCache.GetOrCreate(key1, factory);
        PipelineHandle p2 = pipelineCache.GetOrCreate(key2, factory); // Cache-Hit
        Debug::Log("main.cpp: p1=%u p2=%u same=%d hits=%zu misses=%zu",
            p1.value, p2.value,
            (p1 == p2 ? 1 : 0),
            pipelineCache.HitCount(),
            pipelineCache.MissCount());
        assert(p1 == p2 && "PipelineCache: same key must return same handle");

        // Transparentes Material hat anderen Key → Cache-Miss
        const renderer::PipelineKey transKey = matSys.BuildPipelineKey(transparentMat);
        PipelineHandle pTrans = pipelineCache.GetOrCreate(transKey, factory);
        Debug::Log("main.cpp: transparent pipeline=%u (different from opaque=%d)",
            pTrans.value, (pTrans != p1 ? 1 : 0));
        assert(pTrans != p1 && "PipelineCache: different key must yield different pipeline");

        Debug::Log("main.cpp: PipelineCache final - size=%zu hits=%zu misses=%zu",
            pipelineCache.Size(), pipelineCache.HitCount(), pipelineCache.MissCount());
    }

    // =========================================================================
    // TransformSystem (BFS) - ersetzt naive Rekursion in Scene::PropagateTransforms
    // =========================================================================
    {
        Debug::Log("main.cpp: ===== TransformSystem (BFS) demo =====");
        TransformSystem transformSys;

        // Erste Ausführung: baut sortierte Entitäten-Liste auf
        transformSys.Update(world);
        Debug::Log("main.cpp: TransformSystem: %zu entities in topo order, %u updated",
            transformSys.SortedEntityCount(), transformSys.UpdateCount());

        // Zweite Ausführung ohne Änderungen: kein Rebuild, 0 Updates (alles clean)
        transformSys.Update(world);
        Debug::Log("main.cpp: TransformSystem second pass: %u updated (expect 0)",
            transformSys.UpdateCount());

        // Nach dirty-Markierung: nur betroffene Entities
        EntityID parentCube = scene.FindByName("ParentCube");
        if (parentCube.IsValid())
        {
            world.Get<TransformComponent>(parentCube)->dirty = true;
            transformSys.Update(world);
            Debug::Log("main.cpp: TransformSystem after dirty mark: %u updated",
                transformSys.UpdateCount());
        }
    }

    // =========================================================================
    // ShaderBindingModel - explizite Slot-Verifikation
    // =========================================================================
    {
        Debug::Log("main.cpp: ===== ShaderBindingModel =====");
        using namespace renderer;
        Debug::Log("main.cpp: CB0(PerFrame)=%u  CB1(PerObject)=%u  "
            "CB2(PerMaterial)=%u  CB3(PerPass)=%u",
            CBSlots::PerFrame, CBSlots::PerObject,
            CBSlots::PerMaterial, CBSlots::PerPass);
        Debug::Log("main.cpp: t0=Albedo t1=Normal t2=ORM t4=ShadowMap s3=ShadowPCF");
        Debug::Log("main.cpp: PerFrameConstants size=%zu bytes (expect multiple of 16)",
            renderer::PerFrameConstantsSize);
        Debug::Log("main.cpp: PerObjectConstants size=%zu bytes",
            sizeof(renderer::PerObjectConstants));
        static_assert(renderer::PerFrameConstantsSize % 16u == 0u,
            "PerFrameConstants alignment violated");
        static_assert(sizeof(renderer::PerObjectConstants) % 16u == 0u,
            "PerObjectConstants alignment violated");
    }

    // =========================================================================
    // ResourceAliaser - transiente Ressourcen mit disjunkten Lifetimes
    // =========================================================================
    {
        Debug::Log("main.cpp: ===== ResourceAliaser demo =====");

        // Erzeuge einen frischen RenderGraph für Aliasing-Analyse
        RenderGraph aliasRG;
        FramePipelineBuilder::BuildParams p2;
        p2.viewportWidth=1280; p2.viewportHeight=720;
        p2.shadowMapSize=2048; p2.bloomWidth=640; p2.bloomHeight=360;
        p2.backbufferRT = swapchain->GetBackbufferRenderTarget(0u);
        p2.backbufferTex = swapchain->GetBackbufferTexture(0u);
        FramePipelineCallbacks cb2;
        FramePipelineResources fpr = FramePipelineBuilder::Build(aliasRG, p2, cb2);

        // Transiente Ressourcen allozieren damit Compile() funktioniert
        auto allocForAlias = [&](RGResourceID id, uint32_t w, uint32_t h,
                                  renderer::Format fmt, const char* nm) {
            if (id == RG_INVALID_RESOURCE) return;
            if (aliasRG.GetResources()[id].lifetime != RGResourceLifetime::Transient) return;
            renderer::RenderTargetDesc d;
            d.width=w; d.height=h; d.colorFormat=fmt; d.debugName=nm;
            RenderTargetHandle rt = device->CreateRenderTarget(d);
            aliasRG.SetTransientRenderTarget(id, rt, device->GetRenderTargetColorTexture(rt));
        };
        allocForAlias(fpr.shadowMap,      2048,2048, renderer::Format::D32_FLOAT,           "ShadowMap");
        allocForAlias(fpr.depthBuffer,    1280,720,  renderer::Format::D24_UNORM_S8_UINT,   "MainDepth");
        allocForAlias(fpr.hdrSceneColor,  1280,720,  renderer::Format::RGBA16_FLOAT,        "HDRSceneColor");
        allocForAlias(fpr.bloomExtracted, 640, 360,  renderer::Format::RGBA16_FLOAT,        "BloomExtracted");
        allocForAlias(fpr.bloomBlurH,     640, 360,  renderer::Format::RGBA16_FLOAT,        "BloomBlurH");
        allocForAlias(fpr.bloomBlurV,     640, 360,  renderer::Format::RGBA16_FLOAT,        "BloomBlurV");
        allocForAlias(fpr.tonemapped,     1280,720,  renderer::Format::RGBA8_UNORM_SRGB,    "Tonemapped");
        allocForAlias(fpr.uiOverlay,      1280,720,  renderer::Format::RGBA8_UNORM_SRGB,    "UIOverlay");

        if (aliasRG.Compile())
        {
            rendergraph::ResourceAliaser aliaser;
            auto aliasPlan = aliaser.Analyze(aliasRG,
                [](const rendergraph::RGResourceDesc& r) -> size_t {
                    // Größen-Schätzung: width * height * 4 bytes (Näherung)
                    return static_cast<size_t>(r.width) * r.height * 4u;
                });

            Debug::Log("main.cpp: AliasGroups=%zu nonAliased=%zu saved=%u",
                aliasPlan.groups.size(),
                aliasPlan.nonAliased.size(),
                aliasPlan.aliasingCount);
        }
    }

    // =========================================================================
    // EntityCommandBuffer - deferred structural changes during iteration
    // =========================================================================
    {
        Debug::Log("main.cpp: ===== EntityCommandBuffer demo =====");
        ecs::EntityCommandBuffer ecb;
        uint32_t markedCount = 0u;

        // Iteration: alle Cubes mit negativer X-Position als "out of bounds" markieren
        world.View<TransformComponent, MeshComponent>(
            [&](EntityID id, TransformComponent& tc, MeshComponent&) {
                if (tc.localPosition.x < -5.f)
                {
                    // Während Iteration NICHT direkt modifizieren - ECB puffert
                    ecb.RemoveComponent<MeshComponent>(id);
                    ++markedCount;
                }
            });

        Debug::Log("main.cpp: ECB pending ops: %zu (marked %u for mesh removal)",
            ecb.PendingCount(), markedCount);

        // Nach Iteration committen - jetzt werden die Änderungen angewendet
        ecb.Commit(world);

        // Verifizieren
        uint32_t meshCount = 0u;
        world.View<MeshComponent>([&](EntityID, MeshComponent&) { ++meshCount; });
        Debug::Log("main.cpp: After ECB commit: %u entities still have MeshComponent "
            "(was %u before)", meshCount, meshCount + markedCount);

        // Ein neues Entity via ECB erzeugen
        ecb.CreateEntity([&](EntityID newId) {
            world.Add<NameComponent>(newId, std::string("ECB_Created"));
            world.Add<TransformComponent>(newId);
            Debug::Log("main.cpp: ECB created entity %u", newId.value);
        });
        ecb.Commit(world);
        Debug::Log("main.cpp: EntityCount after ECB create: %zu", world.EntityCount());
    }

    // =========================================================================
    // QueryCache - gecachte Archetype-Queries
    // =========================================================================
    {
        Debug::Log("main.cpp: ===== QueryCache demo =====");
        ecs::QueryCache qcache;

        ComponentSignature sig;
        sig.Set(ComponentTypeID<TransformComponent>::value);
        sig.Set(ComponentTypeID<NameComponent>::value);

        uint32_t hitCount = 0u;

        // Erste Abfrage: baut Cache auf
        qcache.Query(world, sig, [&](ecs::Archetype& arch) {
            hitCount += arch.EntityCount();
        });
        Debug::Log("main.cpp: QueryCache first pass: %u entities matched, "
            "%zu cache entries", hitCount, qcache.EntryCount());

        // Zweite Abfrage: Cache-Hit (StructureVersion unverändert)
        hitCount = 0u;
        qcache.Query(world, sig, [&](ecs::Archetype& arch) {
            hitCount += arch.EntityCount();
        });
        Debug::Log("main.cpp: QueryCache second pass (cache hit): %u entities matched",
            hitCount);

        // Nach Add() ändert sich StructureVersion → Cache wird invalidiert
        EntityID cacheTest = world.CreateEntity();
        world.Add<TransformComponent>(cacheTest);
        world.Add<NameComponent>(cacheTest, std::string("CacheTestEntity"));

        hitCount = 0u;
        qcache.Query(world, sig, [&](ecs::Archetype& arch) {
            hitCount += arch.EntityCount();
        });
        Debug::Log("main.cpp: QueryCache after Add (cache miss + rebuild): "
            "%u entities matched", hitCount);
    }

    // =========================================================================
    // TaskGraph - frame-level task dependencies
    // =========================================================================
    {
        Debug::Log("main.cpp: ===== TaskGraph demo =====");
        jobs::TaskGraph frameGraph;

        // Typische Frame-Topologie
        auto tInput     = frameGraph.Add("Input",      {},                  []{ Debug::Log("tasks: Input");     });
        auto tPhysics   = frameGraph.Add("Physics",    {tInput},            []{ Debug::Log("tasks: Physics");   });
        auto tAnimation = frameGraph.Add("Animation",  {tInput},            []{ Debug::Log("tasks: Animation"); });
        auto tTransform = frameGraph.Add("Transform",  {tPhysics,tAnimation},[]{ Debug::Log("tasks: Transform");});
        auto tExtract   = frameGraph.Add("Extract",    {tTransform},        []{ Debug::Log("tasks: Extract");   });
        auto tCull      = frameGraph.Add("Cull",       {tExtract},          []{ Debug::Log("tasks: Cull");      });
        auto tRender    = frameGraph.Add("Render",     {tCull},             []{ Debug::Log("tasks: Render");    });
        (void)tRender;

        bool built = frameGraph.Build();
        Debug::Log("main.cpp: TaskGraph built=%d, %zu tasks, %zu levels",
            built, frameGraph.TaskCount(), frameGraph.LevelCount());

        // Level-Ausgabe
        for (size_t lvl = 0; lvl < frameGraph.LevelCount(); ++lvl)
            Debug::Log("main.cpp:   Level %zu = kann parallel ausgeführt werden", lvl);

        frameGraph.Execute(jobs);
        Debug::Log("main.cpp: TaskGraph executed");

        // Zyklus-Test: A → B → A (Self-referential via Handle-Vorwärtsplanung)
        // Handle-IDs sind sequentiell - wir können futureHandle=0 als A-Dep nutzen
        // bevor A existiert, um B→A zu simulieren und damit A→B→A = Zyklus zu erzeugen.
        {
            jobs::TaskGraph acyclic;
            auto a0 = acyclic.Add("A", {},    []{});
            auto b0 = acyclic.Add("B", {a0},  []{});
            acyclic.Add("C", {b0}, []{});
            bool ok = acyclic.Build();
            Debug::Log("main.cpp: Acyclic graph build=%d (expected 1)", ok);
        }
        {
            // Echter Selbst-Zyklus: Task 0 hängt von Task 0 ab
            jobs::TaskGraph selfCycle;
            constexpr jobs::TaskHandle SELF = 0u;
            selfCycle.Add("A", std::vector<jobs::TaskHandle>{SELF}, []{});
            bool cycleDetected = !selfCycle.Build();
            Debug::Log("main.cpp: Self-cycle detected=%d (expected 1)", cycleDetected);
        }
    }

    // --- Shutdown ---
    jobs.Shutdown();
    device->WaitIdle();
    swapchain.reset();
    device->Shutdown();
    Debug::Log("main.cpp: Clean exit");
    return 0;
}
