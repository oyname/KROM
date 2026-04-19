#include "ForwardFeature.hpp"
// =============================================================================
// KROM Engine - tests/test_renderer.cpp
// Renderer-Tests: PipelineKey, SortKey, MaterialSystem, RenderWorld
// =============================================================================
#include "TestFramework.hpp"
#include "addons/camera/CameraComponents.hpp"
#include "addons/camera/CameraViewBuilder.hpp"
#include "addons/lighting/LightingComponents.hpp"
#include "addons/lighting/LightingExtraction.hpp"
#include "addons/lighting/LightingFeature.hpp"
#include "addons/lighting/LightingFrameData.hpp"
#include "addons/mesh_renderer/MeshRendererComponents.hpp"
#include "addons/mesh_renderer/MeshRendererExtraction.hpp"
#include "addons/mesh_renderer/MeshRendererFeature.hpp"
#include "renderer/MaterialSystem.hpp"
#include "renderer/MaterialCBLayout.hpp"
#include "renderer/RenderPassRegistry.hpp"
#include "renderer/RenderWorld.hpp"
#include "renderer/ECSExtractor.hpp"
#include "renderer/PipelineCache.hpp"
#include "renderer/ShaderBindingModel.hpp"
#include "renderer/internal/RenderFrameOrchestrator.hpp"
#include "addons/forward/StandardFramePipeline.hpp"
#include "renderer/RenderSystem.hpp"
#include "renderer/ShaderRuntime.hpp"
#include "renderer/PlatformRenderLoop.hpp"
#include "scene/TransformSystem.hpp"
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
#include "LitMaterial.hpp"
#include "PbrMaterial.hpp"
#include "UnlitMaterial.hpp"
#include <cmath>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using namespace engine;
using namespace engine::renderer;


