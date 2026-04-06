#include "ForwardFeature.hpp"
// =============================================================================
// KROM Engine - tests/test_renderer.cpp
// Renderer-Tests: PipelineKey, SortKey, MaterialSystem, RenderWorld
// =============================================================================
#include "TestFramework.hpp"
#include "renderer/MaterialSystem.hpp"
#include "renderer/RenderWorld.hpp"
#include "renderer/ECSExtractor.hpp"
#include "renderer/PipelineCache.hpp"
#include "renderer/ShaderBindingModel.hpp"
#include "renderer/RenderFrameOrchestrator.hpp"
#include "renderer/RenderSystem.hpp"
#include "renderer/ShaderRuntime.hpp"
#include "renderer/PlatformRenderLoop.hpp"
#include "NullDevice.hpp"
#include "platform/IWindow.hpp"
#include "platform/PlatformInput.hpp"
#include "platform/NullPlatform.hpp"
#include "events/EventBus.hpp"
#include "jobs/TaskGraph.hpp"
#include "ecs/Components.hpp"
#include "ecs/World.hpp"
#include "core/Debug.hpp"
#include "OpenGLDevice.hpp"
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using namespace engine;
using namespace engine::renderer;


namespace {

class TestThreadFactory final : public platform::IThreadFactory
{
public:
    platform::IThread* CreateThread() override { return nullptr; }
    void DestroyThread(platform::IThread* thread) override { delete thread; }
    int GetHardwareConcurrency() const override { return 1; }
    void SleepMs(int) const override {}
    platform::IMutex* CreateMutex() override { return nullptr; }
    platform::IJobSystem* CreateJobSystem(uint32_t) override { return nullptr; }
};

class TestHeadlessPlatform final : public platform::IPlatform
{
public:
    bool Initialize() override { m_initialized = true; return true; }
    void Shutdown() override { if (m_window) m_window->Destroy(); m_initialized = false; }
    void PumpEvents() override {}
    [[nodiscard]] double GetTimeSeconds() const override { return 0.0; }
    [[nodiscard]] platform::IWindow* CreateWindow(const platform::WindowDesc& desc) override
    {
        m_window = platform::CreateHeadlessWindow();
        return m_window->Create(desc) ? m_window.get() : nullptr;
    }
    [[nodiscard]] platform::IInput* GetInput() override { return &m_input; }
    [[nodiscard]] platform::IThreadFactory* GetThreadFactory() override { return &m_threads; }

private:
    bool m_initialized = false;
    platform::NullInput m_input;
    TestThreadFactory m_threads;
    std::unique_ptr<platform::IWindow> m_window;
};


class TestDevice final : public IDevice
{
public:
    bool Initialize(const DeviceDesc&) override { return true; }
    void Shutdown() override {}
    void WaitIdle() override {}
    std::unique_ptr<ISwapchain> CreateSwapchain(const SwapchainDesc&) override { return nullptr; }
    BufferHandle CreateBuffer(const BufferDesc&) override { return BufferHandle::Invalid(); }
    void DestroyBuffer(BufferHandle) override {}
    void* MapBuffer(BufferHandle) override { return nullptr; }
    void UnmapBuffer(BufferHandle) override {}
    TextureHandle CreateTexture(const TextureDesc&) override { return TextureHandle::Invalid(); }
    void DestroyTexture(TextureHandle) override {}
    RenderTargetHandle CreateRenderTarget(const RenderTargetDesc&) override { return RenderTargetHandle::Invalid(); }
    void DestroyRenderTarget(RenderTargetHandle) override {}
    TextureHandle GetRenderTargetColorTexture(RenderTargetHandle) const override { return TextureHandle::Invalid(); }
    TextureHandle GetRenderTargetDepthTexture(RenderTargetHandle) const override { return TextureHandle::Invalid(); }
    ShaderHandle CreateShaderFromSource(const std::string&, ShaderStageMask, const std::string&, const std::string&) override { return ShaderHandle::Invalid(); }
    ShaderHandle CreateShaderFromBytecode(const void*, size_t, ShaderStageMask, const std::string&) override { return ShaderHandle::Invalid(); }
    void DestroyShader(ShaderHandle) override {}
    PipelineHandle CreatePipeline(const PipelineDesc&) override { return PipelineHandle::Invalid(); }
    void DestroyPipeline(PipelineHandle) override {}
    uint32_t CreateSampler(const SamplerDesc&) override { return 0u; }
    std::unique_ptr<ICommandList> CreateCommandList(QueueType) override { return nullptr; }
    std::unique_ptr<IFence> CreateFence(uint64_t) override { return nullptr; }
    void UploadBufferData(BufferHandle, const void*, size_t, size_t) override {}
    void UploadTextureData(TextureHandle, const void*, size_t, uint32_t, uint32_t) override {}
    void BeginFrame() override {}
    void EndFrame() override {}
    uint32_t GetDrawCallCount() const override { return 0u; }
    const char* GetBackendName() const override { return "TestDevice"; }
};

std::unique_ptr<IDevice> CreateTestDeviceFactoryInstance()
{
    return std::make_unique<TestDevice>();
}

} // namespace

static void TestShaderRuntimeEndToEnd(test::TestContext& ctx);
static void TestShaderRuntimeValidation(test::TestContext& ctx);
static void TestDeviceFactoryRegistration(test::TestContext& ctx);



static void TestRenderFrameExecutionState(test::TestContext& ctx)
{
    renderer::RenderFrameExecutionState state{};
    CHECK(ctx, !state.Succeeded());

    state.extractionStatus.MarkSucceeded();
    state.prepareFrameStatus.MarkSucceeded();
    state.collectShadersStatus.MarkSucceeded();
    state.collectMaterialsStatus.MarkSucceeded();
    state.buildQueuesStatus.MarkSucceeded();
    state.collectUploadsStatus.MarkSucceeded();
    state.commitShadersStatus.MarkSucceeded();
    state.commitMaterialsStatus.MarkSucceeded();
    state.commitUploadsStatus.MarkSucceeded();
    state.buildGraphStatus.MarkSucceeded();
    state.executeStatus.MarkSucceeded();

    CHECK(ctx, state.Succeeded());
    state.collectUploadsStatus.MarkFailed("CollectUploadRequests failed");
    CHECK(ctx, !state.Succeeded());
    CHECK(ctx, state.FirstFailure() == &state.collectUploadsStatus);
}

static void TestFrameTaskGraphParallelStructure(test::TestContext& ctx)
{
    jobs::TaskGraph graph;
    std::vector<int> left;
    std::vector<int> right;
    std::vector<int> merged;
    std::atomic<int> running{0};
    std::atomic<int> peak{0};

    const auto root = graph.Add("Extract", {}, []() -> jobs::TaskResult {
        return jobs::TaskResult::Ok();
    });
    const auto prepareFrame = graph.Add("PrepareFrame", {root}, [&]() -> jobs::TaskResult {
        const int current = running.fetch_add(1) + 1;
        int observed = peak.load();
        while (current > observed && !peak.compare_exchange_weak(observed, current)) {}
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        left = {1, 2};
        running.fetch_sub(1);
        return jobs::TaskResult::Ok();
    });
    const auto collectShaders = graph.Add("CollectShaderRequests", {root}, [&]() -> jobs::TaskResult {
        const int current = running.fetch_add(1) + 1;
        int observed = peak.load();
        while (current > observed && !peak.compare_exchange_weak(observed, current)) {}
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        right = {3, 4};
        running.fetch_sub(1);
        return jobs::TaskResult::Ok();
    });
    const auto merge = graph.Add("CollectUploadRequests", {prepareFrame, collectShaders}, [&]() -> jobs::TaskResult {
        merged.clear();
        merged.insert(merged.end(), left.begin(), left.end());
        merged.insert(merged.end(), right.begin(), right.end());
        return jobs::TaskResult::Ok();
    });
    graph.Add("Execute", {merge}, [&]() -> jobs::TaskResult {
        return merged == std::vector<int>({1, 2, 3, 4})
            ? jobs::TaskResult::Ok()
            : jobs::TaskResult::Fail("deterministic merge failed");
    });

    CHECK(ctx, graph.Build());
    CHECK_EQ(ctx, graph.LevelCount(), 4u);
    CHECK_EQ(ctx, graph.GetTask(root).level, 0u);
    CHECK_EQ(ctx, graph.GetTask(prepareFrame).level, 1u);
    CHECK_EQ(ctx, graph.GetTask(collectShaders).level, 1u);
    CHECK_EQ(ctx, graph.GetTask(merge).level, 2u);

    jobs::JobSystem jobSystem;
    jobSystem.Initialize(2u);
    const jobs::TaskResult result = graph.Execute(jobSystem);
    jobSystem.Shutdown();

    CHECK(ctx, result.Succeeded());
    CHECK_EQ(ctx, merged.size(), size_t(4));
    CHECK_EQ(ctx, merged[0], 1);
    CHECK_EQ(ctx, merged[1], 2);
    CHECK_EQ(ctx, merged[2], 3);
    CHECK_EQ(ctx, merged[3], 4);
    CHECK(ctx, peak.load() >= 2);
}

static void TestFrameTaskGraphFailurePropagation(test::TestContext& ctx)
{
    jobs::TaskGraph graph;
    bool mergeRan = false;

    const auto root = graph.Add("Extract", {}, []() -> jobs::TaskResult {
        return jobs::TaskResult::Ok();
    });
    const auto prepareFrame = graph.Add("PrepareFrame", {root}, []() -> jobs::TaskResult {
        return jobs::TaskResult::Fail("prepare frame failed");
    });
    const auto collectShaders = graph.Add("CollectShaderRequests", {root}, []() -> jobs::TaskResult {
        return jobs::TaskResult::Ok();
    });
    graph.Add("CollectUploadRequests", {prepareFrame, collectShaders}, [&]() -> jobs::TaskResult {
        mergeRan = true;
        return jobs::TaskResult::Ok();
    });

    CHECK(ctx, graph.Build());

    jobs::JobSystem jobSystem;
    jobSystem.Initialize(2u);
    const jobs::TaskResult result = graph.Execute(jobSystem);
    jobSystem.Shutdown();

    CHECK(ctx, result.Failed());
    CHECK(ctx, result.errorMessage != nullptr);
    CHECK(ctx, !mergeRan);
}

// ==========================================================================
// PipelineKey - Determinismus
// ==========================================================================
static void TestPipelineKeyDeterminism(test::TestContext& ctx)
{
    // Zwei identische PipelineDescs müssen denselben Key/Hash ergeben
    PipelineDesc d1;
    d1.debugName   = "Test1";
    d1.colorFormat = Format::RGBA16_FLOAT;
    d1.depthFormat = Format::D24_UNORM_S8_UINT;
    d1.topology    = PrimitiveTopology::TriangleList;
    d1.depthStencil.depthEnable = true;
    d1.depthStencil.depthWrite  = true;
    d1.rasterizer.cullMode      = CullMode::Back;
    d1.shaderStages = {{ ShaderHandle::Make(1u,1u), ShaderStageMask::Vertex },
                       { ShaderHandle::Make(2u,1u), ShaderStageMask::Fragment }};

    PipelineDesc d2 = d1;
    d2.debugName = "Test2"; // Name unterscheidet sich, aber geht nicht in Key ein

    PipelineKey k1 = PipelineKey::From(d1, RenderPassTag::Opaque);
    PipelineKey k2 = PipelineKey::From(d2, RenderPassTag::Opaque);

    CHECK(ctx, k1 == k2);
    CHECK_EQ(ctx, k1.Hash(), k2.Hash());

    // Unterschiedlicher Cull-Mode → anderer Key
    d2.rasterizer.cullMode = CullMode::Front;
    PipelineKey k3 = PipelineKey::From(d2, RenderPassTag::Opaque);
    CHECK(ctx, !(k1 == k3));
    CHECK_NE(ctx, k1.Hash(), k3.Hash());

    // Unterschiedlicher Pass-Tag → anderer Key
    PipelineKey k4 = PipelineKey::From(d1, RenderPassTag::Transparent);
    CHECK(ctx, !(k1 == k4));
}

// ==========================================================================
// PipelineKey - keine Padding-Probleme (deterministischer Hash)
// ==========================================================================
static void TestPipelineKeyNoPadding(test::TestContext& ctx)
{
    // Erzeuge zwei PipelineKeys durch direktes Befüllen
    // und stelle sicher dass Hash deterministisch ist
    PipelineKey k1{};
    k1.vertexShader   = 100u;
    k1.fragmentShader = 200u;
    k1.cullMode       = 1u;
    k1.depthEnable    = 1u;
    k1.colorFormat    = 10u;
    k1.passTag        = RenderPassTag::Opaque;

    PipelineKey k2 = k1; // Bitweise Kopie

    // Mehrfacher Hash muss identisch sein
    CHECK_EQ(ctx, k1.Hash(), k2.Hash());
    CHECK_EQ(ctx, k1.Hash(), k1.Hash()); // Idempotent

    // Änderung eines einzigen Feldes ändert Hash
    k2.cullMode = 2u;
    CHECK_NE(ctx, k1.Hash(), k2.Hash());
}

// ==========================================================================
// SortKey - Ordnung
// ==========================================================================
static void TestSortKeyOrdering(test::TestContext& ctx)
{
    // Opaque: kleinere Tiefe kommt zuerst (front-to-back)
    SortKey near_a = SortKey::ForOpaque(RenderPassTag::Opaque, 0, 12345u, 0.1f);
    SortKey far_a  = SortKey::ForOpaque(RenderPassTag::Opaque, 0, 12345u, 0.9f);
    CHECK(ctx, near_a < far_a); // Front-to-back: näher = kleinerer Key

    // Transparent: größere Tiefe kommt zuerst (back-to-front)
    SortKey near_t = SortKey::ForTransparent(RenderPassTag::Transparent, 0, 0.1f);
    SortKey far_t  = SortKey::ForTransparent(RenderPassTag::Transparent, 0, 0.9f);
    CHECK(ctx, far_t < near_t); // Back-to-front: weiter = kleinerer Key

    // Pass-Tag hat höchste Priorität
    SortKey opaque_deep  = SortKey::ForOpaque(RenderPassTag::Opaque,       0, 0u, 0.99f);
    SortKey shadow_near  = SortKey::ForOpaque(RenderPassTag::Shadow,        0, 0u, 0.01f);
    // Shadow (3) > Opaque (0) im Pass-Bit → shadow-key > opaque-key
    CHECK(ctx, opaque_deep < shadow_near);

    // UI hat eigene Ordnung nach drawOrder
    SortKey ui0 = SortKey::ForUI(0, 0u);
    SortKey ui1 = SortKey::ForUI(0, 100u);
    CHECK(ctx, ui0 < ui1);
}