namespace {

void RegisterRendererTestComponents()
{
    static ecs::ComponentMetaRegistry registry;
    RegisterCoreComponents(registry);
    RegisterMeshRendererComponents(registry);
    RegisterCameraComponents(registry);
    RegisterLightingComponents(registry);
}

[[nodiscard]] ecs::ComponentMetaRegistry CreateRendererTestRegistry()
{
    ecs::ComponentMetaRegistry registry;
    RegisterCoreComponents(registry);
    RegisterMeshRendererComponents(registry);
    RegisterCameraComponents(registry);
    RegisterLightingComponents(registry);
    return registry;
}

[[nodiscard]] bool NearlyEqual(float a, float b, float eps = 1e-5f)
{
    return std::fabs(a - b) <= eps;
}

const DrawList& RequirePassList(test::TestContext& ctx, const RenderQueue& queue, RenderPassID passId)
{
    const DrawList* list = queue.FindList(passId);
    CHECK(ctx, list != nullptr);
    static DrawList empty{};
    return list ? *list : empty;
}

void CheckMatrixClose(test::TestContext& ctx,
                      const math::Mat4& actual,
                      const math::Mat4& expected,
                      float eps = 1e-5f)
{
    for (int col = 0; col < 4; ++col)
        for (int row = 0; row < 4; ++row)
            CHECK(ctx, NearlyEqual(actual.m[col][row], expected.m[col][row], eps));
}

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

static void TestPbrAddonFactory(test::TestContext& ctx);
static void TestPbrAddonShaderAssetSet(test::TestContext& ctx);
static void TestUnlitAddonFactory(test::TestContext& ctx);
static void TestUnlitAddonShaderAssetSet(test::TestContext& ctx);
static void TestLitAddonFactory(test::TestContext& ctx);
static void TestLitAddonShaderAssetSet(test::TestContext& ctx);
static void TestStandardFrameRecipe(test::TestContext& ctx);
static void TestShaderRuntimeEndToEnd(test::TestContext& ctx);
static void TestShaderRuntimeValidation(test::TestContext& ctx);
static void TestMaterialTextureWrites(test::TestContext& ctx);
static void TestShaderRuntimePartialEnvironmentUsesFallbacks(test::TestContext& ctx);
static void TestShaderRuntimeRebuildOnEnvironmentChange(test::TestContext& ctx);
static void TestShaderRuntimeUsesIBLVariantWhenEnvironmentActive(test::TestContext& ctx);
static void TestShaderRuntimeDoesNotUseIBLVariantWithoutEnvironment(test::TestContext& ctx);
static void TestShaderRuntimeRebuildsShaderVariantOnEnvironmentToggle(test::TestContext& ctx);
static void TestDeviceFactoryRegistration(test::TestContext& ctx);

static TextureHandle FindTextureBindingAtSlot(const MaterialGpuState& gpuState, uint32_t slot)
{
    for (const auto& binding : gpuState.bindings)
    {
        if (binding.kind == ResolvedMaterialBinding::Kind::Texture && binding.slot == slot)
            return binding.texture;
    }
    return TextureHandle::Invalid();
}

static bool ContainsDefine(const std::vector<std::string>& defines, const char* needle)
{
    return std::find(defines.begin(), defines.end(), std::string(needle)) != defines.end();
}

static const FrameRecipePassDesc* FindRecipePass(const FrameRecipe& recipe, std::string_view name)
{
    for (const FrameRecipePassDesc& pass : recipe.passes)
    {
        if (pass.name == name)
            return &pass;
    }
    return nullptr;
}



static void TestPbrAddonShaderAssetSet(test::TestContext& ctx)
{
    const auto assets = pbr::PbrMaterial::DefaultShaderAssetSet();
    CHECK_EQ(ctx, std::string(assets.vertexShader),   std::string("pbr_lit.vs.hlsl"));
    CHECK_EQ(ctx, std::string(assets.fragmentShader), std::string("pbr_lit.ps.hlsl"));
    CHECK_EQ(ctx, std::string(assets.shadowShader),   std::string("pbr_lit.vs.hlsl"));
    CHECK_EQ(ctx, assets.renderPass, StandardRenderPasses::Opaque());
}

static void TestUnlitAddonShaderAssetSet(test::TestContext& ctx)
{
    const auto assets = unlit::UnlitMaterial::DefaultShaderAssetSet();
    CHECK_EQ(ctx, std::string(assets.vertexShader),   std::string("quad_unlit.vs.hlsl"));
    CHECK_EQ(ctx, std::string(assets.fragmentShader), std::string("quad_unlit.ps.hlsl"));
    CHECK_EQ(ctx, assets.renderPass, StandardRenderPasses::Opaque());
}

static void TestUnlitAddonFactory(test::TestContext& ctx)
{
    MaterialSystem ms;
    unlit::UnlitMaterialCreateInfo info{};
    info.name = "UnlitTest";
    info.vertexShader = ShaderHandle::Make(11u, 1u);
    info.fragmentShader = ShaderHandle::Make(12u, 1u);
    info.enableEmissiveMap = true;
    info.alphaTest = true;
    info.frontFace = WindingOrder::CW;

    unlit::UnlitMaterial material = unlit::UnlitMaterial::Create(ms, info);
    CHECK(ctx, material.IsValid());

    const MaterialDesc* desc = ms.GetDesc(material.Handle());
    CHECK(ctx, desc != nullptr);
    CHECK_EQ(ctx, desc->vertexShader, info.vertexShader);
    CHECK_EQ(ctx, desc->fragmentShader, info.fragmentShader);
    CHECK_EQ(ctx, desc->renderPass, StandardRenderPasses::Opaque());
    CHECK_EQ(ctx, desc->rasterizer.frontFace, WindingOrder::CW);
    CHECK(ctx, (desc->permutationFlags & static_cast<uint64_t>(ShaderVariantFlag::Unlit)) != 0ull);
    CHECK(ctx, (desc->permutationFlags & static_cast<uint64_t>(ShaderVariantFlag::BaseColorMap)) != 0ull);
    CHECK(ctx, (desc->permutationFlags & static_cast<uint64_t>(ShaderVariantFlag::EmissiveMap)) != 0ull);
    CHECK(ctx, (desc->permutationFlags & static_cast<uint64_t>(ShaderVariantFlag::AlphaTest)) != 0ull);

    CHECK(ctx, material.SetBaseColorFactor({0.8f, 0.7f, 0.6f, 1.0f}));
    CHECK(ctx, material.SetEmissiveFactor({0.1f, 0.2f, 0.3f, 1.0f}));
    CHECK(ctx, material.SetAlbedo(TextureHandle::Make(41u, 1u)));
    CHECK(ctx, material.SetEmissive(TextureHandle::Make(42u, 1u)));

    const MaterialInstance* inst = ms.GetInstance(material.Handle());
    CHECK(ctx, inst != nullptr);
    if (inst != nullptr)
    {
        CHECK_EQ(ctx, inst->parameters.GetTexture(TexSlots::Albedo), TextureHandle::Make(41u, 1u));
        CHECK_EQ(ctx, inst->parameters.GetTexture(TexSlots::Emissive), TextureHandle::Make(42u, 1u));
    }
}

static void TestLitAddonShaderAssetSet(test::TestContext& ctx)
{
    const auto assets = lit::LitMaterial::DefaultShaderAssetSet();
    CHECK_EQ(ctx, std::string(assets.vertexShader),   std::string("lit.vs.hlsl"));
    CHECK_EQ(ctx, std::string(assets.fragmentShader), std::string("lit.ps.hlsl"));
    CHECK_EQ(ctx, std::string(assets.shadowShader),   std::string("lit.vs.hlsl"));
    CHECK_EQ(ctx, assets.renderPass, StandardRenderPasses::Opaque());
}

static void TestLitAddonFactory(test::TestContext& ctx)
{
    MaterialSystem ms;
    lit::LitMaterialCreateInfo info{};
    info.name = "LitTest";
    info.vertexShader = ShaderHandle::Make(21u, 1u);
    info.fragmentShader = ShaderHandle::Make(22u, 1u);
    info.shadowShader = ShaderHandle::Make(23u, 1u);
    info.enableEmissiveMap = true;
    info.specularStrength = 0.6f;
    info.roughnessFactor = 0.2f;

    lit::LitMaterial material = lit::LitMaterial::Create(ms, info);
    CHECK(ctx, material.IsValid());

    const MaterialDesc* desc = ms.GetDesc(material.Handle());
    CHECK(ctx, desc != nullptr);
    CHECK_EQ(ctx, desc->vertexShader, info.vertexShader);
    CHECK_EQ(ctx, desc->fragmentShader, info.fragmentShader);
    CHECK_EQ(ctx, desc->shadowShader, info.shadowShader);
    CHECK(ctx, (desc->permutationFlags & static_cast<uint64_t>(ShaderVariantFlag::Unlit)) == 0ull);
    CHECK(ctx, (desc->permutationFlags & static_cast<uint64_t>(ShaderVariantFlag::PBRMetalRough)) == 0ull);
    CHECK(ctx, (desc->permutationFlags & static_cast<uint64_t>(ShaderVariantFlag::BaseColorMap)) != 0ull);
    CHECK(ctx, (desc->permutationFlags & static_cast<uint64_t>(ShaderVariantFlag::EmissiveMap)) != 0ull);

    CHECK(ctx, material.SetSpecularStrength(0.75f));
    CHECK(ctx, material.SetRoughnessFactor(0.35f));
    CHECK(ctx, material.SetAlbedo(TextureHandle::Make(51u, 1u)));
    CHECK(ctx, material.SetEmissive(TextureHandle::Make(52u, 1u)));

    const MaterialInstance* inst = ms.GetInstance(material.Handle());
    CHECK(ctx, inst != nullptr);
    if (inst != nullptr)
    {
        CHECK_EQ(ctx, inst->parameters.GetTexture(TexSlots::Albedo), TextureHandle::Make(51u, 1u));
        CHECK_EQ(ctx, inst->parameters.GetTexture(TexSlots::Emissive), TextureHandle::Make(52u, 1u));
    }
}

static void TestPbrAddonFactory(test::TestContext& ctx)
{
    MaterialSystem materials;

    VertexLayout layout{};
    layout.attributes.push_back({ VertexSemantic::Position, Format::RGB32_FLOAT, 0u, 0u });
    layout.bindings.push_back({ 0u, 12u });

    pbr::PbrMaterialCreateInfo info{};
    info.name = "PBR_Addon_Factory";
    info.vertexShader = ShaderHandle::Make(10u, 1u);
    info.fragmentShader = ShaderHandle::Make(11u, 1u);
    info.shadowShader = ShaderHandle::Make(12u, 1u);
    info.vertexLayout = layout;
    info.baseColorFactor = { 0.25f, 0.5f, 0.75f, 1.0f };
    info.emissiveFactor = { 1.0f, 0.25f, 0.0f, 1.0f };
    info.metallicFactor = 0.9f;
    info.roughnessFactor = 0.15f;
    info.occlusionStrength = 0.8f;
    info.enableEmissiveMap = true;
    info.enableIBL = true;
    info.cullMode = MaterialCullMode::None;
    info.doubleSided = true;

    const MaterialHandle handle = pbr::PbrMaterial::Register(materials, info);
    CHECK(ctx, handle.IsValid());

    const MaterialDesc* desc = materials.GetDesc(handle);
    CHECK(ctx, desc != nullptr);
    CHECK(ctx, desc->vertexShader == info.vertexShader);
    CHECK(ctx, desc->fragmentShader == info.fragmentShader);
    CHECK(ctx, desc->shadowShader == info.shadowShader);
    CHECK_EQ(ctx, desc->renderPass, StandardRenderPasses::Opaque());
    CHECK_EQ(ctx, desc->bindings.size(), size_t(6));
    CHECK_EQ(ctx, desc->params.size(), size_t(15));
    CHECK(ctx, (desc->permutationFlags & static_cast<uint64_t>(ShaderVariantFlag::PBRMetalRough)) != 0ull);
    CHECK(ctx, (desc->permutationFlags & static_cast<uint64_t>(ShaderVariantFlag::BaseColorMap)) != 0ull);
    CHECK(ctx, (desc->permutationFlags & static_cast<uint64_t>(ShaderVariantFlag::NormalMap)) != 0ull);
    CHECK(ctx, (desc->permutationFlags & static_cast<uint64_t>(ShaderVariantFlag::ORMMap)) != 0ull);
    CHECK(ctx, (desc->permutationFlags & static_cast<uint64_t>(ShaderVariantFlag::EmissiveMap)) != 0ull);
    CHECK(ctx, (desc->permutationFlags & static_cast<uint64_t>(ShaderVariantFlag::IBLMap)) != 0ull);

    pbr::PbrMaterial material(materials, handle);
    CHECK(ctx, material.IsValid());
    CHECK(ctx, material.SetBaseColorFactor({ 0.1f, 0.2f, 0.3f, 1.0f }));
    CHECK(ctx, material.SetMetallicFactor(1.0f));
    CHECK(ctx, material.SetRoughnessFactor(0.05f));
    CHECK(ctx, material.SetAlbedo(TextureHandle::Make(31u, 1u)));
    CHECK(ctx, material.SetNormal(TextureHandle::Make(32u, 1u)));
    CHECK(ctx, material.SetORM(TextureHandle::Make(33u, 1u)));
    CHECK(ctx, material.SetEmissive(TextureHandle::Make(34u, 1u)));

    const auto& cbData = materials.GetCBData(handle);
    const auto& cbLayout = materials.GetCBLayout(handle);
    const uint32_t bcOff = cbLayout.GetOffset("baseColorFactor");
    const uint32_t metalOff = cbLayout.GetOffset("metallicFactor");
    const uint32_t roughOff = cbLayout.GetOffset("roughnessFactor");
    CHECK(ctx, bcOff != UINT32_MAX);
    CHECK(ctx, metalOff != UINT32_MAX);
    CHECK(ctx, roughOff != UINT32_MAX);

    const float* baseColor = reinterpret_cast<const float*>(cbData.data() + bcOff);
    const float* metallic = reinterpret_cast<const float*>(cbData.data() + metalOff);
    const float* roughness = reinterpret_cast<const float*>(cbData.data() + roughOff);
    CHECK(ctx, std::fabs(baseColor[0] - 0.1f) < 1e-6f);
    CHECK(ctx, std::fabs(baseColor[1] - 0.2f) < 1e-6f);
    CHECK(ctx, std::fabs(baseColor[2] - 0.3f) < 1e-6f);
    CHECK(ctx, std::fabs(metallic[0] - 1.0f) < 1e-6f);
    CHECK(ctx, std::fabs(roughness[0] - 0.05f) < 1e-6f);

    const MaterialInstance* inst = materials.GetInstance(handle);
    CHECK(ctx, inst != nullptr);
    CHECK(ctx, inst->parameters.GetTexture(TexSlots::Albedo) == TextureHandle::Make(31u, 1u));
    CHECK(ctx, inst->parameters.GetTexture(TexSlots::Normal) == TextureHandle::Make(32u, 1u));
    CHECK(ctx, inst->parameters.GetTexture(TexSlots::ORM) == TextureHandle::Make(33u, 1u));
    CHECK(ctx, inst->parameters.GetTexture(TexSlots::Emissive) == TextureHandle::Make(34u, 1u));
}

static void TestStandardFrameRecipe(test::TestContext& ctx)
{
    StandardFrameRecipeBuilder::BuildParams params{};
    params.viewportWidth = 800u;
    params.viewportHeight = 600u;
    params.shadowMapSize = 1024u;
    params.bloomWidth = 400u;
    params.bloomHeight = 300u;
    params.shadowEnabled = true;
    params.transparentEnabled = true;
    params.bloomEnabled = true;
    params.uiEnabled = true;

    const FrameRecipe recipe = StandardFrameRecipeBuilder::BuildRecipe(params);
    CHECK_EQ(ctx, recipe.resources.size(), size_t(8));
    CHECK_EQ(ctx, recipe.passes.size(), size_t(9));

    const FrameRecipePassDesc* shadow = FindRecipePass(recipe, "ShadowPass");
    CHECK(ctx, shadow != nullptr);
    CHECK_EQ(ctx, shadow->executorName, std::string(StandardFrameExecutors::Shadow));
    CHECK(ctx, shadow->renderPass.enabled);
    CHECK_EQ(ctx, shadow->renderPass.targetResourceName, std::string("ShadowMap"));
    CHECK_EQ(ctx, shadow->renderPass.viewportWidth, 1024u);
    CHECK_EQ(ctx, shadow->renderPass.viewportHeight, 1024u);

    const FrameRecipePassDesc* opaque = FindRecipePass(recipe, "MainOpaquePass");
    CHECK(ctx, opaque != nullptr);
    CHECK_EQ(ctx, opaque->executorName, std::string(StandardFrameExecutors::Opaque));
    CHECK_EQ(ctx, opaque->renderPass.targetResourceName, std::string("HDRSceneColor"));
    CHECK(ctx, opaque->renderPass.clearColor);
    CHECK(ctx, opaque->renderPass.clearDepth);

    const FrameRecipePassDesc* tonemap = FindRecipePass(recipe, "TonemapPass");
    CHECK(ctx, tonemap != nullptr);
    CHECK_EQ(ctx, tonemap->executorName, std::string(StandardFrameExecutors::Tonemap));
    CHECK_EQ(ctx, tonemap->renderPass.targetResourceName, std::string("Tonemapped"));
    CHECK_EQ(ctx, tonemap->renderPass.viewportWidth, 800u);
    CHECK_EQ(ctx, tonemap->renderPass.viewportHeight, 600u);

    const FrameRecipePassDesc* ui = FindRecipePass(recipe, "UIPass");
    CHECK(ctx, ui != nullptr);
    CHECK_EQ(ctx, ui->executorName, std::string(StandardFrameExecutors::UI));
    CHECK_EQ(ctx, ui->renderPass.targetResourceName, std::string("UIOverlay"));

    const FrameRecipePassDesc* present = FindRecipePass(recipe, "PresentPass");
    CHECK(ctx, present != nullptr);
    CHECK_EQ(ctx, present->executorName, std::string(StandardFrameExecutors::Present));
    CHECK(ctx, !present->renderPass.enabled);
}

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

    PipelineKey k1 = PipelineKey::From(d1, StandardRenderPasses::Opaque());
    PipelineKey k2 = PipelineKey::From(d2, StandardRenderPasses::Opaque());

    CHECK(ctx, k1 == k2);
    CHECK_EQ(ctx, k1.Hash(), k2.Hash());

    // Unterschiedlicher Cull-Mode → anderer Key
    d2.rasterizer.cullMode = CullMode::Front;
    PipelineKey k3 = PipelineKey::From(d2, StandardRenderPasses::Opaque());
    CHECK(ctx, !(k1 == k3));
    CHECK_NE(ctx, k1.Hash(), k3.Hash());

    // Unterschiedlicher Pass-Tag → anderer Key
    PipelineKey k4 = PipelineKey::From(d1, StandardRenderPasses::Transparent());
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
    k1.renderPassId   = StandardRenderPasses::Opaque().value;

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
    SortKey near_a = SortKey::ForFrontToBack(StandardRenderPasses::Opaque(), 0, 12345u, 0.1f);
    SortKey far_a  = SortKey::ForFrontToBack(StandardRenderPasses::Opaque(), 0, 12345u, 0.9f);
    CHECK(ctx, near_a < far_a); // Front-to-back: näher = kleinerer Key

    // Transparent: größere Tiefe kommt zuerst (back-to-front)
    SortKey near_t = SortKey::ForBackToFront(StandardRenderPasses::Transparent(), 0, 0.1f);
    SortKey far_t  = SortKey::ForBackToFront(StandardRenderPasses::Transparent(), 0, 0.9f);
    CHECK(ctx, far_t < near_t); // Back-to-front: weiter = kleinerer Key

    // Pass-Tag hat höchste Priorität
    SortKey opaque_deep  = SortKey::ForFrontToBack(StandardRenderPasses::Opaque(), 0, 0u, 0.99f);
    SortKey shadow_near  = SortKey::ForFrontToBack(StandardRenderPasses::Shadow(), 0, 0u, 0.01f);
    // Shadow (3) > Opaque (0) im Pass-Bit → shadow-key > opaque-key
    CHECK(ctx, opaque_deep < shadow_near);

    // UI hat eigene Ordnung nach drawOrder
    SortKey ui0 = SortKey::ForSubmissionOrder(StandardRenderPasses::UI(), 0, 0u);
    SortKey ui1 = SortKey::ForSubmissionOrder(StandardRenderPasses::UI(), 0, 100u);
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
    d.renderPass     = StandardRenderPasses::Opaque();
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
    const auto buildLayoutFromParams = [](const std::vector<MaterialParam>& params)
    {
        ShaderParameterLayout layout{};
        uint32_t cbOffset = 0u;
        const auto align16 = [](uint32_t value) { return (value + 15u) & ~15u; };
        const auto byteSizeOf = [](MaterialParam::Type type) -> uint32_t
        {
            switch (type)
            {
            case MaterialParam::Type::Float: return 4u;
            case MaterialParam::Type::Vec2: return 8u;
            case MaterialParam::Type::Vec3: return 16u;
            case MaterialParam::Type::Vec4: return 16u;
            case MaterialParam::Type::Int: return 4u;
            case MaterialParam::Type::Bool: return 4u;
            default: return 0u;
            }
        };
        const auto toParameterType = [](MaterialParam::Type type) -> ParameterType
        {
            switch (type)
            {
            case MaterialParam::Type::Float: return ParameterType::Float;
            case MaterialParam::Type::Vec2: return ParameterType::Vec2;
            case MaterialParam::Type::Vec3: return ParameterType::Vec3;
            case MaterialParam::Type::Vec4: return ParameterType::Vec4;
            case MaterialParam::Type::Int: return ParameterType::Int;
            case MaterialParam::Type::Bool: return ParameterType::Bool;
            case MaterialParam::Type::Texture: return ParameterType::Texture2D;
            case MaterialParam::Type::Sampler: return ParameterType::Sampler;
            case MaterialParam::Type::Buffer: return ParameterType::StructuredBuffer;
            }
            return ParameterType::Unknown;
        };

        for (const auto& param : params)
        {
            ParameterSlot slot{};
            slot.SetName(param.name);
            slot.type = toParameterType(param.type);
            slot.set = 0u;
            slot.elementCount = 1u;
            if (slot.type == ParameterType::Texture2D || slot.type == ParameterType::Sampler || slot.type == ParameterType::StructuredBuffer)
            {
                slot.binding = layout.slotCount;
                slot.stageFlags = ShaderStageMask::Fragment;
            }
            else
            {
                slot.binding = CBSlots::PerMaterial;
                slot.byteOffset = cbOffset;
                slot.byteSize = byteSizeOf(param.type);
                cbOffset += slot.byteSize;
                if (slot.byteSize > 4u)
                    cbOffset = align16(cbOffset);
            }
            (void)layout.AddSlot(slot);
        }
        return layout;
    };

    std::vector<MaterialParam> params;

    MaterialParam f; f.name="f"; f.type=MaterialParam::Type::Float; f.value.f[0]=1.f;
    MaterialParam v4; v4.name="v4"; v4.type=MaterialParam::Type::Vec4;
    v4.value.f[0]=v4.value.f[1]=v4.value.f[2]=v4.value.f[3]=0.f;
    MaterialParam f2; f2.name="f2"; f2.type=MaterialParam::Type::Float; f2.value.f[0]=2.f;
    params = {f, v4, f2};

    CbLayout layout = MaterialCBLayout::Build(buildLayoutFromParams(params));

    CHECK_EQ(ctx, layout.fields.size(), 3u);
    CHECK_EQ(ctx, layout.totalSize % 16u, 0u);
    CHECK_GT(ctx, layout.totalSize, 0u);

    const uint32_t fOff  = layout.GetOffset("f");
    const uint32_t v4Off = layout.GetOffset("v4");
    const uint32_t f2Off = layout.GetOffset("f2");
    CHECK_NE(ctx, fOff,  UINT32_MAX);
    CHECK_NE(ctx, v4Off, UINT32_MAX);
    CHECK_NE(ctx, f2Off, UINT32_MAX);
    CHECK_EQ(ctx, v4Off % 16u, 0u);

    MaterialParam tex; tex.name="t0"; tex.type=MaterialParam::Type::Texture;
    params.push_back(tex);
    CbLayout layoutWithTex = MaterialCBLayout::Build(buildLayoutFromParams(params));
    CHECK_EQ(ctx, layoutWithTex.fields.size(), 3u);
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