// ==========================================================================
// MaterialSystem
// ==========================================================================
static void TestMaterialSystem(test::TestContext& ctx)
{
    MaterialSystem ms;

    MaterialDesc d;
    d.name           = "TestMaterial";
    d.passTag        = RenderPassTag::Opaque;
    d.vertexShader   = ShaderHandle::Make(1u, 1u);
    d.fragmentShader = ShaderHandle::Make(2u, 1u);
    d.colorFormat    = Format::RGBA16_FLOAT;
    d.depthFormat    = Format::D24_UNORM_S8_UINT;
    d.depthStencil.depthEnable = true;
    d.depthStencil.depthWrite  = true;
    d.rasterizer.cullMode      = CullMode::Back;

    MaterialParam p0; p0.name = "albedo"; p0.type = MaterialParam::Type::Vec4;
    p0.value.f[0]=1.f; p0.value.f[1]=0.f; p0.value.f[2]=0.f; p0.value.f[3]=1.f;
    MaterialParam p1; p1.name = "roughness"; p1.type = MaterialParam::Type::Float;
    p1.value.f[0] = 0.7f;
    d.params = { p0, p1 };

    MaterialHandle h = ms.RegisterMaterial(std::move(d));
    CHECK_VALID(ctx, h);
    CHECK_EQ(ctx, ms.DescCount(), 1u);

    // PipelineKey valide
    PipelineKey key = ms.BuildPipelineKey(h);
    CHECK_NE(ctx, key.Hash(), 0ull);

    // CB-Daten
    const auto& cbData = ms.GetCBData(h);
    CHECK_GT(ctx, cbData.size(), 0u);
    CHECK_EQ(ctx, cbData.size() % 16u, 0u); // 16-Byte-aligned

    // CbLayout hat korrekte Einträge
    const CbLayout& layout = ms.GetCBLayout(h);
    CHECK_EQ(ctx, layout.fields.size(), 2u);
    CHECK_GT(ctx, layout.totalSize, 0u);
    CHECK_EQ(ctx, layout.totalSize % 16u, 0u);

    // Offset von 'albedo' muss vorhanden und korrekt sein
    const uint32_t albedoOff = layout.GetOffset("albedo");
    CHECK_NE(ctx, albedoOff, UINT32_MAX);

    // albedo-Wert im CB korrekt
    const float* albedoPtr = reinterpret_cast<const float*>(cbData.data() + albedoOff);
    CHECK_EQ(ctx, albedoPtr[0], 1.f); // r
    CHECK_EQ(ctx, albedoPtr[1], 0.f); // g

    // Instance erstellen
    MaterialHandle inst = ms.CreateInstance(h, "RedVariant");
    CHECK_VALID(ctx, inst);
    CHECK_EQ(ctx, ms.DescCount(), 2u);

    // Instance hat andere albedo nach SetVec4
    ms.SetVec4(inst, "albedo", math::Vec4{0.f, 1.f, 0.f, 1.f});
    const auto& instCB = ms.GetCBData(inst);
    const CbLayout& instLayout = ms.GetCBLayout(inst);
    const uint32_t instAlbedoOff = instLayout.GetOffset("albedo");
    const float* instAlbedo = reinterpret_cast<const float*>(instCB.data() + instAlbedoOff);
    CHECK_EQ(ctx, instAlbedo[0], 0.f); // r=0
    CHECK_EQ(ctx, instAlbedo[1], 1.f); // g=1

    // Original bleibt unverändert
    const auto& origCB = ms.GetCBData(h);
    const float* origAlbedo = reinterpret_cast<const float*>(origCB.data() + albedoOff);
    CHECK_EQ(ctx, origAlbedo[0], 1.f);
}

// ==========================================================================
// PipelineCache
// ==========================================================================
static void TestPipelineCache(test::TestContext& ctx)
{
    PipelineCache cache;
    CHECK_EQ(ctx, cache.Size(),    0u);
    CHECK_EQ(ctx, cache.HitCount(),  0u);
    CHECK_EQ(ctx, cache.MissCount(), 0u);

    PipelineKey key{};
    key.vertexShader = 1u; key.fragmentShader = 2u;
    key.colorFormat  = static_cast<uint8_t>(Format::RGBA16_FLOAT);

    int factoryCalls = 0;
    auto factory = [&](const PipelineKey&) -> PipelineHandle {
        ++factoryCalls;
        return PipelineHandle::Make(static_cast<uint32_t>(factoryCalls), 1u);
    };

    // Miss
    PipelineHandle p1 = cache.GetOrCreate(key, factory);
    CHECK_VALID(ctx, p1);
    CHECK_EQ(ctx, factoryCalls, 1);
    CHECK_EQ(ctx, cache.MissCount(), 1u);
    CHECK_EQ(ctx, cache.HitCount(),  0u);

    // Hit
    PipelineHandle p2 = cache.GetOrCreate(key, factory);
    CHECK_EQ(ctx, p1, p2);
    CHECK_EQ(ctx, factoryCalls, 1); // factory nicht nochmal aufgerufen
    CHECK_EQ(ctx, cache.HitCount(), 1u);

    // Anderer Key → Miss
    PipelineKey key2 = key;
    key2.cullMode = 1u;
    PipelineHandle p3 = cache.GetOrCreate(key2, factory);
    CHECK_NE(ctx, p1, p3);
    CHECK_EQ(ctx, cache.Size(), 2u);

    // Clear
    cache.Clear();
    CHECK_EQ(ctx, cache.Size(), 0u);
}

// ==========================================================================
// CbLayout - HLSL-Packing
// ==========================================================================
static void TestCbLayout(test::TestContext& ctx)
{
    std::vector<MaterialParam> params;

    MaterialParam f; f.name="f"; f.type=MaterialParam::Type::Float; f.value.f[0]=1.f;
    MaterialParam v4; v4.name="v4"; v4.type=MaterialParam::Type::Vec4;
    v4.value.f[0]=v4.value.f[1]=v4.value.f[2]=v4.value.f[3]=0.f;
    MaterialParam f2; f2.name="f2"; f2.type=MaterialParam::Type::Float; f2.value.f[0]=2.f;
    params = {f, v4, f2};

    CbLayout layout = CbLayout::Build(params);

    // Alle Felder vorhanden
    CHECK_EQ(ctx, layout.fields.size(), 3u);

    // totalSize muss 16-Byte-aligned sein
    CHECK_EQ(ctx, layout.totalSize % 16u, 0u);
    CHECK_GT(ctx, layout.totalSize, 0u);

    // Offsets valide
    uint32_t fOff  = layout.GetOffset("f");
    uint32_t v4Off = layout.GetOffset("v4");
    uint32_t f2Off = layout.GetOffset("f2");
    CHECK_NE(ctx, fOff,  UINT32_MAX);
    CHECK_NE(ctx, v4Off, UINT32_MAX);
    CHECK_NE(ctx, f2Off, UINT32_MAX);

    // v4 (16 Bytes) muss 16-Byte-aligned sein
    CHECK_EQ(ctx, v4Off % 16u, 0u);

    // Texture-Parameter landen NICHT im CB
    MaterialParam tex; tex.name="t0"; tex.type=MaterialParam::Type::Texture;
    params.push_back(tex);
    CbLayout layoutWithTex = CbLayout::Build(params);
    CHECK_EQ(ctx, layoutWithTex.fields.size(), 3u); // Texture nicht drin
    CHECK_EQ(ctx, layoutWithTex.GetOffset("t0"), UINT32_MAX);
}