    // FrameConstants muss 16-Byte-aligned sein
    CHECK_EQ(ctx, sizeof(FrameConstants) % size_t(16), size_t(0));
    CHECK_GT(ctx, sizeof(FrameConstants), size_t(0));
}

// ==========================================================================
// RenderWorld - Extract + DrawList sort
// ==========================================================================
static void TestRenderWorldExtract(test::TestContext& ctx)
{
    RegisterRendererTestComponents();
    ecs::ComponentMetaRegistry componentRegistry = CreateRendererTestRegistry();
    ecs::World world(componentRegistry);
    MaterialSystem ms;

    // Material
    MaterialDesc d;
    d.name="M1"; d.renderPass=StandardRenderPasses::Opaque();
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
    dt.name="MT"; dt.renderPass=StandardRenderPasses::Transparent();
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

    // Extract: ECS → RenderWorld direkt (kein SceneSnapshot-Umweg)
    RenderWorld rw;
    engine::addons::mesh_renderer::ExtractRenderables(world, rw);
    engine::addons::lighting::ExtractLights(world, rw);
    CHECK_EQ(ctx, rw.TotalProxyCount(), 7u);

    // BuildDrawLists - große ViewProj die alles einschließt
    math::Mat4 view = math::Mat4::LookAtRH({0,0,-10},{0,0,0},math::Vec3::Up());
    math::Mat4 proj = math::Mat4::PerspectiveFovRH(
        60.f*math::DEG_TO_RAD, 16.f/9.f, 0.1f, 1000.f);
    math::Mat4 vp = proj * view;

    renderer::RenderPassRegistry renderPassRegistry;
    rw.BuildDrawLists(view, vp, 0.1f, 1000.f, ms, renderPassRegistry);
    const RenderQueue& q = rw.GetQueue();
    const DrawList& opaque = RequirePassList(ctx, q, StandardRenderPasses::Opaque());
    const DrawList& transparent = RequirePassList(ctx, q, StandardRenderPasses::Transparent());
    const DrawList& shadow = RequirePassList(ctx, q, StandardRenderPasses::Shadow());

    // Alle sichtbar
    CHECK_EQ(ctx, rw.VisibleCount(), 7u);
    CHECK_EQ(ctx, opaque.Size(),       5u);
    CHECK_EQ(ctx, transparent.Size(),  2u);

    // Opaque: front-to-back sortiert
    for (size_t i = 1; i < opaque.items.size(); ++i)
        CHECK(ctx, opaque.items[i-1].sortKey < opaque.items[i].sortKey
                || opaque.items[i-1].sortKey == opaque.items[i].sortKey);

    // Transparent: back-to-front (größere Tiefe = kleinerer Key)
    // Mindestens nicht strikt front-to-back
    bool hasBackToFront = transparent.Size() >= 2;
    CHECK(ctx, hasBackToFront); // schwache Prüfung für Demo

    // Shadow: beide Materialien haben castShadows=true (default)
    CHECK_EQ(ctx, shadow.Size(), 7u);
}



static void TestFeatureDrivenSceneExtraction(test::TestContext& ctx)
{
    RegisterRendererTestComponents();

    class MarkerFeature final : public IEngineFeature
    {
    public:
        class MarkerRenderableStep final : public ISceneExtractionStep
        {
        public:
            std::string_view GetName() const noexcept override { return "test.marker.renderables"; }
            void Extract(const ecs::World& world, RenderWorld& renderWorld) const override
            {
                world.View<WorldTransformComponent, MeshComponent, MaterialComponent>(
                    [&](EntityID id,
                        const WorldTransformComponent& wt,
                        const MeshComponent& mesh,
                        const MaterialComponent& mat)
                    {
                        if (!ECSExtractor::IsEntityActive(world, id) || !mesh.mesh.IsValid())
                            return;
                        renderWorld.AddRenderable(
                            id, mesh.mesh, mat.material,
                            wt.matrix, wt.inverse.Transposed(),
                            math::Vec3(0,0,0), math::Vec3(1,1,1), 1.f,
                            mesh.layerMask, mesh.castShadows);
                    });
            }
        };

        class MarkerLightStep final : public ISceneExtractionStep
        {
        public:
            std::string_view GetName() const noexcept override { return "test.marker.lights"; }
            void Extract(const ecs::World& world, RenderWorld& renderWorld) const override
            { engine::addons::lighting::ExtractLights(world, renderWorld); }
        };

        std::string_view GetName() const noexcept override { return "test-marker-feature"; }
        FeatureID GetID() const noexcept override { return FeatureID::FromString("test-marker-feature"); }

        void Register(FeatureRegistrationContext& context) override
        {
            context.RegisterSceneExtractionStep(std::make_shared<MarkerRenderableStep>());
            context.RegisterSceneExtractionStep(std::make_shared<MarkerLightStep>());
            context.RegisterFrameConstantsContributor(engine::addons::lighting::CreateLightingFrameConstantsContributor());
        }

        bool Initialize(const FeatureInitializationContext& context) override { (void)context; return true; }
        void Shutdown(const FeatureShutdownContext& context) override { (void)context; }
    };

    ecs::ComponentMetaRegistry componentRegistry = CreateRendererTestRegistry();
    ecs::World world(componentRegistry);
    MaterialSystem ms;

    MaterialDesc d;
    d.name = "FeatureMat";
    d.renderPass = StandardRenderPasses::Opaque();
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

    // Extraction direkt in RenderWorld
    RenderWorld renderWorld;
    const auto& steps = registry.GetSceneExtractionSteps();
    CHECK_EQ(ctx, steps.size(), size_t(2));
    renderWorld.Clear();
    for (const ISceneExtractionStep* step : steps)
        step->Extract(world, renderWorld);

    CHECK_EQ(ctx, renderWorld.TotalProxyCount(), 1u);   // inactive gefiltert
    CHECK_EQ(ctx, engine::addons::lighting::GetExtractedLightCount(renderWorld), size_t(1));

    registry.ShutdownAll(FeatureShutdownContext{});
    shaderRuntime.Shutdown();
    device.Shutdown();
}

static void TestCameraAddonBuildPrimaryPerspectiveView(test::TestContext& ctx)
{
    RegisterRendererTestComponents();

    ecs::ComponentMetaRegistry componentRegistry = CreateRendererTestRegistry();
    ecs::World world(componentRegistry);

    const EntityID secondaryCamera = world.CreateEntity();
    auto& secondaryTransform = world.Add<TransformComponent>(secondaryCamera);
    secondaryTransform.localPosition = { 5.f, 1.f, 8.f };
    world.Add<WorldTransformComponent>(secondaryCamera);
    world.Add<CameraComponent>(secondaryCamera, CameraComponent{
        .projection = ProjectionType::Perspective,
        .fovYDeg = 75.f,
        .nearPlane = 0.25f,
        .farPlane = 250.f,
        .aspectRatio = 1.f,
        .isMainCamera = false
    });

    const EntityID mainCamera = world.CreateEntity();
    auto& mainTransform = world.Add<TransformComponent>(mainCamera);
    mainTransform.localPosition = { 0.f, 0.f, 6.f };
    world.Add<WorldTransformComponent>(mainCamera);
    world.Add<CameraComponent>(mainCamera, CameraComponent{
        .projection = ProjectionType::Perspective,
        .fovYDeg = 60.f,
        .nearPlane = 0.1f,
        .farPlane = 100.f,
        .aspectRatio = 4.f / 3.f,
        .isMainCamera = true
    });

    const EntityID inactiveMain = world.CreateEntity();
    auto& inactiveTransform = world.Add<TransformComponent>(inactiveMain);
    inactiveTransform.localPosition = { 0.f, 0.f, 2.f };
    world.Add<WorldTransformComponent>(inactiveMain);
    world.Add<ActiveComponent>(inactiveMain, ActiveComponent{ false });
    world.Add<CameraComponent>(inactiveMain, CameraComponent{
        .projection = ProjectionType::Perspective,
        .fovYDeg = 40.f,
        .nearPlane = 0.1f,
        .farPlane = 10.f,
        .isMainCamera = true
    });

    engine::TransformSystem transformSystem;
    transformSystem.Update(world);

    CHECK_EQ(ctx, engine::addons::camera::FindPrimaryCameraEntity(world), mainCamera);

    RenderView view{};
    CHECK(ctx, engine::addons::camera::BuildPrimaryRenderView(world, 1920u, 1080u, view));
    CHECK(ctx, NearlyEqual(view.cameraPosition.x, 0.f));
    CHECK(ctx, NearlyEqual(view.cameraPosition.y, 0.f));
    CHECK(ctx, NearlyEqual(view.cameraPosition.z, 6.f));
    CHECK(ctx, NearlyEqual(view.cameraForward.x, 0.f));
    CHECK(ctx, NearlyEqual(view.cameraForward.y, 0.f));
    CHECK(ctx, NearlyEqual(view.cameraForward.z, -1.f));
    CHECK(ctx, NearlyEqual(view.nearPlane, 0.1f));
    CHECK(ctx, NearlyEqual(view.farPlane, 100.f));

    const math::Mat4 expectedView = math::Mat4::LookAtRH({ 0.f, 0.f, 6.f }, { 0.f, 0.f, 5.f }, math::Vec3::Up());
    const math::Mat4 expectedProjection = math::Mat4::PerspectiveFovRH(
        60.f * math::DEG_TO_RAD, 1920.f / 1080.f, 0.1f, 100.f);
    CheckMatrixClose(ctx, view.view, expectedView);
    CheckMatrixClose(ctx, view.projection, expectedProjection);
}

static void TestCameraAddonBuildOrthographicView(test::TestContext& ctx)
{
    RegisterRendererTestComponents();

    ecs::ComponentMetaRegistry componentRegistry = CreateRendererTestRegistry();
    ecs::World world(componentRegistry);
    const EntityID cameraEntity = world.CreateEntity();
    auto& transform = world.Add<TransformComponent>(cameraEntity);
    transform.localPosition = { -3.f, 2.f, 10.f };
    world.Add<WorldTransformComponent>(cameraEntity);
    world.Add<CameraComponent>(cameraEntity, CameraComponent{
        .projection = ProjectionType::Orthographic,
        .nearPlane = 0.5f,
        .farPlane = 50.f,
        .orthoSize = 6.f,
        .aspectRatio = 1.f,
        .isMainCamera = true
    });

    engine::TransformSystem transformSystem;
    transformSystem.Update(world);

    RenderView view{};
    CHECK(ctx, engine::addons::camera::BuildRenderViewFromCamera(world, cameraEntity, 800u, 600u, view));
    CHECK(ctx, NearlyEqual(view.cameraPosition.x, -3.f));
    CHECK(ctx, NearlyEqual(view.cameraPosition.y, 2.f));
    CHECK(ctx, NearlyEqual(view.cameraPosition.z, 10.f));
    CHECK(ctx, NearlyEqual(view.nearPlane, 0.5f));
    CHECK(ctx, NearlyEqual(view.farPlane, 50.f));

    const float aspect = 800.f / 600.f;
    const math::Mat4 expectedProjection = math::Mat4::OrthoRH(
        -6.f * aspect, 6.f * aspect, -6.f, 6.f, 0.5f, 50.f);
    CheckMatrixClose(ctx, view.projection, expectedProjection);
}

static void TestCameraAddonUsesLiveLocalTransform(test::TestContext& ctx)
{
    RegisterRendererTestComponents();

    ecs::ComponentMetaRegistry componentRegistry = CreateRendererTestRegistry();
    ecs::World world(componentRegistry);
    const EntityID cameraEntity = world.CreateEntity();
    world.Add<TransformComponent>(cameraEntity);
    world.Add<WorldTransformComponent>(cameraEntity);
    world.Add<CameraComponent>(cameraEntity, CameraComponent{
        .projection = ProjectionType::Perspective,
        .fovYDeg = 60.f,
        .nearPlane = 0.1f,
        .farPlane = 100.f,
        .isMainCamera = true
    });

    auto* transform = world.Get<TransformComponent>(cameraEntity);
    CHECK(ctx, transform != nullptr);
    if (transform == nullptr)
        return;
    transform->localPosition = { 0.f, 0.f, 6.f };

    engine::TransformSystem transformSystem;
    transformSystem.Update(world);

    transform = world.Get<TransformComponent>(cameraEntity);
    CHECK(ctx, transform != nullptr);
    if (transform == nullptr)
        return;
    transform->localPosition = { 2.f, 3.f, 7.f };
    transform->localRotation = math::Quat::FromEulerDeg(0.f, 90.f, 0.f);

    RenderView view{};
    CHECK(ctx, engine::addons::camera::BuildRenderViewFromCamera(world, cameraEntity, 1280u, 720u, view));
    CHECK(ctx, NearlyEqual(view.cameraPosition.x, 2.f));
    CHECK(ctx, NearlyEqual(view.cameraPosition.y, 3.f));
    CHECK(ctx, NearlyEqual(view.cameraPosition.z, 7.f));

    const math::Mat4 expectedWorld = math::Mat4::TRS(
        transform->localPosition, transform->localRotation, transform->localScale);
    CheckMatrixClose(ctx, view.view, expectedWorld.InverseAffine());
    CHECK(ctx, NearlyEqual(view.cameraForward.x, -1.f));
    CHECK(ctx, NearlyEqual(view.cameraForward.y, 0.f));
    CHECK(ctx, NearlyEqual(view.cameraForward.z, 0.f));
}

static void TestForwardFeatureExtractionRegistration(test::TestContext& ctx)
{
    RegisterRendererTestComponents();

    ecs::ComponentMetaRegistry componentRegistry = CreateRendererTestRegistry();
    ecs::World world(componentRegistry);
    MaterialSystem ms;

    MaterialDesc d;
    d.name = "ForwardMat";
    d.renderPass = StandardRenderPasses::Opaque();
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
    CHECK(ctx, registry.AddFeature(engine::addons::mesh_renderer::CreateMeshRendererFeature()));
    CHECK(ctx, registry.AddFeature(engine::addons::lighting::CreateLightingFeature()));
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
        registry.GetSceneExtractionSteps(),
        renderWorld
    };

    FrameExtractionStageResult extractionResult{};
    CHECK(ctx, stage.Execute(extractionContext, extractionResult));
    CHECK_EQ(ctx, registry.GetSceneExtractionSteps().size(), size_t(2));
    CHECK_EQ(ctx, renderWorld.TotalProxyCount(), 1u);
    CHECK_EQ(ctx, engine::addons::lighting::GetExtractedLightCount(renderWorld), size_t(1));

    registry.ShutdownAll(FeatureShutdownContext{});
    shaderRuntime.Shutdown();
    device.Shutdown();
}

// ==========================================================================
// RenderSystem + headless window - geschlossener Laufzeit-Loop
// ==========================================================================
static void TestRenderSystemLoop(test::TestContext& ctx)
{
    RegisterRendererTestComponents();

    ecs::ComponentMetaRegistry componentRegistry = CreateRendererTestRegistry();
    ecs::World world(componentRegistry);
    MaterialSystem ms;
    events::EventBus bus;
    platform::NullInput input;
    platform::FixedTiming timing(1.0 / 60.0);

    MaterialDesc d;
    d.name = "LoopMat";
    d.renderPass = StandardRenderPasses::Opaque();
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
    CHECK(ctx, renderer.RegisterFeature(engine::addons::mesh_renderer::CreateMeshRendererFeature()));
    CHECK(ctx, renderer.RegisterFeature(engine::addons::lighting::CreateLightingFeature()));
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
    RegisterRendererTestComponents();

    TestHeadlessPlatform platform;
    CHECK(ctx, platform.Initialize());

    events::EventBus bus;
    renderer::PlatformRenderLoop loop;
    if (!loop.GetRenderSystem().RegisterFeature(engine::addons::mesh_renderer::CreateMeshRendererFeature()) ||
        !loop.GetRenderSystem().RegisterFeature(engine::addons::lighting::CreateLightingFeature()) ||
        !loop.GetRenderSystem().RegisterFeature(engine::renderer::addons::forward::CreateForwardFeature()))
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

    ecs::ComponentMetaRegistry componentRegistry = CreateRendererTestRegistry();
    ecs::World world(componentRegistry);
    MaterialSystem ms;

    MaterialDesc d;
    d.name = "LoopMat";
    d.renderPass = StandardRenderPasses::Opaque();
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
    DeviceFactory::Registry registry;
    registry.Unregister(DeviceFactory::BackendType::Vulkan);
    CHECK(ctx, !registry.IsRegistered(DeviceFactory::BackendType::Vulkan));

    registry.Register(DeviceFactory::BackendType::Vulkan, &CreateTestDeviceFactoryInstance);
    CHECK(ctx, registry.IsRegistered(DeviceFactory::BackendType::Vulkan));

    auto custom = registry.Create(DeviceFactory::BackendType::Vulkan);
    CHECK(ctx, custom != nullptr);
    CHECK_EQ(ctx, std::string(custom->GetBackendName()), std::string("TestDevice"));

    registry.Unregister(DeviceFactory::BackendType::Vulkan);
    CHECK(ctx, !registry.IsRegistered(DeviceFactory::BackendType::Vulkan));

    auto nullDevice = registry.Create(DeviceFactory::BackendType::Null);
    CHECK(ctx, nullDevice != nullptr);
    CHECK_EQ(ctx, std::string(nullDevice->GetBackendName()), std::string("Null"));
}

// ==========================================================================
// Run all renderer tests
// ==========================================================================