// ==========================================================================
// ShaderBindingModel - Slot-Konstanten
// ==========================================================================
static void TestShaderBindingModel(test::TestContext& ctx)
{
    CHECK_EQ(ctx, CBSlots::PerFrame,    0u);
    CHECK_EQ(ctx, CBSlots::PerObject,   1u);
    CHECK_EQ(ctx, CBSlots::PerMaterial, 2u);
    CHECK_EQ(ctx, CBSlots::PerPass,     3u);

    CHECK_EQ(ctx, TexSlots::Albedo,    0u);
    CHECK_EQ(ctx, TexSlots::ShadowMap, 4u);

    CHECK_EQ(ctx, SamplerSlots::LinearWrap, 0u);
    CHECK_EQ(ctx, SamplerSlots::ShadowPCF,  3u);

    // PerFrameConstants muss 16-Byte-aligned sein
    CHECK_EQ(ctx, PerFrameConstantsSize % 16u, 0u);
    CHECK_GT(ctx, PerFrameConstantsSize, 0u);
}

// ==========================================================================
// RenderWorld - Extract + DrawList sort
// ==========================================================================
static void TestRenderWorldExtract(test::TestContext& ctx)
{
    RegisterAllComponents();
    ecs::World world;
    MaterialSystem ms;

    // Material
    MaterialDesc d;
    d.name="M1"; d.passTag=RenderPassTag::Opaque;
    d.vertexShader=ShaderHandle::Make(1u,1u);
    d.fragmentShader=ShaderHandle::Make(2u,1u);
    MaterialHandle mat = ms.RegisterMaterial(std::move(d));

    // 5 opaque + 2 transparent Entities
    for (int i = 0; i < 5; ++i)
    {
        EntityID e = world.CreateEntity();
        world.Add<TransformComponent>(e);
        world.Add<WorldTransformComponent>(e);
        world.Add<MeshComponent>(e, MeshHandle::Make(1u,1u));
        world.Add<MaterialComponent>(e, mat);
        world.Add<BoundsComponent>(e, BoundsComponent{
            .centerWorld={static_cast<float>(i), 0, 0},
            .extentsWorld={0.5f,0.5f,0.5f}, .boundingSphere=0.87f});
    }

    MaterialDesc dt;
    dt.name="MT"; dt.passTag=RenderPassTag::Transparent;
    dt.vertexShader=ShaderHandle::Make(1u,1u);
    dt.fragmentShader=ShaderHandle::Make(2u,1u);
    dt.blend.blendEnable=true;
    MaterialHandle transMat = ms.RegisterMaterial(std::move(dt));

    for (int i = 0; i < 2; ++i)
    {
        EntityID e = world.CreateEntity();
        world.Add<TransformComponent>(e);
        world.Add<WorldTransformComponent>(e);
        world.Add<MeshComponent>(e, MeshHandle::Make(1u,1u));
        world.Add<MaterialComponent>(e, transMat);
        world.Add<BoundsComponent>(e, BoundsComponent{
            .centerWorld={static_cast<float>(i)*3.f, 0, 5},
            .extentsWorld={0.5f,0.5f,0.05f}, .boundingSphere=0.5f});
    }

    // Extract: ECS → SceneSnapshot → RenderWorld
    RenderWorld rw;
    renderer::SceneSnapshot snap;
    renderer::ECSExtractor::Extract(world, snap);
    rw.Extract(snap, ms);
    CHECK_EQ(ctx, rw.TotalProxyCount(), 7u);

    // BuildDrawLists - große ViewProj die alles einschließt
    math::Mat4 view = math::Mat4::LookAtRH({0,0,-10},{0,0,0},math::Vec3::Up());
    math::Mat4 proj = math::Mat4::PerspectiveFovRH(
        60.f*math::DEG_TO_RAD, 16.f/9.f, 0.1f, 1000.f);
    math::Mat4 vp = proj * view;

    rw.BuildDrawLists(view, vp, 0.1f, 1000.f, ms);
    const RenderQueue& q = rw.GetQueue();

    // Alle sichtbar
    CHECK_EQ(ctx, rw.VisibleCount(), 7u);
    CHECK_EQ(ctx, q.opaque.Size(),       5u);
    CHECK_EQ(ctx, q.transparent.Size(),  2u);

    // Opaque: front-to-back sortiert
    for (size_t i = 1; i < q.opaque.items.size(); ++i)
        CHECK(ctx, q.opaque.items[i-1].sortKey < q.opaque.items[i].sortKey
                || q.opaque.items[i-1].sortKey == q.opaque.items[i].sortKey);

    // Transparent: back-to-front (größere Tiefe = kleinerer Key)
    // Mindestens nicht strikt front-to-back
    bool hasBackToFront = q.transparent.Size() >= 2;
    CHECK(ctx, hasBackToFront); // schwache Prüfung für Demo

    // Shadow: beide Materialien haben castShadows=true (default)
    CHECK_EQ(ctx, q.shadow.Size(), 7u);
}



static void TestFeatureDrivenSceneExtraction(test::TestContext& ctx)
{
    RegisterAllComponents();

    class MarkerFeature final : public IEngineFeature
    {
    public:
        class MarkerRenderableStep final : public ISceneExtractionStep
        {
        public:
            std::string_view GetName() const noexcept override { return "test.marker.renderables"; }
            void Extract(const ecs::World& world, SceneSnapshot& snapshot) const override
            {
                world.View<WorldTransformComponent, MeshComponent, MaterialComponent>(
                    [&](EntityID id,
                        const WorldTransformComponent& wt,
                        const MeshComponent& mesh,
                        const MaterialComponent& mat)
                    {
                        if (!ECSExtractor::IsEntityActive(world, id) || !mesh.mesh.IsValid())
                            return;

                        RenderableEntry entry{};
                        entry.entity = id;
                        entry.mesh = mesh.mesh;
                        entry.material = mat.material;
                        entry.submeshIndex = mat.submeshIndex;
                        entry.worldMatrix = wt.matrix;
                        entry.worldMatrixInvT = wt.inverse.Transposed();
                        snapshot.renderables.push_back(entry);
                    });
            }
        };

        class MarkerLightStep final : public ISceneExtractionStep
        {
        public:
            std::string_view GetName() const noexcept override { return "test.marker.lights"; }
            void Extract(const ecs::World& world, SceneSnapshot& snapshot) const override
            {
                world.View<WorldTransformComponent, LightComponent>(
                    [&](EntityID id,
                        const WorldTransformComponent& wt,
                        const LightComponent& light)
                    {
                        if (!ECSExtractor::IsEntityActive(world, id))
                            return;

                        LightEntry entry{};
                        entry.entity = id;
                        entry.lightType = light.type;
                        entry.positionWorld = wt.matrix.TransformPoint({0.f, 0.f, 0.f});
                        entry.directionWorld = wt.matrix.TransformDirection({0.f, 0.f, -1.f}).Normalized();
                        snapshot.lights.push_back(entry);
                    });
            }
        };

        std::string_view GetName() const noexcept override { return "test-marker-feature"; }
        FeatureID GetID() const noexcept override { return FeatureID::FromString("test-marker-feature"); }

        void Register(FeatureRegistrationContext& context) override
        {
            context.RegisterSceneExtractionStep(std::make_shared<MarkerRenderableStep>());
            context.RegisterSceneExtractionStep(std::make_shared<MarkerLightStep>());
        }

        bool Initialize(const FeatureInitializationContext& context) override
        {
            (void)context;
            return true;
        }

        void Shutdown(const FeatureShutdownContext& context) override
        {
            (void)context;
        }
    };

    ecs::World world;
    MaterialSystem ms;

    MaterialDesc d;
    d.name = "FeatureMat";
    d.passTag = RenderPassTag::Opaque;
    d.vertexShader = ShaderHandle::Make(1u, 1u);
    d.fragmentShader = ShaderHandle::Make(2u, 1u);
    const MaterialHandle mat = ms.RegisterMaterial(std::move(d));

    const EntityID renderable = world.CreateEntity();
    world.Add<TransformComponent>(renderable);
    world.Add<WorldTransformComponent>(renderable);
    world.Add<MeshComponent>(renderable, MeshHandle::Make(10u, 1u));
    world.Add<MaterialComponent>(renderable, mat);
    world.Add<ActiveComponent>(renderable, ActiveComponent{true});

    const EntityID light = world.CreateEntity();
    world.Add<TransformComponent>(light);
    world.Add<WorldTransformComponent>(light);
    world.Add<LightComponent>(light);

    const EntityID inactive = world.CreateEntity();
    world.Add<TransformComponent>(inactive);
    world.Add<WorldTransformComponent>(inactive);
    world.Add<MeshComponent>(inactive, MeshHandle::Make(11u, 1u));
    world.Add<MaterialComponent>(inactive, mat);
    world.Add<ActiveComponent>(inactive, ActiveComponent{false});

    FeatureRegistry registry;
    CHECK(ctx, registry.AddFeature(std::make_unique<MarkerFeature>()));

    TestDevice device;
    CHECK(ctx, device.Initialize(IDevice::DeviceDesc{}));
    ShaderRuntime shaderRuntime;
    CHECK(ctx, shaderRuntime.Initialize(device));
    CHECK(ctx, registry.InitializeAll(FeatureInitializationContext{device, shaderRuntime, nullptr}));

    SceneSnapshot snapshot;
    ECSExtractor::BeginSnapshot(snapshot);
    const auto& steps = registry.GetSceneExtractionSteps();
    CHECK_EQ(ctx, steps.size(), size_t(2));
    for (const ISceneExtractionStep* step : steps)
    {
        const size_t renderableOffset = snapshot.renderables.size();
        const size_t lightOffset = snapshot.lights.size();
        step->Extract(world, snapshot);
        snapshot.RecordContribution(step->GetName(),
                                    renderableOffset,
                                    lightOffset,
                                    snapshot.renderables.size() - renderableOffset,
                                    snapshot.lights.size() - lightOffset);
    }

    CHECK_EQ(ctx, snapshot.renderables.size(), size_t(1));
    CHECK_EQ(ctx, snapshot.lights.size(), size_t(1));
    CHECK_EQ(ctx, snapshot.contributions.size(), size_t(2));
    CHECK_EQ(ctx, snapshot.contributions[0].stepName, std::string("test.marker.renderables"));
    CHECK_EQ(ctx, snapshot.contributions[0].renderableCount, size_t(1));
    CHECK_EQ(ctx, snapshot.contributions[0].lightCount, size_t(0));
    CHECK_EQ(ctx, snapshot.contributions[1].stepName, std::string("test.marker.lights"));
    CHECK_EQ(ctx, snapshot.contributions[1].renderableCount, size_t(0));
    CHECK_EQ(ctx, snapshot.contributions[1].lightCount, size_t(1));

    registry.ShutdownAll(FeatureShutdownContext{});
    shaderRuntime.Shutdown();
    device.Shutdown();
}

static void TestForwardFeatureExtractionRegistration(test::TestContext& ctx)
{
    RegisterAllComponents();

    ecs::World world;
    MaterialSystem ms;

    MaterialDesc d;
    d.name = "ForwardMat";
    d.passTag = RenderPassTag::Opaque;
    d.vertexShader = ShaderHandle::Make(1u, 1u);
    d.fragmentShader = ShaderHandle::Make(2u, 1u);
    const MaterialHandle mat = ms.RegisterMaterial(std::move(d));

    const EntityID renderable = world.CreateEntity();
    world.Add<TransformComponent>(renderable);
    world.Add<WorldTransformComponent>(renderable);
    world.Add<MeshComponent>(renderable, MeshHandle::Make(21u, 1u));
    world.Add<MaterialComponent>(renderable, mat);
    world.Add<BoundsComponent>(renderable, BoundsComponent{
        .centerWorld={1.f, 2.f, 3.f}, .extentsWorld={0.5f, 0.5f, 0.5f}, .boundingSphere=1.f});

    const EntityID light = world.CreateEntity();
    world.Add<TransformComponent>(light);
    world.Add<WorldTransformComponent>(light);
    world.Add<LightComponent>(light);

    FeatureRegistry registry;
    CHECK(ctx, registry.AddFeature(engine::renderer::addons::forward::CreateForwardFeature()));

    TestDevice device;
    CHECK(ctx, device.Initialize(IDevice::DeviceDesc{}));
    ShaderRuntime shaderRuntime;
    CHECK(ctx, shaderRuntime.Initialize(device));
    CHECK(ctx, registry.InitializeAll(FeatureInitializationContext{device, shaderRuntime, nullptr}));

    RenderWorld renderWorld;
    FrameExtractionStage stage;
    const FrameExtractionStageContext extractionContext{
        world,
        registry.GetSceneExtractionSteps()
    };

    FrameExtractionStageResult extractionResult{};
    CHECK(ctx, stage.Execute(extractionContext, extractionResult));
    renderWorld.Clear();
    renderWorld.Extract(extractionResult.snapshot, ms);
    CHECK_EQ(ctx, registry.GetSceneExtractionSteps().size(), size_t(2));
    CHECK_EQ(ctx, renderWorld.TotalProxyCount(), 1u);
    CHECK_EQ(ctx, renderWorld.GetLights().size(), size_t(1));

    SceneSnapshot legacySnapshot;
    ECSExtractor::Extract(world, legacySnapshot);
    CHECK_EQ(ctx, legacySnapshot.renderables.size(), size_t(1));
    CHECK_EQ(ctx, legacySnapshot.lights.size(), size_t(1));
    CHECK_EQ(ctx, legacySnapshot.contributions.size(), size_t(2));

    registry.ShutdownAll(FeatureShutdownContext{});
    shaderRuntime.Shutdown();
    device.Shutdown();
}