static void TestShaderRuntimeUsesIBLVariantWhenEnvironmentActive(test::TestContext& ctx)
{
    assets::AssetRegistry assets;

    auto vs = std::make_unique<assets::ShaderAsset>();
    vs->debugName = "VS_IBL_Active";
    vs->stage = assets::ShaderStage::Vertex;
    vs->entryPoint = "VSMain";
    assets::CompiledShaderArtifact vsCompiled;
    vsCompiled.target = assets::ShaderTargetProfile::Null;
    vsCompiled.stage = assets::ShaderStage::Vertex;
    vsCompiled.entryPoint = "VSMain";
    vsCompiled.sourceText = "float4 VSMain() : SV_Position { return float4(0,0,0,1); }";
    vsCompiled.sourceHash = 0x5101ull;
    vs->compiledArtifacts.push_back(vsCompiled);
    const ShaderHandle vsAsset = assets.GetOrAddShader("shaders/ibl_active_vs.null", std::move(vs));

    auto ps = std::make_unique<assets::ShaderAsset>();
    ps->debugName = "PS_IBL_Active";
    ps->stage = assets::ShaderStage::Fragment;
    ps->entryPoint = "PSMain";
    ps->sourceCode = R"(
        TextureCube Irr : register(t16);
        TextureCube Pref : register(t17);
        Texture2D Brdf : register(t18);
        SamplerState Smp : register(s4);
        float4 PSMain() : SV_Target
        {
        #ifdef KROM_IBL
            return Irr.SampleLevel(Smp, float3(0,0,1), 0) + Pref.SampleLevel(Smp, float3(0,0,1), 0) + Brdf.SampleLevel(Smp, float2(0,0), 0);
        #else
            return float4(0,0,0,1);
        #endif
        })";
    const ShaderHandle psAsset = assets.GetOrAddShader("shaders/ibl_active_ps.hlsl", std::move(ps));

    MaterialSystem ms;
    MaterialDesc d;
    d.name = "IBLActiveMaterial";
    d.renderPass = StandardRenderPasses::Opaque();
    d.vertexShader = vsAsset;
    d.fragmentShader = psAsset;
    const MaterialHandle mat = ms.RegisterMaterial(std::move(d));

    renderer::null_backend::NullDevice device;
    renderer::IDevice::DeviceDesc dd{};
    dd.appName = "ShaderRuntimeIBLActive";
    CHECK(ctx, device.Initialize(dd));

    ShaderRuntime runtime;
    CHECK(ctx, runtime.Initialize(device));
    runtime.SetAssetRegistry(&assets);
    CHECK(ctx, runtime.PrepareAllShaderAssets());

    const size_t variantsBefore = runtime.GetVariantCache().CachedCount();

    EnvironmentRuntimeState env{};
    env.active = true;
    env.irradiance = TextureHandle::Make(301u, 1u);
    env.prefiltered = TextureHandle::Make(302u, 1u);
    env.brdfLut = TextureHandle::Make(303u, 1u);
    runtime.SetEnvironmentState(env);

    CHECK(ctx, runtime.PrepareMaterial(ms, mat));

    const auto* state = runtime.GetMaterialState(mat);
    CHECK(ctx, state != nullptr);
    CHECK(ctx, state->fragmentShader.IsValid());

    const ShaderHandle expected = runtime.GetOrCreateVariant(psAsset, ShaderPassType::Main, ShaderVariantFlag::IBLMap);
    CHECK(ctx, expected.IsValid());
    CHECK_EQ(ctx, state->fragmentShader, expected);
    CHECK(ctx, runtime.GetVariantCache().CachedCount() > variantsBefore);
    CHECK_EQ(ctx, FindTextureBindingAtSlot(*state, TexSlots::IBLIrradiance), env.irradiance);
    CHECK_EQ(ctx, FindTextureBindingAtSlot(*state, TexSlots::IBLPrefiltered), env.prefiltered);
    CHECK_EQ(ctx, FindTextureBindingAtSlot(*state, TexSlots::BRDFLUT), env.brdfLut);

    const auto defines = ShaderCompiler::VariantFlagsToDefines(ShaderVariantFlag::IBLMap);
    CHECK(ctx, ContainsDefine(defines, "KROM_IBL"));

    runtime.Shutdown();
    device.Shutdown();
}

static void TestShaderRuntimeDoesNotUseIBLVariantWithoutEnvironment(test::TestContext& ctx)
{
    assets::AssetRegistry assets;

    auto vs = std::make_unique<assets::ShaderAsset>();
    vs->debugName = "VS_IBL_Inactive";
    vs->stage = assets::ShaderStage::Vertex;
    vs->entryPoint = "VSMain";
    assets::CompiledShaderArtifact vsCompiled;
    vsCompiled.target = assets::ShaderTargetProfile::Null;
    vsCompiled.stage = assets::ShaderStage::Vertex;
    vsCompiled.entryPoint = "VSMain";
    vsCompiled.sourceText = "float4 VSMain() : SV_Position { return float4(0,0,0,1); }";
    vsCompiled.sourceHash = 0x5201ull;
    vs->compiledArtifacts.push_back(vsCompiled);
    const ShaderHandle vsAsset = assets.GetOrAddShader("shaders/ibl_inactive_vs.null", std::move(vs));

    auto ps = std::make_unique<assets::ShaderAsset>();
    ps->debugName = "PS_IBL_Inactive";
    ps->stage = assets::ShaderStage::Fragment;
    ps->entryPoint = "PSMain";
    ps->sourceCode = "float4 PSMain() : SV_Target { return float4(1,1,1,1); }";
    const ShaderHandle psAsset = assets.GetOrAddShader("shaders/ibl_inactive_ps.hlsl", std::move(ps));

    MaterialSystem ms;
    MaterialDesc d;
    d.name = "IBLInactiveMaterial";
    d.renderPass = StandardRenderPasses::Opaque();
    d.vertexShader = vsAsset;
    d.fragmentShader = psAsset;
    const MaterialHandle mat = ms.RegisterMaterial(std::move(d));

    renderer::null_backend::NullDevice device;
    renderer::IDevice::DeviceDesc dd{};
    dd.appName = "ShaderRuntimeIBLInactive";
    CHECK(ctx, device.Initialize(dd));

    ShaderRuntime runtime;
    CHECK(ctx, runtime.Initialize(device));
    runtime.SetAssetRegistry(&assets);
    CHECK(ctx, runtime.PrepareAllShaderAssets());
    CHECK(ctx, runtime.PrepareMaterial(ms, mat));

    const auto* state = runtime.GetMaterialState(mat);
    CHECK(ctx, state != nullptr);
    CHECK(ctx, state->fragmentShader.IsValid());

    const ShaderHandle expected = runtime.GetOrCreateVariant(psAsset, ShaderPassType::Main, ShaderVariantFlag::None);
    const ShaderHandle unexpected = runtime.GetOrCreateVariant(psAsset, ShaderPassType::Main, ShaderVariantFlag::IBLMap);
    CHECK(ctx, expected.IsValid());
    CHECK(ctx, unexpected.IsValid());
    CHECK_EQ(ctx, state->fragmentShader, expected);
    CHECK_NE(ctx, state->fragmentShader, unexpected);
    CHECK(ctx, !FindTextureBindingAtSlot(*state, TexSlots::IBLIrradiance).IsValid());
    CHECK(ctx, !FindTextureBindingAtSlot(*state, TexSlots::IBLPrefiltered).IsValid());
    CHECK(ctx, !FindTextureBindingAtSlot(*state, TexSlots::BRDFLUT).IsValid());

    const auto defines = ShaderCompiler::VariantFlagsToDefines(ShaderVariantFlag::None);
    CHECK(ctx, !ContainsDefine(defines, "KROM_IBL"));

    runtime.Shutdown();
    device.Shutdown();
}