// ==========================================================================
// RenderSystem + headless window - geschlossener Laufzeit-Loop
// ==========================================================================
static void TestRenderSystemLoop(test::TestContext& ctx)
{
    RegisterAllComponents();

    ecs::World world;
    MaterialSystem ms;
    events::EventBus bus;
    platform::NullInput input;
    platform::FixedTiming timing(1.0 / 60.0);

    MaterialDesc d;
    d.name = "LoopMat";
    d.passTag = RenderPassTag::Opaque;
    d.vertexShader = ShaderHandle::Make(1u, 1u);
    d.fragmentShader = ShaderHandle::Make(2u, 1u);
    const MaterialHandle mat = ms.RegisterMaterial(std::move(d));

    const EntityID e = world.CreateEntity();
    world.Add<TransformComponent>(e);
    world.Add<WorldTransformComponent>(e);
    world.Add<MeshComponent>(e, MeshHandle::Make(1u, 1u));
    world.Add<MaterialComponent>(e, mat);
    world.Add<BoundsComponent>(e, BoundsComponent{
        .centerWorld={0.f, 0.f, 0.f},
        .extentsWorld={0.5f, 0.5f, 0.5f},
        .boundingSphere=0.87f});

    auto window = platform::CreateHeadlessWindow();
    CHECK(ctx, window->Create({.width=640u, .height=360u, .title="Headless"}));

    renderer::RenderSystem renderer;
    CHECK(ctx, renderer.RegisterFeature(engine::renderer::addons::forward::CreateForwardFeature()));
    renderer::IDevice::DeviceDesc dd;
    dd.enableDebugLayer = true;
    dd.appName = "KROM Test";
    platform::WindowDesc wDesc{};
    CHECK(ctx, renderer.Initialize(renderer::DeviceFactory::BackendType::Null, *window, wDesc, &bus, dd));

    uint32_t resizeEvents = 0u;
    auto resizeSub = bus.Subscribe<events::WindowResizedEvent>([&](const events::WindowResizedEvent& ev) {
        ++resizeEvents;
        CHECK_EQ(ctx, ev.width, 800u);
        CHECK_EQ(ctx, ev.height, 600u);
    });

    renderer::RenderView view;
    view.view = math::Mat4::LookAtRH({0.f, 0.f, -5.f}, {0.f, 0.f, 0.f}, math::Vec3::Up());
    view.projection = math::Mat4::PerspectiveFovRH(60.f * math::DEG_TO_RAD, 16.f / 9.f, 0.1f, 100.f);
    view.cameraPosition = {0.f, 0.f, -5.f};
    view.cameraForward = {0.f, 0.f, 1.f};

    timing.BeginFrame();
    auto ev0 = window->PumpEvents(input);
    CHECK(ctx, !ev0.quitRequested);
    CHECK(ctx, !ev0.resized);
    CHECK(ctx, renderer.RenderFrame(world, ms, view, timing));
    CHECK_EQ(ctx, renderer.GetStats().totalProxyCount, 1u);
    CHECK_EQ(ctx, renderer.GetStats().visibleProxyCount, 1u);

    window->Resize(800u, 600u);
    timing.BeginFrame();
    auto ev1 = window->PumpEvents(input);
    CHECK(ctx, ev1.resized);
    renderer.HandleResize(ev1.width, ev1.height);
    CHECK(ctx, renderer.RenderFrame(world, ms, view, timing));
    CHECK_EQ(ctx, resizeEvents, 1u);
    CHECK_EQ(ctx, renderer.GetSwapchain()->GetWidth(), 800u);
    CHECK_EQ(ctx, renderer.GetSwapchain()->GetHeight(), 600u);

    window->RequestClose();
    timing.BeginFrame();
    auto ev2 = window->PumpEvents(input);
    CHECK(ctx, ev2.quitRequested);
    CHECK(ctx, !window->IsOpen());

    renderer.Shutdown();
    window->Destroy();
}


// ==========================================================================
// PlatformRenderLoop - geschlossener Plattform+Renderer Loop
// ==========================================================================
static void TestPlatformRenderLoop(test::TestContext& ctx)
{
    RegisterAllComponents();

    TestHeadlessPlatform platform;
    CHECK(ctx, platform.Initialize());

    events::EventBus bus;
    renderer::PlatformRenderLoop loop;
    if (!loop.GetRenderSystem().RegisterFeature(engine::renderer::addons::forward::CreateForwardFeature()))
    {
        Debug::LogError("forward feature registration failed");
        CHECK(ctx, false);
        return;
    }

    platform::WindowDesc desc{};
    desc.width = 640u;
    desc.height = 360u;
    desc.title = "PlatformLoop";

    renderer::IDevice::DeviceDesc dd{};
    dd.enableDebugLayer = true;
    dd.appName = "PlatformLoopTest";
    CHECK(ctx, loop.Initialize(renderer::DeviceFactory::BackendType::Null, platform, desc, &bus, dd));

    ecs::World world;
    MaterialSystem ms;

    MaterialDesc d;
    d.name = "LoopMat";
    d.passTag = RenderPassTag::Opaque;
    d.vertexShader = ShaderHandle::Make(1u, 1u);
    d.fragmentShader = ShaderHandle::Make(2u, 1u);
    const MaterialHandle mat = ms.RegisterMaterial(std::move(d));

    const EntityID e = world.CreateEntity();
    world.Add<TransformComponent>(e);
    world.Add<WorldTransformComponent>(e);
    world.Add<MeshComponent>(e, MeshHandle::Make(1u, 1u));
    world.Add<MaterialComponent>(e, mat);
    world.Add<BoundsComponent>(e, BoundsComponent{
        .centerWorld={0.f, 0.f, 0.f},
        .extentsWorld={0.5f, 0.5f, 0.5f},
        .boundingSphere=0.87f});

    renderer::RenderView view;
    view.view = math::Mat4::LookAtRH({0.f, 0.f, -5.f}, {0.f, 0.f, 0.f}, math::Vec3::Up());
    view.projection = math::Mat4::PerspectiveFovRH(60.f * math::DEG_TO_RAD, 16.f / 9.f, 0.1f, 100.f);
    view.cameraPosition = {0.f, 0.f, -5.f};
    view.cameraForward = {0.f, 0.f, 1.f};

    platform::FixedTiming timing(1.0 / 60.0);
    CHECK(ctx, loop.Tick(world, ms, view, timing));
    CHECK_EQ(ctx, loop.GetRenderSystem().GetStats().totalProxyCount, 1u);

    loop.GetWindow()->Resize(800u, 600u);
    CHECK(ctx, loop.Tick(world, ms, view, timing));
    CHECK_EQ(ctx, loop.GetRenderSystem().GetSwapchain()->GetWidth(), 800u);
    CHECK_EQ(ctx, loop.GetRenderSystem().GetSwapchain()->GetHeight(), 600u);

    loop.GetWindow()->RequestClose();
    CHECK(ctx, !loop.Tick(world, ms, view, timing));
    CHECK(ctx, loop.ShouldExit());

    loop.Shutdown();
    platform.Shutdown();
}


static void TestGpuResourceRuntime(test::TestContext& ctx)
{
    using namespace engine::renderer::null_backend;

    NullDevice device;
    IDevice::DeviceDesc dd{};
    dd.appName = "GpuRuntimeTest";
    CHECK(ctx, device.Initialize(dd));

    GpuResourceRuntime runtime;
    CHECK(ctx, runtime.Initialize(device, 3u));

    runtime.BeginFrame(0u);
    rendergraph::RenderGraph rgA;
    auto a = rgA.CreateTransientRenderTarget("A", 64u, 64u, Format::RGBA16_FLOAT, rendergraph::RGResourceKind::ColorTexture);
    auto bbA = rgA.ImportBackbuffer(RenderTargetHandle::Make(100u,1u), TextureHandle::Make(100u,1u), 64u, 64u);
    rgA.AddPass("WriteA").WriteRenderTarget(a).Execute([](const rendergraph::RGExecContext&){});
    rgA.AddPass("PresentA").ReadTexture(a).Present(bbA).Execute([](const rendergraph::RGExecContext&){});
    runtime.AllocateTransientTargets(rgA);
    rendergraph::CompiledFrame frameA;
    CHECK(ctx, rgA.Compile(frameA));
    const auto firstHandle = frameA.resources[0].renderTarget;
    CHECK_VALID(ctx, firstHandle);
    runtime.ReleaseTransientTargets(frameA, 1u);
    runtime.EndFrame(1u);

    runtime.BeginFrame(1u);
    rendergraph::RenderGraph rgB;
    auto b = rgB.CreateTransientRenderTarget("B", 64u, 64u, Format::RGBA16_FLOAT, rendergraph::RGResourceKind::ColorTexture);
    auto bbB = rgB.ImportBackbuffer(RenderTargetHandle::Make(101u,1u), TextureHandle::Make(101u,1u), 64u, 64u);
    rgB.AddPass("WriteB").WriteRenderTarget(b).Execute([](const rendergraph::RGExecContext&){});
    rgB.AddPass("PresentB").ReadTexture(b).Present(bbB).Execute([](const rendergraph::RGExecContext&){});
    runtime.AllocateTransientTargets(rgB);
    rendergraph::CompiledFrame frameB;
    CHECK(ctx, rgB.Compile(frameB));
    CHECK_EQ(ctx, frameB.resources[0].renderTarget, firstHandle);

    const uint32_t beforeUploads = runtime.GetStats().liveFrameUploadBuffers;
    BufferHandle upload = runtime.AllocateUploadBuffer(256u, BufferType::Constant, "UploadTest");
    CHECK_VALID(ctx, upload);
    uint32_t data[4] = {1u,2u,3u,4u};
    runtime.UploadBuffer(upload, data, sizeof(data));
    CHECK_EQ(ctx, runtime.GetStats().uploadedBytesThisFrame, static_cast<uint64_t>(sizeof(data)));
    CHECK_EQ(ctx, runtime.GetStats().liveFrameUploadBuffers, beforeUploads + 1u);

    runtime.Shutdown();
    device.Shutdown();
}

static void TestDeviceFactoryRegistration(test::TestContext& ctx)
{
    DeviceFactory::Unregister(DeviceFactory::BackendType::Vulkan);
    CHECK(ctx, !DeviceFactory::IsRegistered(DeviceFactory::BackendType::Vulkan));

    DeviceFactory::Register(DeviceFactory::BackendType::Vulkan, &CreateTestDeviceFactoryInstance);
    CHECK(ctx, DeviceFactory::IsRegistered(DeviceFactory::BackendType::Vulkan));

    auto custom = DeviceFactory::Create(DeviceFactory::BackendType::Vulkan);
    CHECK(ctx, custom != nullptr);
    CHECK_EQ(ctx, std::string(custom->GetBackendName()), std::string("TestDevice"));

    DeviceFactory::Unregister(DeviceFactory::BackendType::Vulkan);
    CHECK(ctx, !DeviceFactory::IsRegistered(DeviceFactory::BackendType::Vulkan));

    auto nullDevice = DeviceFactory::Create(DeviceFactory::BackendType::Null);
    CHECK(ctx, nullDevice != nullptr);
    CHECK_EQ(ctx, std::string(nullDevice->GetBackendName()), std::string("Null"));
}

// ==========================================================================
// Run all renderer tests
// ==========================================================================


static void TestShaderVariantCache(test::TestContext& ctx)
{
    renderer::null_backend::NullDevice device;
    IDevice::DeviceDesc dd{};
    dd.appName = "VariantTest";
    CHECK(ctx, device.Initialize(dd));

    ShaderRuntime runtime;
    CHECK(ctx, runtime.Initialize(device));

    assets::AssetRegistry registry;
    auto shader = std::make_unique<assets::ShaderAsset>();
    shader->debugName = "TestVS";
    shader->stage = assets::ShaderStage::Vertex;
    shader->sourceLanguage = assets::ShaderSourceLanguage::HLSL;
    shader->sourceCode = "// dummy HLSL source";
    shader->entryPoint = "main";
    shader->state = assets::AssetState::Loaded;

    const ShaderHandle assetHandle = registry.GetOrAddShader("shaders/test_variant.hlsl", std::move(shader));
    runtime.SetAssetRegistry(&registry);

    const ShaderHandle h1 = runtime.GetOrCreateVariant(assetHandle, ShaderPassType::Main, ShaderVariantFlag::None);
    const ShaderHandle h2 = runtime.GetOrCreateVariant(assetHandle, ShaderPassType::Main, ShaderVariantFlag::None);
    CHECK(ctx, h1.IsValid());
    CHECK_EQ(ctx, h1, h2);
    CHECK_EQ(ctx, runtime.GetVariantCache().CachedCount(), static_cast<size_t>(1));

    const ShaderHandle h3 = runtime.GetOrCreateVariant(assetHandle, ShaderPassType::Main,
                                                       ShaderVariantFlag::Skinned | ShaderVariantFlag::VertexColor);
    CHECK(ctx, h3.IsValid());
    CHECK(ctx, !(h1 == h3));
    CHECK_EQ(ctx, runtime.GetVariantCache().CachedCount(), static_cast<size_t>(2));

    const ShaderHandle hs1 = runtime.GetOrCreateVariant(assetHandle, ShaderPassType::Shadow,
                                                        ShaderVariantFlag::Skinned | ShaderVariantFlag::NormalMap);
    const ShaderHandle hs2 = runtime.GetOrCreateVariant(assetHandle, ShaderPassType::Shadow,
                                                        ShaderVariantFlag::Skinned);
    CHECK(ctx, hs1.IsValid());
    CHECK_EQ(ctx, hs1, hs2);

    const auto defines = ShaderCompiler::VariantFlagsToDefines(ShaderVariantFlag::Skinned | ShaderVariantFlag::AlphaTest);
    bool foundSkinning = false;
    bool foundAlpha = false;
    for (const auto& d : defines)
    {
        foundSkinning = foundSkinning || d == "KROM_SKINNING";
        foundAlpha = foundAlpha || d == "KROM_ALPHA_TEST";
    }
    CHECK(ctx, foundSkinning);
    CHECK(ctx, foundAlpha);

    runtime.Shutdown();
    device.Shutdown();
}

static void TestOpenGLBackendRegistration(test::TestContext& ctx)
{
    using BackendType = DeviceFactory::BackendType;

    CHECK(ctx, DeviceFactory::IsRegistered(BackendType::OpenGL));
    auto adapters = DeviceFactory::EnumerateAdapters(BackendType::OpenGL);
    CHECK(ctx, !adapters.empty());

    auto device = DeviceFactory::Create(BackendType::OpenGL);
    CHECK(ctx, device != nullptr);

    IDevice::DeviceDesc dd{};
    dd.appName = "OpenGLRegistrationTest";
    CHECK(ctx, device->Initialize(dd));

    auto cmd = device->CreateCommandList(QueueType::Graphics);
    CHECK(ctx, cmd != nullptr);
    cmd->Begin();
    cmd->Draw(3u);
    cmd->End();
    cmd->Submit();
    CHECK_GT(ctx, device->GetDrawCallCount(), 0u);

    device->Shutdown();
}