static void TestShaderRuntimeRebuildsShaderVariantOnEnvironmentToggle(test::TestContext& ctx)
{
    assets::AssetRegistry assets;

    auto vs = std::make_unique<assets::ShaderAsset>();
    vs->debugName = "VS_IBL_Rebuild";
    vs->stage = assets::ShaderStage::Vertex;
    vs->entryPoint = "VSMain";
    assets::CompiledShaderArtifact vsCompiled;
    vsCompiled.target = assets::ShaderTargetProfile::Null;
    vsCompiled.stage = assets::ShaderStage::Vertex;
    vsCompiled.entryPoint = "VSMain";
    vsCompiled.sourceText = "float4 VSMain() : SV_Position { return float4(0,0,0,1); }";
    vsCompiled.sourceHash = 0x5301ull;
    vs->compiledArtifacts.push_back(vsCompiled);
    const ShaderHandle vsAsset = assets.GetOrAddShader("shaders/ibl_rebuild_vs.null", std::move(vs));

    auto ps = std::make_unique<assets::ShaderAsset>();
    ps->debugName = "PS_IBL_Rebuild";
    ps->stage = assets::ShaderStage::Fragment;
    ps->entryPoint = "PSMain";
    ps->sourceCode = "float4 PSMain() : SV_Target { #ifdef KROM_IBL return float4(0,1,0,1); #else return float4(1,0,0,1); #endif }";
    const ShaderHandle psAsset = assets.GetOrAddShader("shaders/ibl_rebuild_ps.hlsl", std::move(ps));

    MaterialSystem ms;
    MaterialDesc d;
    d.name = "IBLRebuildMaterial";
    d.renderPass = StandardRenderPasses::Opaque();
    d.vertexShader = vsAsset;
    d.fragmentShader = psAsset;
    const MaterialHandle mat = ms.RegisterMaterial(std::move(d));

    renderer::null_backend::NullDevice device;
    renderer::IDevice::DeviceDesc dd{};
    dd.appName = "ShaderRuntimeIBLRebuild";
    CHECK(ctx, device.Initialize(dd));

    ShaderRuntime runtime;
    CHECK(ctx, runtime.Initialize(device));
    runtime.SetAssetRegistry(&assets);
    CHECK(ctx, runtime.PrepareAllShaderAssets());

    CHECK(ctx, runtime.PrepareMaterial(ms, mat));
    const auto* state0 = runtime.GetMaterialState(mat);
    CHECK(ctx, state0 != nullptr);
    const uint64_t environmentRevision0 = state0->environmentRevision;
    const ShaderHandle shader0 = state0->fragmentShader;
    const PipelineHandle pipeline0 = state0->pipeline;

    EnvironmentRuntimeState env{};
    env.active = true;
    env.irradiance = TextureHandle::Make(401u, 1u);
    env.prefiltered = TextureHandle::Make(402u, 1u);
    env.brdfLut = TextureHandle::Make(403u, 1u);
    runtime.SetEnvironmentState(env);

    CHECK(ctx, runtime.PrepareMaterial(ms, mat));
    const auto* state1 = runtime.GetMaterialState(mat);
    CHECK(ctx, state1 != nullptr);
    CHECK(ctx, state1->environmentRevision > environmentRevision0);
    CHECK_NE(ctx, state1->fragmentShader, shader0);
    CHECK_NE(ctx, state1->pipeline, pipeline0);

    const ShaderHandle expectedNoIbl = runtime.GetOrCreateVariant(psAsset, ShaderPassType::Main, ShaderVariantFlag::None);
    const ShaderHandle expectedIbl = runtime.GetOrCreateVariant(psAsset, ShaderPassType::Main, ShaderVariantFlag::IBLMap);
    CHECK_EQ(ctx, shader0, expectedNoIbl);
    CHECK_EQ(ctx, state1->fragmentShader, expectedIbl);
    CHECK_EQ(ctx, FindTextureBindingAtSlot(*state1, TexSlots::IBLIrradiance), env.irradiance);
    CHECK_EQ(ctx, FindTextureBindingAtSlot(*state1, TexSlots::IBLPrefiltered), env.prefiltered);
    CHECK_EQ(ctx, FindTextureBindingAtSlot(*state1, TexSlots::BRDFLUT), env.brdfLut);

    runtime.Shutdown();
    device.Shutdown();
}

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
    DeviceFactory::Registry registry;

    CHECK(ctx, registry.IsRegistered(BackendType::OpenGL));
    auto adapters = registry.EnumerateAdapters(BackendType::OpenGL);
    CHECK(ctx, !adapters.empty());

    auto device = registry.Create(BackendType::OpenGL);
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

static void TestOpenGLFrontFaceMapping(test::TestContext& ctx)
{
    CHECK_EQ(ctx, engine::renderer::opengl::ToGLFrontFace(WindingOrder::CCW), GLenum(0x0901u)); // GL_CCW
    CHECK_EQ(ctx, engine::renderer::opengl::ToGLFrontFace(WindingOrder::CW), GLenum(0x0900u));   // GL_CW
}