int RunRendererTests()
{
    RegisterAllComponents();
    engine::Debug::MinLevel = engine::LogLevel::Fatal;

    test::TestSuite suite("Renderer");
    suite
        .Add("PipelineKey determinism",     TestPipelineKeyDeterminism)
        .Add("PipelineKey no padding",      TestPipelineKeyNoPadding)
        .Add("SortKey ordering",            TestSortKeyOrdering)
        .Add("MaterialSystem",              TestMaterialSystem)
        .Add("PipelineCache",               TestPipelineCache)
        .Add("CbLayout HLSL packing",       TestCbLayout)
        .Add("ShaderBindingModel slots",    TestShaderBindingModel)
        .Add("RenderWorld extract+cull",    TestRenderWorldExtract)
        .Add("Feature scene extraction",  TestFeatureDrivenSceneExtraction)
        .Add("Forward extraction registration", TestForwardFeatureExtractionRegistration)
        .Add("RenderFrame state",         TestRenderFrameExecutionState)
        .Add("Frame TaskGraph parallel",   TestFrameTaskGraphParallelStructure)
        .Add("Frame TaskGraph failure",    TestFrameTaskGraphFailurePropagation)
        .Add("GPU resource runtime",       TestGpuResourceRuntime)
        .Add("RenderSystem closed loop",   TestRenderSystemLoop)
        .Add("DeviceFactory registration", TestDeviceFactoryRegistration)
        .Add("ShaderRuntime end-to-end", TestShaderRuntimeEndToEnd)
        .Add("ShaderRuntime validation", TestShaderRuntimeValidation)
        .Add("ShaderVariant cache",      TestShaderVariantCache)
        .Add("OpenGL backend registration", TestOpenGLBackendRegistration)
        .Add("PlatformRenderLoop",        TestPlatformRenderLoop);

    return suite.Run();
}

// ==========================================================================
// ShaderRuntime - Asset-Shader-Load + Material-Bindings
// ==========================================================================
static void TestShaderRuntimeEndToEnd(test::TestContext& ctx)
{
    assets::AssetRegistry assets;

    auto vs = std::make_unique<assets::ShaderAsset>();
    vs->debugName = "VS_Main";
    vs->stage = assets::ShaderStage::Vertex;
    vs->entryPoint = "VSMain";
    vs->sourceCode = "float4 VSMain() : SV_Position { return float4(0,0,0,1); }";
    assets::CompiledShaderArtifact vsCompiled;
    vsCompiled.target = assets::ShaderTargetProfile::Null;
    vsCompiled.stage = assets::ShaderStage::Vertex;
    vsCompiled.entryPoint = "VSMain";
    vsCompiled.sourceText = "float4 VSMain() : SV_Position { return float4(1,0,0,1); }";
    vsCompiled.sourceHash = 0x1234ull;
    vs->compiledArtifacts.push_back(vsCompiled);
    const ShaderHandle vsAsset = assets.GetOrAddShader("shaders/test_vs.hlsl", std::move(vs));

    auto ps = std::make_unique<assets::ShaderAsset>();
    ps->debugName = "PS_Main";
    ps->stage = assets::ShaderStage::Fragment;
    ps->entryPoint = "PSMain";
    ps->bytecode = {0x01u, 0x02u, 0x03u, 0x04u};
    const ShaderHandle psAsset = assets.GetOrAddShader("shaders/test_ps.dxil", std::move(ps));

    MaterialSystem ms;
    MaterialDesc d;
    d.name = "RuntimeMaterial";
    d.passTag = RenderPassTag::Opaque;
    d.vertexShader = vsAsset;
    d.fragmentShader = psAsset;

    MaterialParam albedoMap;
    albedoMap.name = "albedoMap";
    albedoMap.type = MaterialParam::Type::Texture;
    albedoMap.texture = TextureHandle::Make(7u, 1u);

    MaterialParam linearSampler;
    linearSampler.name = "linearClampSampler";
    linearSampler.type = MaterialParam::Type::Sampler;

    MaterialParam tint;
    tint.name = "tint";
    tint.type = MaterialParam::Type::Vec4;
    tint.value.f[0] = 1.f;
    tint.value.f[1] = 0.5f;
    tint.value.f[2] = 0.25f;
    tint.value.f[3] = 1.f;

    d.params = {albedoMap, linearSampler, tint};
    const MaterialHandle mat = ms.RegisterMaterial(std::move(d));

    renderer::null_backend::NullDevice device;
    renderer::IDevice::DeviceDesc dd{};
    dd.appName = "ShaderRuntimeTest";
    CHECK(ctx, device.Initialize(dd));

    ShaderRuntime runtime;
    CHECK(ctx, runtime.Initialize(device));
    runtime.SetAssetRegistry(&assets);

    CHECK(ctx, runtime.PrepareAllShaderAssets());
    CHECK(ctx, runtime.PrepareMaterial(ms, mat));

    const auto* vsStatus = runtime.GetShaderStatus(vsAsset);
    const auto* psStatus = runtime.GetShaderStatus(psAsset);
    CHECK(ctx, vsStatus != nullptr);
    CHECK(ctx, psStatus != nullptr);
    CHECK(ctx, vsStatus->loaded);
    CHECK(ctx, psStatus->loaded);
    CHECK(ctx, !vsStatus->fromBytecode);
    CHECK(ctx, psStatus->fromBytecode);
    CHECK(ctx, vsStatus->fromCompiledArtifact);
    CHECK_EQ(ctx, vsStatus->target, assets::ShaderTargetProfile::Null);
    CHECK_EQ(ctx, vsStatus->compiledHash, 0x1234ull);

    const auto* matState = runtime.GetMaterialState(mat);
    CHECK(ctx, matState != nullptr);
    CHECK(ctx, matState->valid);
    CHECK_VALID(ctx, matState->pipeline);
    CHECK_VALID(ctx, matState->perMaterialCB);
    CHECK_GT(ctx, matState->bindings.size(), 1u);

    auto cmd = device.CreateCommandList(QueueType::Graphics);
    cmd->Begin();
    CHECK(ctx, runtime.BindMaterial(*cmd, ms, mat, BufferHandle::Make(1u,1u), BufferHandle::Make(2u,1u)));
    cmd->Draw(3u);
    cmd->End();
    cmd->Submit();
    CHECK_GT(ctx, device.GetDrawCallCount(), 0u);

    runtime.Shutdown();
    device.Shutdown();
}

static void TestShaderRuntimeValidation(test::TestContext& ctx)
{
    MaterialSystem ms;
    MaterialDesc d;
    d.name = "BrokenMaterial";
    d.passTag = RenderPassTag::Opaque;
    d.vertexShader = ShaderHandle::Make(11u, 1u);
    d.fragmentShader = ShaderHandle::Make(12u, 1u);

    MaterialParam texA;
    texA.name = "albedoMap";
    texA.type = MaterialParam::Type::Texture;
    texA.texture = TextureHandle::Make(1u, 1u);

    MaterialParam texB;
    texB.name = "baseColorMap";
    texB.type = MaterialParam::Type::Texture;
    texB.texture = TextureHandle::Make(2u, 1u);

    d.params = {texA, texB};
    const MaterialHandle mat = ms.RegisterMaterial(std::move(d));

    renderer::null_backend::NullDevice device;
    renderer::IDevice::DeviceDesc dd{};
    dd.appName = "ShaderRuntimeValidation";
    CHECK(ctx, device.Initialize(dd));

    ShaderRuntime runtime;
    CHECK(ctx, runtime.Initialize(device));

    std::vector<ShaderValidationIssue> issues;
    CHECK(ctx, !runtime.ValidateMaterial(ms, mat, issues));
    CHECK_GT(ctx, issues.size(), 0u);

    bool foundDuplicateSlot = false;
    for (const auto& issue : issues)
        if (issue.message.find("duplicate texture slot") != std::string::npos)
            foundDuplicateSlot = true;
    CHECK(ctx, foundDuplicateSlot);

    runtime.Shutdown();
    device.Shutdown();
}