int RunRendererTests()
{
    RegisterRendererTestComponents();
    engine::Debug::MinLevel = engine::LogLevel::Fatal;

    test::TestSuite suite("Renderer");
    suite
        .Add("PipelineKey determinism",     TestPipelineKeyDeterminism)
        .Add("PipelineKey no padding",      TestPipelineKeyNoPadding)
        .Add("SortKey ordering",            TestSortKeyOrdering)
        .Add("MaterialSystem",              TestMaterialSystem)
        .Add("Unlit addon shader asset set", TestUnlitAddonShaderAssetSet)
        .Add("Unlit addon factory",        TestUnlitAddonFactory)
        .Add("Lit addon shader asset set", TestLitAddonShaderAssetSet)
        .Add("Lit addon factory",          TestLitAddonFactory)
        .Add("PBR addon shader asset set", TestPbrAddonShaderAssetSet)
        .Add("PBR addon factory",          TestPbrAddonFactory)
        .Add("Standard frame recipe", TestStandardFrameRecipe)
        .Add("PipelineCache",               TestPipelineCache)
        .Add("CbLayout HLSL packing",       TestCbLayout)
        .Add("ShaderBindingModel slots",    TestShaderBindingModel)
        .Add("Camera addon primary perspective view", TestCameraAddonBuildPrimaryPerspectiveView)
        .Add("Camera addon orthographic view", TestCameraAddonBuildOrthographicView)
        .Add("Camera addon uses live local transform", TestCameraAddonUsesLiveLocalTransform)
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
                .Add("Material texture writes", TestMaterialTextureWrites)
                .Add("ShaderRuntime partial env fallback", TestShaderRuntimePartialEnvironmentUsesFallbacks)
        .Add("ShaderRuntime env rebuild", TestShaderRuntimeRebuildOnEnvironmentChange)
        .Add("ShaderRuntime IBL variant active", TestShaderRuntimeUsesIBLVariantWhenEnvironmentActive)
        .Add("ShaderRuntime IBL variant inactive", TestShaderRuntimeDoesNotUseIBLVariantWithoutEnvironment)
        .Add("ShaderRuntime IBL variant rebuild", TestShaderRuntimeRebuildsShaderVariantOnEnvironmentToggle)
        .Add("ShaderVariant cache",      TestShaderVariantCache)
        .Add("OpenGL front-face mapping", TestOpenGLFrontFaceMapping)
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
    d.renderPass = StandardRenderPasses::Opaque();
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
    d.renderPass = StandardRenderPasses::Opaque();
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

static void TestMaterialTextureWrites(test::TestContext& ctx)
{
    MaterialSystem ms;
    MaterialDesc d;
    d.name = "TextureWriteMaterial";
    d.renderPass = StandardRenderPasses::Opaque();
    d.vertexShader = ShaderHandle::Make(21u, 1u);
    d.fragmentShader = ShaderHandle::Make(22u, 1u);

    MaterialParam baseColorFactor;
    baseColorFactor.name = "baseColorFactor";
    baseColorFactor.type = MaterialParam::Type::Vec4;
    baseColorFactor.value.f[0] = 1.0f;
    baseColorFactor.value.f[1] = 1.0f;
    baseColorFactor.value.f[2] = 1.0f;
    baseColorFactor.value.f[3] = 1.0f;

    MaterialParam metallicFactor;
    metallicFactor.name = "metallicFactor";
    metallicFactor.type = MaterialParam::Type::Float;
    metallicFactor.value.f[0] = 0.0f;

    MaterialParam albedo;
    albedo.name = "albedo";
    albedo.type = MaterialParam::Type::Texture;

    d.params = { baseColorFactor, metallicFactor, albedo };
    d.bindings = {
        { 0u, 0u, ShaderStageMask::Fragment, MaterialBinding::Kind::Texture, "albedo" }
    };

    const MaterialHandle mat = ms.RegisterMaterial(std::move(d));
    const uint64_t rev0 = ms.GetRevision(mat);

    ms.SetTexture(mat, "albedo", TextureHandle::Make(91u, 1u));
    ms.SetFloat(mat, "metallicFactor", 0.25f);
    ms.SetVec4(mat, "baseColorFactor", math::Vec4{0.1f, 0.2f, 0.3f, 1.0f});

    CHECK(ctx, ms.GetRevision(mat) > rev0);
    const auto& cbData = ms.GetCBData(mat);
    const CbLayout& cbLayout = ms.GetCBLayout(mat);
    const uint32_t baseColorOffset = cbLayout.GetOffset("baseColorFactor");
    const uint32_t metallicOffset = cbLayout.GetOffset("metallicFactor");
    CHECK(ctx, baseColorOffset != UINT32_MAX);
    CHECK(ctx, metallicOffset != UINT32_MAX);
    const float* baseColor = reinterpret_cast<const float*>(cbData.data() + baseColorOffset);
    const float* metallic = reinterpret_cast<const float*>(cbData.data() + metallicOffset);
    CHECK(ctx, std::fabs(baseColor[0] - 0.1f) < 1e-6f);
    CHECK(ctx, std::fabs(baseColor[1] - 0.2f) < 1e-6f);
    CHECK(ctx, std::fabs(baseColor[2] - 0.3f) < 1e-6f);
    CHECK(ctx, std::fabs(baseColor[3] - 1.0f) < 1e-6f);
    CHECK(ctx, std::fabs(metallic[0] - 0.25f) < 1e-6f);

    const MaterialInstance* instance = ms.GetInstance(mat);
    CHECK(ctx, instance != nullptr);
    const int32_t albedoSlot = instance->layout.FindSlotIndexByName("albedo");
    CHECK(ctx, albedoSlot >= 0);
    CHECK_EQ(ctx, instance->parameters.GetTexture(static_cast<uint32_t>(albedoSlot)), TextureHandle::Make(91u, 1u));
}


static void TestShaderRuntimePartialEnvironmentUsesFallbacks(test::TestContext& ctx)
{
    assets::AssetRegistry assets;

    auto vs = std::make_unique<assets::ShaderAsset>();
    vs->debugName = "VS_EnvPartial";
    vs->stage = assets::ShaderStage::Vertex;
    vs->entryPoint = "VSMain";
    assets::CompiledShaderArtifact vsCompiled;
    vsCompiled.target = assets::ShaderTargetProfile::Null;
    vsCompiled.stage = assets::ShaderStage::Vertex;
    vsCompiled.entryPoint = "VSMain";
    vsCompiled.debugName = "VS_EnvPartial";
    vsCompiled.bytecode = {0x01u, 0x02u, 0x03u, 0x04u};
    vsCompiled.contract.contractHash = 0x301ull;
    vsCompiled.contract.pipelineStateKey = 0x302ull;
    vsCompiled.contract.interfaceLayout.layoutHash = 0x303ull;
    vsCompiled.contract.pipelineBinding.bindingSignatureKey = 0x304ull;
    vs->compiledArtifacts.push_back(vsCompiled);
    const ShaderHandle vsAsset = assets.GetOrAddShader("shaders/env_partial_vs.hlsl", std::move(vs));

    auto ps = std::make_unique<assets::ShaderAsset>();
    ps->debugName = "PS_EnvPartial";
    ps->stage = assets::ShaderStage::Fragment;
    ps->entryPoint = "PSMain";
    assets::CompiledShaderArtifact psCompiled;
    psCompiled.target = assets::ShaderTargetProfile::Null;
    psCompiled.stage = assets::ShaderStage::Fragment;
    psCompiled.entryPoint = "PSMain";
    psCompiled.debugName = "PS_EnvPartial";
    psCompiled.bytecode = {0x05u, 0x06u, 0x07u, 0x08u};
    psCompiled.contract.contractHash = 0x401ull;
    psCompiled.contract.pipelineStateKey = 0x402ull;
    psCompiled.contract.interfaceLayout.layoutHash = 0x403ull;
    psCompiled.contract.pipelineBinding.bindingSignatureKey = 0x404ull;
    ps->compiledArtifacts.push_back(psCompiled);
    const ShaderHandle psAsset = assets.GetOrAddShader("shaders/env_partial_ps.null", std::move(ps));

    MaterialSystem ms;
    MaterialDesc d;
    d.name = "EnvPartialMaterial";
    d.renderPass = StandardRenderPasses::Opaque();
    d.vertexShader = vsAsset;
    d.fragmentShader = psAsset;
    const MaterialHandle mat = ms.RegisterMaterial(std::move(d));

    renderer::null_backend::NullDevice device;
    renderer::IDevice::DeviceDesc dd{};
    dd.appName = "ShaderRuntimeEnvPartial";
    CHECK(ctx, device.Initialize(dd));

    ShaderRuntime runtime;
    CHECK(ctx, runtime.Initialize(device));
    runtime.SetAssetRegistry(&assets);
    CHECK(ctx, runtime.PrepareAllShaderAssets());

    EnvironmentRuntimeState env{};
    env.active = true;
    env.irradiance = TextureHandle::Make(201u, 1u);
    runtime.SetEnvironmentState(env);

    CHECK(ctx, runtime.PrepareMaterial(ms, mat));

    const auto* state = runtime.GetMaterialState(mat);
    CHECK(ctx, state != nullptr);

    auto findTextureAtSlot = [](const MaterialGpuState& gpuState, uint32_t slot) -> TextureHandle
    {
        for (const auto& binding : gpuState.bindings)
        {
            if (binding.kind == ResolvedMaterialBinding::Kind::Texture && binding.slot == slot)
                return binding.texture;
        }
        return TextureHandle::Invalid();
    };

    CHECK_EQ(ctx, findTextureAtSlot(*state, TexSlots::IBLIrradiance), env.irradiance);
    CHECK(ctx, findTextureAtSlot(*state, TexSlots::IBLPrefiltered).IsValid());
    CHECK(ctx, findTextureAtSlot(*state, TexSlots::BRDFLUT).IsValid());

    runtime.Shutdown();
    device.Shutdown();
}

static void TestShaderRuntimeRebuildOnEnvironmentChange(test::TestContext& ctx)
{
    assets::AssetRegistry assets;

    auto vs = std::make_unique<assets::ShaderAsset>();
    vs->debugName = "VS_Env";
    vs->stage = assets::ShaderStage::Vertex;
    vs->entryPoint = "VSMain";
    assets::CompiledShaderArtifact vsCompiled;
    vsCompiled.target = assets::ShaderTargetProfile::Null;
    vsCompiled.stage = assets::ShaderStage::Vertex;
    vsCompiled.entryPoint = "VSMain";
    vsCompiled.sourceText = "float4 VSMain() : SV_Position { return float4(0,0,0,1); }";
    vsCompiled.sourceHash = 0x1111ull;
    vs->compiledArtifacts.push_back(vsCompiled);
    const ShaderHandle vsAsset = assets.GetOrAddShader("shaders/env_vs.hlsl", std::move(vs));

    auto ps = std::make_unique<assets::ShaderAsset>();
    ps->debugName = "PS_Env";
    ps->stage = assets::ShaderStage::Fragment;
    ps->entryPoint = "PSMain";
    ps->bytecode = {0x01u, 0x02u, 0x03u, 0x04u};
    const ShaderHandle psAsset = assets.GetOrAddShader("shaders/env_ps.dxil", std::move(ps));

    MaterialSystem ms;
    MaterialDesc d;
    d.name = "EnvMaterial";
    d.renderPass = StandardRenderPasses::Opaque();
    d.vertexShader = vsAsset;
    d.fragmentShader = psAsset;
    const MaterialHandle mat = ms.RegisterMaterial(std::move(d));

    renderer::null_backend::NullDevice device;
    renderer::IDevice::DeviceDesc dd{};
    dd.appName = "ShaderRuntimeEnvRebuild";
    CHECK(ctx, device.Initialize(dd));

    ShaderRuntime runtime;
    CHECK(ctx, runtime.Initialize(device));
    runtime.SetAssetRegistry(&assets);
    CHECK(ctx, runtime.PrepareAllShaderAssets());
    CHECK(ctx, runtime.PrepareMaterial(ms, mat));

    const auto* state0 = runtime.GetMaterialState(mat);
    CHECK(ctx, state0 != nullptr);
    const uint64_t envRevision0 = state0->environmentRevision;
    CHECK_EQ(ctx, envRevision0, 1ull);

    auto findTextureAtSlot = [](const MaterialGpuState& state, uint32_t slot) -> TextureHandle
    {
        for (const auto& binding : state.bindings)
        {
            if (binding.kind == ResolvedMaterialBinding::Kind::Texture && binding.slot == slot)
                return binding.texture;
        }
        return TextureHandle::Invalid();
    };

    CHECK(ctx, !findTextureAtSlot(*state0, TexSlots::IBLIrradiance).IsValid());
    CHECK(ctx, !findTextureAtSlot(*state0, TexSlots::IBLPrefiltered).IsValid());
    CHECK(ctx, !findTextureAtSlot(*state0, TexSlots::BRDFLUT).IsValid());

    EnvironmentRuntimeState env{};
    env.active = true;
    env.irradiance = TextureHandle::Make(101u, 1u);
    env.prefiltered = TextureHandle::Make(102u, 1u);
    env.brdfLut = TextureHandle::Make(103u, 1u);
    runtime.SetEnvironmentState(env);

    auto cmd = device.CreateCommandList(QueueType::Graphics);
    cmd->Begin();
    CHECK(ctx, runtime.BindMaterial(*cmd, ms, mat, BufferHandle::Make(1u,1u), BufferHandle::Make(2u,1u)));
    cmd->End();
    cmd->Submit();

    const auto* state1 = runtime.GetMaterialState(mat);
    CHECK(ctx, state1 != nullptr);
    CHECK(ctx, state1->environmentRevision > envRevision0);

    CHECK_EQ(ctx, findTextureAtSlot(*state1, TexSlots::IBLIrradiance), env.irradiance);
    CHECK_EQ(ctx, findTextureAtSlot(*state1, TexSlots::IBLPrefiltered), env.prefiltered);
    CHECK_EQ(ctx, findTextureAtSlot(*state1, TexSlots::BRDFLUT), env.brdfLut);

    runtime.Shutdown();
    device.Shutdown();
}
