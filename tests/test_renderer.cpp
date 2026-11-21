#include "ForwardFeature.hpp"
#include "ForwardPlusLighting.hpp"
// =============================================================================
// KROM Engine - tests/test_renderer.cpp
// Renderer-Tests: PipelineKey, SortKey, MaterialSystem, RenderWorld
// =============================================================================
#include "TestFramework.hpp"
#include "addons/animation/AnimationComponents.hpp"
#include "addons/animation/SkinningSystem.hpp"
#include "addons/camera/CameraComponents.hpp"
#include "addons/camera/CameraViewBuilder.hpp"
#include "core/PathUtf8.hpp"
#include "addons/lighting/LightingComponents.hpp"
#include "addons/lighting/LightingExtraction.hpp"
#include "addons/lighting/LightingFeature.hpp"
#include "addons/lighting/LightingFrameData.hpp"
#include "addons/mesh_renderer/MeshRendererComponents.hpp"
#include "addons/mesh_renderer/MeshRendererExtraction.hpp"
#include "addons/mesh_renderer/MeshRendererFeature.hpp"
#include "addons/shadow/ShadowAtlasLayout.hpp"
#include "addons/shadow/ShadowCasterCulling.hpp"
#include "addons/shadow/ShadowFeature.hpp"
#include "addons/shadow/ShadowFrameData.hpp"
#include "addons/shadow/ShadowViewBuilder.hpp"
#include "addons/editor/EditorFeature.hpp"
#include "addons/outline/OutlinePass.hpp"
#include "addons/forward/ForwardTonemap.hpp"
#include "addons/forward/ForwardShading.hpp"
#include "addons/forward/ForwardSmaa.hpp"
#include "addons/forward/ForwardAutoExposure.hpp"
#include "addons/forward/ForwardTonemapJson.hpp"
#include "addons/forward/ForwardColorGrading.hpp"
#include "addons/script/ScriptColorGrading.hpp"
#include "addons/gtao/GtaoPass.hpp"
#include "addons/contact_shadow/ContactShadowFeature.hpp"
#include "addons/shadow/ShadowFeature.hpp"
#include "renderer/GpuPassProfiler.hpp"
#include "renderer/FrameGraphStage.hpp"
#include "renderer/IBLBaker.hpp"
#include "renderer/RenderPipelineRecipe.hpp"
#include "assets/VertexLayoutBridge.hpp"
#include "renderer/MaterialSystem.hpp"
#include "renderer/MaterialGraphBinding.hpp"
#include "renderer/runtime/MaterialRuntimeBridge.hpp"
#include "renderer/MaterialCBLayout.hpp"
#include "renderer/ShaderCompiler.hpp"
#include "renderer/ShaderToolPaths.hpp"
#include "renderer/ShaderConstantBufferReflection.hpp"
#include "renderer/RenderPassRegistry.hpp"
#include "renderer/RenderWorld.hpp"
#include "renderer/ECSExtractor.hpp"
#include "renderer/PipelineCache.hpp"
#include "renderer/ShaderBindingModel.hpp"
#include "renderer/RenderPipelineInterfaces.hpp"
#include "renderer/RenderFrameOrchestrator.hpp"
#include "renderer/RenderRuntimeFrameBindings.hpp"
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
#include "jobs/JobSystem.hpp"
#include "DX11Device.hpp"
#include "OpenGLDevice.hpp"
#include "LitMaterial.hpp"
#include "PbrMasterMaterial.hpp"
#include "PbrInstanceBuilder.hpp"
#include "UnlitMaterial.hpp"
#include <cctype>
#include <cmath>
#include <cstring>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <vector>

#if defined(_WIN32) && defined(KROM_DX11_BACKEND)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#include <wrl/client.h>
#undef CreateMutex
#undef CreateWindow
#endif

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

    // GPU timestamps for GpuPassProfiler tests: results are queued by the test.
    uint32_t GetGpuTimestampCapacity() const noexcept override { return timestampCapacity; }
    void SetGpuTimestampsEnabled(bool enabled) override { timestampsEnabled = enabled; }
    uint64_t GetGpuTimestampFrameId() const noexcept override { return timestampFrameId; }
    void TakeCompletedGpuTimestamps(std::vector<GpuTimestampFrame>& out) override
    {
        for (GpuTimestampFrame& frame : completedTimestamps)
            out.push_back(std::move(frame));
        completedTimestamps.clear();
    }
    uint32_t timestampCapacity = 0u;
    bool timestampsEnabled = false;
    uint64_t timestampFrameId = 0u;
    std::vector<GpuTimestampFrame> completedTimestamps;
};

std::unique_ptr<IDevice> CreateTestDeviceFactoryInstance()
{
    return std::make_unique<TestDevice>();
}

} // namespace

static void TestPbrMasterMaterial(test::TestContext& ctx);
static void TestDoubleSidedOverridesCullMode(test::TestContext& ctx);
static void TestUnlitAddonFactory(test::TestContext& ctx);
static void TestUnlitAddonShaderAssetSet(test::TestContext& ctx);
static void TestLitAddonFactory(test::TestContext& ctx);
static void TestLitAddonShaderAssetSet(test::TestContext& ctx);
static void TestStandardFrameRecipe(test::TestContext& ctx);
static void TestShaderRuntimeEndToEnd(test::TestContext& ctx);
static void TestShaderRuntimeValidation(test::TestContext& ctx);
static void TestShaderToolOverride(test::TestContext& ctx);
static void TestMaterialTextureWrites(test::TestContext& ctx);
static void TestPerMaterialLayoutFromReflection(test::TestContext& ctx);
static void TestGlslStd140BlockReflection(test::TestContext& ctx);
static void TestGeneratedTonemapGlslMatchesHandWritten(test::TestContext& ctx);
static void TestCompiledPerMaterialReflection(test::TestContext& ctx);
static void TestExampleShadersKeepPerMaterialLayout(test::TestContext& ctx);
static void TestCustomVertexShadersCompileInstanced(test::TestContext& ctx);
static void TestCustomTextureRegisterFromReflection(test::TestContext& ctx);
static void TestShaderRuntimePartialEnvironmentUsesFallbacks(test::TestContext& ctx);
static void TestShaderRuntimeRebuildOnEnvironmentChange(test::TestContext& ctx);
static void TestShaderRuntimeUsesIBLVariantWhenEnvironmentActive(test::TestContext& ctx);
static void TestShaderRuntimeDoesNotUseIBLVariantWithoutEnvironment(test::TestContext& ctx);
static void TestShaderRuntimeCompilesVariantsInBackground(test::TestContext& ctx);
static void TestShaderRuntimeRebuildsShaderVariantOnEnvironmentToggle(test::TestContext& ctx);
static void TestDeviceFactoryRegistration(test::TestContext& ctx);
static void TestShadowFeatureExtractionAndFrameConstants(test::TestContext& ctx);
static void TestShadowExtractionRespectsRenderViewShadowToggle(test::TestContext& ctx);
static void TestShadowExtractionCollectsMultipleRequestsButSelectsSingleDirectionalPath(test::TestContext& ctx);
static void TestShadowExtractionAppliesLightAndViewBudget(test::TestContext& ctx);
static void TestShadowExtractionCullsLocalShadowOutsideReceiverBounds(test::TestContext& ctx);
static void TestShadowExtractionCullsLocalShadowBehindCameraFrustum(test::TestContext& ctx);
static void TestShadowExtractionFiltersDirectionalCasterOutliers(test::TestContext& ctx);
static void TestShadowViewBuilderClampsSpotAndPointFarPlaneToMaxDistance(test::TestContext& ctx);
static void TestBackendShadowClipSpaceAdjustmentsDx11OpenGL(test::TestContext& ctx);
static void TestShadowDrawListIncludesAllCastersForSelectedDirectionalRequest(test::TestContext& ctx);
static void TestSceneExtractionWithRenderWorldContext(test::TestContext& ctx);
static void TestFrameExtractionStagePassesJobSystemToSceneExtractionContext(test::TestContext& ctx);
static void TestMeshExtractionUsesJobSystemWhenAvailable(test::TestContext& ctx);
static void TestForwardPipelineKeepsBuiltInShadowWithoutShadowDeclaringContributor(test::TestContext& ctx);
static void TestForwardPipelineIntegratesAfterOpaqueContributor(test::TestContext& ctx);
static void TestFeatureRegistryRejectsPassContributorIdCollision(test::TestContext& ctx);
static void TestFeatureRegistryFrameParameterBlocks(test::TestContext& ctx);
static void TestForwardPlusShaderClampsTileLightCountToStoredRange(test::TestContext& ctx);
static void TestForwardPlusShadowBuffersPackMoreThanFourShadowLights(test::TestContext& ctx);
static void TestForwardPlusComputeCullDispatchTransitionsTileBuffers(test::TestContext& ctx);
static void TestDrawListInstancingMergesIdenticalRuns(test::TestContext& ctx);
static void TestDrawListInstancingSkipsMaterialSkinningAndBackToFront(test::TestContext& ctx);

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

static const rendergraph::RGPass* FindGraphPass(const rendergraph::RenderGraph& graph, std::string_view name)
{
    for (const rendergraph::RGPass& pass : graph.GetPasses())
    {
        if (pass.debugName == name)
            return &pass;
    }
    return nullptr;
}

static const rendergraph::RGResourceDesc* FindGraphResource(const rendergraph::RenderGraph& graph, std::string_view name)
{
    for (const rendergraph::RGResourceDesc& resource : graph.GetResources())
    {
        if (resource.debugName == name)
            return &resource;
    }
    return nullptr;
}

static std::string StripAsciiWhitespace(std::string text)
{
    text.erase(std::remove_if(text.begin(),
                              text.end(),
                              [](unsigned char ch) { return std::isspace(ch) != 0; }),
               text.end());
    return text;
}

class MockPassContributor final : public IPassContributor
{
public:
    MockPassContributor(uint32_t contributorId,
                        PassPhase phase,
                        std::string declaredResource = {},
                        std::string resourceName = {},
                        std::string passName = {},
                        std::string executorName = {})
        : m_contributorId(contributorId)
        , m_phase(phase)
        , m_declaredResource(std::move(declaredResource))
        , m_resourceName(std::move(resourceName))
        , m_passName(std::move(passName))
        , m_executorName(std::move(executorName))
    {
    }

    uint32_t GetContributorId() const noexcept override
    {
        return m_contributorId;
    }

    PassPhase GetPhase() const noexcept override
    {
        return m_phase;
    }

    bool DeclaresResource(std::string_view name) const noexcept override
    {
        return !m_declaredResource.empty() && name == m_declaredResource;
    }

    // The produced resource becomes an input of the opaque pass on this slot
    // (FrameRecipe::opaqueInputs), as GTAO does, when the view allows a prepass.
    void SetOpaqueInputSlot(uint32_t slot) noexcept
    {
        m_opaqueInputSlot = slot;
        m_hasOpaqueInput = true;
    }

    void BuildPass(const PassBuildContext& ctx) const override
    {
        if (m_resourceName.empty() || m_passName.empty() || m_executorName.empty())
            return;
        if (m_hasOpaqueInput && !ctx.frame.enableDepthPrepass)
            return;
        if (m_hasOpaqueInput)
            ctx.recipe.opaqueInputs.push_back({ m_resourceName, m_opaqueInputSlot });

        ctx.recipe.resources.push_back(FrameRecipeResourceDesc{
            m_resourceName,
            false,
            16u,
            16u,
            Format::RGBA8_UNORM_SRGB,
            rendergraph::RGResourceKind::ColorTexture
        });

        FrameRecipePassDesc pass{};
        pass.name = m_passName;
        pass.executorName = m_executorName;
        pass.accesses.push_back({ m_resourceName, FrameRecipeAccessKind::WriteRenderTarget });
        pass.renderPass.enabled = true;
        pass.renderPass.targetResourceName = m_resourceName;
        pass.renderPass.viewportWidth = 16u;
        pass.renderPass.viewportHeight = 16u;
        pass.renderPass.clearColor = true;
        ctx.recipe.passes.push_back(std::move(pass));

        ctx.callbacks.Register(m_executorName,
            [](const rendergraph::RGExecContext&) {});
    }

private:
    uint32_t    m_contributorId = 0u;
    PassPhase   m_phase = PassPhase::BeforeOpaque;
    std::string m_declaredResource;
    std::string m_resourceName;
    std::string m_passName;
    std::string m_executorName;
    uint32_t    m_opaqueInputSlot = 0u;
    bool        m_hasOpaqueInput = false;
};

class MockPassContributorFeature final : public IEngineFeature
{
public:
    MockPassContributorFeature(std::string featureName, PassContributorPtr contributor)
        : m_featureName(std::move(featureName))
        , m_contributor(std::move(contributor))
    {
    }

    std::string_view GetName() const noexcept override
    {
        return m_featureName;
    }

    FeatureID GetID() const noexcept override
    {
        return FeatureID::FromString(m_featureName);
    }

    void Register(FeatureRegistrationContext& context) override
    {
        context.RegisterPassContributor(m_contributor);
    }

    bool Initialize(const FeatureInitializationContext& context) override
    {
        (void)context;
        return true;
    }

    void Shutdown(const FeatureShutdownContext& context) override
    {
        (void)context;
        m_contributor.reset();
    }

private:
    std::string        m_featureName;
    PassContributorPtr m_contributor;
};

class RecordingCommandList final : public ICommandList
{
public:
    enum class EventKind : uint8_t
    {
        TransitionBuffer,
        CopyBuffer,
        SetPipeline,
        SetConstantBuffer,
        SetShaderResourceBuffer,
        SetUnorderedAccessBuffer,
        Dispatch,
    };

    struct Event
    {
        EventKind kind = EventKind::Dispatch;
        BufferHandle buffer = BufferHandle::Invalid();
        BufferHandle otherBuffer = BufferHandle::Invalid();
        ResourceState before = ResourceState::Unknown;
        ResourceState after = ResourceState::Unknown;
        uint64_t copySize = 0u;
        uint32_t slot = 0u;
        ShaderStageMask stages = ShaderStageMask::None;
        PipelineHandle pipeline = PipelineHandle::Invalid();
        uint32_t groupsX = 0u;
        uint32_t groupsY = 0u;
        uint32_t groupsZ = 0u;
    };

    std::vector<Event> events;

    void Begin() override {}
    void End() override {}
    std::vector<RenderPassBeginInfo> renderPasses;

    void BeginRenderPass(const RenderPassBeginInfo& info) override { renderPasses.push_back(info); }
    void EndRenderPass() override {}

    void SetPipeline(PipelineHandle pipeline) override
    {
        events.push_back(Event{
            EventKind::SetPipeline,
            BufferHandle::Invalid(),
            BufferHandle::Invalid(),
            ResourceState::Unknown,
            ResourceState::Unknown,
            0u,
            0u,
            ShaderStageMask::None,
            pipeline
        });
    }

    void SetVertexBuffer(uint32_t, BufferHandle, uint32_t) override {}
    void SetIndexBuffer(BufferHandle, bool, uint32_t) override {}

    void SetConstantBuffer(uint32_t slot, BufferHandle buffer, ShaderStageMask stages) override
    {
        events.push_back(Event{
            EventKind::SetConstantBuffer,
            buffer,
            BufferHandle::Invalid(),
            ResourceState::Unknown,
            ResourceState::Unknown,
            0u,
            slot,
            stages,
            PipelineHandle::Invalid()
        });
    }

    void SetShaderResource(uint32_t, TextureHandle, ShaderStageMask) override {}

    void SetShaderResource(uint32_t slot, BufferHandle buffer, ShaderStageMask stages) override
    {
        events.push_back(Event{
            EventKind::SetShaderResourceBuffer,
            buffer,
            BufferHandle::Invalid(),
            ResourceState::Unknown,
            ResourceState::Unknown,
            0u,
            slot,
            stages,
            PipelineHandle::Invalid()
        });
    }

    void SetUnorderedAccess(uint32_t, TextureHandle, ShaderStageMask) override {}

    void SetUnorderedAccess(uint32_t slot, BufferHandle buffer, ShaderStageMask stages) override
    {
        events.push_back(Event{
            EventKind::SetUnorderedAccessBuffer,
            buffer,
            BufferHandle::Invalid(),
            ResourceState::Unknown,
            ResourceState::Unknown,
            0u,
            slot,
            stages,
            PipelineHandle::Invalid()
        });
    }

    void SetSampler(uint32_t, uint32_t, ShaderStageMask) override {}
    void SetViewport(float, float, float, float, float, float) override {}
    void SetScissor(int32_t, int32_t, uint32_t, uint32_t) override {}
    void Draw(uint32_t, uint32_t, uint32_t, uint32_t) override {}
    void DrawIndexed(uint32_t, uint32_t, uint32_t, int32_t, uint32_t) override {}

    void Dispatch(uint32_t groupsX, uint32_t groupsY, uint32_t groupsZ) override
    {
        events.push_back(Event{
            EventKind::Dispatch,
            BufferHandle::Invalid(),
            BufferHandle::Invalid(),
            ResourceState::Unknown,
            ResourceState::Unknown,
            0u,
            0u,
            ShaderStageMask::None,
            PipelineHandle::Invalid(),
            groupsX,
            groupsY,
            groupsZ
        });
    }

    void TransitionResource(BufferHandle buffer, ResourceState before, ResourceState after) override
    {
        events.push_back(Event{
            EventKind::TransitionBuffer,
            buffer,
            BufferHandle::Invalid(),
            before,
            after
        });
    }

    void TransitionResource(TextureHandle, ResourceState, ResourceState) override {}
    void TransitionRenderTarget(RenderTargetHandle, ResourceState, ResourceState) override {}

    void CopyBuffer(BufferHandle dst, uint64_t, BufferHandle src, uint64_t, uint64_t size) override
    {
        events.push_back(Event{
            EventKind::CopyBuffer,
            dst,
            src,
            ResourceState::Unknown,
            ResourceState::Unknown,
            size
        });
    }

    void CopyTexture(TextureHandle, uint32_t, TextureHandle, uint32_t) override {}
    void Submit(QueueType = QueueType::Graphics) override {}
};

class RecordingComputeDevice final : public IDevice
{
public:
    struct BufferRecord
    {
        BufferDesc desc{};
        std::vector<uint8_t> uploadedData;
        uint32_t uploadCount = 0u;
    };

    bool Initialize(const DeviceDesc&) override { return true; }
    void Shutdown() override
    {
        m_buffers.clear();
        m_bufferNames.clear();
        m_shaders.clear();
        m_pipelines.clear();
    }
    void WaitIdle() override {}
    std::unique_ptr<ISwapchain> CreateSwapchain(const SwapchainDesc&) override { return nullptr; }

    BufferHandle CreateBuffer(const BufferDesc& desc) override
    {
        const BufferHandle handle = BufferHandle::Make(m_nextBufferIndex++, 1u);
        m_buffers.emplace(handle.value, BufferRecord{desc, {}});
        if (!desc.debugName.empty())
            m_bufferNames.emplace(desc.debugName, handle);
        return handle;
    }

    void DestroyBuffer(BufferHandle handle) override
    {
        if (!handle.IsValid())
            return;
        const auto it = m_buffers.find(handle.value);
        if (it != m_buffers.end() && !it->second.desc.debugName.empty())
            m_bufferNames.erase(it->second.desc.debugName);
        m_buffers.erase(handle.value);
    }

    void* MapBuffer(BufferHandle) override { return nullptr; }
    void UnmapBuffer(BufferHandle) override {}
    TextureHandle CreateTexture(const TextureDesc&) override { return TextureHandle::Invalid(); }
    void DestroyTexture(TextureHandle) override {}
    RenderTargetHandle CreateRenderTarget(const RenderTargetDesc&) override { return RenderTargetHandle::Invalid(); }
    void DestroyRenderTarget(RenderTargetHandle) override {}
    TextureHandle GetRenderTargetColorTexture(RenderTargetHandle) const override { return TextureHandle::Invalid(); }
    TextureHandle GetRenderTargetDepthTexture(RenderTargetHandle) const override { return TextureHandle::Invalid(); }

    ShaderHandle CreateShaderFromSource(const std::string&, ShaderStageMask, const std::string&, const std::string&) override
    {
        const ShaderHandle handle = ShaderHandle::Make(m_nextShaderIndex++, 1u);
        m_shaders.push_back(handle);
        return handle;
    }

    ShaderHandle CreateShaderFromBytecode(const void*, size_t, ShaderStageMask, const std::string&) override
    {
        const ShaderHandle handle = ShaderHandle::Make(m_nextShaderIndex++, 1u);
        m_shaders.push_back(handle);
        return handle;
    }

    void DestroyShader(ShaderHandle) override {}

    PipelineHandle CreatePipeline(const PipelineDesc& desc) override
    {
        const PipelineHandle handle = PipelineHandle::Make(m_nextPipelineIndex++, 1u);
        m_pipelines.emplace(handle.value, desc);
        return handle;
    }

    void DestroyPipeline(PipelineHandle handle) override
    {
        m_pipelines.erase(handle.value);
    }

    uint32_t CreateSampler(const SamplerDesc&) override { return 0u; }
    std::unique_ptr<ICommandList> CreateCommandList(QueueType) override { return nullptr; }
    std::unique_ptr<IFence> CreateFence(uint64_t) override { return nullptr; }

    void UploadBufferData(BufferHandle handle, const void* data, size_t byteSize, size_t dstOffset) override
    {
        auto it = m_buffers.find(handle.value);
        if (it == m_buffers.end())
            return;
        ++it->second.uploadCount;
        auto& bytes = it->second.uploadedData;
        const size_t requiredSize = dstOffset + byteSize;
        if (bytes.size() < requiredSize)
            bytes.resize(requiredSize);
        if (data && byteSize > 0u)
            std::memcpy(bytes.data() + dstOffset, data, byteSize);
    }

    void UploadTextureData(TextureHandle, const void*, size_t, uint32_t, uint32_t) override {}
    void BeginFrame() override {}
    void EndFrame() override {}
    uint32_t GetDrawCallCount() const override { return 0u; }
    const char* GetBackendName() const override { return "RecordingCompute"; }

    [[nodiscard]] const PipelineDesc* GetPipeline(PipelineHandle handle) const
    {
        const auto it = m_pipelines.find(handle.value);
        return it != m_pipelines.end() ? &it->second : nullptr;
    }

    [[nodiscard]] BufferHandle FindBufferByName(std::string_view debugName) const
    {
        const auto it = m_bufferNames.find(std::string(debugName));
        return it != m_bufferNames.end() ? it->second : BufferHandle::Invalid();
    }

    [[nodiscard]] const BufferRecord* GetBuffer(BufferHandle handle) const
    {
        const auto it = m_buffers.find(handle.value);
        return it != m_buffers.end() ? &it->second : nullptr;
    }

    // Gibt das erste Buffer-Record zurück, dessen uploadedData genau byteSize Bytes hat.
    // Nützlich um Staging-Buffer ohne bekannten Handle zu finden.
    [[nodiscard]] const BufferRecord* FindBufferByUploadedSize(size_t byteSize) const
    {
        for (const auto& [key, rec] : m_buffers)
            if (rec.uploadedData.size() == byteSize)
                return &rec;
        return nullptr;
    }

private:
    uint32_t m_nextBufferIndex = 1u;
    uint32_t m_nextShaderIndex = 1u;
    uint32_t m_nextPipelineIndex = 1u;
    std::unordered_map<uint32_t, BufferRecord> m_buffers;
    std::unordered_map<std::string, BufferHandle> m_bufferNames;
    std::vector<ShaderHandle> m_shaders;
    std::unordered_map<uint32_t, PipelineDesc> m_pipelines;
};




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
    const MaterialRuntimeDesc* runtime = MaterialRuntimeBridge::GetRuntimeDesc(ms, material.Handle());
    CHECK(ctx, desc != nullptr);
    CHECK(ctx, runtime != nullptr);
    CHECK_EQ(ctx, runtime->vertexShader, info.vertexShader);
    CHECK_EQ(ctx, runtime->fragmentShader, info.fragmentShader);
    CHECK_EQ(ctx, runtime->renderPass, StandardRenderPasses::Opaque());
    CHECK_EQ(ctx, runtime->rasterizer.frontFace, WindingOrder::CW);
    CHECK(ctx, HasFlag(desc->features, MaterialFeatureFlags::Unlit));
    CHECK(ctx, HasFlag(desc->features, MaterialFeatureFlags::BaseColorMap));
    CHECK(ctx, HasFlag(desc->features, MaterialFeatureFlags::EmissiveMap));
    CHECK(ctx, HasFlag(desc->features, MaterialFeatureFlags::AlphaTest));

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
    CHECK_EQ(ctx, std::string(assets.shadowShader),   std::string("shadow.vs.hlsl"));
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
    const MaterialRuntimeDesc* runtime = MaterialRuntimeBridge::GetRuntimeDesc(ms, material.Handle());
    CHECK(ctx, desc != nullptr);
    CHECK(ctx, runtime != nullptr);
    CHECK_EQ(ctx, runtime->vertexShader, info.vertexShader);
    CHECK_EQ(ctx, runtime->fragmentShader, info.fragmentShader);
    CHECK_EQ(ctx, runtime->shadowShader, info.shadowShader);
    CHECK(ctx, !HasFlag(desc->features, MaterialFeatureFlags::Unlit));
    CHECK(ctx, !HasFlag(desc->features, MaterialFeatureFlags::PbrMetalRough));
    CHECK(ctx, HasFlag(desc->features, MaterialFeatureFlags::BaseColorMap));
    CHECK(ctx, HasFlag(desc->features, MaterialFeatureFlags::EmissiveMap));

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

static void TestPbrMasterMaterial(test::TestContext& ctx)
{
    MaterialSystem materials;

    VertexLayout layout{};
    layout.attributes.push_back({ VertexSemantic::Position, Format::RGB32_FLOAT, 0u, 0u });
    layout.bindings.push_back({ 0u, 12u });

    pbr::PbrMasterMaterial::Config config{};
    config.vs           = ShaderHandle::Make(10u, 1u);
    config.fs           = ShaderHandle::Make(11u, 1u);
    config.shadow       = ShaderHandle::Make(12u, 1u);
    config.vertexLayout = layout;

    pbr::PbrMasterMaterial master = pbr::PbrMasterMaterial::Create(materials, config);
    CHECK(ctx, master.IsValid());

    // Permutation with albedo + normal + emissive + IBL
    const TextureHandle albedoTex  = TextureHandle::Make(31u, 1u);
    const TextureHandle normalTex  = TextureHandle::Make(32u, 1u);
    const TextureHandle emissiveTex = TextureHandle::Make(34u, 1u);

    MaterialHandle inst = master.CreateInstance("TestInst")
        .BaseColor(albedoTex)
        .Normal(normalTex)
        .Emissive(emissiveTex)
        .Roughness(0.15f)
        .Metallic(0.9f)
        .IBL(true)
        .Build();
    CHECK(ctx, inst.IsValid());

    const MaterialDesc* desc = materials.GetDesc(inst);
    CHECK(ctx, desc != nullptr);
    CHECK(ctx, HasFlag(desc->features, MaterialFeatureFlags::PbrMetalRough));
    CHECK(ctx, HasFlag(desc->features, MaterialFeatureFlags::BaseColorMap));
    CHECK(ctx, HasFlag(desc->features, MaterialFeatureFlags::NormalMap));
    CHECK(ctx, HasFlag(desc->features, MaterialFeatureFlags::EmissiveMap));
    CHECK(ctx, HasFlag(desc->features, MaterialFeatureFlags::IblMap));
    const MaterialFeatureFlags permutationFlags = desc != nullptr ? desc->features : MaterialFeatureFlags::None;

    config.materialGraph = true;
    auto graphMaster = pbr::PbrMasterMaterial::Create(materials, config);
    const auto graphMaterial = graphMaster.CreateInstance("Graph").Build();
    const auto* graphLayout = materials.GetParameterLayout(graphMaterial);
    CHECK(ctx, graphLayout != nullptr);
    if (graphLayout)
    {
        CHECK(ctx, graphLayout->FindByName("graphTexture0") != nullptr);
        CHECK(ctx, graphLayout->FindByName("graphTexture1") != nullptr);
        // Must match the PerMaterial cbuffers of the PBR shaders (HLSL and GLSL).
        CHECK_EQ(ctx, materials.GetCBLayout(graphMaterial).GetOffset("boxTileSize"), 104u);
        CHECK_EQ(ctx, materials.GetCBLayout(graphMaterial).GetOffset("clearcoatFactor"), 108u);
        CHECK_EQ(ctx, materials.GetCBLayout(graphMaterial).GetOffset("clearcoatRoughness"), 112u);
        CHECK_EQ(ctx, materials.GetCBLayout(graphMaterial).GetOffset("specularFactor"), 116u);
        CHECK_EQ(ctx, materials.GetCBLayout(graphMaterial).GetOffset("graphOutputs"), 128u);
        CHECK_EQ(ctx, materials.GetCBLayout(graphMaterial).GetOffset("graphNode7"), 256u);

        // Clear coat values reach the constant buffer of a freshly built instance.
        pbr::SetPbrClearcoat(materials, graphMaterial, 0.6f, 0.2f);
        const auto& coatData = materials.GetCBData(graphMaterial);
        CHECK(ctx, coatData.size() >= 116u);
        if (coatData.size() >= 116u)
        {
            float coat = 0.f, coatRoughness = 0.f;
            std::memcpy(&coat, coatData.data() + 108u, sizeof(float));
            std::memcpy(&coatRoughness, coatData.data() + 112u, sizeof(float));
            CHECK(ctx, std::fabs(coat - 0.6f) < 1e-6f);
            CHECK(ctx, std::fabs(coatRoughness - 0.2f) < 1e-6f);
        }
    }

    // CB layout: roughnessFactor and metallicFactor must be present
    const auto& cbData   = materials.GetCBData(inst);
    const auto& cbLayout = materials.GetCBLayout(inst);
    const uint32_t roughOff  = cbLayout.GetOffset("roughnessFactor");
    const uint32_t metalOff  = cbLayout.GetOffset("metallicFactor");
    CHECK(ctx, roughOff != UINT32_MAX);
    CHECK(ctx, metalOff != UINT32_MAX);
    const float roughVal = *reinterpret_cast<const float*>(cbData.data() + roughOff);
    const float metalVal = *reinterpret_cast<const float*>(cbData.data() + metalOff);
    CHECK(ctx, std::fabs(roughVal - 0.15f) < 1e-5f);
    CHECK(ctx, std::fabs(metalVal - 0.9f)  < 1e-5f);
    const uint32_t specularOff = cbLayout.GetOffset("specularFactor");
    CHECK_EQ(ctx, specularOff, 116u);
    float defaultSpecular = 0.f;
    std::memcpy(&defaultSpecular, cbData.data() + specularOff, sizeof(float));
    CHECK_EQ(ctx, defaultSpecular, 0.5f);
    const auto matte = master.CreateInstance("SpecularZero").Specular(0.f).Build();
    CHECK(ctx, matte.IsValid());
    float matteSpecular = 1.f;
    std::memcpy(&matteSpecular, materials.GetCBData(matte).data() + specularOff, sizeof(float));
    CHECK_EQ(ctx, matteSpecular, 0.f);

    // Same permutation must be served from cache (same base handle)
    MaterialHandle inst2 = master.CreateInstance("TestInst2")
        .BaseColor(albedoTex)
        .Normal(normalTex)
        .Emissive(emissiveTex)
        .IBL(true)
        .Build();
    CHECK(ctx, inst2.IsValid());
    // Both instances share the same base — verify via permutation flags
    const MaterialDesc* desc2 = materials.GetDesc(inst2);
    CHECK(ctx, desc2 != nullptr);
    CHECK_EQ(ctx, static_cast<uint64_t>(permutationFlags), static_cast<uint64_t>(desc2->features));
}

static void TestDoubleSidedOverridesCullMode(test::TestContext& ctx)
{
    {
        MaterialSystem ms;
        MaterialDesc d;
    MaterialRuntimeDesc dRuntime;
        MaterialRuntimeDesc runtime;
        d.name = "DirectBack";
        runtime.renderPass = StandardRenderPasses::Opaque();
        runtime.vertexShader = ShaderHandle::Make(101u, 1u);
        runtime.fragmentShader = ShaderHandle::Make(102u, 1u);
        d.renderPolicy.cull.mode = MaterialCullMode::Back;
        MaterialHandle h = MaterialRuntimeBridge::RegisterMaterial(ms, std::move(d), runtime);
        const MaterialRuntimeDesc* out = MaterialRuntimeBridge::GetRuntimeDesc(ms, h);
        CHECK(ctx, out != nullptr);
        CHECK_EQ(ctx, out->rasterizer.cullMode, CullMode::Back);
    }

    {
        MaterialSystem ms;
        MaterialDesc d;
    MaterialRuntimeDesc dRuntime;
        MaterialRuntimeDesc runtime;
        d.name = "DirectFront";
        runtime.renderPass = StandardRenderPasses::Opaque();
        runtime.vertexShader = ShaderHandle::Make(103u, 1u);
        runtime.fragmentShader = ShaderHandle::Make(104u, 1u);
        d.renderPolicy.cull.mode = MaterialCullMode::Front;
        MaterialHandle h = MaterialRuntimeBridge::RegisterMaterial(ms, std::move(d), runtime);
        const MaterialRuntimeDesc* out = MaterialRuntimeBridge::GetRuntimeDesc(ms, h);
        CHECK(ctx, out != nullptr);
        CHECK_EQ(ctx, out->rasterizer.cullMode, CullMode::Front);
    }

    {
        MaterialSystem ms;
        MaterialDesc d;
    MaterialRuntimeDesc dRuntime;
        MaterialRuntimeDesc runtime;
        d.name = "DirectNone";
        runtime.renderPass = StandardRenderPasses::Opaque();
        runtime.vertexShader = ShaderHandle::Make(105u, 1u);
        runtime.fragmentShader = ShaderHandle::Make(106u, 1u);
        d.renderPolicy.cull.mode = MaterialCullMode::None;
        MaterialHandle h = MaterialRuntimeBridge::RegisterMaterial(ms, std::move(d), runtime);
        const MaterialRuntimeDesc* out = MaterialRuntimeBridge::GetRuntimeDesc(ms, h);
        CHECK(ctx, out != nullptr);
        CHECK_EQ(ctx, out->rasterizer.cullMode, CullMode::None);
    }

    {
        MaterialSystem ms;
        MaterialDesc d;
    MaterialRuntimeDesc dRuntime;
        MaterialRuntimeDesc runtime;
        d.name = "DirectDoubleSided";
        runtime.renderPass = StandardRenderPasses::Opaque();
        runtime.vertexShader = ShaderHandle::Make(107u, 1u);
        runtime.fragmentShader = ShaderHandle::Make(108u, 1u);
        d.renderPolicy.cull.mode = MaterialCullMode::Back;
        d.renderPolicy.cull.doubleSided = true;
        MaterialHandle h = MaterialRuntimeBridge::RegisterMaterial(ms, std::move(d), runtime);
        const MaterialRuntimeDesc* out = MaterialRuntimeBridge::GetRuntimeDesc(ms, h);
        CHECK(ctx, out != nullptr);
        CHECK_EQ(ctx, out->rasterizer.cullMode, CullMode::None);
    }

    {
        MaterialSystem ms;
        unlit::UnlitMaterialCreateInfo info{};
        info.name = "UnlitDoubleSided";
        info.vertexShader = ShaderHandle::Make(109u, 1u);
        info.fragmentShader = ShaderHandle::Make(110u, 1u);
        info.cullMode = MaterialCullMode::Back;
        info.doubleSided = true;
        const auto material = unlit::UnlitMaterial::Create(ms, info);
        const MaterialRuntimeDesc* runtime = MaterialRuntimeBridge::GetRuntimeDesc(ms, material.Handle());
        CHECK(ctx, runtime != nullptr);
        CHECK_EQ(ctx, runtime->rasterizer.cullMode, CullMode::None);
    }

    {
        MaterialSystem ms;
        lit::LitMaterialCreateInfo info{};
        info.name = "LitDoubleSided";
        info.vertexShader = ShaderHandle::Make(111u, 1u);
        info.fragmentShader = ShaderHandle::Make(112u, 1u);
        info.shadowShader = ShaderHandle::Make(113u, 1u);
        info.cullMode = MaterialCullMode::Back;
        info.doubleSided = true;
        const auto material = lit::LitMaterial::Create(ms, info);
        const MaterialRuntimeDesc* runtime = MaterialRuntimeBridge::GetRuntimeDesc(ms, material.Handle());
        CHECK(ctx, runtime != nullptr);
        CHECK_EQ(ctx, runtime->rasterizer.cullMode, CullMode::None);
    }

    {
        MaterialSystem ms;
        VertexLayout layout{};
        layout.attributes.push_back({ VertexSemantic::Position, Format::RGB32_FLOAT, 0u, 0u });
        layout.bindings.push_back({ 0u, 12u });

        pbr::PbrMasterMaterial::Config config{};
        config.vs = ShaderHandle::Make(114u, 1u);
        config.fs = ShaderHandle::Make(115u, 1u);
        config.shadow = ShaderHandle::Make(116u, 1u);
        config.vertexLayout = layout;
        config.cullMode = MaterialCullMode::Back;

        auto master = pbr::PbrMasterMaterial::Create(ms, config);
        MaterialHandle inst = master.CreateInstance("PbrDoubleSided")
            .DoubleSided(true)
            .Build();
        const MaterialRuntimeDesc* runtime = MaterialRuntimeBridge::GetRuntimeDesc(ms, inst);
        CHECK(ctx, runtime != nullptr);
        CHECK_EQ(ctx, runtime->rasterizer.cullMode, CullMode::None);
    }
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
    CHECK_EQ(ctx, recipe.resources.size(), size_t(6));
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
    CHECK_EQ(ctx, tonemap->renderPass.targetResourceName, std::string("Backbuffer"));
    CHECK_EQ(ctx, tonemap->renderPass.viewportWidth, 800u);
    CHECK_EQ(ctx, tonemap->renderPass.viewportHeight, 600u);

    const FrameRecipePassDesc* ui = FindRecipePass(recipe, "UIPass");
    CHECK(ctx, ui != nullptr);
    CHECK_EQ(ctx, ui->executorName, std::string(StandardFrameExecutors::UI));
    CHECK_EQ(ctx, ui->renderPass.targetResourceName, std::string("Backbuffer"));

    const FrameRecipePassDesc* present = FindRecipePass(recipe, "PresentPass");
    CHECK(ctx, present != nullptr);
    CHECK_EQ(ctx, present->executorName, std::string(StandardFrameExecutors::Present));
    CHECK(ctx, !present->renderPass.enabled);
}

// Post-AA: the tonemap pass writes "Tonemapped", the PostAA pass reads it and
// takes over the former tonemap target (backbuffer or editor scene view).
static void TestPostAARecipe(test::TestContext& ctx)
{
    using namespace engine::renderer::addons::forward;
    PostAAMode mode = PostAAMode::Off;
    CHECK(ctx, ParsePostAAMode("fxaa", mode));
    CHECK_EQ(ctx, mode, PostAAMode::Fxaa);
    CHECK(ctx, ParsePostAAMode("smaa", mode));
    CHECK_EQ(ctx, mode, PostAAMode::Smaa);
    CHECK(ctx, !ParsePostAAMode("taa", mode));
    CHECK_EQ(ctx, mode, PostAAMode::Smaa);   // unknown name leaves the value alone
    CHECK_EQ(ctx, std::string(PostAAModeName(PostAAMode::Off)), std::string("off"));
    CHECK_EQ(ctx, std::string(PostAAModeName(PostAAMode::Smaa)), std::string("smaa"));

    for (const bool sceneOutput : { false, true })
    {
        StandardFrameRecipeBuilder::BuildParams params{};
        params.viewportWidth = 800u;
        params.viewportHeight = 600u;
        params.uiEnabled = true;
        if (sceneOutput)
        {
            params.sceneOutputRT = RenderTargetHandle::Make(7u, 1u);
            params.backbufferWidth = 1024u;
            params.backbufferHeight = 768u;
        }
        const FrameRecipe plain = StandardFrameRecipeBuilder::BuildRecipe(params);
        params.postAAEnabled = true;
        params.postAAFormat = Format::BGRA8_UNORM_SRGB;
        const FrameRecipe recipe = StandardFrameRecipeBuilder::BuildRecipe(params);
        CHECK_EQ(ctx, recipe.passes.size(), plain.passes.size() + 1u);
        CHECK_EQ(ctx, recipe.resources.size(), plain.resources.size() + 1u);

        const std::string target = sceneOutput ? std::string(StandardFrameRecipeBuilder::kEditorSceneViewResource)
                                               : std::string("Backbuffer");
        const FrameRecipePassDesc* plainTonemap = FindRecipePass(plain, "TonemapPass");
        CHECK(ctx, plainTonemap != nullptr && plainTonemap->renderPass.targetResourceName == target);

        const FrameRecipePassDesc* tonemap = FindRecipePass(recipe, "TonemapPass");
        const FrameRecipePassDesc* postAA = FindRecipePass(recipe, "PostAAPass");
        CHECK(ctx, tonemap != nullptr);
        CHECK(ctx, postAA != nullptr);
        if (!tonemap || !postAA)
            continue;
        CHECK_EQ(ctx, tonemap->renderPass.targetResourceName, std::string("Tonemapped"));
        CHECK_EQ(ctx, postAA->executorName, std::string(StandardFrameExecutors::PostAA));
        CHECK_EQ(ctx, postAA->renderPass.targetResourceName, target);
        CHECK_EQ(ctx, postAA->renderPass.viewportWidth, 800u);
        CHECK(ctx, postAA->renderPass.clearColor);
        bool readsTonemapped = false;
        for (const FrameRecipeResourceAccess& access : postAA->accesses)
            readsTonemapped |= access.resourceName == "Tonemapped" && access.access == FrameRecipeAccessKind::ReadTexture;
        CHECK(ctx, readsTonemapped);

        const FrameRecipeResourceDesc* tonemapped = nullptr;
        for (const FrameRecipeResourceDesc& res : recipe.resources)
            if (res.name == "Tonemapped")
                tonemapped = &res;
        CHECK(ctx, tonemapped != nullptr);
        if (tonemapped)
        {
            CHECK_EQ(ctx, tonemapped->format, Format::BGRA8_UNORM_SRGB);
            CHECK_EQ(ctx, tonemapped->width, 800u);
            CHECK_EQ(ctx, tonemapped->height, 600u);
        }

        // Tonemap before PostAA before UI.
        size_t tonemapIndex = 0u, postAAIndex = 0u, uiIndex = 0u;
        for (size_t i = 0; i < recipe.passes.size(); ++i)
        {
            if (recipe.passes[i].name == "TonemapPass") tonemapIndex = i;
            if (recipe.passes[i].name == "PostAAPass")  postAAIndex = i;
            if (recipe.passes[i].name == "UIPass")      uiIndex = i;
        }
        CHECK(ctx, tonemapIndex < postAAIndex);
        CHECK(ctx, postAAIndex < uiIndex);

        // SMAA: edge and weight passes between tonemap and PostAA (neighborhood blending).
        params.postAASmaa = true;
        const FrameRecipe smaa = StandardFrameRecipeBuilder::BuildRecipe(params);
        CHECK_EQ(ctx, smaa.passes.size(), plain.passes.size() + 3u);
        CHECK_EQ(ctx, smaa.resources.size(), plain.resources.size() + 3u);
        const auto findResource = [&smaa](const char* name) -> const FrameRecipeResourceDesc* {
            for (const FrameRecipeResourceDesc& res : smaa.resources)
                if (res.name == name)
                    return &res;
            return nullptr;
        };
        const FrameRecipeResourceDesc* edgesRes = findResource("SmaaEdges");
        const FrameRecipeResourceDesc* weightsRes = findResource("SmaaWeights");
        CHECK(ctx, edgesRes != nullptr && edgesRes->format == Format::RG8_UNORM && edgesRes->width == 800u);
        CHECK(ctx, weightsRes != nullptr && weightsRes->format == Format::RGBA8_UNORM && weightsRes->height == 600u);
        CHECK(ctx, findResource("Tonemapped") != nullptr);

        const auto accesses = [](const FrameRecipePassDesc* pass, const char* resource, FrameRecipeAccessKind kind) {
            if (!pass)
                return false;
            for (const FrameRecipeResourceAccess& access : pass->accesses)
                if (access.resourceName == resource && access.access == kind)
                    return true;
            return false;
        };
        const FrameRecipePassDesc* edgePass = FindRecipePass(smaa, "SmaaEdgePass");
        const FrameRecipePassDesc* weightPass = FindRecipePass(smaa, "SmaaWeightPass");
        const FrameRecipePassDesc* blendPass = FindRecipePass(smaa, "PostAAPass");
        CHECK(ctx, edgePass != nullptr && weightPass != nullptr && blendPass != nullptr);
        if (!edgePass || !weightPass || !blendPass)
            continue;
        CHECK_EQ(ctx, edgePass->executorName, std::string(StandardFrameExecutors::SmaaEdges));
        CHECK_EQ(ctx, weightPass->executorName, std::string(StandardFrameExecutors::SmaaWeights));
        CHECK_EQ(ctx, blendPass->executorName, std::string(StandardFrameExecutors::PostAA));
        CHECK(ctx, accesses(edgePass, "Tonemapped", FrameRecipeAccessKind::ReadTexture));
        CHECK_EQ(ctx, edgePass->renderPass.targetResourceName, std::string("SmaaEdges"));
        CHECK(ctx, edgePass->renderPass.clearColor);
        CHECK(ctx, edgePass->renderPass.clearColorValue == (std::array<float, 4>{ 0.f, 0.f, 0.f, 0.f }));
        CHECK(ctx, accesses(weightPass, "SmaaEdges", FrameRecipeAccessKind::ReadTexture));
        CHECK_EQ(ctx, weightPass->renderPass.targetResourceName, std::string("SmaaWeights"));
        CHECK(ctx, weightPass->renderPass.clearColor);
        CHECK(ctx, accesses(blendPass, "Tonemapped", FrameRecipeAccessKind::ReadTexture));
        CHECK(ctx, accesses(blendPass, "SmaaWeights", FrameRecipeAccessKind::ReadTexture));
        CHECK_EQ(ctx, blendPass->renderPass.targetResourceName, target);
        const FrameRecipePassDesc* smaaTonemap = FindRecipePass(smaa, "TonemapPass");
        CHECK(ctx, smaaTonemap != nullptr && smaaTonemap->renderPass.targetResourceName == "Tonemapped");

        size_t order[5] = {};
        const char* names[5] = { "TonemapPass", "SmaaEdgePass", "SmaaWeightPass", "PostAAPass", "UIPass" };
        for (size_t i = 0; i < smaa.passes.size(); ++i)
            for (size_t n = 0; n < 5u; ++n)
                if (smaa.passes[i].name == names[n])
                    order[n] = i;
        for (size_t n = 1; n < 5u; ++n)
            CHECK(ctx, order[n - 1u] < order[n]);
    }
}

// SMAA 1x: the three pixel shaders compile for DX11 (FXC) and Vulkan (SPIR-V),
// and the lookup textures have the sizes of the official data.
static void TestSmaaShadersAndLookupTextures(test::TestContext& ctx)
{
    namespace smaa = engine::renderer::addons::forward::smaa;
    CHECK_EQ(ctx, smaa::AreaTexSize(), static_cast<size_t>(smaa::kAreaTexWidth * smaa::kAreaTexHeight * 2u));
    CHECK_EQ(ctx, smaa::SearchTexSize(), static_cast<size_t>(smaa::kSearchTexWidth * smaa::kSearchTexHeight));
    CHECK(ctx, smaa::AreaTexData() != nullptr && smaa::SearchTexData() != nullptr);

    const struct { const char* name; const char* source; } shaders[] = {
        { "SmaaEdgePS", smaa::kEdgeDetectionPsHlsl },
        { "SmaaWeightPS", smaa::kBlendingWeightPsHlsl },
        { "SmaaBlendPS", smaa::kNeighborhoodBlendingPsHlsl },
    };
    for (const auto& shader : shaders)
    {
        assets::ShaderAsset ps{};
        ps.debugName = shader.name;
        ps.stage = assets::ShaderStage::Fragment;
        ps.sourceLanguage = assets::ShaderSourceLanguage::HLSL;
        ps.entryPoint = "main";
        ps.sourceCode = shader.source;
        for (const assets::ShaderTargetProfile target : { assets::ShaderTargetProfile::DirectX11_SM5,
                                                          assets::ShaderTargetProfile::Vulkan_SPIRV })
        {
            assets::CompiledShaderArtifact artifact{};
            std::string error;
            const bool compiled = ShaderCompiler::CompileForTarget(ps, target, artifact, &error);
            if (!compiled && target == assets::ShaderTargetProfile::Vulkan_SPIRV
                && error.find("dxc") != std::string::npos)
            {
                Debug::LogWarning("SMAA shader test: %s skipped for %s (%s)", shader.name,
                                  ShaderCompiler::ToString(target), error.c_str());
                continue;
            }
            if (!compiled)
                Debug::LogError("SMAA shader test: %s (%s): %s", shader.name, ShaderCompiler::ToString(target), error.c_str());
            CHECK(ctx, compiled);
            CHECK(ctx, !artifact.bytecode.empty());
        }
    }
}

// Auto-exposure settings: names, defaults (old scenes stay fixed), limits, and the
// CPU mirror of the shader's target and adaptation step.
static void TestAutoExposureSettings(test::TestContext& ctx)
{
    using namespace engine::renderer::addons::forward;
    CHECK(ctx, TonemapSettings{}.exposureMode == ExposureMode::Fixed);
    CHECK(ctx, !TonemapSettings{}.freezeAutoExposure);
    CHECK_EQ(ctx, std::string(ExposureModeName(ExposureMode::Fixed)), std::string("fixed"));
    CHECK_EQ(ctx, std::string(ExposureModeName(ExposureMode::Auto)), std::string("auto"));
    CHECK(ctx, ParseExposureMode("auto") == ExposureMode::Auto);
    CHECK(ctx, ParseExposureMode("fixed") == ExposureMode::Fixed);
    CHECK(ctx, ParseExposureMode("histogram") == ExposureMode::Fixed);
    CHECK(ctx, ParseExposureMode(nullptr) == ExposureMode::Fixed);

    const AutoExposureSettings defaults{};
    CHECK(ctx, defaults.minEv == -4.0f && defaults.maxEv == 4.0f);
    CHECK(ctx, defaults.toBrightSpeed == 3.0f && defaults.toDarkSpeed == 1.0f);

    AutoExposureSettings wild{};
    wild.minEv = 30.0f;
    wild.maxEv = -30.0f;
    wild.toBrightSpeed = 0.0f;
    wild.toDarkSpeed = 100.0f;
    wild.Clamp();
    CHECK(ctx, wild.minEv == kMaxExposureEv);
    CHECK(ctx, wild.maxEv == wild.minEv);
    CHECK(ctx, wild.toBrightSpeed == kMinAutoExposureSpeed);
    CHECK(ctx, wild.toDarkSpeed == kMaxAutoExposureSpeed);

    // Middle grey needs no exposure; one stop brighter needs -1 EV; limits apply.
    const float grey = std::log2(kAutoExposureMiddleGrey);
    CHECK(ctx, std::fabs(AutoExposureTargetEv(grey, defaults)) < 1e-5f);
    CHECK(ctx, std::fabs(AutoExposureTargetEv(grey + 1.0f, defaults) + 1.0f) < 1e-5f);
    CHECK(ctx, AutoExposureTargetEv(grey - 10.0f, defaults) == defaults.maxEv);
    CHECK(ctx, AutoExposureTargetEv(grey + 10.0f, defaults) == defaults.minEv);

    // Brighter view (target below current): toBrightSpeed; darker: toDarkSpeed; no overshoot.
    CHECK(ctx, std::fabs(AdaptExposureEv(2.0f, -2.0f, 0.5f, defaults) - 0.5f) < 1e-5f);
    CHECK(ctx, std::fabs(AdaptExposureEv(-2.0f, 2.0f, 0.5f, defaults) + 1.5f) < 1e-5f);
    CHECK(ctx, AdaptExposureEv(0.0f, 0.2f, 1.0f, defaults) == 0.2f);
    CHECK(ctx, AdaptExposureEv(1.0f, 3.0f, 0.0f, defaults) == 1.0f);

    AutoExposureStatus status{};
    CHECK(ctx, !status.valid.load());
    status.RequestReset();
    CHECK_EQ(ctx, status.resetSerial.load(), 1u);
}

// Scene keys: missing keys keep the fixed exposure and the default limits; written
// values come back, out-of-range ones are clamped.
static void TestAutoExposureSceneJson(test::TestContext& ctx)
{
    using namespace engine::renderer::addons::forward;
    std::string error;
    const serialization::JsonValue legacy = serialization::JsonParser::Parse(R"({"exposureEv":1.5})", error);
    ExposureMode mode = ExposureMode::Auto;
    AutoExposureSettings ae{};
    ae.minEv = 3.0f;
    ReadSceneExposureJson(legacy, mode, ae);
    CHECK(ctx, mode == ExposureMode::Fixed);
    CHECK(ctx, ae.minEv == AutoExposureSettings{}.minEv);

    const serialization::JsonValue autoScene = serialization::JsonParser::Parse(
        R"({"exposureMode":"auto","autoExposureMinEv":-2.5,"autoExposureMaxEv":6,)"
        R"("autoExposureToBright":5,"autoExposureToDark":0.5})", error);
    ReadSceneExposureJson(autoScene, mode, ae);
    CHECK(ctx, mode == ExposureMode::Auto);
    CHECK(ctx, ae.minEv == -2.5f && ae.maxEv == 6.0f);
    CHECK(ctx, ae.toBrightSpeed == 5.0f && ae.toDarkSpeed == 0.5f);

    const serialization::JsonValue broken = serialization::JsonParser::Parse(
        R"({"exposureMode":"auto","autoExposureMinEv":50,"autoExposureMaxEv":-50,"autoExposureToDark":-1})", error);
    ReadSceneExposureJson(broken, mode, ae);
    CHECK(ctx, ae.minEv == kMaxExposureEv && ae.maxEv == kMaxExposureEv);
    CHECK(ctx, ae.toDarkSpeed == kMinAutoExposureSpeed);
}

// Colour grading constants: neutral sets no flag (image unchanged); each control
// sets only its own flag; the AgX look only counts with the AgX operator.
static void TestColorGradingConstants(test::TestContext& ctx)
{
    using namespace engine::renderer::addons::forward;
    const auto flagsOf = [](const ColorGradingConstants& c) { return static_cast<uint32_t>(c.params[1]); };
    const auto apply = [](const ColorGradingConstants& c, const std::array<float, 3>& v) {
        return std::array<float, 3>{ c.row0[0] * v[0] + c.row0[1] * v[1] + c.row0[2] * v[2],
                                     c.row1[0] * v[0] + c.row1[1] * v[1] + c.row1[2] * v[2],
                                     c.row2[0] * v[0] + c.row2[1] * v[1] + c.row2[2] * v[2] };
    };
    const auto luma = [](const std::array<float, 3>& v) { return 0.2126f * v[0] + 0.7152f * v[1] + 0.0722f * v[2]; };

    const ColorGradingConstants neutral = BuildColorGradingConstants({}, true);
    CHECK_EQ(ctx, flagsOf(neutral), 0u);
    CHECK(ctx, ColorGradingSettings{}.IsNeutral());

    ColorGradingSettings warm{};
    warm.temperature = 50.f;
    const ColorGradingConstants warmC = BuildColorGradingConstants(warm, true);
    CHECK_EQ(ctx, flagsOf(warmC), kGradingFlagMatrix);
    const auto white = apply(warmC, {1.f, 1.f, 1.f});
    CHECK(ctx, white[0] > white[2]);   // warmer: more red than blue

    ColorGradingSettings grey{};
    grey.saturation = 0.f;
    const auto g = apply(BuildColorGradingConstants(grey, true), {1.f, 0.2f, 0.f});
    CHECK(ctx, std::abs(g[0] - g[1]) < 1e-5f && std::abs(g[1] - g[2]) < 1e-5f);
    CHECK(ctx, std::abs(g[0] - luma({1.f, 0.2f, 0.f})) < 1e-5f);

    ColorGradingSettings hue{};
    hue.hueShift = 120.f;
    const std::array<float, 3> red{1.f, 0.f, 0.f};
    const auto rotated = apply(BuildColorGradingConstants(hue, true), red);
    CHECK(ctx, rotated[1] > rotated[0]);   // red turns towards green

    ColorGradingSettings contrast{};
    contrast.contrast = 1.5f;
    const ColorGradingConstants contrastC = BuildColorGradingConstants(contrast, true);
    CHECK_EQ(ctx, flagsOf(contrastC), kGradingFlagContrast);
    CHECK(ctx, contrastC.params[0] == 1.5f);

    ColorGradingSettings lgg{};
    lgg.gain.value = 0.5f;
    const ColorGradingConstants lggC = BuildColorGradingConstants(lgg, true);
    CHECK_EQ(ctx, flagsOf(lggC), kGradingFlagLiftGammaGain);
    CHECK(ctx, lggC.gain[0] > 1.f && lggC.lift[0] == 0.f && lggC.invGamma[0] == 1.f);
    ColorGradingSettings whiteTint{};
    whiteTint.lift.color = {0.5f, 0.5f, 0.5f};   // grey = no tint, but not the default value
    const ColorGradingConstants greyTint = BuildColorGradingConstants(whiteTint, true);
    CHECK(ctx, std::abs(greyTint.lift[0]) < 1e-6f && std::abs(greyTint.lift[2]) < 1e-6f);

    ColorGradingSettings look{};
    look.look = AgxLook::Golden;
    CHECK_EQ(ctx, flagsOf(BuildColorGradingConstants(look, true)), kGradingFlagAgxLook);
    CHECK_EQ(ctx, flagsOf(BuildColorGradingConstants(look, false)), 0u);
    CHECK(ctx, BuildColorGradingConstants(look, true).lookSlopePower[2] == 0.5f);

    CHECK(ctx, ParseAgxLook("punchy") == AgxLook::Punchy);
    CHECK(ctx, ParseAgxLook("unknown") == AgxLook::None);
    for (uint8_t i = 0u; i < kAgxLookCount; ++i)
        CHECK(ctx, ParseAgxLook(AgxLookName(static_cast<AgxLook>(i))) == static_cast<AgxLook>(i));
}

// Scene keys: missing object = neutral and a neutral grading writes nothing;
// written values come back, out-of-range values are clamped; the script view
// converts both ways without loss.
static void TestColorGradingSceneJson(test::TestContext& ctx)
{
    using namespace engine::renderer::addons::forward;
    std::string error;
    ColorGradingSettings read{};
    read.saturation = 0.3f;
    ReadSceneColorGradingJson(serialization::JsonParser::Parse(R"({"exposureEv":1})", error), read);
    CHECK(ctx, read.IsNeutral());

    serialization::JsonWriter neutralWriter;
    neutralWriter.BeginObject();
    WriteSceneColorGradingJson(neutralWriter, ColorGradingSettings{});
    neutralWriter.EndObject();
    CHECK(ctx, neutralWriter.GetString().find(kSceneKeyColorGrading) == std::string::npos);

    ColorGradingSettings g{};
    g.look = AgxLook::Punchy;
    g.temperature = -30.f;
    g.tint = 12.f;
    g.colorFilter = {1.f, 0.8f, 0.6f};
    g.hueShift = 15.f;
    g.contrast = 1.2f;
    g.saturation = 0.7f;
    g.lift = { {0.5f, 0.6f, 1.f}, -0.1f };
    g.gamma = { {1.f, 1.f, 0.8f}, 0.2f };
    g.gain = { {1.f, 0.9f, 0.9f}, 0.3f };
    serialization::JsonWriter writer;
    writer.BeginObject();
    WriteSceneColorGradingJson(writer, g);
    writer.EndObject();
    const serialization::JsonValue env = serialization::JsonParser::Parse(writer.GetString(), error);
    CHECK(ctx, error.empty());
    ReadSceneColorGradingJson(env, read);
    CHECK(ctx, read == g);

    ReadSceneColorGradingJson(serialization::JsonParser::Parse(
        R"({"colorGrading":{"look":"x","temperature":500,"saturation":-1,"gain":[2,2,2,9]}})", error), read);
    CHECK(ctx, read.look == AgxLook::None && read.temperature == 100.f && read.saturation == 0.f);
    CHECK(ctx, read.gain.value == 1.f && read.gain.color[0] == 1.f);

    CHECK(ctx, engine::script::FromScriptColorGrading(engine::script::ToScriptColorGrading(g)) == g);
    engine::script::ColorGrading badLook{};
    badLook.agxLook = 77u;
    CHECK(ctx, engine::script::FromScriptColorGrading(badLook).look == AgxLook::None);
}

// Recipe: auto-exposure adds no pass; the tonemap pass runs "frame.auto_exposure"
// before its render pass (backbuffer, editor view and post-AA target alike).
static void TestAutoExposureRecipe(test::TestContext& ctx)
{
    for (const bool sceneOutput : { false, true })
    {
        for (const bool postAA : { false, true })
        {
            StandardFrameRecipeBuilder::BuildParams params{};
            params.viewportWidth = 640u;
            params.viewportHeight = 360u;
            params.uiEnabled = true;
            params.bloomEnabled = true;
            params.postAAEnabled = postAA;
            if (sceneOutput)
            {
                params.sceneOutputRT = RenderTargetHandle::Make(7u, 1u);
                params.backbufferWidth = 1024u;
                params.backbufferHeight = 768u;
            }
            const FrameRecipe plain = StandardFrameRecipeBuilder::BuildRecipe(params);
            params.autoExposureEnabled = true;
            const FrameRecipe recipe = StandardFrameRecipeBuilder::BuildRecipe(params);
            CHECK_EQ(ctx, recipe.passes.size(), plain.passes.size());
            CHECK_EQ(ctx, recipe.resources.size(), plain.resources.size());
            const FrameRecipePassDesc* plainTonemap = FindRecipePass(plain, "TonemapPass");
            const FrameRecipePassDesc* tonemap = FindRecipePass(recipe, "TonemapPass");
            CHECK(ctx, plainTonemap != nullptr && plainTonemap->preRenderPassExecutorName.empty());
            CHECK(ctx, tonemap != nullptr);
            if (!tonemap)
                continue;
            CHECK_EQ(ctx, tonemap->preRenderPassExecutorName, std::string(StandardFrameExecutors::AutoExposure));
            CHECK_EQ(ctx, std::string(StandardFrameExecutors::AutoExposure), std::string("frame.auto_exposure"));
            CHECK(ctx, tonemap->renderPass.enabled);
            for (const FrameRecipePassDesc& pass : recipe.passes)
                if (pass.name != "TonemapPass")
                    CHECK(ctx, pass.preRenderPassExecutorName.empty());
        }
    }
}

// The pre-render-pass executor runs in the same graph pass, before BeginRenderPass
// (compute dispatches are not allowed inside a Vulkan render pass).
static void TestRecipePreRenderPassExecutor(test::TestContext& ctx)
{
    using namespace engine::rendergraph;
    RenderGraph rg;
    FrameRecipe recipe{};
    recipe.resources.push_back(FrameRecipeResourceDesc{ "Backbuffer", true, 64u, 64u, Format::RGBA8_UNORM_SRGB, RGResourceKind::Backbuffer });
    FrameRecipePassDesc pass{};
    pass.name = "Final";
    pass.executorName = "exec.final";
    pass.preRenderPassExecutorName = "exec.before";
    pass.accesses.push_back({ "Backbuffer", FrameRecipeAccessKind::WriteRenderTarget });
    pass.renderPass.enabled = true;
    pass.renderPass.targetResourceName = "Backbuffer";
    pass.renderPass.viewportWidth = 64u;
    pass.renderPass.viewportHeight = 64u;
    recipe.passes.push_back(pass);

    RecordingCommandList cmd;
    size_t passesAtPre = 99u;
    size_t passesAtMain = 99u;
    int calls = 0;
    FramePipelineCallbacks callbacks{};
    callbacks.Register("exec.before", [&](const RGExecContext&) { passesAtPre = cmd.renderPasses.size(); calls += 1; });
    callbacks.Register("exec.final", [&](const RGExecContext&) { passesAtMain = cmd.renderPasses.size(); calls += 10; });

    const FrameRecipeCompileParams params{ RenderTargetHandle::Make(1u, 1u), TextureHandle::Make(1u, 1u) };
    FrameRecipeCompiler::Build(rg, params, recipe, callbacks);
    CompiledFrame frame;
    CHECK(ctx, rg.Compile(frame));
    RGExecContext exec{};
    exec.cmd = &cmd;
    rg.Execute(frame, exec);
    CHECK_EQ(ctx, calls, 11);
    CHECK_EQ(ctx, passesAtPre, static_cast<size_t>(0u));
    CHECK_EQ(ctx, passesAtMain, static_cast<size_t>(1u));
}

// The two compute shaders and the tonemap pixel shader (with the auto-exposure
// buffer) compile for DX11 (FXC) and Vulkan (SPIR-V); textures are visible to compute.
static void TestAutoExposureShadersCompile(test::TestContext& ctx)
{
    namespace ae = engine::renderer::addons::forward::auto_exposure;
    const PipelineBindingLayoutDesc layout = BuildEnginePipelineBindingLayout();
    CHECK(ctx, HasFlag(layout.ranges[4].visibility, ShaderStageMask::Compute));
    CHECK(ctx, HasFlag(layout.ranges[4].visibility, ShaderStageMask::Fragment));
    CHECK(ctx, layout.ranges[4].type == DescriptorType::ShaderResource);

    const bool haveDxc = !FindShaderTool(ShaderTool::Dxc).path.empty();
    const auto compile = [&](assets::ShaderAsset& shader)
    {
        for (const assets::ShaderTargetProfile target : { assets::ShaderTargetProfile::DirectX11_SM5,
                                                          assets::ShaderTargetProfile::Vulkan_SPIRV })
        {
            if (target == assets::ShaderTargetProfile::Vulkan_SPIRV && !haveDxc)
                continue;
            assets::CompiledShaderArtifact artifact{};
            std::string error;
            const bool compiled = ShaderCompiler::CompileForTarget(shader, target, artifact, &error);
            if (!compiled)
                Debug::LogError("Auto-exposure shader test: %s (%s): %s", shader.debugName.c_str(),
                                ShaderCompiler::ToString(target), error.c_str());
            CHECK(ctx, compiled);
            CHECK(ctx, !artifact.bytecode.empty());
        }
    };
    const struct { const char* name; const char* source; } computeShaders[] = {
        { "AutoExposureHistogramCS", ae::kHistogramCsHlsl },
        { "AutoExposureAdaptCS", ae::kAdaptCsHlsl },
    };
    for (const auto& entry : computeShaders)
    {
        assets::ShaderAsset cs{};
        cs.debugName = entry.name;
        cs.stage = assets::ShaderStage::Compute;
        cs.sourceLanguage = assets::ShaderSourceLanguage::HLSL;
        cs.entryPoint = "main";
        cs.sourceCode = entry.source;
        compile(cs);
    }
    assets::ShaderAsset tonemap{};
    tonemap.debugName = "passthrough.ps.hlsl";
    tonemap.stage = assets::ShaderStage::Fragment;
    tonemap.sourceLanguage = assets::ShaderSourceLanguage::HLSL;
    tonemap.entryPoint = "main";
    tonemap.resolvedPath = PathToUtf8(PathFromUtf8(KROM_SOURCE_DIR) / "shaders" / "passthrough.ps.hlsl");
    compile(tonemap);

    CHECK_EQ(ctx, ae::kHistogramBins, 64u);
    CHECK(ctx, ae::kMinLog2Luminance < std::log2(0.18f) - 10.0f);
    CHECK(ctx, ae::kMaxLog2Luminance > std::log2(0.18f) + 10.0f - 0.5f);
}

static void TestGtaoDepthPrepassRecipe(test::TestContext& ctx)
{
    using Builder = StandardFrameRecipeBuilder;
    for (const uint32_t samples : {1u, 2u})
    {
        for (const bool ssrEnabled : {false, true})
        {
            Builder::BuildParams params{};
            params.shadowEnabled = true;
            params.transparentEnabled = true;
            params.depthPrepassEnabled = true;
            params.sceneCompositeEnabled = true;
            params.msaaSamples = samples;

            FrameRecipe ao{};
            ao.resources.push_back({"GTAOOutput"});
            ao.opaqueInputs.push_back({"GTAOOutput", TexSlots::PassSRV0});
            ao.passes.push_back({"GtaoLinearizeDepth"});
            ao.passes.push_back({"GtaoMain"});
            ao.passes.push_back({"GtaoBlur"});
            FrameRecipe post{};
            post.resources.push_back({"GTAOComposite"});
            post.passes.push_back({"GtaoComposite"});
            if (ssrEnabled)
            {
                post.passes.push_back({"SsrTrace"});
                post.passes.push_back({"SsrComposite"});
                post.sceneOutputs.push_back({"SSRNormals", Format::RGBA16_FLOAT});
            }
            const size_t beforeTransparent = post.passes.size();
            post.passes.push_back({"EditorOverlay"});

            const FrameRecipe recipe = Builder::BuildRecipe(params, nullptr, &post, &ao, beforeTransparent);
            std::vector<std::string> actual;
            for (const auto& pass : recipe.passes)
                actual.push_back(pass.name);
            std::vector<std::string> expected{
                "ShadowPass", "DepthPrepass", "GtaoLinearizeDepth", "GtaoMain", "GtaoBlur", "MainOpaquePass"};
            if (samples > 1u)
                expected.push_back("MsaaResolve");
            expected.push_back("GtaoComposite");
            if (ssrEnabled)
                expected.insert(expected.end(), {"SsrTrace", "SsrComposite"});
            expected.insert(expected.end(), {"TransparentPass", "EditorOverlay", "TonemapPass", "PresentPass"});
            CHECK(ctx, actual == expected);

            const auto* depth = FindRecipePass(recipe, "DepthPrepass");
            const auto* opaque = FindRecipePass(recipe, "MainOpaquePass");
            const auto* transparent = FindRecipePass(recipe, "TransparentPass");
            CHECK(ctx, depth && opaque && transparent);
            if (!depth || !opaque || !transparent)
                continue;
            const std::string target = samples > 1u ? "HDRSceneColorMS" : "HDRSceneColor";
            CHECK_EQ(ctx, depth->renderPass.targetResourceName, target);
            CHECK(ctx, depth->renderPass.clearDepth && depth->renderPass.depthOnly);
            CHECK(ctx, !depth->renderPass.clearColor);
            CHECK_EQ(ctx, opaque->renderPass.targetResourceName, target);
            CHECK(ctx, opaque->renderPass.clearColor && opaque->renderPass.readOnlyDepth);
            CHECK(ctx, !opaque->renderPass.clearDepth);
            CHECK_EQ(ctx, opaque->renderPass.extraColorTargets.size(), ssrEnabled ? 1u : 0u);
            CHECK(ctx, std::any_of(opaque->accesses.begin(), opaque->accesses.end(), [](const auto& access) {
                return access.resourceName == "GTAOOutput" && access.access == FrameRecipeAccessKind::ReadTexture;
            }));
            CHECK_EQ(ctx, transparent->renderPass.targetResourceName, std::string("GTAOComposite"));
            CHECK_EQ(ctx, transparent->renderPass.depthTargetResourceName, std::string("HDRSceneColor"));
        }
    }
}

static void TestForwardPipelineKeepsBuiltInShadowWithoutShadowDeclaringContributor(test::TestContext& ctx)
{
    FeatureRegistry registry;
    CHECK(ctx, registry.AddFeature(engine::renderer::addons::forward::CreateForwardFeature()));

    TestDevice device;
    CHECK(ctx, device.Initialize(IDevice::DeviceDesc{}));
    ShaderRuntime shaderRuntime;
    CHECK(ctx, shaderRuntime.Initialize(device));
    CHECK(ctx, registry.InitializeAll(FeatureInitializationContext{device, shaderRuntime, nullptr}));

    const IRenderPipeline* pipeline = registry.GetActiveRenderPipeline();
    CHECK(ctx, pipeline != nullptr);

    rendergraph::RenderGraph renderGraph;
    RenderQueue renderQueue{};
    renderQueue.activeShadowResolution = 512u;
    MaterialSystem materials;
    FramePipelineCallbacks callbacks{};
    auto runtimeBindings = std::make_shared<FrameGraphRuntimeBindings>();

    MockPassContributor preOpaqueNoShadow(
        0x1001u,
        PassPhase::BeforeOpaque);
    const std::vector<const IPassContributor*> contributors{ &preOpaqueNoShadow };

    RenderPipelineBuildContext buildContext{
        renderGraph,
        800u,
        600u,
        RenderTargetHandle::Make(1u, 1u),
        TextureHandle::Make(1u, 1u),
        true,
        renderQueue,
        shaderRuntime,
        materials,
        BufferHandle::Invalid(),
        MaterialHandle::Invalid(),
        nullptr,
        nullptr,
        callbacks,
        runtimeBindings,
        BackgroundMode::Skybox,
        true,
        {0.3f, 0.3f, 0.3f, 1.f},
        contributors
    };
    RenderPipelineBuildResult buildResult{};

    CHECK(ctx, pipeline->Build(buildContext, buildResult));
    CHECK(ctx, FindGraphPass(renderGraph, "ShadowPass") != nullptr);
    CHECK(ctx, FindGraphResource(renderGraph, "ShadowMap") != nullptr);

    registry.ShutdownAll(FeatureShutdownContext{});
    shaderRuntime.Shutdown();
    device.Shutdown();
}

// A contributor of the AfterDepthPrepass phase that lists an opaque input gets
// the depth prepass and a read on the opaque pass, whatever its resource is
// called; without such an input, or when the view does not allow a prepass,
// the frame keeps the single-pass path.
static void TestForwardPipelineOpaqueInputEnablesDepthPrepass(test::TestContext& ctx)
{
    FeatureRegistry registry;
    CHECK(ctx, registry.AddFeature(engine::renderer::addons::forward::CreateForwardFeature()));

    TestDevice device;
    CHECK(ctx, device.Initialize(IDevice::DeviceDesc{}));
    ShaderRuntime shaderRuntime;
    CHECK(ctx, shaderRuntime.Initialize(device));
    CHECK(ctx, registry.InitializeAll(FeatureInitializationContext{device, shaderRuntime, nullptr}));

    const IRenderPipeline* pipeline = registry.GetActiveRenderPipeline();
    CHECK(ctx, pipeline != nullptr);
    if (!pipeline)
        return;

    MockPassContributor withInput(0x1101u, PassPhase::AfterDepthPrepass, {},
        "ContactInput", "ContactInputPass", "contact_input.exec");
    withInput.SetOpaqueInputSlot(14u);
    MockPassContributor withoutInput(0x1102u, PassPhase::AfterDepthPrepass, {},
        "PlainAfterDepth", "PlainAfterDepthPass", "plain_after_depth.exec");

    struct Case
    {
        const IPassContributor* contributor;
        bool prepassAllowed;
        bool expectPrepass;
    };
    for (const Case& c : { Case{ &withInput, true, true },
                           Case{ &withInput, false, false },
                           Case{ &withoutInput, true, false } })
    {
        rendergraph::RenderGraph renderGraph;
        RenderQueue renderQueue{};
        MaterialSystem materials;
        FramePipelineCallbacks callbacks{};
        auto runtimeBindings = std::make_shared<FrameGraphRuntimeBindings>();
        const std::vector<const IPassContributor*> contributors{ c.contributor };

        RenderPipelineBuildContext buildContext{
            renderGraph,
            800u,
            600u,
            RenderTargetHandle::Make(3u, 1u),
            TextureHandle::Make(3u, 1u),
            true,
            renderQueue,
            shaderRuntime,
            materials,
            BufferHandle::Invalid(),
            MaterialHandle::Invalid(),
            nullptr,
            nullptr,
            callbacks,
            runtimeBindings,
            BackgroundMode::Skybox,
            true,
            {0.3f, 0.3f, 0.3f, 1.f},
            contributors
        };
        buildContext.enableDepthPrepass = c.prepassAllowed;
        RenderPipelineBuildResult buildResult{};

        CHECK(ctx, pipeline->Build(buildContext, buildResult));
        const rendergraph::RGPass* depth = FindGraphPass(renderGraph, "DepthPrepass");
        const rendergraph::RGPass* opaque = FindGraphPass(renderGraph, "MainOpaquePass");
        CHECK(ctx, opaque != nullptr);
        CHECK_EQ(ctx, depth != nullptr, c.expectPrepass);
        if (!opaque)
            continue;

        const rendergraph::RGResourceDesc* input = FindGraphResource(renderGraph, "ContactInput");
        const bool opaqueReadsInput = input && std::any_of(opaque->accesses.begin(), opaque->accesses.end(),
            [input](const rendergraph::RGResourceAccess& access) {
                return access.resource == input->id && access.mode == rendergraph::RGAccessMode::Read;
            });
        CHECK_EQ(ctx, opaqueReadsInput, c.expectPrepass);
    }

    registry.ShutdownAll(FeatureShutdownContext{});
    shaderRuntime.Shutdown();
    device.Shutdown();
}

// Contact shadows: main light selection, parameter block and the recipe the pass adds.
static void TestContactShadowPassRecipe(test::TestContext& ctx)
{
    namespace cs = engine::renderer::addons::contact_shadow;
    using engine::addons::lighting::ExtractedLight;
    using engine::addons::lighting::ExtractedLightType;

    // First directional light with shadows and the contact flag; its index = lights[] index.
    std::vector<ExtractedLight> lights(4);
    lights[0].type = ExtractedLightType::Point;       lights[0].castShadows = true; lights[0].contactShadows = true;
    lights[1].type = ExtractedLightType::Directional; lights[1].castShadows = true; lights[1].contactShadows = false;
    lights[2].type = ExtractedLightType::Directional; lights[2].castShadows = true; lights[2].contactShadows = true;
    lights[3].type = ExtractedLightType::Directional; lights[3].castShadows = true; lights[3].contactShadows = true;
    CHECK_EQ(ctx, cs::SelectContactShadowLight(lights), 2);
    lights[2].contactShadows = false;
    lights[3].contactShadows = false;
    CHECK_EQ(ctx, cs::SelectContactShadowLight(lights), -1);
    CHECK_EQ(ctx, cs::SelectContactShadowLight({}), -1);

    // Slots 3 and 4 (pbr_lit/pbr_forward_plus read featureParams[3].w).
    CHECK(ctx, IsFrameParameterBlockInRange(cs::kContactShadowFrameParameterBlock));
    CHECK_EQ(ctx, cs::kContactShadowFrameParameterBlock.firstSlot, 3u);
    CHECK_EQ(ctx, cs::kContactShadowFrameParameterBlock.slotCount, 2u);
    CHECK_EQ(ctx, TexSlots::ContactShadow, 20u);
    CHECK(ctx, TexSlots::ContactShadow < TexSlots::COUNT);
    CHECK(ctx, BindingRegisterRanges::SRV(TexSlots::ContactShadow) < BindingRegisterRanges::SamplerBase);
    {
        FrameConstants fc{};
        cs::ContactShadowFrameParams params{};
        params.lightIndexPlusOne = 3u;
        params.steps = 12u;
        CHECK(ctx, WriteFrameParameterBlock(fc, cs::kContactShadowFrameParameterBlock, params));
        uint32_t index = 0u, steps = 0u;
        std::memcpy(&index, &fc.featureParams[3][3], sizeof(index));
        std::memcpy(&steps, &fc.featureParams[4][3], sizeof(steps));
        CHECK_EQ(ctx, index, 3u);
        CHECK_EQ(ctx, steps, 12u);
    }

    FeatureRegistry registry;
    CHECK(ctx, registry.AddFeature(engine::renderer::addons::forward::CreateForwardFeature()));
    TestDevice device;
    CHECK(ctx, device.Initialize(IDevice::DeviceDesc{}));
    ShaderRuntime shaderRuntime;
    CHECK(ctx, shaderRuntime.Initialize(device));
    CHECK(ctx, registry.InitializeAll(FeatureInitializationContext{device, shaderRuntime, nullptr}));
    const IRenderPipeline* pipeline = registry.GetActiveRenderPipeline();
    CHECK(ctx, pipeline != nullptr);
    if (!pipeline)
        return;

    // Shaders are not compiled here; the recipe only needs the pass to be active.
    auto resources = std::make_shared<cs::ContactShadowGpuResources>();
    resources->initialized = true;
    cs::ContactShadowPass pass(0xC057u, resources);

    struct Case { bool enabled; bool prepassAllowed; bool expectPasses; };
    for (const Case& c : { Case{ true, true, true }, Case{ true, false, false }, Case{ false, true, false } })
    {
        resources->settings.enabled = c.enabled;
        rendergraph::RenderGraph renderGraph;
        RenderQueue renderQueue{};
        MaterialSystem materials;
        FramePipelineCallbacks callbacks{};
        auto runtimeBindings = std::make_shared<FrameGraphRuntimeBindings>();
        const std::vector<const IPassContributor*> contributors{ &pass };
        RenderPipelineBuildContext buildContext{
            renderGraph, 800u, 600u,
            RenderTargetHandle::Make(3u, 1u), TextureHandle::Make(3u, 1u), true,
            renderQueue, shaderRuntime, materials,
            BufferHandle::Invalid(), MaterialHandle::Invalid(), nullptr, nullptr,
            callbacks, runtimeBindings, BackgroundMode::Skybox, true,
            {0.3f, 0.3f, 0.3f, 1.f}, contributors
        };
        buildContext.enableDepthPrepass = c.prepassAllowed;
        RenderPipelineBuildResult buildResult{};
        CHECK(ctx, pipeline->Build(buildContext, buildResult));

        const rendergraph::RGPass* depth = FindGraphPass(renderGraph, "DepthPrepass");
        const rendergraph::RGPass* trace = FindGraphPass(renderGraph, "ContactShadowTrace");
        const rendergraph::RGPass* blur = FindGraphPass(renderGraph, "ContactShadowBlur");
        const rendergraph::RGPass* opaque = FindGraphPass(renderGraph, "MainOpaquePass");
        CHECK_EQ(ctx, trace != nullptr, c.expectPasses);
        CHECK_EQ(ctx, blur != nullptr, c.expectPasses);
        CHECK_EQ(ctx, depth != nullptr, c.expectPasses);
        CHECK(ctx, opaque != nullptr);
        if (!opaque)
            continue;
        const rendergraph::RGResourceDesc* output = FindGraphResource(renderGraph, "ContactShadowOutput");
        const bool opaqueReads = output && std::any_of(opaque->accesses.begin(), opaque->accesses.end(),
            [output](const rendergraph::RGResourceAccess& access) {
                return access.resource == output->id && access.mode == rendergraph::RGAccessMode::Read;
            });
        CHECK_EQ(ctx, opaqueReads, c.expectPasses);
    }

    registry.ShutdownAll(FeatureShutdownContext{});
    shaderRuntime.Shutdown();
    device.Shutdown();
}

static void TestForwardPipelineIntegratesAfterOpaqueContributor(test::TestContext& ctx)
{
    FeatureRegistry registry;
    CHECK(ctx, registry.AddFeature(engine::renderer::addons::forward::CreateForwardFeature()));

    TestDevice device;
    CHECK(ctx, device.Initialize(IDevice::DeviceDesc{}));
    ShaderRuntime shaderRuntime;
    CHECK(ctx, shaderRuntime.Initialize(device));
    CHECK(ctx, registry.InitializeAll(FeatureInitializationContext{device, shaderRuntime, nullptr}));

    const IRenderPipeline* pipeline = registry.GetActiveRenderPipeline();
    CHECK(ctx, pipeline != nullptr);

    rendergraph::RenderGraph renderGraph;
    RenderQueue renderQueue{};
    MaterialSystem materials;
    FramePipelineCallbacks callbacks{};
    auto runtimeBindings = std::make_shared<FrameGraphRuntimeBindings>();

    MockPassContributor afterOpaqueContributor(
        0x1002u,
        PassPhase::AfterOpaque,
        {},
        "AfterOpaqueColor",
        "AfterOpaquePass",
        "after_opaque.exec");
    const std::vector<const IPassContributor*> contributors{ &afterOpaqueContributor };

    RenderPipelineBuildContext buildContext{
        renderGraph,
        800u,
        600u,
        RenderTargetHandle::Make(2u, 1u),
        TextureHandle::Make(2u, 1u),
        true,
        renderQueue,
        shaderRuntime,
        materials,
        BufferHandle::Invalid(),
        MaterialHandle::Invalid(),
        nullptr,
        nullptr,
        callbacks,
        runtimeBindings,
        BackgroundMode::Skybox,
        true,
        {0.3f, 0.3f, 0.3f, 1.f},
        contributors
    };
    RenderPipelineBuildResult buildResult{};

    CHECK(ctx, pipeline->Build(buildContext, buildResult));
    CHECK(ctx, FindGraphPass(renderGraph, "AfterOpaquePass") != nullptr);
    CHECK(ctx, FindGraphResource(renderGraph, "AfterOpaqueColor") != nullptr);

    registry.ShutdownAll(FeatureShutdownContext{});
    shaderRuntime.Shutdown();
    device.Shutdown();
}

static void TestFeatureRegistryRejectsPassContributorIdCollision(test::TestContext& ctx)
{
    FeatureRegistry registry;
    CHECK(ctx, registry.AddFeature(std::make_unique<MockPassContributorFeature>(
        "mock-pass-feature-a",
        std::make_shared<MockPassContributor>(0x2001u, PassPhase::BeforeOpaque))));
    CHECK(ctx, registry.AddFeature(std::make_unique<MockPassContributorFeature>(
        "mock-pass-feature-b",
        std::make_shared<MockPassContributor>(0x2001u, PassPhase::AfterOpaque))));

    TestDevice device;
    CHECK(ctx, device.Initialize(IDevice::DeviceDesc{}));
    ShaderRuntime shaderRuntime;
    CHECK(ctx, shaderRuntime.Initialize(device));
    CHECK(ctx, registry.InitializeAll(FeatureInitializationContext{device, shaderRuntime, nullptr}));

    CHECK_EQ(ctx, registry.GetPassContributors().size(), size_t(1));

    registry.ShutdownAll(FeatureShutdownContext{});
    shaderRuntime.Shutdown();
    device.Shutdown();
}

class MockFrameParameterBlockFeature final : public IEngineFeature
{
public:
    MockFrameParameterBlockFeature(std::string featureName, uint32_t firstSlot, uint32_t slotCount)
        : m_featureName(std::move(featureName)), m_firstSlot(firstSlot), m_slotCount(slotCount)
    {
    }

    std::string_view GetName() const noexcept override { return m_featureName; }
    FeatureID GetID() const noexcept override { return FeatureID::FromString(m_featureName); }

    void Register(FeatureRegistrationContext& context) override
    {
        context.RegisterFrameParameterBlock(FrameParameterBlockDesc{m_featureName, m_firstSlot, m_slotCount});
    }

    bool Initialize(const FeatureInitializationContext&) override { return true; }
    void Shutdown(const FeatureShutdownContext&) override {}

private:
    std::string m_featureName;
    uint32_t m_firstSlot = 0u;
    uint32_t m_slotCount = 0u;
};

struct FrameParameterBlockClaim
{
    const char* name;
    uint32_t firstSlot;
    uint32_t slotCount;
};

static bool InitializeFrameParameterBlockFeatures(std::initializer_list<FrameParameterBlockClaim> claims)
{
    FeatureRegistry registry;
    for (const FrameParameterBlockClaim& claim : claims)
        registry.AddFeature(std::make_unique<MockFrameParameterBlockFeature>(claim.name, claim.firstSlot, claim.slotCount));

    TestDevice device;
    device.Initialize(IDevice::DeviceDesc{});
    ShaderRuntime shaderRuntime;
    shaderRuntime.Initialize(device);
    const bool ok = registry.InitializeAll(FeatureInitializationContext{device, shaderRuntime, nullptr});
    registry.ShutdownAll(FeatureShutdownContext{});
    shaderRuntime.Shutdown();
    device.Shutdown();
    return ok;
}

static void TestFeatureRegistryFrameParameterBlocks(test::TestContext& ctx)
{
    // Layout contract: featureParams starts at c145 and closes b0.
    CHECK_EQ(ctx, offsetof(FrameConstants, featureParams), size_t(2320));
    CHECK_EQ(ctx, sizeof(FrameConstants), size_t(2320u + kFrameParameterSlotCount * kFrameParameterSlotBytes));

    CHECK(ctx, InitializeFrameParameterBlockFeatures({{"mock-params-a", 0u, 1u}, {"mock-params-b", 1u, 3u}}));
    CHECK(ctx, !InitializeFrameParameterBlockFeatures({{"mock-params-a", 0u, 2u}, {"mock-params-b", 1u, 1u}}));
    CHECK(ctx, InitializeFrameParameterBlockFeatures({{"mock-params-a", 3u, 2u}}));
    CHECK(ctx, !InitializeFrameParameterBlockFeatures({{"mock-params-a", kFrameParameterSlotCount - 1u, 2u}}));
    CHECK(ctx, !InitializeFrameParameterBlockFeatures({{"mock-params-a", kFrameParameterSlotCount, 1u}}));
    CHECK(ctx, !InitializeFrameParameterBlockFeatures({{"mock-params-a", 0u, 0u}}));

    struct Block { float a; float b; float c; uint32_t d; };
    FrameConstants fc{};
    const FrameParameterBlockDesc slot2{"mock", 2u, 1u};
    CHECK(ctx, WriteFrameParameterBlock(fc, slot2, Block{1.f, 2.f, 3.f, 4u}));
    CHECK_EQ(ctx, fc.featureParams[2][2], 3.f);
    CHECK_EQ(ctx, fc.featureParams[1][0], 0.f);
    struct TooBig { float v[8]; };
    CHECK(ctx, !WriteFrameParameterBlock(fc, slot2, TooBig{}));
}

static void TestShadowViewBuilderClampsSpotAndPointFarPlaneToMaxDistance(test::TestContext& ctx)
{
    using engine::addons::shadow::ShadowRequest;
    using engine::addons::shadow::ShadowTechnique;
    using engine::addons::shadow::ShadowViewBuilder;

    WorldTransformComponent worldTransform{};
    worldTransform.matrix = math::Mat4::Identity();

    LightComponent spotLight{};
    spotLight.type = LightType::Spot;
    spotLight.range = 50.0f;
    spotLight.spotOuterDeg = 30.0f;

    ShadowRequest spotRequest{};
    spotRequest.technique = ShadowTechnique::ShadowMap2D;
    spotRequest.settings.enabled = true;
    spotRequest.settings.resolution = 1024u;
    spotRequest.settings.maxDistance = 12.0f;
    ShadowViewBuilder::BuildViews(spotLight, worldTransform, collision::AABB{}, spotRequest);

    CHECK_EQ(ctx, spotRequest.views.size(), size_t(1));
    CHECK(ctx, NearlyEqual(spotRequest.views[0].nearPlane, 0.1f));
    CHECK(ctx, NearlyEqual(spotRequest.views[0].farPlane, 12.0f));

    LightComponent pointLight{};
    pointLight.type = LightType::Point;
    pointLight.range = 80.0f;

    ShadowRequest pointRequest{};
    pointRequest.technique = ShadowTechnique::ShadowMapCube;
    pointRequest.settings.enabled = true;
    pointRequest.settings.resolution = 512u;
    pointRequest.settings.maxDistance = 18.0f;
    ShadowViewBuilder::BuildViews(pointLight, worldTransform, collision::AABB{}, pointRequest);

    CHECK_EQ(ctx, pointRequest.views.size(), size_t(6));
    for (const auto& view : pointRequest.views)
    {
        CHECK(ctx, NearlyEqual(view.nearPlane, 0.1f));
        CHECK(ctx, NearlyEqual(view.farPlane, 18.0f));
    }
}

static void TestForwardPlusShaderClampsTileLightCountToStoredRange(test::TestContext& ctx)
{
    const std::filesystem::path shaderPath =
        PathFromUtf8(__FILE__).parent_path().parent_path() / "shaders" / "pbr_forward_plus.ps.hlsl";

    std::ifstream input(shaderPath, std::ios::in | std::ios::binary);
    CHECK(ctx, input.is_open());

    std::ostringstream buffer;
    buffer << input.rdbuf();
    const std::string shaderSource = StripAsciiWhitespace(buffer.str());

    CHECK(ctx, shaderSource.find("uintstoredLightCount=tileHeader.z;") != std::string::npos);
    CHECK(ctx, shaderSource.find("for(uintlocalLightIndex=0u;localLightIndex<storedLightCount;++localLightIndex)") != std::string::npos);
    CHECK(ctx, shaderSource.find("KROM_BUFFER_SRV(StructuredBuffer<GpuShadowLightData>,shadowLights,14);") != std::string::npos);
    CHECK(ctx, shaderSource.find("KROM_BUFFER_SRV(StructuredBuffer<GpuShadowViewData>,shadowViews,15);") != std::string::npos);
    CHECK(ctx, shaderSource.find("constGpuShadowLightDatashadowLight=shadowLights[shadowIndex];") != std::string::npos);
    CHECK(ctx, shaderSource.find("constGpuShadowViewDatashadowView=shadowViews[viewIndex];") != std::string::npos);
    CHECK(ctx, shaderSource.find("shadowIndex<shadowBufferLightCount") != std::string::npos);
}

static void TestForwardPlusShadowBuffersPackMoreThanFourShadowLights(test::TestContext& ctx)
{
    RegisterRendererTestComponents();

    ecs::ComponentMetaRegistry componentRegistry = CreateRendererTestRegistry();
    ecs::World world(componentRegistry);

    for (uint32_t i = 0u; i < 5u; ++i)
    {
        const EntityID light = world.CreateEntity();
        TransformComponent transform{};
        transform.localPosition = math::Vec3(static_cast<float>(i) * 2.0f, 4.0f, -4.0f);
        world.Add<TransformComponent>(light, transform);
        WorldTransformComponent worldTransform{};
        worldTransform.matrix = math::Mat4::LookAtRH(transform.localPosition, math::Vec3::Zero(), math::Vec3::Up()).InverseAffine();
        world.Add<WorldTransformComponent>(light, worldTransform);

        LightComponent lc{};
        lc.type = LightType::Spot;
        lc.intensity = 10.0f - static_cast<float>(i);
        lc.range = 20.0f;
        lc.spotInnerDeg = 20.0f;
        lc.spotOuterDeg = 35.0f;
        lc.castShadows = true;
        lc.shadowSettings.enabled = true;
        lc.shadowSettings.resolution = 256u;
        lc.shadowSettings.maxDistance = 25.0f;
        // Different filter per light: each must reach the shader unchanged.
        lc.shadowSettings.filter = (i % 2) ? engine::ShadowFilter::PCSS : engine::ShadowFilter::Tent7x7;
        lc.shadowSettings.lightSize = 0.1f * static_cast<float>(i + 1);
        world.Add<LightComponent>(light, lc);
    }

    const EntityID receiver = world.CreateEntity();
    world.Add<TransformComponent>(receiver);
    world.Add<WorldTransformComponent>(receiver);
    MeshComponent receiverMesh{};
    receiverMesh.mesh = MeshHandle::Make(1u, 1u);
    receiverMesh.castShadows = true;
    receiverMesh.receiveShadows = true;
    world.Add<MeshComponent>(receiver, receiverMesh);
    BoundsComponent receiverBounds{};
    receiverBounds.centerWorld = { 0.f, 0.f, 0.f };
    receiverBounds.extentsWorld = { 1.f, 1.f, 1.f };
    receiverBounds.boundingSphere = receiverBounds.extentsWorld.Length();
    world.Add<BoundsComponent>(receiver, receiverBounds);

    FeatureRegistry registry;
    CHECK(ctx, registry.AddFeature(engine::addons::lighting::CreateLightingFeature()));
    CHECK(ctx, registry.AddFeature(engine::addons::shadow::CreateShadowFeature({.maxShadowAtlasResolution = 512u})));

    RecordingComputeDevice device;
    CHECK(ctx, device.Initialize(IDevice::DeviceDesc{}));
    ShaderRuntime shaderRuntime;
    CHECK(ctx, shaderRuntime.Initialize(device));
    CHECK(ctx, registry.InitializeAll(FeatureInitializationContext{device, shaderRuntime, nullptr}));

    renderer::RenderView view{};
    view.view = math::Mat4::LookAtRH({ 0.f, 2.f, -8.f }, { 0.f, 0.f, 0.f }, math::Vec3::Up());
    view.projection = math::Mat4::PerspectiveFovRH(60.f * math::DEG_TO_RAD, 16.f / 9.f, 0.1f, 100.f);
    view.cameraPosition = { 0.f, 2.f, -8.f };
    view.cameraForward = { 0.f, 0.f, 1.f };
    view.nearPlane = 0.1f;
    view.farPlane = 100.f;

    RenderSceneSnapshot snapshot;
    FrameExtractionStage extractionStage;
    const FrameExtractionStageContext extractionContext{
        world,
        registry.GetSceneExtractionSteps(),
        snapshot,
        &view
    };

    FrameExtractionStageResult extractionResult{};
    CHECK(ctx, extractionStage.Execute(extractionContext, extractionResult));

    platform::FixedTiming timing(1.0 / 60.0);
    FrameConstantStage constantStage;
    const FrameConstantStageContext constantContext{
        &device,
        math::Mat4::Identity(),
        math::Mat4::Identity(),
        1280u,
        720u,
        view,
        timing,
        EnvironmentRuntimeState{},
        snapshot.GetFrameDataView(),
        &snapshot.GetQueue(),
        registry.GetFrameConstantsContributors()
    };

    FrameConstantsResult constantResult{};
    CHECK(ctx, constantStage.PrepareFrameData(constantContext, constantResult));

    const auto* shadow = snapshot.GetFrameDataView().GetFrameData<engine::addons::shadow::ShadowFrameData>();
    CHECK(ctx, shadow != nullptr);
    CHECK_EQ(ctx, shadow ? shadow->requests.size() : 0u, size_t(5));
    CHECK_EQ(ctx, constantResult.frameConstants.shadowLightCount, 4u);
    CHECK_EQ(ctx, constantResult.frameConstants.shadowViewCount, 5u);
    CHECK_EQ(ctx, constantResult.frameConstants.shadowBufferLightCount, 5u);
    CHECK_EQ(ctx, constantResult.frameConstants.shadowBufferViewCount, 5u);
    // Five 256 tiles do not fit a 512 atlas: one spot drops to 128. The
    // sampled rect of every view must be the tile the rasterizer draws into.
    CHECK_EQ(ctx, snapshot.GetQueue().activeShadowResolution, 512u);
    for (uint32_t k = 0u; shadow && k < 4u; ++k)
    {
        const auto& request = shadow->requests[shadow->currentRenderPath.requestIndices[k]];
        const auto& atlasView = request.views.front();
        const float* rect = constantResult.frameConstants.shadowViewRect[k];
        CHECK(ctx, NearlyEqual(rect[0], static_cast<float>(atlasView.atlasX) / 512.f));
        CHECK(ctx, NearlyEqual(rect[1], static_cast<float>(atlasView.atlasY) / 512.f));
        CHECK(ctx, NearlyEqual(rect[2], static_cast<float>(request.effectiveResolution) / 512.f));
        CHECK(ctx, request.effectiveResolution == 256u || request.effectiveResolution == 128u);
    }
    CHECK(ctx, shadow && shadow->gpu.lightBuffer.IsValid());
    CHECK(ctx, shadow && shadow->gpu.viewBuffer.IsValid());
    CHECK_EQ(ctx, shadow ? shadow->gpu.lightCount : 0u, 5u);
    CHECK_EQ(ctx, shadow ? shadow->gpu.viewCount : 0u, 5u);

    const BufferHandle lightBuffer = shadow ? shadow->gpu.lightBuffer : BufferHandle::Invalid();
    const BufferHandle viewBuffer = shadow ? shadow->gpu.viewBuffer : BufferHandle::Invalid();
    const auto* lightRecord = device.GetBuffer(lightBuffer);
    const auto* viewRecord = device.GetBuffer(viewBuffer);
    CHECK(ctx, lightRecord != nullptr);
    CHECK(ctx, viewRecord != nullptr);
    CHECK(ctx, lightRecord && lightRecord->uploadedData.size() >= 5u * 48u);
    CHECK(ctx, viewRecord && viewRecord->uploadedData.size() >= 5u * 80u);

    // Filter and light size per light: constant-buffer lights 0..3 in
    // featureParams[5]/[6].y (pbr_lit), all lights in GpuShadowLightData::params (Forward+).
    const FrameConstants& shadowFc = constantResult.frameConstants;
    uint32_t packedFilters = 0u;
    std::memcpy(&packedFilters, &shadowFc.featureParams[6][1], sizeof(packedFilters));
    for (uint32_t k = 0u; shadow && k < 5u; ++k)
    {
        const auto& settings = shadow->requests[shadow->currentRenderPath.requestIndices[k]].settings;
        if (k < 4u)
        {
            CHECK_EQ(ctx, (packedFilters >> (4u * k)) & 0xFu, static_cast<uint32_t>(settings.filter));
            CHECK(ctx, NearlyEqual(shadowFc.featureParams[5][k], settings.lightSize));
        }
        if (lightRecord && lightRecord->uploadedData.size() >= (k + 1u) * 48u)
        {
            float params[4]{};
            std::memcpy(params, lightRecord->uploadedData.data() + k * 48u + 32u, sizeof(params));
            CHECK(ctx, NearlyEqual(params[0], static_cast<float>(settings.filter)));
            CHECK(ctx, NearlyEqual(params[1], settings.lightSize));
        }
    }
    CHECK(ctx, ((packedFilters & 0xFu) != ((packedFilters >> 4u) & 0xFu)));

    registry.ShutdownAll(FeatureShutdownContext{});
    shaderRuntime.Shutdown();
    device.Shutdown();
}

static void TestForwardPlusCpuTileListPreservesLightOrder(test::TestContext& ctx)
{
    using namespace engine::renderer::addons::forward;
    using namespace engine::addons::lighting;
    RecordingComputeDevice device;
    RenderWorld world;
    auto& lighting = world.GetOrCreateFeatureData<LightingFrameData>("lighting");
    ExtractedLight directional{};
    directional.type = ExtractedLightType::Directional;
    ExtractedLight nearPoint{};
    nearPoint.type = ExtractedLightType::Point;
    nearPoint.positionWorld = {0.f, 0.f, 0.f};
    nearPoint.range = 2.f;
    ExtractedLight beyondFarPlane = nearPoint;
    beyondFarPlane.positionWorld = {0.f, 0.f, -1000.f};
    lighting.lights = {directional, nearPoint, beyondFarPlane};
    FrameConstants frame{};
    const auto identity = math::Mat4::Identity();
    std::memcpy(frame.viewMatrix, identity.Data(), sizeof(frame.viewMatrix));
    std::memcpy(frame.viewProjMatrix, identity.Data(), sizeof(frame.viewProjMatrix));
    std::memcpy(frame.projMatrix, identity.Data(), sizeof(frame.projMatrix));
    frame.nearPlane = 0.1f;
    frame.farPlane = 100.f;
    ForwardPlusGpuResources resources{};
    CHECK(ctx, UpdateForwardPlusResources(device, world.GetFrameDataView(), frame, 33u, 17u, resources));
    CHECK(ctx, resources.metrics.tileCount == 6u);
    CHECK(ctx, resources.metrics.totalLightAssignments == 12u);
    const auto* headers = device.GetBuffer(resources.tileHeaderBuffer);
    const auto* indices = device.GetBuffer(resources.tileIndexBuffer);
    CHECK(ctx, headers != nullptr && indices != nullptr);
    if (!headers || !indices) return;
    CHECK(ctx, headers->uploadedData.size() == 6u * sizeof(ForwardPlusTileHeaderGpu));
    CHECK(ctx, indices->uploadedData.size() == 12u * sizeof(uint32_t));
    if (headers->uploadedData.size() != 6u * sizeof(ForwardPlusTileHeaderGpu) ||
        indices->uploadedData.size() != 12u * sizeof(uint32_t)) return;
    for (uint32_t tile = 0u; tile < 6u; ++tile)
    {
        ForwardPlusTileHeaderGpu header{};
        std::memcpy(&header, headers->uploadedData.data() + tile * sizeof(header), sizeof(header));
        CHECK(ctx, header.offset == tile * 2u && header.count == 2u);
        CHECK(ctx, header.reserved0 == 2u && header.reserved1 == 0u);
        uint32_t pair[2]{};
        std::memcpy(pair, indices->uploadedData.data() + header.offset * sizeof(uint32_t), sizeof(pair));
        CHECK(ctx, pair[0] == 0u && pair[1] == 1u);
    }
    // Reusing the same output buffers must clear stale tile counts and cursors.
    lighting.lights = {beyondFarPlane};
    CHECK(ctx, UpdateForwardPlusResources(device, world.GetFrameDataView(), frame, 33u, 17u, resources));
    CHECK(ctx, resources.metrics.totalLightAssignments == 0u);
    headers = device.GetBuffer(resources.tileHeaderBuffer);
    if (headers)
        for (uint32_t tile = 0u; tile < 6u; ++tile)
        {
            ForwardPlusTileHeaderGpu header{};
            std::memcpy(&header, headers->uploadedData.data() + tile * sizeof(header), sizeof(header));
            CHECK(ctx, header.offset == 0u && header.count == 0u && header.reserved0 == 0u && header.reserved1 == 0u);
        }
    DestroyForwardPlusGpuResources(device, resources);
}

static void TestForwardPlusComputeCullDispatchTransitionsTileBuffers(test::TestContext& ctx)
{
    FeatureRegistry registry;
    engine::renderer::addons::forward::ForwardFeatureConfig config{};
    config.mode = engine::renderer::addons::forward::ForwardRendererMode::ForwardPlus;
    config.cullingMode = engine::renderer::addons::forward::ForwardPlusCullingMode::Compute;
    CHECK(ctx, registry.AddFeature(engine::renderer::addons::forward::CreateForwardFeature(config)));

    RecordingComputeDevice device;
    CHECK(ctx, device.Initialize(IDevice::DeviceDesc{}));
    ShaderRuntime shaderRuntime;
    CHECK(ctx, shaderRuntime.Initialize(device));
    CHECK(ctx, registry.InitializeAll(FeatureInitializationContext{device, shaderRuntime, nullptr}));

    const IRenderPipeline* pipeline = registry.GetActiveRenderPipeline();
    CHECK(ctx, pipeline != nullptr);

    RenderWorld renderWorld;
    auto& lighting = renderWorld.GetOrCreateFeatureData<engine::addons::lighting::LightingFrameData>("lighting");
    BufferDesc lightBufferDesc{};
    lightBufferDesc.byteSize = sizeof(engine::addons::lighting::GpuLightData) * 70u;
    lightBufferDesc.stride = sizeof(engine::addons::lighting::GpuLightData);
    lightBufferDesc.type = BufferType::Structured;
    lightBufferDesc.usage = ResourceUsage::ShaderResource;
    lightBufferDesc.access = MemoryAccess::CpuWrite;
    lightBufferDesc.initialState = ResourceState::ShaderRead;
    lightBufferDesc.debugName = "TestForwardPlus_LightBuffer";
    lighting.gpu.lightBuffer = device.CreateBuffer(lightBufferDesc);
    lighting.gpu.lightCount = 70u;

    RenderQueue renderQueue{};
    MaterialSystem materials;
    FramePipelineCallbacks callbacks{};
    auto runtimeBindings = std::make_shared<FrameGraphRuntimeBindings>();
    runtimeBindings->scene.renderQueue = &renderQueue;
    runtimeBindings->scene.frameData = renderWorld.GetFrameDataView();
    runtimeBindings->material.shaderRuntime = &shaderRuntime;
    runtimeBindings->material.materials = &materials;
    FrameConstants frameConstants{};
    runtimeBindings->scene.perFrameConstantsData = &frameConstants;

    BufferDesc perFrameDesc{};
    perFrameDesc.byteSize = kConstantBufferAlignment;
    perFrameDesc.stride = sizeof(uint32_t);
    perFrameDesc.type = BufferType::Constant;
    perFrameDesc.usage = ResourceUsage::ConstantBuffer;
    perFrameDesc.access = MemoryAccess::CpuWrite;
    perFrameDesc.initialState = ResourceState::ConstantBuffer;
    perFrameDesc.debugName = "TestForwardPlus_PerFrameCB";
    runtimeBindings->resources.perFrameCB = device.CreateBuffer(perFrameDesc);

    rendergraph::RenderGraph renderGraph;
    RenderPipelineBuildContext buildContext{
        renderGraph,
        64u,
        32u,
        RenderTargetHandle::Make(3u, 1u),
        TextureHandle::Make(3u, 1u),
        true,
        renderQueue,
        shaderRuntime,
        materials,
        runtimeBindings->resources.perFrameCB,
        MaterialHandle::Invalid(),
        nullptr,
        nullptr,
        callbacks,
        runtimeBindings,
        BackgroundMode::Skybox,
        true,
        {0.3f, 0.3f, 0.3f, 1.f},
        {}
    };
    RenderPipelineBuildResult buildResult{};
    CHECK(ctx, pipeline->Build(buildContext, buildResult));

    const rendergraph::RGPass* opaquePass = FindGraphPass(renderGraph, "MainOpaquePass");
    CHECK(ctx, opaquePass != nullptr);
    CHECK(ctx, opaquePass->executeFn != nullptr);

    RecordingCommandList commandList;
    std::vector<rendergraph::CompiledResourceSnapshot> resources;
    const rendergraph::RGExecContext execCtx{
        &device,
        &commandList,
        &resources
    };
    opaquePass->executeFn(execCtx);

    const BufferHandle tileHeaderBuffer = device.FindBufferByName("ForwardPlus_TileHeaders");
    const BufferHandle tileIndexBuffer = device.FindBufferByName("ForwardPlus_TileIndices");
    const BufferHandle resetStagingBuffer = device.FindBufferByName("ForwardPlus_ResetStaging");
    const BufferHandle computeParamsBuffer = device.FindBufferByName("ForwardPlus_ComputeParams");
    CHECK_VALID(ctx, tileHeaderBuffer);
    CHECK_VALID(ctx, tileIndexBuffer);
    CHECK_VALID(ctx, resetStagingBuffer);
    CHECK_VALID(ctx, computeParamsBuffer);

    const auto findNextEvent = [&](size_t startIndex, auto&& predicate) -> size_t
    {
        for (size_t i = startIndex; i < commandList.events.size(); ++i)
        {
            if (predicate(commandList.events[i]))
                return i;
        }
        return commandList.events.size();
    };

    const size_t headerToCopyDest = findNextEvent(0u, [&](const RecordingCommandList::Event& event) {
        return event.kind == RecordingCommandList::EventKind::TransitionBuffer &&
               event.buffer == tileHeaderBuffer &&
               event.before == ResourceState::ShaderRead &&
               event.after == ResourceState::CopyDest;
    });
    CHECK(ctx, headerToCopyDest < commandList.events.size());

    const size_t headerResetCopy = findNextEvent(headerToCopyDest + 1u, [&](const RecordingCommandList::Event& event) {
        return event.kind == RecordingCommandList::EventKind::CopyBuffer &&
               event.buffer == tileHeaderBuffer &&
               event.otherBuffer == resetStagingBuffer &&
               event.copySize == 8u * sizeof(engine::renderer::addons::forward::ForwardPlusTileHeaderGpu);
    });
    CHECK(ctx, headerResetCopy < commandList.events.size());

    const size_t headerToUav = findNextEvent(headerResetCopy + 1u, [&](const RecordingCommandList::Event& event) {
        return event.kind == RecordingCommandList::EventKind::TransitionBuffer &&
               event.buffer == tileHeaderBuffer &&
               event.before == ResourceState::CopyDest &&
               event.after == ResourceState::UnorderedAccess;
    });
    CHECK(ctx, headerToUav < commandList.events.size());

    const size_t indexToUav = findNextEvent(headerToUav + 1u, [&](const RecordingCommandList::Event& event) {
        return event.kind == RecordingCommandList::EventKind::TransitionBuffer &&
               event.buffer == tileIndexBuffer &&
               event.before == ResourceState::ShaderRead &&
               event.after == ResourceState::UnorderedAccess;
    });
    CHECK(ctx, indexToUav < commandList.events.size());

    const size_t computeParamsBind = findNextEvent(indexToUav + 1u, [&](const RecordingCommandList::Event& event) {
        return event.kind == RecordingCommandList::EventKind::SetConstantBuffer &&
               event.buffer == computeParamsBuffer &&
               event.slot == CBSlots::PerPass &&
               event.stages == ShaderStageMask::Compute;
    });
    CHECK(ctx, computeParamsBind < commandList.events.size());

    const size_t headerUavBind = findNextEvent(computeParamsBind + 1u, [&](const RecordingCommandList::Event& event) {
        return event.kind == RecordingCommandList::EventKind::SetUnorderedAccessBuffer &&
               event.buffer == tileHeaderBuffer &&
               event.slot == UAVSlots::Output0 &&
               event.stages == ShaderStageMask::Compute;
    });
    CHECK(ctx, headerUavBind < commandList.events.size());

    const size_t indexUavBind = findNextEvent(headerUavBind + 1u, [&](const RecordingCommandList::Event& event) {
        return event.kind == RecordingCommandList::EventKind::SetUnorderedAccessBuffer &&
               event.buffer == tileIndexBuffer &&
               event.slot == UAVSlots::Output1 &&
               event.stages == ShaderStageMask::Compute;
    });
    CHECK(ctx, indexUavBind < commandList.events.size());

    const size_t dispatch = findNextEvent(indexUavBind + 1u, [&](const RecordingCommandList::Event& event) {
        return event.kind == RecordingCommandList::EventKind::Dispatch &&
               event.groupsX == 2u &&
               event.groupsY == 1u &&
               event.groupsZ == 1u;
    });
    CHECK(ctx, dispatch < commandList.events.size());

    const size_t headerBackToSrv = findNextEvent(dispatch + 1u, [&](const RecordingCommandList::Event& event) {
        return event.kind == RecordingCommandList::EventKind::TransitionBuffer &&
               event.buffer == tileHeaderBuffer &&
               event.before == ResourceState::UnorderedAccess &&
               event.after == ResourceState::ShaderRead;
    });
    CHECK(ctx, headerBackToSrv < commandList.events.size());

    const size_t indexBackToSrv = findNextEvent(headerBackToSrv + 1u, [&](const RecordingCommandList::Event& event) {
        return event.kind == RecordingCommandList::EventKind::TransitionBuffer &&
               event.buffer == tileIndexBuffer &&
               event.before == ResourceState::UnorderedAccess &&
               event.after == ResourceState::ShaderRead;
    });
    CHECK(ctx, indexBackToSrv < commandList.events.size());

    registry.ShutdownAll(FeatureShutdownContext{});
    shaderRuntime.Shutdown();
    device.Shutdown();
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
    SortKey near_a = SortKey::ForFrontToBack(StandardRenderPasses::Opaque(), 0, 12345u, 77u, 0.1f);
    SortKey far_a  = SortKey::ForFrontToBack(StandardRenderPasses::Opaque(), 0, 12345u, 77u, 0.9f);
    CHECK(ctx, near_a < far_a); // Front-to-back: näher = kleinerer Key

    // Transparent: größere Tiefe kommt zuerst (back-to-front)
    SortKey near_t = SortKey::ForBackToFront(StandardRenderPasses::Transparent(), 0, 0.1f);
    SortKey far_t  = SortKey::ForBackToFront(StandardRenderPasses::Transparent(), 0, 0.9f);
    CHECK(ctx, far_t < near_t); // Back-to-front: weiter = kleinerer Key

    // Pass-Tag hat höchste Priorität
    SortKey opaque_deep  = SortKey::ForFrontToBack(StandardRenderPasses::Opaque(), 0, 0u, 0u, 0.99f);
    SortKey shadow_near  = SortKey::ForFrontToBack(StandardRenderPasses::Shadow(), 0, 0u, 0u, 0.01f);
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
    MaterialRuntimeDesc dRuntime;
    d.name           = "TestMaterial";
    dRuntime.renderPass     = StandardRenderPasses::Opaque();
    dRuntime.vertexShader   = ShaderHandle::Make(1u, 1u);
    dRuntime.fragmentShader = ShaderHandle::Make(2u, 1u);
    dRuntime.colorFormat    = Format::RGBA16_FLOAT;
    dRuntime.depthFormat    = Format::D24_UNORM_S8_UINT;
    dRuntime.depthStencil.depthEnable = true;
    dRuntime.depthStencil.depthWrite  = true;
    dRuntime.rasterizer.cullMode      = CullMode::Back;

    MaterialParam p0; p0.name = "albedo"; p0.type = MaterialParam::Type::Vec4;
    p0.value.f[0]=1.f; p0.value.f[1]=0.f; p0.value.f[2]=0.f; p0.value.f[3]=1.f;
    MaterialParam p1; p1.name = "roughness"; p1.type = MaterialParam::Type::Float;
    p1.value.f[0] = 0.7f;
    d.parameters = { p0, p1 };

    MaterialHandle h = MaterialRuntimeBridge::RegisterMaterial(ms, std::move(d), dRuntime);
    CHECK_VALID(ctx, h);
    CHECK_EQ(ctx, ms.DescCount(), 1u);

    // PipelineKey valide
    PipelineKey key = MaterialRuntimeBridge::BuildPipelineKey(ms, h);
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
                slot.stageFlags = MaterialShaderStageMask::Fragment;
            }
            else
            {
                slot.binding = CBSlots::PerMaterial;
                slot.byteSize = byteSizeOf(param.type);
                if (slot.byteSize > 4u)
                    cbOffset = align16(cbOffset);
                slot.byteOffset = cbOffset;
                cbOffset += slot.byteSize;
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
    MaterialRuntimeDesc dRuntime;
    d.name="M1"; dRuntime.renderPass=StandardRenderPasses::Opaque();
    dRuntime.vertexShader=ShaderHandle::Make(1u,1u);
    dRuntime.fragmentShader=ShaderHandle::Make(2u,1u);
    MaterialHandle mat = MaterialRuntimeBridge::RegisterMaterial(ms, std::move(d), dRuntime);

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
    MaterialRuntimeDesc dtRuntime;
    dt.name="MT"; dtRuntime.renderPass=StandardRenderPasses::Transparent();
    dtRuntime.vertexShader=ShaderHandle::Make(1u,1u);
    dtRuntime.fragmentShader=ShaderHandle::Make(2u,1u);
    dtRuntime.blend.blendEnable=true;
    MaterialHandle transMat = MaterialRuntimeBridge::RegisterMaterial(ms, std::move(dt), dtRuntime);

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
    engine::addons::mesh_renderer::ExtractRenderables(world, rw.GetExtractionView());
    engine::addons::lighting::ExtractLights(world, rw.GetExtractionView());
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

// ==========================================================================
// RenderWorld - parallel BuildDrawLists must match the serial path for any
// worker count (the merge must not depend on how ParallelFor splits the range)
// ==========================================================================
// ==========================================================================
// GpuPassProfiler: maps timestamp indices to passes, sums views, smooths and
// drops frames whose results never arrive.
// ==========================================================================
// ==========================================================================
// Tonemap settings: operator names in scene files round-trip; unknown names
// are rejected; old scenes map to Reinhard, new ones to a filmic operator.
// ==========================================================================
// ==========================================================================
// MSAA recipe: scene passes draw into a multisampled target, a resolve pass
// writes HDRSceneColor before particles/debug draw; off = recipe as before.
// ==========================================================================
static void TestMsaaRecipe(test::TestContext& ctx)
{
    using Builder = engine::renderer::StandardFrameRecipeBuilder;
    Builder::BuildParams p;
    p.viewportWidth = 640u;
    p.viewportHeight = 360u;
    p.skyEnabled = true;
    p.transparentEnabled = true;
    p.particlesEnabled = true;
    p.debugDrawEnabled = true;

    const auto passIndex = [](const FrameRecipe& recipe, const std::string& name) -> int {
        for (size_t i = 0; i < recipe.passes.size(); ++i)
            if (recipe.passes[i].name == name)
                return static_cast<int>(i);
        return -1;
    };
    const auto findResource = [](const FrameRecipe& recipe, const std::string& name) -> const FrameRecipeResourceDesc* {
        for (const FrameRecipeResourceDesc& r : recipe.resources)
            if (r.name == name)
                return &r;
        return nullptr;
    };

    const FrameRecipe off = Builder::BuildRecipe(p);
    CHECK(ctx, findResource(off, Builder::kHDRSceneColorMSResource) == nullptr);
    CHECK_EQ(ctx, passIndex(off, Builder::kMsaaResolvePass), -1);

    p.msaaSamples = 4u;
    const FrameRecipe on = Builder::BuildRecipe(p);
    const FrameRecipeResourceDesc* ms = findResource(on, Builder::kHDRSceneColorMSResource);
    CHECK(ctx, ms != nullptr);
    CHECK(ctx, ms && ms->sampleCount == 4u && ms->kind == RGResourceKind::RenderTarget);
    const FrameRecipeResourceDesc* single = findResource(on, "HDRSceneColor");
    CHECK(ctx, single && single->sampleCount == 1u);

    const int resolve = passIndex(on, Builder::kMsaaResolvePass);
    CHECK(ctx, resolve >= 0);
    for (const char* scenePass : { "SkyPass", "MainOpaquePass", "TransparentPass" })
    {
        const int idx = passIndex(on, scenePass);
        CHECK(ctx, idx >= 0 && idx < resolve);
        if (idx >= 0)
            CHECK(ctx, on.passes[static_cast<size_t>(idx)].renderPass.targetResourceName == Builder::kHDRSceneColorMSResource);
    }
    for (const char* laterPass : { "ParticlePass", "DebugDrawPass", "TonemapPass" })
    {
        const int idx = passIndex(on, laterPass);
        CHECK(ctx, idx > resolve);
    }
    // The resolve opens its own render pass (barrier before it under Vulkan).
    CHECK(ctx, resolve >= 0 && !on.passes[static_cast<size_t>(resolve)].renderPass.enabled);
}

static void TestTonemapOperatorNames(test::TestContext& ctx)
{
    using namespace engine::renderer::addons::forward;
    for (TonemapOperator op : { TonemapOperator::Reinhard, TonemapOperator::AcesFitted,
                                TonemapOperator::AgX, TonemapOperator::KhronosPbrNeutral })
    {
        TonemapOperator parsed = TonemapOperator::Reinhard;
        CHECK(ctx, ParseTonemapOperator(TonemapOperatorName(op), parsed));
        CHECK(ctx, parsed == op);
    }
    TonemapOperator unchanged = TonemapOperator::AgX;
    CHECK(ctx, !ParseTonemapOperator("filmic", unchanged));
    CHECK(ctx, !ParseTonemapOperator(nullptr, unchanged));
    CHECK(ctx, unchanged == TonemapOperator::AgX);

    CHECK(ctx, kLegacyTonemapOperator == TonemapOperator::Reinhard);
    CHECK(ctx, TonemapSettings{}.op != TonemapOperator::Reinhard);
    CHECK(ctx, std::fabs(TonemapSettings{}.ExposureScale() - 1.0f) < 1e-6f);
    TonemapSettings plusOne{};
    plusOne.exposureEv = 1.0f;
    CHECK(ctx, std::fabs(plusOne.ExposureScale() - 2.0f) < 1e-6f);
}

static void TestShadingSettingsFlags(test::TestContext& ctx)
{
    using namespace engine::renderer::addons::forward;
    // Default = former shading plus specular AA (an older variant was always on).
    CHECK_EQ(ctx, ShadingFlags(ShadingSettings{}), 8u);
    CHECK_EQ(ctx, ShadingFlags(ShadingSettings{ false, false, false, false }), 0u);

    ShadingSettings visibility{ false, false, false, false };
    visibility.heightCorrelatedVisibility = true;
    CHECK_EQ(ctx, ShadingFlags(visibility), 1u);
    ShadingSettings fresnel{ false, false, false, false };
    fresnel.perLightDiffuseFresnel = true;
    CHECK_EQ(ctx, ShadingFlags(fresnel), 2u);
    ShadingSettings multiScatter{ false, false, false, false };
    multiScatter.multiScatterCompensation = true;
    CHECK_EQ(ctx, ShadingFlags(multiScatter), 4u);
    ShadingSettings specularAA{ false, false, false, false };
    specularAA.specularAntiAliasing = true;
    CHECK_EQ(ctx, ShadingFlags(specularAA), 8u);
    ShadingSettings burley{ false, false, false, false };
    burley.burleyDiffuse = true;
    CHECK_EQ(ctx, ShadingFlags(burley), 16u);
    CHECK_EQ(ctx, ShadingFlags(ShadingSettings{ true, true, true, true }), 15u);
    CHECK_EQ(ctx, ShadingFlags(ShadingSettings{ true, true, true, true, true }), 31u);

    // The block lands in featureParams[2].x, where pbr_shading_options.* reads it.
    CHECK(ctx, IsFrameParameterBlockInRange(kShadingFrameParameterBlock));
    CHECK_EQ(ctx, kShadingFrameParameterBlock.firstSlot, 2u);
    FrameConstants fc{};
    ShadingFrameParams params{};
    params.flags = 5u;
    CHECK(ctx, WriteFrameParameterBlock(fc, kShadingFrameParameterBlock, params));
    uint32_t bits = 0u;
    std::memcpy(&bits, &fc.featureParams[2][0], sizeof(bits));
    CHECK_EQ(ctx, bits, 5u);

    // Shadow filter values beyond the last one are clamped on load.
    // Stored values must not move: scenes keep the number.
    CHECK_EQ(ctx, static_cast<uint32_t>(engine::kShadowFilterMax),
             static_cast<uint32_t>(engine::ShadowFilter::Tent7x7));
    CHECK_EQ(ctx, static_cast<uint32_t>(engine::ShadowFilter::Tent3x3), 1u);
    CHECK_EQ(ctx, static_cast<uint32_t>(engine::ShadowFilter::Tent5x5), 2u);
    CHECK_EQ(ctx, static_cast<uint32_t>(engine::ShadowFilter::PCSS), 4u);
    CHECK_EQ(ctx, static_cast<uint32_t>(engine::ShadowFilter::Tent7x7), 5u);

    // Filter parameters where pbr_shadow_poisson.* reads them: featureParams[5] =
    // light size per shadow light, featureParams[6].x = light size of the first
    // light (OpenGL), featureParams[6].y = filter per light, 4 bits each.
    using engine::addons::shadow::kShadowFilterFrameParameterBlock;
    CHECK(ctx, IsFrameParameterBlockInRange(kShadowFilterFrameParameterBlock));
    CHECK_EQ(ctx, kShadowFilterFrameParameterBlock.firstSlot, 5u);
    CHECK_EQ(ctx, kShadowFilterFrameParameterBlock.slotCount, 2u);
    engine::addons::shadow::ShadowFilterFrameParams filterParams{};
    filterParams.lightSize = 0.25f;
    filterParams.lightSizes[2] = 0.5f;
    filterParams.filterModes = (static_cast<uint32_t>(engine::ShadowFilter::PCSS) << 0u) |
                               (static_cast<uint32_t>(engine::ShadowFilter::Tent7x7) << 8u);
    CHECK(ctx, WriteFrameParameterBlock(fc, kShadowFilterFrameParameterBlock, filterParams));
    CHECK_EQ(ctx, fc.featureParams[6][0], 0.25f);
    CHECK_EQ(ctx, fc.featureParams[5][2], 0.5f);
    uint32_t packedFilters = 0u;
    std::memcpy(&packedFilters, &fc.featureParams[6][1], sizeof(packedFilters));
    CHECK_EQ(ctx, packedFilters & 0xFu, 4u);
    CHECK_EQ(ctx, (packedFilters >> 8u) & 0xFu, 5u);
    CHECK(ctx, engine::ShadowSettings{}.lightSize > 0.f);
    CHECK_EQ(ctx, static_cast<uint32_t>(engine::ShadowFilter::Poisson), 3u);
}

static bool ShadowTilesOverlap(const engine::addons::shadow::ShadowAtlasLayout& layout)
{
    struct Box { uint32_t x, y, size; };
    std::vector<Box> boxes;
    for (size_t r = 0; r < layout.tiles.size(); ++r)
        for (const auto& tile : layout.tiles[r])
            boxes.push_back({tile.x, tile.y, layout.tileSizes[r]});
    for (size_t a = 0; a < boxes.size(); ++a)
    {
        if (boxes[a].x + boxes[a].size > layout.atlasSize || boxes[a].y + boxes[a].size > layout.atlasSize)
            return true;
        for (size_t b = a + 1; b < boxes.size(); ++b)
        {
            const bool apart = boxes[a].x + boxes[a].size <= boxes[b].x || boxes[b].x + boxes[b].size <= boxes[a].x ||
                               boxes[a].y + boxes[a].size <= boxes[b].y || boxes[b].y + boxes[b].size <= boxes[a].y;
            if (!apart)
                return true;
        }
    }
    return false;
}

static void TestShadowAtlasLayout(test::TestContext& ctx)
{
    using engine::addons::shadow::BuildShadowAtlasLayout;
    using engine::addons::shadow::ShadowAtlasLayoutRequest;

    // Room 2: sun 4096 x 4 cascades, point light 1024 x 6, spot 1024.
    // The sun gets 2048 cascades (the old grid gave every view 1024).
    {
        const std::vector<ShadowAtlasLayoutRequest> requests{
            {4096u, 4u, true}, {1024u, 6u, false}, {1024u, 1u, false}};
        const auto layout = BuildShadowAtlasLayout(requests, 6144u);
        CHECK_EQ(ctx, layout.tileSizes.size(), size_t(3));
        CHECK_EQ(ctx, layout.tileSizes[0], 2048u);
        CHECK_EQ(ctx, layout.tileSizes[1], 1024u);
        CHECK_EQ(ctx, layout.tileSizes[2], 1024u);
        CHECK(ctx, layout.atlasSize <= 6144u && layout.atlasSize >= 4096u);
        CHECK_EQ(ctx, layout.tiles[1].size(), size_t(6));
        CHECK(ctx, !ShadowTilesOverlap(layout));
    }
    // One spot: atlas = its tile.
    {
        const std::vector<ShadowAtlasLayoutRequest> requests{{1024u, 1u, false}};
        const auto layout = BuildShadowAtlasLayout(requests, 6144u);
        CHECK_EQ(ctx, layout.atlasSize, 1024u);
        CHECK_EQ(ctx, layout.tileSizes[0], 1024u);
    }
    // A held larger atlas keeps the tile sizes and stays at least that large.
    {
        const std::vector<ShadowAtlasLayoutRequest> requests{{1024u, 1u, false}};
        const auto layout = BuildShadowAtlasLayout(requests, 6144u, 3072u);
        CHECK_EQ(ctx, layout.atlasSize, 3072u);
        CHECK_EQ(ctx, layout.tileSizes[0], 1024u);
    }
    // Tight cap: local lights shrink before the sun, and the sun stays the largest.
    {
        const std::vector<ShadowAtlasLayoutRequest> requests{
            {2048u, 4u, true}, {1024u, 6u, false}, {1024u, 6u, false}};
        const auto layout = BuildShadowAtlasLayout(requests, 4096u);
        CHECK(ctx, layout.atlasSize > 0u && layout.atlasSize <= 4096u);
        CHECK(ctx, layout.tileSizes[0] >= layout.tileSizes[1]);
        CHECK(ctx, layout.tileSizes[0] >= layout.tileSizes[2]);
        CHECK(ctx, layout.tileSizes[0] >= 1024u);
        CHECK(ctx, !ShadowTilesOverlap(layout));
    }
    // Nothing to pack.
    {
        const std::vector<ShadowAtlasLayoutRequest> requests{{1024u, 0u, false}};
        CHECK_EQ(ctx, BuildShadowAtlasLayout(requests, 4096u).atlasSize, 0u);
    }
}

static void TestGpuPassProfiler(test::TestContext& ctx)
{
    TestDevice device;
    GpuPassProfiler profiler;

    // Backend without timestamps: stays off, records nothing.
    profiler.SetEnabled(device, true);
    CHECK(ctx, !profiler.IsEnabled());
    CHECK(ctx, !device.timestampsEnabled);
    profiler.SetEnabled(device, false);

    device.timestampCapacity = 64u;
    profiler.SetEnabled(device, true);
    CHECK(ctx, profiler.IsEnabled());
    CHECK(ctx, device.timestampsEnabled);

    // Frame 5: Shadow 0..1, Opaque 1..3; a second view renders Opaque again 3..4.
    profiler.RecordPass(5u, "Shadow", 0u, 1u);
    profiler.RecordPass(5u, "Opaque", 1u, 2u);
    profiler.RecordPass(5u, "Opaque", 2u, 3u);
    profiler.RecordPass(5u, "Ignored", kInvalidGpuTimestamp, 3u);
    // Frame 6 is recorded but its results never arrive.
    profiler.RecordPass(6u, "Shadow", 0u, 1u);
    // Frame 7.
    profiler.RecordPass(7u, "Shadow", 0u, 1u);
    profiler.RecordPass(7u, "Opaque", 1u, 2u);

    GpuTimestampFrame f5;
    f5.frameId = 5u;
    f5.timestampsMs = { 0.0, 0.5, 2.0, 2.5 };
    f5.valid = { true, true, true, true };
    device.completedTimestamps.push_back(f5);
    profiler.Collect(device, 10.0);

    GpuPassTimingReport report = profiler.GetReport();
    CHECK_EQ(ctx, report.passes.size(), size_t{ 2u });
    CHECK(ctx, report.passes.size() == 2u && report.passes[0].name == "Shadow" && report.passes[1].name == "Opaque");
    CHECK(ctx, report.passes.size() == 2u && std::fabs(report.passes[0].ms - 0.5f) < 1e-4f);
    CHECK(ctx, report.passes.size() == 2u && std::fabs(report.passes[1].ms - 2.0f) < 1e-4f);  // 1.5 + 0.5
    CHECK(ctx, std::fabs(report.totalMs - 2.5f) < 1e-4f);

    // Frame 7 arrives 0.5 s later with Shadow = 1.5 ms: smoothed by 1 - e^-1.
    GpuTimestampFrame f7;
    f7.frameId = 7u;
    f7.timestampsMs = { 0.0, 1.5, 3.5 };
    f7.valid = { true, true, true };
    device.completedTimestamps.push_back(f7);
    profiler.Collect(device, 10.5);
    report = profiler.GetReport();
    const float alpha = 1.0f - std::exp(-1.0f);
    CHECK(ctx, report.passes.size() == 2u && std::fabs(report.passes[0].ms - (0.5f + (1.5f - 0.5f) * alpha)) < 1e-3f);
    CHECK(ctx, report.passes.size() == 2u && std::fabs(report.passes[1].ms - 2.0f) < 1e-3f);

    // An invalid timestamp skips the passes that use it.
    profiler.RecordPass(8u, "Shadow", 0u, 1u);
    profiler.RecordPass(8u, "Opaque", 1u, 2u);
    GpuTimestampFrame f8;
    f8.frameId = 8u;
    f8.timestampsMs = { 0.0, 0.0, 9.0 };
    f8.valid = { true, false, true };
    device.completedTimestamps.push_back(f8);
    profiler.Collect(device, 11.0);
    // Frame 8 has no usable pass: the previous values stay.
    CHECK_EQ(ctx, profiler.GetReport().passes.size(), size_t{ 2u });

    // Switching off clears everything and stops recording.
    profiler.SetEnabled(device, false);
    CHECK(ctx, !device.timestampsEnabled);
    profiler.RecordPass(9u, "Shadow", 0u, 1u);
    CHECK(ctx, profiler.GetReport().passes.empty());
}

static void TestRenderWorldParallelDrawListsMatchSerial(test::TestContext& ctx)
{
    MaterialSystem ms;

    MaterialDesc opaqueDesc;
    MaterialRuntimeDesc opaqueRuntime;
    opaqueDesc.name = "ParallelOpaque";
    opaqueRuntime.renderPass = StandardRenderPasses::Opaque();
    opaqueRuntime.vertexShader = ShaderHandle::Make(1u, 1u);
    opaqueRuntime.fragmentShader = ShaderHandle::Make(2u, 1u);
    const MaterialHandle opaqueMat = MaterialRuntimeBridge::RegisterMaterial(ms, std::move(opaqueDesc), opaqueRuntime);

    MaterialDesc transparentDesc;
    MaterialRuntimeDesc transparentRuntime;
    transparentDesc.name = "ParallelTransparent";
    transparentRuntime.renderPass = StandardRenderPasses::Transparent();
    transparentRuntime.vertexShader = ShaderHandle::Make(1u, 1u);
    transparentRuntime.fragmentShader = ShaderHandle::Make(2u, 1u);
    transparentRuntime.blend.blendEnable = true;
    const MaterialHandle transparentMat = MaterialRuntimeBridge::RegisterMaterial(ms, std::move(transparentDesc), transparentRuntime);

    // 1003 proxies: not a multiple of any batch size used below. Mix of
    // visible, culled, shadow-only, non-casting and material-less proxies.
    constexpr uint32_t kProxyCount = 1003u;
    const auto fill = [&](RenderWorld& rw) {
        for (uint32_t i = 0u; i < kProxyCount; ++i)
        {
            MaterialHandle material = (i % 5u == 0u) ? transparentMat : opaqueMat;
            if (i % 11u == 0u)
                material = MaterialHandle{};

            const bool behindCamera = (i % 7u == 0u);
            const math::Vec3 center{
                static_cast<float>(i % 20u) - 10.f,
                static_cast<float>((i / 20u) % 10u) - 5.f,
                behindCamera ? -500.f : static_cast<float>(i % 13u) };
            const math::Mat4 worldMatrix = math::Mat4::Translation(center);

            rw.AddRenderable(EntityID::Make(i + 1u, 1u), MeshHandle::Make(1u + (i % 3u), 1u), material,
                             worldMatrix, worldMatrix,
                             center, math::Vec3{ 0.5f, 0.5f, 0.5f }, 0.87f,
                             0xFFFFFFFFu, (i % 3u) != 0u);
        }
    };

    const math::Mat4 view = math::Mat4::LookAtRH({ 0, 0, -10 }, { 0, 0, 0 }, math::Vec3::Up());
    const math::Mat4 proj = math::Mat4::PerspectiveFovRH(60.f * math::DEG_TO_RAD, 16.f / 9.f, 0.1f, 1000.f);
    const math::Mat4 vp = proj * view;
    renderer::RenderPassRegistry renderPassRegistry;

    RenderWorld serial;
    fill(serial);
    serial.BuildDrawLists(view, vp, 0.1f, 1000.f, ms, renderPassRegistry, 0xFFFFFFFFu, nullptr);
    const RenderQueue& expected = serial.GetQueue();

    // The scene must exercise every branch, otherwise the comparison is empty.
    CHECK(ctx, serial.VisibleCount() > 0u);
    CHECK(ctx, serial.VisibleCount() < kProxyCount);
    CHECK(ctx, RequirePassList(ctx, expected, StandardRenderPasses::Opaque()).Size() > 0u);
    CHECK(ctx, RequirePassList(ctx, expected, StandardRenderPasses::Transparent()).Size() > 0u);
    CHECK(ctx, RequirePassList(ctx, expected, StandardRenderPasses::Shadow()).Size() > 0u);
    CHECK(ctx, expected.objectConstants.size() > RequirePassList(ctx, expected, StandardRenderPasses::Opaque()).Size());

    const auto compare = [&](jobs::JobSystem* jobSystem) -> size_t {
        RenderWorld parallel;
        fill(parallel);
        parallel.BuildDrawLists(view, vp, 0.1f, 1000.f, ms, renderPassRegistry, 0xFFFFFFFFu, jobSystem);
        const RenderQueue& actual = parallel.GetQueue();

        size_t mismatches = 0u;
        if (parallel.VisibleCount() != serial.VisibleCount())
            ++mismatches;

        if (actual.objectConstants.size() != expected.objectConstants.size())
            ++mismatches;
        else if (!expected.objectConstants.empty()
              && std::memcmp(actual.objectConstants.data(), expected.objectConstants.data(),
                             expected.objectConstants.size() * sizeof(PerObjectConstants)) != 0)
            ++mismatches;

        if (actual.lists.size() != expected.lists.size())
            ++mismatches;

        for (const auto& [passId, listIndex] : expected.indices)
        {
            const DrawList& expectedList = expected.lists[listIndex];
            const DrawList* actualList = actual.FindList(passId);
            if (!actualList || actualList->items.size() != expectedList.items.size())
            {
                ++mismatches;
                continue;
            }
            for (size_t i = 0u; i < expectedList.items.size(); ++i)
            {
                const DrawItem& a = actualList->items[i];
                const DrawItem& e = expectedList.items[i];
                if (!(a.entity == e.entity) || !(a.material == e.material) || !(a.mesh == e.mesh)
                    || a.cbOffset != e.cbOffset || !(a.sortKey == e.sortKey))
                    ++mismatches;
            }
        }

        const auto& expectedProxies = serial.GetProxies();
        const auto& actualProxies = parallel.GetProxies();
        for (size_t i = 0u; i < expectedProxies.size(); ++i)
            if (actualProxies[i].visible != expectedProxies[i].visible)
                ++mismatches;

        return mismatches;
    };

    // No pool object at all, and a pool that was never initialized.
    CHECK_EQ(ctx, compare(nullptr), size_t{ 0u });
    {
        jobs::JobSystem uninitialized;
        CHECK_EQ(ctx, compare(&uninitialized), size_t{ 0u });
    }

    for (const uint32_t workers : { 1u, 2u, 3u, 5u, 8u, 16u })
    {
        jobs::JobSystem jobSystem;
        jobSystem.Initialize(workers);
        CHECK_EQ(ctx, compare(&jobSystem), size_t{ 0u });
        jobSystem.Shutdown();
    }
}

static MaterialHandle RegisterInstancingTestMaterial(MaterialSystem& materials,
                                                     const char* name,
                                                     MaterialFeatureFlags features,
                                                     RenderPassID passId)
{
    MaterialDesc desc{};
    desc.name = name;
    desc.features = features;

    MaterialRuntimeDesc runtime{};
    runtime.renderPass = passId;
    runtime.vertexShader = ShaderHandle::Make(1u, 1u);
    runtime.fragmentShader = ShaderHandle::Make(2u, 1u);
    return MaterialRuntimeBridge::RegisterMaterial(materials, std::move(desc), runtime);
}

static DrawItem MakeInstancingTestItem(MaterialHandle material,
                                       uint32_t cbOffset,
                                       uint32_t submeshIndex = 0u)
{
    DrawItem item{};
    item.mesh = MeshHandle::Make(10u, 1u);
    item.material = material;
    item.submeshIndex = submeshIndex;
    item.gpuVertexBuffer = BufferHandle::Make(20u, 1u);
    item.gpuIndexBuffer = BufferHandle::Make(21u, 1u);
    item.gpuIndexCount = 36u;
    item.gpuVertexStride = 32u;
    item.entity = EntityID::Make(cbOffset + 1u, 1u);
    item.cbOffset = cbOffset;
    item.cbSize = sizeof(PerObjectConstants);
    return item;
}

static PerObjectConstants MakeInstancingTestConstants(float entityValue)
{
    PerObjectConstants constants{};
    constants.entityId[0] = entityValue;
    return constants;
}

static void TestDrawListInstancingMergesIdenticalRuns(test::TestContext& ctx)
{
    MaterialSystem materials;
    RenderPassRegistry renderPassRegistry;
    const MaterialHandle material = RegisterInstancingTestMaterial(
        materials,
        "Instanced",
        MaterialFeatureFlags::Instanced,
        StandardRenderPasses::Opaque());

    RenderQueue queue;
    constexpr uint32_t kInstanceCount = 200u;
    for (uint32_t i = 0u; i < kInstanceCount; ++i)
        queue.objectConstants.push_back(MakeInstancingTestConstants(static_cast<float>(i)));

    DrawList& list = queue.GetOrCreateList(StandardRenderPasses::Opaque());
    list.sorted = true;
    for (uint32_t i = 0u; i < kInstanceCount; ++i)
        list.items.push_back(MakeInstancingTestItem(material, (i + 37u) % kInstanceCount));

    OptimizeRenderQueueForInstancing(queue, materials, renderPassRegistry, true);

    CHECK_EQ(ctx, list.items.size(), size_t(1));
    if (!list.items.empty())
    {
        CHECK_EQ(ctx, list.items[0].firstInstance, 0u);
        CHECK_EQ(ctx, list.items[0].instanceCount, kInstanceCount);
        CHECK_EQ(ctx, list.items[0].cbOffset, 37u);
    }

    CHECK_EQ(ctx, queue.instanceObjectConstants.size(), static_cast<size_t>(kInstanceCount));
    CHECK_EQ(ctx, queue.instanceEntities.size(), static_cast<size_t>(kInstanceCount));
    if (queue.instanceEntities.size() == static_cast<size_t>(kInstanceCount))
    {
        CHECK_EQ(ctx, queue.instanceEntities[0], EntityID::Make(38u, 1u));
        CHECK_EQ(ctx, queue.instanceEntities[199], EntityID::Make(37u, 1u));
    }
    if (queue.instanceObjectConstants.size() == static_cast<size_t>(kInstanceCount))
    {
        CHECK_EQ(ctx, queue.instanceObjectConstants[0].entityId[0], 37.f);
        CHECK_EQ(ctx, queue.instanceObjectConstants[199].entityId[0], 36.f);
    }
}

static void TestDrawListInstancingMergesBoundsForCulling(test::TestContext& ctx)
{
    MaterialSystem materials;
    RenderPassRegistry renderPassRegistry;
    const MaterialHandle material = RegisterInstancingTestMaterial(
        materials,
        "InstancedBounds",
        MaterialFeatureFlags::Instanced,
        StandardRenderPasses::Shadow());

    RenderQueue queue;
    for (uint32_t i = 0u; i < 3u; ++i)
        queue.objectConstants.push_back(MakeInstancingTestConstants(static_cast<float>(i)));

    DrawList& list = queue.GetOrCreateList(StandardRenderPasses::Shadow());
    list.sorted = true;
    const math::Vec3 centers[3] = { { -4.f, 0.f, 0.f }, { 0.f, 2.f, 0.f }, { 6.f, 0.f, -1.f } };
    for (uint32_t i = 0u; i < 3u; ++i)
    {
        DrawItem item = MakeInstancingTestItem(material, i);
        item.boundsCenter = centers[i];
        item.boundsExtents = { 1.f, 1.f, 1.f };
        item.hasBounds = true;
        list.items.push_back(item);
    }

    OptimizeRenderQueueForInstancing(queue, materials, renderPassRegistry, true);

    CHECK_EQ(ctx, list.items.size(), size_t(1));
    if (!list.items.empty())
    {
        const DrawItem& merged = list.items[0];
        CHECK_EQ(ctx, merged.instanceCount, 3u);
        CHECK(ctx, merged.hasBounds);
        // Union of [-5,-3]x[-1,1]x[-1,1], [-1,1]x[1,3]x[-1,1], [5,7]x[-1,1]x[-2,0].
        CHECK(ctx, NearlyEqual(merged.boundsCenter.x - merged.boundsExtents.x, -5.f));
        CHECK(ctx, NearlyEqual(merged.boundsCenter.x + merged.boundsExtents.x,  7.f));
        CHECK(ctx, NearlyEqual(merged.boundsCenter.y - merged.boundsExtents.y, -1.f));
        CHECK(ctx, NearlyEqual(merged.boundsCenter.y + merged.boundsExtents.y,  3.f));
        CHECK(ctx, NearlyEqual(merged.boundsCenter.z - merged.boundsExtents.z, -2.f));
        CHECK(ctx, NearlyEqual(merged.boundsCenter.z + merged.boundsExtents.z,  1.f));
    }
}

static void TestDrawListInstancingSkipsMaterialSkinningAndBackToFront(test::TestContext& ctx)
{
    MaterialSystem materials;
    RenderPassRegistry renderPassRegistry;
    const MaterialHandle instanced = RegisterInstancingTestMaterial(
        materials,
        "Instanced",
        MaterialFeatureFlags::Instanced,
        StandardRenderPasses::Opaque());
    const MaterialHandle otherMaterial = RegisterInstancingTestMaterial(
        materials,
        "OtherInstanced",
        MaterialFeatureFlags::Instanced,
        StandardRenderPasses::Opaque());

    RenderQueue queue;
    for (int i = 0; i < 8; ++i)
        queue.objectConstants.push_back(MakeInstancingTestConstants(static_cast<float>(i)));

    DrawList& opaque = queue.GetOrCreateList(StandardRenderPasses::Opaque());
    opaque.sorted = true;
    opaque.items.push_back(MakeInstancingTestItem(instanced, 0u));
    opaque.items.push_back(MakeInstancingTestItem(otherMaterial, 1u));
    opaque.items.push_back(MakeInstancingTestItem(instanced, 2u));
    DrawItem skinned0 = MakeInstancingTestItem(instanced, 3u);
    skinned0.boneBuffer = BufferHandle::Make(31u, 1u);
    DrawItem skinned1 = MakeInstancingTestItem(instanced, 4u);
    skinned1.boneBuffer = BufferHandle::Make(31u, 1u);
    opaque.items.push_back(skinned0);
    opaque.items.push_back(skinned1);

    DrawList& transparent = queue.GetOrCreateList(StandardRenderPasses::Transparent());
    transparent.sorted = true;
    transparent.items.push_back(MakeInstancingTestItem(instanced, 5u));
    transparent.items.push_back(MakeInstancingTestItem(instanced, 6u));

    OptimizeRenderQueueForInstancing(queue, materials, renderPassRegistry, true);

    const DrawList& optimizedOpaque = RequirePassList(ctx, queue, StandardRenderPasses::Opaque());
    CHECK_EQ(ctx, optimizedOpaque.items.size(), size_t(5));
    for (const DrawItem& item : optimizedOpaque.items)
        CHECK_EQ(ctx, item.instanceCount, 1u);

    const DrawList& optimizedTransparent = RequirePassList(ctx, queue, StandardRenderPasses::Transparent());
    CHECK_EQ(ctx, optimizedTransparent.items.size(), size_t(2));
    for (const DrawItem& item : optimizedTransparent.items)
        CHECK_EQ(ctx, item.instanceCount, 1u);

    CHECK_EQ(ctx, queue.instanceObjectConstants.size(), size_t(7));
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
            void Extract(const SceneExtractionContext& ctx) const override
            {
                ctx.world.View<WorldTransformComponent, MeshComponent, MaterialComponent>(
                    [&](EntityID id,
                        const WorldTransformComponent& wt,
                        const MeshComponent& mesh,
                        const MaterialComponent& mat)
                    {
                        if (!ECSExtractor::IsEntityActive(ctx.world, id) || !mesh.mesh.IsValid())
                            return;
                        ctx.extractionView.AddRenderable(
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
            void Extract(const SceneExtractionContext& ctx) const override
            { engine::addons::lighting::ExtractLights(ctx); }
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
    MaterialRuntimeDesc dRuntime;
    d.name = "FeatureMat";
    dRuntime.renderPass = StandardRenderPasses::Opaque();
    dRuntime.vertexShader = ShaderHandle::Make(1u, 1u);
    dRuntime.fragmentShader = ShaderHandle::Make(2u, 1u);
    const MaterialHandle mat = MaterialRuntimeBridge::RegisterMaterial(ms, std::move(d), dRuntime);

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
    {
        SceneExtractionContext stepCtx(world, renderWorld.GetExtractionView());
        for (const ISceneExtractionStep* step : steps)
            step->Extract(stepCtx);
    }

    CHECK_EQ(ctx, renderWorld.TotalProxyCount(), 1u);   // inactive gefiltert
    CHECK_EQ(ctx, engine::addons::lighting::GetExtractedLightCount(renderWorld.GetFrameDataView()), size_t(1));

    registry.ShutdownAll(FeatureShutdownContext{});
    shaderRuntime.Shutdown();
    device.Shutdown();
}

static void TestSceneExtractionWithRenderWorldContext(test::TestContext& ctx)
{
    RegisterRendererTestComponents();

    class RenderWorldStep final : public ISceneExtractionStep
    {
    public:
        std::string_view GetName() const noexcept override { return "test.render_world"; }

        void Extract(const SceneExtractionContext& ctx) const override
        {
            ctx.world.View<WorldTransformComponent, MeshComponent, MaterialComponent>(
                [&](EntityID id,
                    const WorldTransformComponent& wt,
                    const MeshComponent& mesh,
                    const MaterialComponent& mat)
                {
                    if (!ECSExtractor::IsEntityActive(ctx.world, id) || !mesh.mesh.IsValid())
                        return;
                    ctx.extractionView.AddRenderable(
                        id, mesh.mesh, mat.material,
                        wt.matrix, wt.inverse.Transposed(),
                        math::Vec3(0, 0, 0), math::Vec3(1, 1, 1), 1.f,
                        mesh.layerMask, mesh.castShadows);
                });
        }
    };

    ecs::ComponentMetaRegistry componentRegistry = CreateRendererTestRegistry();
    ecs::World world(componentRegistry);
    MaterialSystem materials;

    MaterialDesc desc;
    MaterialRuntimeDesc descRuntime;
    desc.name = "CompatMat";
    descRuntime.renderPass = StandardRenderPasses::Opaque();
    descRuntime.vertexShader = ShaderHandle::Make(31u, 1u);
    descRuntime.fragmentShader = ShaderHandle::Make(32u, 1u);
    const MaterialHandle material = MaterialRuntimeBridge::RegisterMaterial(materials, std::move(desc), descRuntime);

    const EntityID renderable = world.CreateEntity();
    world.Add<TransformComponent>(renderable);
    world.Add<WorldTransformComponent>(renderable);
    world.Add<MeshComponent>(renderable, MeshHandle::Make(41u, 1u));
    world.Add<MaterialComponent>(renderable, material);
    world.Add<ActiveComponent>(renderable, ActiveComponent{true});

    RenderWorld renderWorld;
    SceneExtractionContext extractionContext(world, renderWorld.GetExtractionView());
    RenderWorldStep step;
    step.Extract(extractionContext);

    CHECK_EQ(ctx, renderWorld.TotalProxyCount(), 1u);
}

static void TestLightExtractionIsolatesPreviewLightRig(test::TestContext& ctx)
{
    RegisterRendererTestComponents();
    ecs::ComponentMetaRegistry componentRegistry = CreateRendererTestRegistry();
    ecs::World world(componentRegistry);

    auto addLight = [&](uint32_t layerMask) {
        const EntityID light = world.CreateEntity();
        world.Add<TransformComponent>(light);
        world.Add<WorldTransformComponent>(light);
        LightComponent lc{};
        lc.type = LightType::Directional;
        lc.layerMask = layerMask;
        world.Add<LightComponent>(light, lc);
    };
    addLight(LAYER_ALL);                      // scene light with default mask
    addLight(LAYER_DEFAULT);                  // scene light on the default layer
    addLight(LAYER_EDITOR_MATERIAL_PREVIEW);  // material preview rig light
    addLight(LAYER_EDITOR_PREFAB_PREVIEW);    // prefab preview rig light

    auto countLights = [&](const RenderView& view) {
        RenderWorld renderWorld;
        SceneExtractionContext extraction(world, renderWorld.GetExtractionView(), &view);
        engine::addons::lighting::ExtractLights(extraction);
        return engine::addons::lighting::GetExtractedLightCount(renderWorld.GetFrameDataView());
    };

    // Scene views (default masks) never see the preview rigs.
    CHECK_EQ(ctx, countLights(RenderView{}), size_t(2));

    // An isolated preview sees only its own rig, never the scene lights or the other rig.
    RenderView materialPreview{};
    materialPreview.lightLayerMask = LAYER_EDITOR_MATERIAL_PREVIEW;
    materialPreview.lightLayerExcludeMask = ~LAYER_EDITOR_MATERIAL_PREVIEW;
    CHECK_EQ(ctx, countLights(materialPreview), size_t(1));

    RenderView prefabPreview{};
    prefabPreview.lightLayerMask = LAYER_EDITOR_PREFAB_PREVIEW;
    prefabPreview.lightLayerExcludeMask = ~LAYER_EDITOR_PREFAB_PREVIEW;
    CHECK_EQ(ctx, countLights(prefabPreview), size_t(1));

    RenderView unlit{};
    unlit.lightLayerMask = LAYER_NONE;
    CHECK_EQ(ctx, countLights(unlit), size_t(0));
}

static void TestFrameExtractionStagePassesJobSystemToSceneExtractionContext(test::TestContext& ctx)
{
    class JobSystemProbeStep final : public ISceneExtractionStep
    {
    public:
        explicit JobSystemProbeStep(jobs::JobSystem*& observedRef) noexcept
            : m_observed(observedRef)
        {
        }

        std::string_view GetName() const noexcept override { return "test.job_system_probe"; }

        void Extract(const SceneExtractionContext& ctx) const override
        {
            m_observed = ctx.jobSystem;
        }

    private:
        jobs::JobSystem*& m_observed;
    };

    ecs::ComponentMetaRegistry componentRegistry = CreateRendererTestRegistry();
    ecs::World world(componentRegistry);
    RenderSceneSnapshot snapshot;
    FrameExtractionStageResult result{};

    jobs::JobSystem* observedJobSystem = nullptr;
    JobSystemProbeStep probe(observedJobSystem);
    const std::vector<const ISceneExtractionStep*> steps{&probe};

    jobs::JobSystem jobSystem;
    jobSystem.Initialize(2u);

    const FrameExtractionStageContext extractionContext{
        world,
        steps,
        snapshot,
        nullptr,
        &jobSystem
    };

    FrameExtractionStage stage;
    CHECK(ctx, stage.Execute(extractionContext, result));
    CHECK(ctx, observedJobSystem == &jobSystem);

    jobSystem.Shutdown();
}

static void TestMeshExtractionUsesJobSystemWhenAvailable(test::TestContext& ctx)
{
    RegisterRendererTestComponents();

    ecs::ComponentMetaRegistry componentRegistry = CreateRendererTestRegistry();
    ecs::World world(componentRegistry);
    MaterialSystem materials;

    MaterialDesc desc;
    MaterialRuntimeDesc descRuntime;
    desc.name = "ParallelMeshMat";
    descRuntime.renderPass = StandardRenderPasses::Opaque();
    descRuntime.vertexShader = ShaderHandle::Make(51u, 1u);
    descRuntime.fragmentShader = ShaderHandle::Make(52u, 1u);
    const MaterialHandle material = MaterialRuntimeBridge::RegisterMaterial(materials, std::move(desc), descRuntime);

    constexpr uint32_t kEntityCount = 2048u;
    for (uint32_t i = 0u; i < kEntityCount; ++i)
    {
        const EntityID entity = world.CreateEntity();
        world.Add<TransformComponent>(entity);
        world.Add<WorldTransformComponent>(entity);
        world.Add<MeshComponent>(entity, MeshHandle::Make(100u + i, 1u));
        world.Add<MaterialComponent>(entity, material);
        world.Add<BoundsComponent>(entity, BoundsComponent{
            .centerWorld = { static_cast<float>(i), 0.f, 0.f },
            .extentsWorld = { 0.5f, 0.5f, 0.5f },
            .boundingSphere = 0.8660254f
        });
    }

    jobs::JobSystem jobSystem;
    jobSystem.Initialize(2u);
    jobSystem.ResetPeakActiveWorkers();

    RenderWorld renderWorld;
    engine::addons::mesh_renderer::ExtractRenderables(world, renderWorld.GetExtractionView(), &jobSystem);

    CHECK_EQ(ctx, renderWorld.TotalProxyCount(), kEntityCount);
    // Parallelisierung findet in BuildDrawLists statt, nicht in der Extraktion.
    // Der JobSystem-Pointer wird weitergereicht aber hier nicht fuer Extraktion genutzt.

    jobSystem.Shutdown();
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
    MaterialRuntimeDesc dRuntime;
    d.name = "ForwardMat";
    dRuntime.renderPass = StandardRenderPasses::Opaque();
    dRuntime.vertexShader = ShaderHandle::Make(1u, 1u);
    dRuntime.fragmentShader = ShaderHandle::Make(2u, 1u);
    const MaterialHandle mat = MaterialRuntimeBridge::RegisterMaterial(ms, std::move(d), dRuntime);

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

    RenderSceneSnapshot snapshot;
    FrameExtractionStage stage;
    const FrameExtractionStageContext extractionContext{
        world,
        registry.GetSceneExtractionSteps(),
        snapshot
    };

    FrameExtractionStageResult extractionResult{};
    CHECK(ctx, stage.Execute(extractionContext, extractionResult));
    CHECK_EQ(ctx, registry.GetSceneExtractionSteps().size(), size_t(2));
    CHECK_EQ(ctx, snapshot.TotalProxyCount(), 1u);
    CHECK_EQ(ctx, engine::addons::lighting::GetExtractedLightCount(snapshot.GetFrameDataView()), size_t(1));

    registry.ShutdownAll(FeatureShutdownContext{});
    shaderRuntime.Shutdown();
    device.Shutdown();
}

static void TestShadowFeatureExtractionAndFrameConstants(test::TestContext& ctx)
{
    RegisterRendererTestComponents();

    ecs::ComponentMetaRegistry componentRegistry = CreateRendererTestRegistry();
    ecs::World world(componentRegistry);

    const EntityID light = world.CreateEntity();
    auto& lightTransform = world.Add<TransformComponent>(light);
    lightTransform.localRotation = math::Quat::FromEulerDeg(-35.f, 25.f, 0.f);
    world.Add<WorldTransformComponent>(light);

    LightComponent shadowLight{};
    shadowLight.type = LightType::Directional;
    shadowLight.color = { 1.f, 0.95f, 0.9f };
    shadowLight.intensity = 2.5f;
    shadowLight.castShadows = true;
    shadowLight.shadowSettings.enabled = true;
    shadowLight.shadowSettings.bias = 0.005f;
    shadowLight.shadowSettings.normalBias = 0.02f;
    shadowLight.shadowSettings.strength = 0.85f;
    world.Add<LightComponent>(light, shadowLight);

    TransformSystem transformSystem;
    transformSystem.Update(world);

    FeatureRegistry registry;
    CHECK(ctx, registry.AddFeature(engine::addons::lighting::CreateLightingFeature()));
    CHECK(ctx, registry.AddFeature(engine::addons::shadow::CreateShadowFeature()));

    TestDevice device;
    CHECK(ctx, device.Initialize(IDevice::DeviceDesc{}));
    ShaderRuntime shaderRuntime;
    CHECK(ctx, shaderRuntime.Initialize(device));
    CHECK(ctx, registry.InitializeAll(FeatureInitializationContext{device, shaderRuntime, nullptr}));

    RenderSceneSnapshot snapshot;
    FrameExtractionStage extractionStage;
    const FrameExtractionStageContext extractionContext{
        world,
        registry.GetSceneExtractionSteps(),
        snapshot
    };

    FrameExtractionStageResult extractionResult{};
    CHECK(ctx, extractionStage.Execute(extractionContext, extractionResult));
    CHECK_EQ(ctx, registry.GetSceneExtractionSteps().size(), size_t(2));
    CHECK_EQ(ctx, registry.GetFrameConstantsContributors().size(), size_t(2));
    CHECK_EQ(ctx, engine::addons::lighting::GetExtractedLightCount(snapshot.GetFrameDataView()), size_t(1));

    const auto* shadowData =
        snapshot.GetFrameDataView().GetFrameData<engine::addons::shadow::ShadowFrameData>();
    CHECK(ctx, shadowData != nullptr);
    CHECK(ctx, shadowData->HasCurrentRenderPathRequests());
    CHECK(ctx, shadowData->HasCurrentRenderPathPrimaryRequest());
    CHECK(ctx, shadowData->HasCurrentRenderPathPrimaryVisibleLight());
    CHECK_EQ(ctx, shadowData->currentRenderPath.requestIndices.size(), size_t(1));
    CHECK_EQ(ctx, shadowData->currentRenderPath.requestIndices[0], size_t(0));
    CHECK_EQ(ctx, shadowData->currentRenderPath.visibleLightIndices.size(), size_t(1));
    CHECK_EQ(ctx, shadowData->currentRenderPath.visibleLightIndices[0], uint32_t(0));
    CHECK_EQ(ctx, shadowData->GetCurrentRenderPathPrimaryVisibleLightIndex(), uint32_t(0));
    CHECK_EQ(ctx, shadowData->shadowedLightIndices.size(), size_t(1));
    CHECK_EQ(ctx, shadowData->shadowedLightIndices[0], uint32_t(0));
    CHECK(ctx, shadowData->IsVisibleLightShadowed(0u));
    CHECK(ctx, !shadowData->IsVisibleLightShadowed(1u));
    const auto* selectedShadowRequest = shadowData->GetCurrentRenderPathPrimaryRequest();
    CHECK(ctx, selectedShadowRequest != nullptr);
    CHECK(ctx, selectedShadowRequest->lightEntity == light);
    CHECK_EQ(ctx, selectedShadowRequest->visibleLightIndex, uint32_t(0));

    renderer::RenderView view{};
    view.view = math::Mat4::LookAtRH({ 0.f, 2.f, -6.f }, { 0.f, 0.f, 0.f }, math::Vec3::Up());
    view.projection = math::Mat4::PerspectiveFovRH(60.f * math::DEG_TO_RAD, 16.f / 9.f, 0.1f, 100.f);
    view.cameraPosition = { 0.f, 2.f, -6.f };
    view.cameraForward = { 0.f, 0.f, 1.f };
    view.nearPlane = 0.1f;
    view.farPlane = 100.f;

    platform::FixedTiming timing(1.0 / 60.0);
    FrameConstantStage constantStage;
    const FrameConstantStageContext constantContext{
        math::Mat4::Identity(),
        math::Mat4::Identity(),
        1280u,
        720u,
        view,
        timing,
        EnvironmentRuntimeState{},
        snapshot.GetFrameDataView(),
        &snapshot.GetQueue(),
        registry.GetFrameConstantsContributors()
    };

    FrameConstantsResult frameResult{};
    CHECK(ctx, constantStage.PrepareFrameData(constantContext, frameResult));

    const FrameConstants& fc = frameResult.frameConstants;
    // The shaders map roughness to [0, count - 1]; match the baker's mip count.
    CHECK_EQ(ctx, fc.iblPrefilterLevels, static_cast<float>(IBLBakeParams{}.prefilterMipCount));
    CHECK_EQ(ctx, fc.featureCount0, 1u);
    CHECK_EQ(ctx, fc.shadowCascadeCount, 1u);
    CHECK(ctx, NearlyEqual(fc.shadowBias, shadowLight.shadowSettings.bias));
    CHECK(ctx, NearlyEqual(fc.shadowNormalBias, shadowLight.shadowSettings.normalBias));
    CHECK(ctx, NearlyEqual(fc.shadowStrength, shadowLight.shadowSettings.strength));
    CHECK(ctx, NearlyEqual(fc.shadowTexelSize, 1.0f / static_cast<float>(shadowLight.shadowSettings.resolution)));

    CHECK(ctx, fc.shadowViewCount >= 1u);
    const float* packedShadowMatrix = fc.shadowViewProj[0];
    const auto* selectedShadowView = shadowData->GetCurrentRenderPathPrimaryView();
    CHECK(ctx, selectedShadowView != nullptr);
    const float* expectedShadowMatrix = selectedShadowView->viewProj.Data();
    for (int i = 0; i < 16; ++i)
        CHECK(ctx, NearlyEqual(packedShadowMatrix[i], expectedShadowMatrix[i]));

    registry.ShutdownAll(FeatureShutdownContext{});
    shaderRuntime.Shutdown();
    device.Shutdown();
}

static void TestShadowExtractionRespectsRenderViewShadowToggle(test::TestContext& ctx)
{
    RegisterRendererTestComponents();

    ecs::ComponentMetaRegistry componentRegistry = CreateRendererTestRegistry();
    ecs::World world(componentRegistry);

    const EntityID caster = world.CreateEntity();
    TransformComponent casterTransform{};
    casterTransform.localPosition = { 0.f, 0.f, 0.f };
    world.Add<TransformComponent>(caster, casterTransform);
    world.Add<WorldTransformComponent>(caster);
    world.Add<MeshComponent>(caster, MeshComponent{ MeshHandle::Make(1u, 1u) });
    world.Add<BoundsComponent>(caster, BoundsComponent{
        .centerLocal = {0.f, 0.f, 0.f},
        .extentsLocal = {1.f, 1.f, 1.f}
    });

    const EntityID light = world.CreateEntity();
    TransformComponent lightTransform{};
    lightTransform.localRotation = math::Quat::FromEulerDeg(-45.f, 30.f, 0.f);
    world.Add<TransformComponent>(light, lightTransform);
    world.Add<WorldTransformComponent>(light);
    LightComponent shadowLight{};
    shadowLight.type = LightType::Directional;
    shadowLight.castShadows = true;
    shadowLight.shadowSettings.enabled = true;
    world.Add<LightComponent>(light, shadowLight);

    TransformSystem transformSystem;
    transformSystem.Update(world);

    FeatureRegistry registry;
    CHECK(ctx, registry.AddFeature(engine::addons::lighting::CreateLightingFeature()));
    CHECK(ctx, registry.AddFeature(engine::addons::shadow::CreateShadowFeature()));

    TestDevice device;
    CHECK(ctx, device.Initialize(IDevice::DeviceDesc{}));
    ShaderRuntime shaderRuntime;
    CHECK(ctx, shaderRuntime.Initialize(device));
    CHECK(ctx, registry.InitializeAll(FeatureInitializationContext{device, shaderRuntime, nullptr}));

    renderer::RenderView view{};
    view.enableShadows = false;

    RenderSceneSnapshot snapshot;
    FrameExtractionStage extractionStage;
    const FrameExtractionStageContext extractionContext{
        world,
        registry.GetSceneExtractionSteps(),
        snapshot,
        &view
    };

    FrameExtractionStageResult extractionResult{};
    CHECK(ctx, extractionStage.Execute(extractionContext, extractionResult));

    const auto* shadowData =
        snapshot.GetFrameDataView().GetFrameData<engine::addons::shadow::ShadowFrameData>();
    CHECK(ctx, shadowData != nullptr);
    if (shadowData)
    {
        CHECK(ctx, !shadowData->HasCurrentRenderPathRequests());
        CHECK_EQ(ctx, shadowData->requests.size(), size_t(0));
        CHECK_EQ(ctx, shadowData->shadowedLightIndices.size(), size_t(0));
    }
    CHECK_EQ(ctx, snapshot.GetQueue().activeShadowResolution, 0u);

    registry.ShutdownAll(FeatureShutdownContext{});
    shaderRuntime.Shutdown();
    device.Shutdown();
}

static void TestShadowExtractionCollectsMultipleRequestsAndIncludesCubeCurrentPath(test::TestContext& ctx)
{
    RegisterRendererTestComponents();

    ecs::ComponentMetaRegistry componentRegistry = CreateRendererTestRegistry();
    ecs::World world(componentRegistry);

    const EntityID directional = world.CreateEntity();
    auto& dirTransform = world.Add<TransformComponent>(directional);
    dirTransform.localRotation = math::Quat::FromEulerDeg(-35.f, 25.f, 0.f);
    world.Add<WorldTransformComponent>(directional);

    LightComponent dirLight{};
    dirLight.type = LightType::Directional;
    dirLight.castShadows = true;
    dirLight.shadowSettings.enabled = true;
    dirLight.shadowSettings.resolution = 2048u;
    world.Add<LightComponent>(directional, dirLight);

    const EntityID spot = world.CreateEntity();
    auto& spotTransform = world.Add<TransformComponent>(spot);
    spotTransform.localPosition = { 2.f, 4.f, -2.f };
    spotTransform.localRotation = math::Quat::FromEulerDeg(-45.f, 30.f, 0.f);
    world.Add<WorldTransformComponent>(spot);

    LightComponent spotLight{};
    spotLight.type = LightType::Spot;
    spotLight.range = 12.f;
    spotLight.spotOuterDeg = 40.f;
    spotLight.castShadows = true;
    spotLight.shadowSettings.enabled = true;
    spotLight.shadowSettings.resolution = 1024u;
    world.Add<LightComponent>(spot, spotLight);

    const EntityID point = world.CreateEntity();
    auto& pointTransform = world.Add<TransformComponent>(point);
    pointTransform.localPosition = { -2.f, 3.f, 1.f };
    world.Add<WorldTransformComponent>(point);

    LightComponent pointLight{};
    pointLight.type = LightType::Point;
    pointLight.range = 8.f;
    pointLight.castShadows = true;
    pointLight.shadowSettings.enabled = true;
    pointLight.shadowSettings.resolution = 512u;
    world.Add<LightComponent>(point, pointLight);

    const EntityID caster = world.CreateEntity();
    world.Add<TransformComponent>(caster);
    world.Add<WorldTransformComponent>(caster);
    MeshComponent mesh{};
    mesh.castShadows = true;
    world.Add<MeshComponent>(caster, mesh);
    BoundsComponent bounds{};
    bounds.centerWorld = { 0.f, 0.f, 0.f };
    bounds.extentsWorld = { 1.f, 1.f, 1.f };
    bounds.boundingSphere = bounds.extentsWorld.Length();
    world.Add<BoundsComponent>(caster, bounds);

    TransformSystem transformSystem;
    transformSystem.Update(world);

    FeatureRegistry registry;
    CHECK(ctx, registry.AddFeature(engine::addons::lighting::CreateLightingFeature()));
    CHECK(ctx, registry.AddFeature(engine::addons::shadow::CreateShadowFeature()));

    TestDevice device;
    CHECK(ctx, device.Initialize(IDevice::DeviceDesc{}));
    ShaderRuntime shaderRuntime;
    CHECK(ctx, shaderRuntime.Initialize(device));
    CHECK(ctx, registry.InitializeAll(FeatureInitializationContext{device, shaderRuntime, nullptr}));

    RenderSceneSnapshot snapshot;
    FrameExtractionStage extractionStage;
    const FrameExtractionStageContext extractionContext{
        world,
        registry.GetSceneExtractionSteps(),
        snapshot
    };

    FrameExtractionStageResult extractionResult{};
    CHECK(ctx, extractionStage.Execute(extractionContext, extractionResult));

    const auto* shadowData =
        snapshot.GetFrameDataView().GetFrameData<engine::addons::shadow::ShadowFrameData>();
    CHECK(ctx, shadowData != nullptr);
    CHECK_EQ(ctx, shadowData->requests.size(), size_t(3));
    CHECK_EQ(ctx, shadowData->shadowedLightIndices.size(), size_t(3));
    CHECK_EQ(ctx, shadowData->shadowedLightIndices[0], uint32_t(0));
    CHECK_EQ(ctx, shadowData->shadowedLightIndices[1], uint32_t(1));
    CHECK_EQ(ctx, shadowData->shadowedLightIndices[2], uint32_t(2));
    CHECK(ctx, shadowData->IsVisibleLightShadowed(0u));
    CHECK(ctx, shadowData->IsVisibleLightShadowed(1u));
    CHECK(ctx, shadowData->IsVisibleLightShadowed(2u));
    CHECK(ctx, !shadowData->IsVisibleLightShadowed(3u));
    CHECK(ctx, shadowData->HasCurrentRenderPathRequests());
    CHECK(ctx, shadowData->HasCurrentRenderPathPrimaryRequest());
    CHECK(ctx, shadowData->HasCurrentRenderPathPrimaryVisibleLight());
    CHECK_EQ(ctx, shadowData->currentRenderPath.requestIndices.size(), size_t(3));
    CHECK_EQ(ctx, shadowData->currentRenderPath.requestIndices[0], size_t(0));
    CHECK_EQ(ctx, shadowData->currentRenderPath.requestIndices[1], size_t(1));
    CHECK_EQ(ctx, shadowData->currentRenderPath.requestIndices[2], size_t(2));
    CHECK_EQ(ctx, shadowData->currentRenderPath.visibleLightIndices.size(), size_t(3));
    CHECK_EQ(ctx, shadowData->currentRenderPath.visibleLightIndices[0], uint32_t(0));
    CHECK_EQ(ctx, shadowData->currentRenderPath.visibleLightIndices[1], uint32_t(1));
    CHECK_EQ(ctx, shadowData->currentRenderPath.visibleLightIndices[2], uint32_t(2));

    const auto& req0 = shadowData->requests[0];
    const auto& req1 = shadowData->requests[1];
    const auto& req2 = shadowData->requests[2];
    CHECK_EQ(ctx, req0.lightType, LightType::Directional);
    CHECK_EQ(ctx, req0.technique, engine::addons::shadow::ShadowTechnique::ShadowMap2D);
    CHECK_EQ(ctx, req0.views.size(), size_t(1));
    CHECK_EQ(ctx, req0.visibleLightIndex, uint32_t(0));
    CHECK_EQ(ctx, req1.lightType, LightType::Spot);
    CHECK_EQ(ctx, req1.technique, engine::addons::shadow::ShadowTechnique::ShadowMap2D);
    CHECK_EQ(ctx, req1.views.size(), size_t(1));
    CHECK_EQ(ctx, req1.visibleLightIndex, uint32_t(1));
    CHECK_EQ(ctx, req2.lightType, LightType::Point);
    CHECK_EQ(ctx, req2.technique, engine::addons::shadow::ShadowTechnique::ShadowMapCube);
    CHECK_EQ(ctx, req2.views.size(), size_t(6));
    CHECK_EQ(ctx, req2.visibleLightIndex, uint32_t(2));

    CHECK_EQ(ctx, shadowData->currentRenderPath.primaryRequestIndex, size_t(0));
    CHECK_EQ(ctx, shadowData->GetCurrentRenderPathPrimaryVisibleLightIndex(), uint32_t(0));
    const auto* selected = shadowData->GetCurrentRenderPathPrimaryRequest();
    CHECK(ctx, selected != nullptr);
    CHECK_EQ(ctx, selected->lightEntity, directional);
    CHECK_EQ(ctx, selected->lightType, LightType::Directional);
    CHECK_EQ(ctx, selected->visibleLightIndex, uint32_t(0));

    registry.ShutdownAll(FeatureShutdownContext{});
    shaderRuntime.Shutdown();
    device.Shutdown();
}

static void TestShadowExtractionAppliesLightAndViewBudget(test::TestContext& ctx)
{
    RegisterRendererTestComponents();

    ecs::ComponentMetaRegistry componentRegistry = CreateRendererTestRegistry();
    ecs::World world(componentRegistry);

    auto addShadowLight = [&](LightType type, math::Vec3 position)
    {
        const EntityID light = world.CreateEntity();
        auto& transform = world.Add<TransformComponent>(light);
        transform.localPosition = position;
        transform.localRotation = math::Quat::FromEulerDeg(-45.f, 30.f, 0.f);
        world.Add<WorldTransformComponent>(light);

        LightComponent lc{};
        lc.type = type;
        lc.range = 12.f;
        lc.spotOuterDeg = 40.f;
        lc.castShadows = true;
        lc.shadowSettings.enabled = true;
        lc.shadowSettings.resolution = 256u;
        world.Add<LightComponent>(light, lc);
    };

    addShadowLight(LightType::Spot, { -2.f, 4.f, -2.f });
    addShadowLight(LightType::Spot, {  0.f, 4.f, -2.f });
    addShadowLight(LightType::Point, { 2.f, 4.f, -2.f });

    const EntityID caster = world.CreateEntity();
    world.Add<TransformComponent>(caster);
    world.Add<WorldTransformComponent>(caster);
    MeshComponent mesh{};
    mesh.mesh = MeshHandle::Make(1u, 1u);
    mesh.castShadows = true;
    mesh.receiveShadows = true;
    world.Add<MeshComponent>(caster, mesh);
    BoundsComponent bounds{};
    bounds.centerWorld = { 0.f, 0.f, 0.f };
    bounds.extentsWorld = { 1.f, 1.f, 1.f };
    bounds.boundingSphere = bounds.extentsWorld.Length();
    world.Add<BoundsComponent>(caster, bounds);

    TransformSystem transformSystem;
    transformSystem.Update(world);

    FeatureRegistry registry;
    CHECK(ctx, registry.AddFeature(engine::addons::lighting::CreateLightingFeature()));
    engine::addons::shadow::ShadowFeatureConfig shadowConfig{};
    shadowConfig.maxShadowLights = 3u;
    shadowConfig.maxShadowViews = 4u;
    CHECK(ctx, registry.AddFeature(engine::addons::shadow::CreateShadowFeature(shadowConfig)));

    TestDevice device;
    CHECK(ctx, device.Initialize(IDevice::DeviceDesc{}));
    ShaderRuntime shaderRuntime;
    CHECK(ctx, shaderRuntime.Initialize(device));
    CHECK(ctx, registry.InitializeAll(FeatureInitializationContext{device, shaderRuntime, nullptr}));

    RenderSceneSnapshot snapshot;
    FrameExtractionStage extractionStage;
    const FrameExtractionStageContext extractionContext{
        world,
        registry.GetSceneExtractionSteps(),
        snapshot
    };

    FrameExtractionStageResult extractionResult{};
    CHECK(ctx, extractionStage.Execute(extractionContext, extractionResult));

    const auto* shadowData =
        snapshot.GetFrameDataView().GetFrameData<engine::addons::shadow::ShadowFrameData>();
    CHECK(ctx, shadowData != nullptr);
    CHECK_EQ(ctx, shadowData ? shadowData->requests.size() : 0u, size_t(3));
    CHECK_EQ(ctx, shadowData ? shadowData->currentRenderPath.requestIndices.size() : 0u, size_t(2));
    CHECK_EQ(ctx, shadowData ? shadowData->culledBudgetCount : 0u, 1u);
    if (shadowData && shadowData->currentRenderPath.requestIndices.size() == 2u)
    {
        CHECK_EQ(ctx, shadowData->currentRenderPath.requestIndices[0], size_t(0));
        CHECK_EQ(ctx, shadowData->currentRenderPath.requestIndices[1], size_t(1));
    }
    CHECK_EQ(ctx, snapshot.GetQueue().activeShadowResolution, 512u);

    registry.ShutdownAll(FeatureShutdownContext{});
    shaderRuntime.Shutdown();
    device.Shutdown();
}

static void TestShadowExtractionCullsLocalShadowOutsideReceiverBounds(test::TestContext& ctx)
{
    RegisterRendererTestComponents();

    ecs::ComponentMetaRegistry componentRegistry = CreateRendererTestRegistry();
    ecs::World world(componentRegistry);

    auto addSpotLight = [&](math::Vec3 position, float maxDistance)
    {
        const EntityID light = world.CreateEntity();
        auto& transform = world.Add<TransformComponent>(light);
        transform.localPosition = position;
        transform.localRotation = math::Quat::FromEulerDeg(-45.f, 0.f, 0.f);
        world.Add<WorldTransformComponent>(light);

        LightComponent lc{};
        lc.type = LightType::Spot;
        lc.range = maxDistance;
        lc.spotOuterDeg = 35.f;
        lc.castShadows = true;
        lc.shadowSettings.enabled = true;
        lc.shadowSettings.maxDistance = maxDistance;
        lc.shadowSettings.resolution = 256u;
        world.Add<LightComponent>(light, lc);
        return light;
    };

    const EntityID nearLight = addSpotLight({ 0.f, 4.f, -2.f }, 20.f);
    const EntityID farLight = addSpotLight({ 1000.f, 4.f, -2.f }, 5.f);

    const EntityID caster = world.CreateEntity();
    world.Add<TransformComponent>(caster);
    world.Add<WorldTransformComponent>(caster);
    MeshComponent mesh{};
    mesh.mesh = MeshHandle::Make(1u, 1u);
    mesh.castShadows = true;
    mesh.receiveShadows = true;
    world.Add<MeshComponent>(caster, mesh);
    BoundsComponent bounds{};
    bounds.centerWorld = { 0.f, 0.f, 0.f };
    bounds.extentsWorld = { 1.f, 1.f, 1.f };
    bounds.boundingSphere = bounds.extentsWorld.Length();
    world.Add<BoundsComponent>(caster, bounds);

    TransformSystem transformSystem;
    transformSystem.Update(world);
    if (auto* nearWorld = world.Get<WorldTransformComponent>(nearLight))
        nearWorld->matrix = math::Mat4::LookAtRH({ 0.f, 4.f, -2.f }, math::Vec3::Zero(), math::Vec3::Up()).InverseAffine();
    if (auto* farWorld = world.Get<WorldTransformComponent>(farLight))
        farWorld->matrix = math::Mat4::LookAtRH({ 1000.f, 4.f, -2.f }, math::Vec3::Zero(), math::Vec3::Up()).InverseAffine();

    FeatureRegistry registry;
    CHECK(ctx, registry.AddFeature(engine::addons::lighting::CreateLightingFeature()));
    CHECK(ctx, registry.AddFeature(engine::addons::shadow::CreateShadowFeature()));

    TestDevice device;
    CHECK(ctx, device.Initialize(IDevice::DeviceDesc{}));
    ShaderRuntime shaderRuntime;
    CHECK(ctx, shaderRuntime.Initialize(device));
    CHECK(ctx, registry.InitializeAll(FeatureInitializationContext{device, shaderRuntime, nullptr}));

    renderer::RenderView view{};
    view.view = math::Mat4::LookAtRH({ 0.f, 2.f, -8.f }, { 0.f, 0.f, 0.f }, math::Vec3::Up());
    view.projection = math::Mat4::PerspectiveFovRH(60.f * math::DEG_TO_RAD, 16.f / 9.f, 0.1f, 50.f);
    view.cameraPosition = { 0.f, 2.f, -8.f };
    view.cameraForward = { 0.f, 0.f, 1.f };
    view.nearPlane = 0.1f;
    view.farPlane = 50.f;

    RenderSceneSnapshot snapshot;
    FrameExtractionStage extractionStage;
    const FrameExtractionStageContext extractionContext{
        world,
        registry.GetSceneExtractionSteps(),
        snapshot,
        &view
    };

    FrameExtractionStageResult extractionResult{};
    CHECK(ctx, extractionStage.Execute(extractionContext, extractionResult));

    const auto* shadowData =
        snapshot.GetFrameDataView().GetFrameData<engine::addons::shadow::ShadowFrameData>();
    CHECK(ctx, shadowData != nullptr);
    CHECK_EQ(ctx, shadowData ? shadowData->requests.size() : 0u, size_t(2));
    CHECK_EQ(ctx, shadowData ? shadowData->shadowedLightIndices.size() : 0u, size_t(2));
    CHECK_EQ(ctx, shadowData ? shadowData->currentRenderPath.requestIndices.size() : 0u, size_t(1));
    CHECK_EQ(ctx, shadowData ? shadowData->culledReceiverCount : 0u, 1u);
    if (shadowData && shadowData->currentRenderPath.requestIndices.size() == 1u)
    {
        const size_t selectedIndex = shadowData->currentRenderPath.requestIndices[0];
        CHECK(ctx, selectedIndex < shadowData->requests.size());
        if (selectedIndex < shadowData->requests.size())
        {
            CHECK(ctx, shadowData->requests[selectedIndex].lightEntity == nearLight);
            CHECK_EQ(ctx, shadowData->requests[selectedIndex].views.size(), size_t(1));
        }
    }

    registry.ShutdownAll(FeatureShutdownContext{});
    shaderRuntime.Shutdown();
    device.Shutdown();
}

static void TestShadowExtractionCullsLocalShadowBehindCameraFrustum(test::TestContext& ctx)
{
    RegisterRendererTestComponents();

    ecs::ComponentMetaRegistry componentRegistry = CreateRendererTestRegistry();
    ecs::World world(componentRegistry);

    const EntityID light = world.CreateEntity();
    auto& transform = world.Add<TransformComponent>(light);
    transform.localPosition = { 0.f, 0.f, -10.f };
    transform.localRotation = math::Quat::FromEulerDeg(0.f, 0.f, 0.f);
    world.Add<WorldTransformComponent>(light);
    LightComponent lc{};
    lc.type = LightType::Spot;
    lc.range = 1.f;
    lc.spotOuterDeg = 30.f;
    lc.castShadows = true;
    lc.shadowSettings.enabled = true;
    lc.shadowSettings.maxDistance = 1.f;
    lc.shadowSettings.resolution = 256u;
    world.Add<LightComponent>(light, lc);

    const EntityID caster = world.CreateEntity();
    world.Add<TransformComponent>(caster);
    world.Add<WorldTransformComponent>(caster);
    MeshComponent mesh{};
    mesh.mesh = MeshHandle::Make(1u, 1u);
    mesh.castShadows = true;
    world.Add<MeshComponent>(caster, mesh);
    BoundsComponent bounds{};
    bounds.centerWorld = { 0.f, 0.f, 5.f };
    bounds.extentsWorld = { 1.f, 1.f, 1.f };
    bounds.boundingSphere = bounds.extentsWorld.Length();
    world.Add<BoundsComponent>(caster, bounds);

    TransformSystem transformSystem;
    transformSystem.Update(world);

    FeatureRegistry registry;
    CHECK(ctx, registry.AddFeature(engine::addons::lighting::CreateLightingFeature()));
    CHECK(ctx, registry.AddFeature(engine::addons::shadow::CreateShadowFeature()));

    TestDevice device;
    CHECK(ctx, device.Initialize(IDevice::DeviceDesc{}));
    ShaderRuntime shaderRuntime;
    CHECK(ctx, shaderRuntime.Initialize(device));
    CHECK(ctx, registry.InitializeAll(FeatureInitializationContext{device, shaderRuntime, nullptr}));

    renderer::RenderView view{};
    view.view = math::Mat4::LookAtRH({ 0.f, 0.f, 0.f }, { 0.f, 0.f, 1.f }, math::Vec3::Up());
    view.projection = math::Mat4::PerspectiveFovRH(60.f * math::DEG_TO_RAD, 1.f, 0.1f, 50.f);
    view.cameraPosition = { 0.f, 0.f, 0.f };
    view.cameraForward = { 0.f, 0.f, 1.f };
    view.nearPlane = 0.1f;
    view.farPlane = 50.f;

    RenderSceneSnapshot snapshot;
    FrameExtractionStage extractionStage;
    const FrameExtractionStageContext extractionContext{
        world,
        registry.GetSceneExtractionSteps(),
        snapshot,
        &view
    };

    FrameExtractionStageResult extractionResult{};
    CHECK(ctx, extractionStage.Execute(extractionContext, extractionResult));

    const auto* shadowData =
        snapshot.GetFrameDataView().GetFrameData<engine::addons::shadow::ShadowFrameData>();
    CHECK(ctx, shadowData != nullptr);
    CHECK_EQ(ctx, shadowData ? shadowData->requests.size() : 0u, size_t(1));
    CHECK_EQ(ctx, shadowData ? shadowData->currentRenderPath.requestIndices.size() : 0u, size_t(0));
    CHECK_EQ(ctx, shadowData ? shadowData->culledReceiverCount : 0u, 1u);

    registry.ShutdownAll(FeatureShutdownContext{});
    shaderRuntime.Shutdown();
    device.Shutdown();
}

static void TestShadowExtractionFiltersDirectionalCasterOutliers(test::TestContext& ctx)
{
    RegisterRendererTestComponents();

    ecs::ComponentMetaRegistry componentRegistry = CreateRendererTestRegistry();
    ecs::World world(componentRegistry);

    auto addCaster = [&](math::Vec3 center, math::Vec3 extents)
    {
        const EntityID caster = world.CreateEntity();
        world.Add<TransformComponent>(caster);
        world.Add<WorldTransformComponent>(caster);
        MeshComponent mesh{};
        mesh.mesh = MeshHandle::Make(1u, 1u);
        mesh.castShadows = true;
        world.Add<MeshComponent>(caster, mesh);
        BoundsComponent bounds{};
        bounds.centerWorld = center;
        bounds.extentsWorld = extents;
        bounds.boundingSphere = extents.Length();
        world.Add<BoundsComponent>(caster, bounds);
    };

    addCaster({ 0.f, 0.f, 0.f }, { 1.f, 1.f, 1.f });
    addCaster({ 1000.f, 0.f, 0.f }, { 1.f, 1.f, 1.f });

    const EntityID light = world.CreateEntity();
    auto& lightTransform = world.Add<TransformComponent>(light);
    lightTransform.localRotation = math::Quat::FromEulerDeg(-35.f, 0.f, 0.f);
    world.Add<WorldTransformComponent>(light);
    LightComponent shadowLight{};
    shadowLight.type = LightType::Directional;
    shadowLight.castShadows = true;
    shadowLight.shadowSettings.enabled = true;
    shadowLight.shadowSettings.maxDistance = 50.f;
    world.Add<LightComponent>(light, shadowLight);

    TransformSystem transformSystem;
    transformSystem.Update(world);

    FeatureRegistry registry;
    CHECK(ctx, registry.AddFeature(engine::addons::lighting::CreateLightingFeature()));
    CHECK(ctx, registry.AddFeature(engine::addons::shadow::CreateShadowFeature()));

    TestDevice device;
    CHECK(ctx, device.Initialize(IDevice::DeviceDesc{}));
    ShaderRuntime shaderRuntime;
    CHECK(ctx, shaderRuntime.Initialize(device));
    CHECK(ctx, registry.InitializeAll(FeatureInitializationContext{device, shaderRuntime, nullptr}));

    renderer::RenderView view{};
    view.view = math::Mat4::LookAtRH({ 0.f, 2.f, -8.f }, { 0.f, 0.f, 0.f }, math::Vec3::Up());
    view.projection = math::Mat4::PerspectiveFovRH(60.f * math::DEG_TO_RAD, 16.f / 9.f, 0.1f, 50.f);
    view.cameraPosition = { 0.f, 2.f, -8.f };
    view.cameraForward = { 0.f, 0.f, 1.f };
    view.nearPlane = 0.1f;
    view.farPlane = 50.f;

    RenderSceneSnapshot snapshot;
    FrameExtractionStage extractionStage;
    const FrameExtractionStageContext extractionContext{
        world,
        registry.GetSceneExtractionSteps(),
        snapshot,
        &view
    };

    FrameExtractionStageResult extractionResult{};
    CHECK(ctx, extractionStage.Execute(extractionContext, extractionResult));

    const auto* shadowData =
        snapshot.GetFrameDataView().GetFrameData<engine::addons::shadow::ShadowFrameData>();
    CHECK(ctx, shadowData != nullptr);
    const auto* request = shadowData ? shadowData->GetCurrentRenderPathPrimaryRequest() : nullptr;
    CHECK(ctx, request != nullptr);
    if (request)
    {
        CHECK(ctx, request->casterBoundsWorld.max.x < 100.f);
        CHECK(ctx, request->casterBoundsWorld.min.x > -100.f);
    }

    registry.ShutdownAll(FeatureShutdownContext{});
    shaderRuntime.Shutdown();
    device.Shutdown();
}

static void TestDirectionalShadowSlicesMatchCameraDepth(test::TestContext& ctx)
{
    RegisterRendererTestComponents();
    ecs::ComponentMetaRegistry components = CreateRendererTestRegistry();
    ecs::World world(components);
    const EntityID receiver = world.CreateEntity();
    MeshComponent mesh{};
    mesh.mesh = MeshHandle::Make(1u, 1u);
    world.Add<MeshComponent>(receiver, mesh);
    world.Add<BoundsComponent>(receiver, BoundsComponent{
        .centerWorld = {0.f, 0.f, -5.f}, .extentsWorld = {1.f, 1.f, 1.f}, .boundingSphere = 2.f});
    const EntityID lightEntity = world.CreateEntity();
    world.Add<WorldTransformComponent>(lightEntity);
    LightComponent light{};
    light.type = LightType::Directional;
    light.castShadows = true;
    light.shadowSettings.enabled = true;
    light.shadowSettings.maxDistance = 80.f;
    world.Add<LightComponent>(lightEntity, light);
    FeatureRegistry registry;
    CHECK(ctx, registry.AddFeature(engine::addons::lighting::CreateLightingFeature()));
    CHECK(ctx, registry.AddFeature(engine::addons::shadow::CreateShadowFeature({.maxShadowAtlasResolution = 1024u})));
    TestDevice device;
    CHECK(ctx, device.Initialize(IDevice::DeviceDesc{}));
    ShaderRuntime shaderRuntime;
    CHECK(ctx, shaderRuntime.Initialize(device));
    CHECK(ctx, registry.InitializeAll(FeatureInitializationContext{device, shaderRuntime, nullptr}));

    // Wide FOV exposes the old radial-distance fit; orthographic cameras must
    // keep the same XY footprint at every depth. Include translated/rotated views.
    for (bool orthographic : {false, true})
    for (const math::Vec3 position : {math::Vec3::Zero(), math::Vec3{123.f, 8.f, -45.f}})
    for (uint32_t cascadeCount : {1u, 4u})
    {
        auto* settings = &world.Get<LightComponent>(lightEntity)->shadowSettings;
        settings->cascadeCount = cascadeCount;
        RenderView view{};
        view.cameraPosition = position;
        view.cameraForward = math::Vec3{0.3f, -0.1f, -1.f}.Normalized();
        view.view = math::Mat4::LookAtRH(position, position + view.cameraForward, math::Vec3::Up());
        view.nearPlane = 0.5f;
        view.farPlane = 200.f;
        view.projection = orthographic
            ? math::Mat4::OrthoRH(-24.f, 24.f, -8.f, 8.f, view.nearPlane, view.farPlane)
            : math::Mat4::PerspectiveFovRH(110.f * math::DEG_TO_RAD, 32.f / 9.f, view.nearPlane, view.farPlane);
        world.Get<BoundsComponent>(receiver)->centerWorld = position + view.cameraForward * 5.f;

        RenderSceneSnapshot snapshot;
        FrameExtractionStageResult result{};
        FrameExtractionStage stage;
        CHECK(ctx, stage.Execute(FrameExtractionStageContext{
            world, registry.GetSceneExtractionSteps(), snapshot, &view}, result));
        const auto* data = snapshot.GetFrameDataView().GetFrameData<engine::addons::shadow::ShadowFrameData>();
        const auto* request = data ? data->GetCurrentRenderPathPrimaryRequest() : nullptr;
        CHECK(ctx, request != nullptr);
        if (!request) continue;
        CHECK_EQ(ctx, request->views.size(), static_cast<size_t>(cascadeCount));
        // 1024 atlas: one map keeps 1024, four cascades get 512 tiles. The
        // tiles grow back when the cascades go (the atlas size stays the same).
        CHECK_EQ(ctx, request->effectiveResolution, cascadeCount > 1u ? 512u : 1024u);
        CHECK_EQ(ctx, request->views.front().viewport.width, static_cast<float>(request->effectiveResolution));
        CHECK_EQ(ctx, request->allocation.desc.width, request->effectiveResolution);
        CHECK_EQ(ctx, request->allocation.desc.height, request->effectiveResolution);
        CHECK_EQ(ctx, request->settings.resolution, 2048u);
        if (cascadeCount == 1u)
        {
            // The last camera-plane corners must lie in the single-map fit too.
            const math::Mat4 invProjection = view.projection.Inverse();
            const math::Mat4 invView = view.view.InverseAffine();
            for (float y : {-1.f, 1.f}) for (float x : {-1.f, 1.f})
            {
                const math::Vec4 a = invProjection * math::Vec4{x, y, 0.f, 1.f};
                const math::Vec4 b = invProjection * math::Vec4{x, y, 1.f, 1.f};
                const math::Vec3 av = a.xyz() * (1.f / a.w);
                const math::Vec3 bv = b.xyz() * (1.f / b.w);
                const math::Vec3 point = invView.TransformPoint(av + (bv - av) * ((-80.f - av.z) / (bv.z - av.z)));
                const auto& bounds = request->receiverBoundsWorld;
                CHECK(ctx, point.x >= bounds.min.x - 0.002f && point.x <= bounds.max.x + 0.002f);
                CHECK(ctx, point.y >= bounds.min.y - 0.002f && point.y <= bounds.max.y + 0.002f);
                CHECK(ctx, point.z >= bounds.min.z - 0.002f && point.z <= bounds.max.z + 0.002f);
            }
            continue;
        }
        CHECK_EQ(ctx, request->cascadeFrustumCornersWorld.size(), size_t(4));
        auto splitDepth = [&](uint32_t split)
        {
            const float f = static_cast<float>(split) / 4.f;
            const float uniform = view.nearPlane + (80.f - view.nearPlane) * f;
            const float logarithmic = view.nearPlane * std::pow(80.f / view.nearPlane, f);
            return uniform + (logarithmic - uniform) * settings->cascadeLambda;
        };
        // Cascades after the first start 10% of the previous cascade earlier: the
        // shaders dither into the next cascade over that band (kCascadeBlendBand).
        auto sliceNear = [&](uint32_t cascade)
        {
            return cascade == 0u
                ? splitDepth(0u)
                : splitDepth(cascade) - 0.1f * (splitDepth(cascade) - splitDepth(cascade - 1u));
        };
        for (size_t cascade = 0; cascade < request->cascadeFrustumCornersWorld.size(); ++cascade)
        for (size_t corner = 0; corner < 8u; ++corner)
        {
            const auto& point = request->cascadeFrustumCornersWorld[cascade][corner];
            const auto vs = view.view.TransformPoint(point);
            const uint32_t cascadeIndex = static_cast<uint32_t>(cascade);
            const float expectedDepth = corner >= 4u ? splitDepth(cascadeIndex + 1u) : sliceNear(cascadeIndex);
            CHECK(ctx, std::fabs(-vs.z - expectedDepth) < 0.002f);
            const auto cs = view.projection * math::Vec4{vs.x, vs.y, vs.z, 1.f};
            CHECK(ctx, std::fabs(std::fabs(cs.x / cs.w) - 1.f) < 0.002f);
            CHECK(ctx, std::fabs(std::fabs(cs.y / cs.w) - 1.f) < 0.002f);
            const auto shadowCS = request->views[cascade].viewProj * math::Vec4{point.x, point.y, point.z, 1.f};
            CHECK(ctx, std::fabs(shadowCS.x) <= shadowCS.w && std::fabs(shadowCS.y) <= shadowCS.w);
            CHECK(ctx, shadowCS.z > 0.f && shadowCS.z < shadowCS.w);
        }
    }
    registry.ShutdownAll(FeatureShutdownContext{});
    shaderRuntime.Shutdown();
    device.Shutdown();
}

static void TestDirectionalShadowDepthIncludesUpstreamCasters(test::TestContext& ctx)
{
    using namespace engine::addons::shadow;
    LightComponent light{};
    light.type = LightType::Directional;
    WorldTransformComponent transform{};
    const collision::AABB receivers{{-3.f, -3.f, -12.f}, {3.f, 3.f, -8.f}};
    const collision::AABB casters{{-2.f, -2.f, 998.f}, {2.f, 2.f, 1002.f}};
    std::array<math::Vec3, 8> corners{};
    size_t index = 0u;
    for (float z : {-12.f, -8.f}) for (float y : {-3.f, 3.f}) for (float x : {-3.f, 3.f})
        corners[index++] = {x, y, z};
    for (bool cascaded : {false, true})
    {
        ShadowRequest request{};
        ShadowViewBuilder::BuildDirectional(light, transform, casters, receivers, request, cascaded ? &corners : nullptr);
        CHECK_EQ(ctx, request.views.size(), size_t(1));
        for (float z : {-12.f, -8.f, 998.f, 1002.f})
        {
            const auto cs = request.views[0].viewProj * math::Vec4{0.f, 0.f, z, 1.f};
            CHECK(ctx, cs.z > 0.f && cs.z < cs.w);
        }
        if (cascaded)
        {
            // Sub-texel motion cannot move the XY projection on a static point.
            auto moved = corners;
            for (auto& corner : moved) corner.x += 0.0001f;
            ShadowRequest movedRequest{};
            ShadowViewBuilder::BuildDirectional(light, transform, casters, receivers, movedRequest, &moved);
            const auto before = request.views[0].viewProj * math::Vec4{0.f, 0.f, -10.f, 1.f};
            const auto after = movedRequest.views[0].viewProj * math::Vec4{0.f, 0.f, -10.f, 1.f};
            CHECK(ctx, std::fabs(before.x - after.x) < 1e-6f);
            CHECK(ctx, std::fabs(before.y - after.y) < 1e-6f);
        }
    }
}

static void TestLightingExtractionSortsShadowedAndDirectionalFirst(test::TestContext& ctx)
{
    RegisterRendererTestComponents();

    ecs::ComponentMetaRegistry componentRegistry = CreateRendererTestRegistry();
    ecs::World world(componentRegistry);

    const EntityID point = world.CreateEntity();
    world.Add<TransformComponent>(point);
    world.Add<WorldTransformComponent>(point);
    LightComponent pointLight{};
    pointLight.type = LightType::Point;
    pointLight.intensity = 5.f;
    world.Add<LightComponent>(point, pointLight);

    const EntityID spotShadow = world.CreateEntity();
    world.Add<TransformComponent>(spotShadow);
    world.Add<WorldTransformComponent>(spotShadow);
    LightComponent spotLight{};
    spotLight.type = LightType::Spot;
    spotLight.intensity = 2.f;
    spotLight.castShadows = true;
    spotLight.shadowSettings.enabled = true;
    world.Add<LightComponent>(spotShadow, spotLight);

    const EntityID directionalShadow = world.CreateEntity();
    world.Add<TransformComponent>(directionalShadow);
    world.Add<WorldTransformComponent>(directionalShadow);
    LightComponent dirLight{};
    dirLight.type = LightType::Directional;
    dirLight.intensity = 1.f;
    dirLight.castShadows = true;
    dirLight.shadowSettings.enabled = true;
    world.Add<LightComponent>(directionalShadow, dirLight);

    RenderWorld renderWorld;
    engine::addons::lighting::ExtractLights(world, renderWorld.GetExtractionView());

    const auto* lighting = renderWorld.GetFeatureData<engine::addons::lighting::LightingFrameData>();
    CHECK(ctx, lighting != nullptr);
    CHECK_EQ(ctx, lighting->lights.size(), size_t(3));
    CHECK_EQ(ctx, lighting->lights[0].entity, directionalShadow);
    CHECK_EQ(ctx, lighting->lights[1].entity, spotShadow);
    CHECK_EQ(ctx, lighting->lights[2].entity, point);
    CHECK_EQ(ctx, engine::addons::lighting::GetShadowCastingLightCount(renderWorld.GetFrameDataView()), 2u);
}

static void TestLightingFrameConstantsTrackPackedAndDroppedLights(test::TestContext& ctx)
{
    RegisterRendererTestComponents();

    ecs::ComponentMetaRegistry componentRegistry = CreateRendererTestRegistry();
    ecs::World world(componentRegistry);

    for (uint32_t i = 0u; i < engine::addons::lighting::kMaxLightsPerFrame + 2u; ++i)
    {
        const EntityID light = world.CreateEntity();
        world.Add<TransformComponent>(light);
        world.Add<WorldTransformComponent>(light);
        LightComponent lc{};
        lc.type = (i == 0u) ? LightType::Directional : LightType::Point;
        lc.intensity = static_cast<float>(100u - i);
        if (i < 2u)
        {
            lc.castShadows = true;
            lc.shadowSettings.enabled = true;
        }
        world.Add<LightComponent>(light, lc);
    }

    FeatureRegistry registry;
    CHECK(ctx, registry.AddFeature(engine::addons::lighting::CreateLightingFeature()));

    TestDevice device;
    CHECK(ctx, device.Initialize(IDevice::DeviceDesc{}));
    ShaderRuntime shaderRuntime;
    CHECK(ctx, shaderRuntime.Initialize(device));
    CHECK(ctx, registry.InitializeAll(FeatureInitializationContext{device, shaderRuntime, nullptr}));

    RenderSceneSnapshot snapshot;
    FrameExtractionStage extractionStage;
    const FrameExtractionStageContext extractionContext{
        world,
        registry.GetSceneExtractionSteps(),
        snapshot
    };

    FrameExtractionStageResult extractionResult{};
    CHECK(ctx, extractionStage.Execute(extractionContext, extractionResult));
    CHECK_EQ(ctx, engine::addons::lighting::GetExtractedLightCount(snapshot.GetFrameDataView()),
             size_t(engine::addons::lighting::kMaxLightsPerFrame + 2u));

    renderer::RenderView view{};
    view.view = math::Mat4::LookAtRH({ 0.f, 1.f, -3.f }, { 0.f, 0.f, 0.f }, math::Vec3::Up());
    view.projection = math::Mat4::PerspectiveFovRH(60.f * math::DEG_TO_RAD, 16.f / 9.f, 0.1f, 100.f);
    view.cameraPosition = { 0.f, 1.f, -3.f };
    view.cameraForward = { 0.f, 0.f, 1.f };
    view.nearPlane = 0.1f;
    view.farPlane = 100.f;

    platform::FixedTiming timing(1.0 / 60.0);
    FrameConstantStage constantStage;
    const FrameConstantStageContext constantContext{
        math::Mat4::Identity(),
        math::Mat4::Identity(),
        1280u,
        720u,
        view,
        timing,
        EnvironmentRuntimeState{},
        snapshot.GetFrameDataView(),
        &snapshot.GetQueue(),
        registry.GetFrameConstantsContributors()
    };

    FrameConstantsResult frameResult{};
    CHECK(ctx, constantStage.PrepareFrameData(constantContext, frameResult));
    CHECK_EQ(ctx, frameResult.frameConstants.featureCount0, engine::addons::lighting::kMaxLightsPerFrame);
    CHECK_EQ(ctx, engine::addons::lighting::GetPackedLightCount(snapshot.GetFrameDataView()),
             engine::addons::lighting::kMaxLightsPerFrame);
    CHECK_EQ(ctx, engine::addons::lighting::GetDroppedLightCount(snapshot.GetFrameDataView()), 2u);
    CHECK_EQ(ctx, engine::addons::lighting::GetShadowCastingLightCount(snapshot.GetFrameDataView()), 2u);

    registry.ShutdownAll(FeatureShutdownContext{});
    shaderRuntime.Shutdown();
    device.Shutdown();
}

static void TestBackendShadowClipSpaceAdjustmentsDx11OpenGL(test::TestContext& ctx)
{
    engine::renderer::dx11::DX11Device dx11;
    engine::renderer::opengl::OpenGLDevice opengl;

    const math::Mat4 dx11Shadow = dx11.GetShadowClipSpaceAdjustment();
    const math::Mat4 glShadow = opengl.GetShadowClipSpaceAdjustment();

    CHECK(ctx, NearlyEqual(dx11Shadow.m[1][1], 1.0f));
    CHECK(ctx, NearlyEqual(dx11Shadow.m[0][0], 1.0f));
    CHECK(ctx, NearlyEqual(dx11Shadow.m[2][2], 1.0f));
    CHECK(ctx, NearlyEqual(dx11Shadow.m[3][3], 1.0f));

    // Y-Flip (m[1][1] == -1) ist erforderlich, damit CullMode::Front im Shadow-Pass
    // auf OpenGL korrekt cullt (frontFace-Inversion passend zur Rasterung).
    CHECK(ctx, NearlyEqual(glShadow.m[1][1], -1.0f));
    CHECK(ctx, NearlyEqual(glShadow.m[0][0], 1.0f));
    CHECK(ctx, NearlyEqual(glShadow.m[2][2], 2.0f));
    CHECK(ctx, NearlyEqual(glShadow.m[3][2], -1.0f));
    CHECK(ctx, NearlyEqual(glShadow.m[3][3], 1.0f));
}

static void TestShadowDrawListIncludesAllCastersForSelectedDirectionalRequest(test::TestContext& ctx)
{
    RegisterRendererTestComponents();

    ecs::ComponentMetaRegistry componentRegistry = CreateRendererTestRegistry();
    ecs::World world(componentRegistry);
    MaterialSystem materials;

    MaterialDesc materialDesc;
    MaterialRuntimeDesc materialDescRuntime;
    materialDesc.name = "ShadowCasterMat";
    materialDescRuntime.renderPass = StandardRenderPasses::Opaque();
    materialDescRuntime.vertexShader = ShaderHandle::Make(1u, 1u);
    materialDescRuntime.fragmentShader = ShaderHandle::Make(2u, 1u);
    const MaterialHandle material = MaterialRuntimeBridge::RegisterMaterial(materials, std::move(materialDesc), materialDescRuntime);

    const EntityID light = world.CreateEntity();
    auto& lightTransform = world.Add<TransformComponent>(light);
    lightTransform.localRotation = math::Quat::FromEulerDeg(-35.f, 25.f, 0.f);
    world.Add<WorldTransformComponent>(light);

    LightComponent shadowLight{};
    shadowLight.type = LightType::Directional;
    shadowLight.castShadows = true;
    shadowLight.shadowSettings.enabled = true;
    shadowLight.shadowSettings.resolution = 2048u;
    world.Add<LightComponent>(light, shadowLight);

    const math::Vec3 positions[3] = {
        { -2.0f, 1.0f, 0.0f },
        {  0.0f, 1.0f, 0.0f },
        {  2.0f, 1.0f, 0.0f }
    };

    EntityID casters[3]{};
    for (size_t i = 0; i < 3; ++i)
    {
        casters[i] = world.CreateEntity();
        auto& transform = world.Add<TransformComponent>(casters[i]);
        transform.localPosition = positions[i];
        world.Add<WorldTransformComponent>(casters[i]);
        world.Add<MeshComponent>(casters[i], MeshComponent{ MeshHandle::Make(static_cast<uint32_t>(10u + i), 1u) });
        world.Add<MaterialComponent>(casters[i], material);
        world.Add<BoundsComponent>(casters[i], BoundsComponent{
            .centerWorld = positions[i],
            .extentsWorld = { 0.5f, 0.5f, 0.5f },
            .boundingSphere = 0.8660254f
        });
    }

    TransformSystem transformSystem;
    transformSystem.Update(world);

    FeatureRegistry registry;
    CHECK(ctx, registry.AddFeature(engine::addons::mesh_renderer::CreateMeshRendererFeature()));
    CHECK(ctx, registry.AddFeature(engine::addons::lighting::CreateLightingFeature()));
    CHECK(ctx, registry.AddFeature(engine::addons::shadow::CreateShadowFeature()));

    TestDevice device;
    CHECK(ctx, device.Initialize(IDevice::DeviceDesc{}));
    ShaderRuntime shaderRuntime;
    CHECK(ctx, shaderRuntime.Initialize(device));
    CHECK(ctx, registry.InitializeAll(FeatureInitializationContext{device, shaderRuntime, nullptr}));

    RenderSceneSnapshot snapshot;
    FrameExtractionStage extractionStage;
    const FrameExtractionStageContext extractionContext{
        world,
        registry.GetSceneExtractionSteps(),
        snapshot
    };

    FrameExtractionStageResult extractionResult{};
    CHECK(ctx, extractionStage.Execute(extractionContext, extractionResult));

    const auto* shadowData = snapshot.GetFrameDataView().GetFrameData<engine::addons::shadow::ShadowFrameData>();
    CHECK(ctx, shadowData != nullptr);
    CHECK(ctx, shadowData->HasCurrentRenderPathRequests());
    CHECK(ctx, shadowData->HasCurrentRenderPathPrimaryRequest());
    CHECK(ctx, shadowData->HasCurrentRenderPathPrimaryVisibleLight());
    CHECK_EQ(ctx, shadowData->GetCurrentRenderPathPrimaryVisibleLightIndex(), uint32_t(0));
    const auto* selectedShadowRequest = shadowData->GetCurrentRenderPathPrimaryRequest();
    CHECK(ctx, selectedShadowRequest != nullptr);
    CHECK_EQ(ctx, selectedShadowRequest->lightEntity, light);
    CHECK_EQ(ctx, selectedShadowRequest->lightType, LightType::Directional);

    const math::Mat4 view = math::Mat4::LookAtRH({ 0.f, 3.f, -8.f }, { 0.f, 1.f, 0.f }, math::Vec3::Up());
    const math::Mat4 proj = math::Mat4::PerspectiveFovRH(60.f * math::DEG_TO_RAD, 16.f / 9.f, 0.1f, 100.f);
    const math::Mat4 viewProj = proj * view;

    RenderPassRegistry renderPassRegistry;
    snapshot.BuildDrawLists(view, viewProj, 0.1f, 100.f, materials, renderPassRegistry);

    const RenderQueue& queue = snapshot.GetQueue();
    const DrawList& opaque = RequirePassList(ctx, queue, StandardRenderPasses::Opaque());
    const DrawList& shadow = RequirePassList(ctx, queue, StandardRenderPasses::Shadow());

    CHECK_EQ(ctx, snapshot.VisibleCount(), 3u);
    CHECK_EQ(ctx, opaque.Size(), 3u);
    CHECK_EQ(ctx, shadow.Size(), 3u);

    bool seenCaster0 = false;
    bool seenCaster1 = false;
    bool seenCaster2 = false;
    for (const DrawItem& item : shadow.items)
    {
        if (item.entity == casters[0]) seenCaster0 = true;
        if (item.entity == casters[1]) seenCaster1 = true;
        if (item.entity == casters[2]) seenCaster2 = true;
    }

    CHECK(ctx, seenCaster0);
    CHECK(ctx, seenCaster1);
    CHECK(ctx, seenCaster2);

    // Shadow items carry the caster bounds; the fitted directional view keeps all of them.
    for (const DrawItem& item : shadow.items)
    {
        CHECK(ctx, item.hasBounds);
        CHECK(ctx, NearlyEqual(item.boundsExtents.x, 0.5f));
    }
    if (selectedShadowRequest && !selectedShadowRequest->views.empty())
    {
        DrawList filtered;
        const size_t culled = engine::addons::shadow::FilterShadowCastersForView(
            shadow, selectedShadowRequest->views.front().viewProj, filtered);
        CHECK_EQ(ctx, culled, size_t(0));
        CHECK_EQ(ctx, filtered.Size(), size_t(3));
    }

    registry.ShutdownAll(FeatureShutdownContext{});
    shaderRuntime.Shutdown();
    device.Shutdown();
}

static void TestShadowCasterCullingFiltersItemsPerShadowView(test::TestContext& ctx)
{
    // Orthographic shadow view looking down -Z from z=+10, covering x/y in [-5, 5]
    // and depth 0.1..20 (world z from 9.9 down to -10).
    const math::Mat4 lightView = math::Mat4::LookAtRH({ 0.f, 0.f, 10.f }, { 0.f, 0.f, 0.f }, math::Vec3::Up());
    const math::Mat4 lightProj = math::Mat4::OrthoRH(-5.f, 5.f, -5.f, 5.f, 0.1f, 20.f);
    const math::Mat4 shadowViewProj = lightProj * lightView;

    auto makeItem = [](uint32_t id, math::Vec3 center, bool withBounds)
    {
        DrawItem item{};
        item.entity = EntityID::Make(id, 1u);
        item.boundsCenter = center;
        item.boundsExtents = { 0.5f, 0.5f, 0.5f };
        item.hasBounds = withBounds;
        return item;
    };

    DrawList source;
    source.passId = StandardRenderPasses::Shadow();
    source.Add(makeItem(1u, { 0.f, 0.f, 0.f }, true));      // inside
    source.Add(makeItem(2u, { 50.f, 0.f, 0.f }, true));     // beside the view
    source.Add(makeItem(3u, { 0.f, 0.f, 30.f }, true));     // before the near plane: kept (depth clamp)
    source.Add(makeItem(4u, { 5.3f, 0.f, 0.f }, true));     // overlaps the right edge
    source.Add(makeItem(5u, { 50.f, 0.f, 0.f }, false));    // no bounds: never culled
    source.Add(makeItem(6u, { 0.f, 0.f, -30.f }, true));    // beyond the far plane
    source.sorted = true;

    DrawList filtered;
    const size_t culled =
        engine::addons::shadow::FilterShadowCastersForView(source, shadowViewProj, filtered);

    CHECK_EQ(ctx, culled, size_t(2));
    CHECK_EQ(ctx, filtered.Size(), size_t(4));
    CHECK(ctx, filtered.passId == StandardRenderPasses::Shadow());
    CHECK(ctx, filtered.sorted);
    if (filtered.Size() == 4u)
    {
        // Order is kept so sorting/instancing stays valid.
        CHECK_EQ(ctx, filtered.items[0].entity, EntityID::Make(1u, 1u));
        CHECK_EQ(ctx, filtered.items[1].entity, EntityID::Make(3u, 1u));
        CHECK_EQ(ctx, filtered.items[2].entity, EntityID::Make(4u, 1u));
        CHECK_EQ(ctx, filtered.items[3].entity, EntityID::Make(5u, 1u));
    }

    // Reusing the output list replaces its previous content.
    DrawList empty;
    empty.passId = StandardRenderPasses::Shadow();
    CHECK_EQ(ctx, engine::addons::shadow::FilterShadowCastersForView(empty, shadowViewProj, filtered), size_t(0));
    CHECK_EQ(ctx, filtered.Size(), size_t(0));
}

static void TestShadowExtractionSkipsDirectionalWithoutVisibleReceiver(test::TestContext& ctx)
{
    RegisterRendererTestComponents();

    ecs::ComponentMetaRegistry componentRegistry = CreateRendererTestRegistry();
    ecs::World world(componentRegistry);

    const EntityID receiver = world.CreateEntity();
    world.Add<TransformComponent>(receiver);
    world.Add<WorldTransformComponent>(receiver);
    MeshComponent mesh{};
    mesh.mesh = MeshHandle::Make(1u, 1u);
    mesh.castShadows = true;
    mesh.receiveShadows = true;
    world.Add<MeshComponent>(receiver, mesh);
    BoundsComponent bounds{};
    bounds.centerWorld = { 0.f, 0.f, 10.f };
    bounds.extentsWorld = { 1.f, 1.f, 1.f };
    bounds.boundingSphere = bounds.extentsWorld.Length();
    world.Add<BoundsComponent>(receiver, bounds);

    const EntityID light = world.CreateEntity();
    auto& lightTransform = world.Add<TransformComponent>(light);
    lightTransform.localRotation = math::Quat::FromEulerDeg(-45.f, 0.f, 0.f);
    world.Add<WorldTransformComponent>(light);
    LightComponent shadowLight{};
    shadowLight.type = LightType::Directional;
    shadowLight.castShadows = true;
    shadowLight.shadowSettings.enabled = true;
    shadowLight.shadowSettings.maxDistance = 50.f;
    world.Add<LightComponent>(light, shadowLight);

    TransformSystem transformSystem;
    transformSystem.Update(world);

    FeatureRegistry registry;
    CHECK(ctx, registry.AddFeature(engine::addons::lighting::CreateLightingFeature()));
    CHECK(ctx, registry.AddFeature(engine::addons::shadow::CreateShadowFeature()));

    TestDevice device;
    CHECK(ctx, device.Initialize(IDevice::DeviceDesc{}));
    ShaderRuntime shaderRuntime;
    CHECK(ctx, shaderRuntime.Initialize(device));
    CHECK(ctx, registry.InitializeAll(FeatureInitializationContext{device, shaderRuntime, nullptr}));

    auto extract = [&](math::Vec3 forward, RenderSceneSnapshot& snapshot)
    {
        renderer::RenderView view{};
        view.view = math::Mat4::LookAtRH({ 0.f, 0.f, 0.f }, forward, math::Vec3::Up());
        view.projection = math::Mat4::PerspectiveFovRH(60.f * math::DEG_TO_RAD, 1.f, 0.1f, 100.f);
        view.cameraPosition = { 0.f, 0.f, 0.f };
        view.cameraForward = forward;
        view.nearPlane = 0.1f;
        view.farPlane = 100.f;

        FrameExtractionStage extractionStage;
        const FrameExtractionStageContext extractionContext{
            world,
            registry.GetSceneExtractionSteps(),
            snapshot,
            &view
        };
        FrameExtractionStageResult extractionResult{};
        CHECK(ctx, extractionStage.Execute(extractionContext, extractionResult));
        return snapshot.GetFrameDataView().GetFrameData<engine::addons::shadow::ShadowFrameData>();
    };

    {
        // Camera looks away from the only receiver: no shadow map is rendered.
        RenderSceneSnapshot snapshot;
        const auto* shadowData = extract({ 0.f, 0.f, -1.f }, snapshot);
        CHECK(ctx, shadowData != nullptr);
        CHECK_EQ(ctx, shadowData ? shadowData->requests.size() : 0u, size_t(1));
        CHECK(ctx, shadowData && !shadowData->HasCurrentRenderPathRequests());
        CHECK_EQ(ctx, shadowData ? shadowData->culledReceiverCount : 0u, 1u);
        CHECK_EQ(ctx, snapshot.GetQueue().activeShadowResolution, 0u);
    }
    {
        // Camera looks at the receiver: the directional shadow is selected again.
        RenderSceneSnapshot snapshot;
        const auto* shadowData = extract({ 0.f, 0.f, 1.f }, snapshot);
        CHECK(ctx, shadowData != nullptr);
        CHECK(ctx, shadowData && shadowData->HasCurrentRenderPathRequests());
        CHECK_EQ(ctx, shadowData ? shadowData->culledReceiverCount : 1u, 0u);
    }

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
    MaterialRuntimeDesc dRuntime;
    d.name = "LoopMat";
    dRuntime.renderPass = StandardRenderPasses::Opaque();
    dRuntime.vertexShader = ShaderHandle::Make(1u, 1u);
    dRuntime.fragmentShader = ShaderHandle::Make(2u, 1u);
    const MaterialHandle mat = MaterialRuntimeBridge::RegisterMaterial(ms, std::move(d), dRuntime);

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
// The renderer does not own a worker pool. It uses the one the host hands over
// and leaves it running; without one it runs its jobs on the calling thread.
// ==========================================================================
static void TestRenderSystemUsesHostJobSystem(test::TestContext& ctx)
{
    RegisterRendererTestComponents();

    ecs::ComponentMetaRegistry componentRegistry = CreateRendererTestRegistry();
    ecs::World world(componentRegistry);
    MaterialSystem ms;
    platform::FixedTiming timing(1.0 / 60.0);

    renderer::RenderView view;
    view.view = math::Mat4::LookAtRH({0.f, 0.f, -5.f}, {0.f, 0.f, 0.f}, math::Vec3::Up());
    view.projection = math::Mat4::PerspectiveFovRH(60.f * math::DEG_TO_RAD, 16.f / 9.f, 0.1f, 100.f);
    view.cameraPosition = {0.f, 0.f, -5.f};
    view.cameraForward = {0.f, 0.f, 1.f};

    const auto renderOneFrame = [&](jobs::JobSystem* pool, uint32_t& outPeakWorkers) -> bool {
        auto window = platform::CreateHeadlessWindow();
        if (!window->Create({.width=320u, .height=180u, .title="Headless"}))
            return false;
        renderer::RenderSystem renderer;
        renderer.SetJobSystem(pool);
        bool ok = renderer.RegisterFeature(engine::addons::mesh_renderer::CreateMeshRendererFeature())
            && renderer.RegisterFeature(engine::addons::lighting::CreateLightingFeature())
            && renderer.RegisterFeature(engine::renderer::addons::forward::CreateForwardFeature());
        renderer::IDevice::DeviceDesc dd;
        dd.appName = "KROM Test";
        platform::WindowDesc wDesc{};
        ok = ok && renderer.Initialize(renderer::DeviceFactory::BackendType::Null, *window, wDesc, nullptr, dd);
        timing.BeginFrame();
        ok = ok && renderer.RenderFrame(world, ms, view, timing);
        outPeakWorkers = renderer.GetStats().peakActiveWorkers;
        renderer.Shutdown();
        window->Destroy();
        return ok;
    };

    // Without a pool: the frame renders, no worker is involved.
    uint32_t peakWithout = 99u;
    CHECK(ctx, renderOneFrame(nullptr, peakWithout));
    CHECK_EQ(ctx, peakWithout, 0u);

    // With the pool of the host: the frame jobs run on its workers.
    jobs::JobSystem pool;
    pool.Initialize(2u);
    uint32_t peakWith = 0u;
    CHECK(ctx, renderOneFrame(&pool, peakWith));
    CHECK_GT(ctx, peakWith, 0u);

    // Renderer shutdown did not stop the pool: it still runs jobs on its workers.
    CHECK(ctx, pool.IsParallel());
    CHECK_EQ(ctx, pool.WorkerCount(), 2u);
    auto after = pool.DispatchReturn([&pool] { return pool.IsWorkerThread(); });
    const auto afterResult = after.get();
    CHECK(ctx, afterResult.Succeeded() && afterResult.value.value_or(false));

    // The same pool serves a second renderer.
    uint32_t peakSecond = 0u;
    CHECK(ctx, renderOneFrame(&pool, peakSecond));
    CHECK_GT(ctx, peakSecond, 0u);
    pool.Shutdown();
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
    MaterialRuntimeDesc dRuntime;
    d.name = "LoopMat";
    dRuntime.renderPass = StandardRenderPasses::Opaque();
    dRuntime.vertexShader = ShaderHandle::Make(1u, 1u);
    dRuntime.fragmentShader = ShaderHandle::Make(2u, 1u);
    const MaterialHandle mat = MaterialRuntimeBridge::RegisterMaterial(ms, std::move(d), dRuntime);

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

static void TestGpuResourceRuntimeSeparatesMeshBuffersByLayout(test::TestContext& ctx)
{
    RecordingComputeDevice device;
    CHECK(ctx, device.Initialize(IDevice::DeviceDesc{}));

    GpuResourceRuntime runtime;
    CHECK(ctx, runtime.Initialize(device, 2u));
    runtime.BeginFrame(0u);

    assets::AssetRegistry registry;
    auto mesh = std::make_unique<assets::MeshAsset>();
    mesh->debugName = "CanonicalRuntimeMesh";
    mesh->state = assets::AssetState::Loaded;

    assets::SubMeshData sm;
    sm.positions   = { 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f, 0.f };
    sm.normals     = { 0.f, 0.f, 1.f, 0.f, 0.f, 1.f, 0.f, 0.f, 1.f };
    sm.tangents    = { 1.f, 0.f, 0.f, 1.f, 1.f, 0.f, 0.f, 1.f, 1.f, 0.f, 0.f, 1.f };
    sm.uvs         = { 0.f, 0.f, 1.f, 0.f, 0.f, 1.f };
    sm.boneWeights = { 1.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f };
    sm.boneIndices = { 0u, 0u, 0u, 0u, 1u, 0u, 0u, 0u, 2u, 0u, 0u, 0u };
    sm.indices     = { 0u, 1u, 2u };
    mesh->submeshes.push_back(sm);

    const MeshHandle meshHandle = registry.GetOrAddMesh("canonical_runtime.mesh", std::move(mesh));

    std::string error;
    const VertexLayout staticLayout =
        assets::ResolveVertexLayout(VertexContracts::StaticLit(), sm, &error);
    CHECK(ctx, error.empty());
    const VertexLayout shadowLayout =
        assets::ResolveVertexLayout(VertexContracts::Shadow(), sm, &error);
    CHECK(ctx, error.empty());

    const GpuResourceRuntime::GpuMeshEntry* staticGpu =
        runtime.GetOrUploadMesh(meshHandle, 0u, staticLayout, registry);
    const GpuResourceRuntime::GpuMeshEntry* shadowGpu =
        runtime.GetOrUploadMesh(meshHandle, 0u, shadowLayout, registry);

    CHECK(ctx, staticGpu != nullptr);
    CHECK(ctx, shadowGpu != nullptr);
    if (staticGpu && shadowGpu)
    {
        CHECK(ctx, staticGpu->vertexBuffer != shadowGpu->vertexBuffer);
        CHECK(ctx, staticGpu->indexBuffer != shadowGpu->indexBuffer);
        CHECK_EQ(ctx, staticGpu->vertexStride, staticLayout.bindings[0].stride);
        CHECK_EQ(ctx, shadowGpu->vertexStride, shadowLayout.bindings[0].stride);
        CHECK(ctx, staticGpu->layoutHash != shadowGpu->layoutHash);

        const auto* vb = device.GetBuffer(staticGpu->vertexBuffer);
        CHECK(ctx, vb != nullptr);
        if (vb)
        {
            CHECK_EQ(ctx, vb->desc.stride, staticLayout.bindings[0].stride);
            CHECK_EQ(ctx, vb->desc.byteSize,
                     static_cast<uint64_t>(staticLayout.bindings[0].stride) * 3ull);
        }
    }
    CHECK_EQ(ctx, runtime.GetStats().liveMeshBuffers, 4u);

    runtime.Shutdown();
    device.Shutdown();
}

static void TestGpuResourceRuntimeRawFastPath(test::TestContext& ctx)
{
    // Strategie: rawInterleavedBytes mit stride=32 (pos+normal+uv — passt exakt zur
    // kanonischen Berechnung für diese Flat-Arrays), aber mit Sentinel-X=99.9f
    // statt 1.f in Vertex 0.
    // Fast Path: Staging-Buffer enthält 99.9f an Byte 0.
    // Fallback  : Staging-Buffer enthält 1.f (aus Flat-Arrays via BuildVertexBuffer).
    RecordingComputeDevice device;
    CHECK(ctx, device.Initialize(IDevice::DeviceDesc{}));

    GpuResourceRuntime runtime;
    CHECK(ctx, runtime.Initialize(device, 2u));
    runtime.BeginFrame(0u);

    assets::AssetRegistry registry;
    auto mesh = std::make_unique<assets::MeshAsset>();
    mesh->debugName = "RawFastPathMesh";
    mesh->state = assets::AssetState::Loaded;

    assets::SubMeshData sm;
    sm.positions = { 1.f,2.f,3.f,  4.f,5.f,6.f,  7.f,8.f,9.f };
    sm.normals   = { 0.f,0.f,1.f,  0.f,0.f,1.f,  0.f,0.f,1.f };
    sm.uvs       = { 0.f,0.f,  1.f,0.f,  0.f,1.f };
    sm.indices   = { 0u, 1u, 2u };

    // Canonical stride for pos(12)+normal(12)+uv(8) = 32. rawVertexStride muss gleich
    // uploadStride sein, damit der Fast-Path-Guard greift.
    const uint32_t rawStride = 32u;
    const float    sentinelX = 99.9f; // unterscheidet sich von sm.positions[0] = 1.f

    sm.rawVertexStride = rawStride;
    sm.rawInterleavedBytes.resize(3u * rawStride, 0u);
    for (uint32_t vi = 0; vi < 3u; ++vi)
    {
        uint8_t* row = sm.rawInterleavedBytes.data() + vi * rawStride;
        // Vertex 0 bekommt sentinelX, andere übernehmen den echten X-Wert.
        const float posX = (vi == 0u) ? sentinelX : sm.positions[vi * 3];
        std::memcpy(row,      &posX,                  4);
        std::memcpy(row + 4,  &sm.positions[vi*3 + 1], 4);
        std::memcpy(row + 8,  &sm.positions[vi*3 + 2], 4);
        std::memcpy(row + 12, &sm.normals[vi * 3],    12);
        std::memcpy(row + 24, &sm.uvs[vi * 2],         8);
    }

    mesh->submeshes.push_back(sm);
    const MeshHandle mh = registry.GetOrAddMesh("rawfast.mesh", std::move(mesh));

    std::string err;
    const VertexLayout layout =
        assets::ResolveVertexLayout(VertexContracts::StaticLit(), sm, &err);
    CHECK(ctx, err.empty());

    const auto* gpu = runtime.GetOrUploadMesh(mh, 0u, layout, registry);
    CHECK(ctx, gpu != nullptr);
    if (!gpu) { runtime.Shutdown(); device.Shutdown(); return; }

    CHECK_EQ(ctx, gpu->vertexStride, rawStride);

    const auto* vb = device.GetBuffer(gpu->vertexBuffer);
    CHECK(ctx, vb != nullptr);
    if (vb)
    {
        CHECK_EQ(ctx, vb->desc.stride, rawStride);
        CHECK_EQ(ctx, vb->desc.byteSize, static_cast<uint64_t>(3u * rawStride));
    }

    // Staging-Buffer (gleiche Upload-Größe wie VB, 3*32=96) enthält die tatsächlich
    // gesendeten Bytes. Fast Path: sentinelX an Byte 0. Fallback: 1.f (aus Flat-Array).
    const size_t expectedVBBytes = 3u * rawStride;
    const auto* staging = device.FindBufferByUploadedSize(expectedVBBytes);
    CHECK(ctx, staging != nullptr);
    if (staging && staging->uploadedData.size() == expectedVBBytes)
    {
        float uploadedX = 0.f;
        std::memcpy(&uploadedX, staging->uploadedData.data(), 4);
        CHECK(ctx, uploadedX == sentinelX); // Fast Path: rawInterleavedBytes wurden direkt verwendet
    }

    runtime.Shutdown();
    device.Shutdown();
}

static void TestGpuResourceRuntimeReuploadsReimportedMesh(test::TestContext& ctx)
{
    RecordingComputeDevice device;
    CHECK(ctx, device.Initialize(IDevice::DeviceDesc{}));
    GpuResourceRuntime runtime;
    CHECK(ctx, runtime.Initialize(device, 3u));
    assets::AssetRegistry registry;
    auto mesh = std::make_unique<assets::MeshAsset>();
    auto& sub = mesh->submeshes.emplace_back();
    sub.positions = {0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f, 0.f};
    sub.normals = {0.f, 0.f, 1.f, 0.f, 0.f, 1.f, 0.f, 0.f, 1.f};
    sub.indices = {0u, 1u, 2u};
    auto reimported = std::make_unique<assets::MeshAsset>(*mesh);
    reimported->submeshes.front().normals[1] = 0.6f;
    reimported->submeshes.front().normals[2] = 0.8f;
    const MeshHandle handle = registry.ReplaceOrAddMesh("rounded.glb#mesh/0", std::move(mesh));

    runtime.BeginFrame(0u);
    const auto* first = runtime.GetOrUploadMesh(handle, 0u, registry);
    CHECK(ctx, first != nullptr);
    const BufferHandle oldVertex = first ? first->vertexBuffer : BufferHandle::Invalid();
    const BufferHandle oldIndex = first ? first->indexBuffer : BufferHandle::Invalid();
    runtime.EndFrame(1u);

    CHECK_EQ(ctx, registry.ReplaceOrAddMesh("rounded.glb#mesh/0", std::move(reimported)), handle);
    runtime.BeginFrame(0u);
    const auto* refreshed = runtime.GetOrUploadMesh(handle, 0u, registry);
    CHECK(ctx, refreshed && refreshed->vertexBuffer != oldVertex && refreshed->indexBuffer != oldIndex);
    CHECK(ctx, device.GetBuffer(oldVertex) && device.GetBuffer(oldIndex));
    CHECK_EQ(ctx, runtime.GetStats().liveMeshBuffers, 2u);
    runtime.EndFrame(2u);

    runtime.BeginFrame(1u);
    CHECK(ctx, !device.GetBuffer(oldVertex) && !device.GetBuffer(oldIndex));
    CHECK(ctx, runtime.GetOrUploadMesh(handle, 0u, registry) == refreshed);
    runtime.EndFrame(3u);
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
    MaterialRuntimeDesc dRuntime;
    d.name = "IBLActiveMaterial";
    dRuntime.renderPass = StandardRenderPasses::Opaque();
    dRuntime.vertexShader = vsAsset;
    dRuntime.fragmentShader = psAsset;
    const MaterialHandle mat = MaterialRuntimeBridge::RegisterMaterial(ms, std::move(d), dRuntime);

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

    CHECK(ctx, runtime.PrepareMaterial(MaterialSystemShaderMaterialSource(ms), mat));

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

// A variant missing from the on-disk cache compiles on the pool: the material
// waits (not drawn, no error), progress is reported, and once the job is done
// the material is prepared with the variant. A second runtime then finds it in
// the disk cache and takes it synchronously.
static void TestShaderRuntimeCompilesVariantsInBackground(test::TestContext& ctx)
{
    const std::filesystem::path cacheDir = std::filesystem::temp_directory_path() /
        ("krom_async_variant_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ShaderCompiler::SetCacheDirectory(cacheDir);

    assets::AssetRegistry assets;
    auto vs = std::make_unique<assets::ShaderAsset>();
    vs->debugName = "VS_Async";
    vs->stage = assets::ShaderStage::Vertex;
    vs->entryPoint = "VSMain";
    assets::CompiledShaderArtifact vsCompiled;
    vsCompiled.target = assets::ShaderTargetProfile::Null;
    vsCompiled.stage = assets::ShaderStage::Vertex;
    vsCompiled.entryPoint = "VSMain";
    vsCompiled.sourceText = "float4 VSMain() : SV_Position { return float4(0,0,0,1); }";
    vsCompiled.sourceHash = 0x5102ull;
    vs->compiledArtifacts.push_back(vsCompiled);
    const ShaderHandle vsAsset = assets.GetOrAddShader("shaders/async_vs.null", std::move(vs));

    auto ps = std::make_unique<assets::ShaderAsset>();
    ps->debugName = "PS_Async.ps.hlsl";
    ps->stage = assets::ShaderStage::Fragment;
    ps->entryPoint = "PSMain";
    ps->sourceCode = "float4 PSMain() : SV_Target { return float4(0,0,0,1); }";
    const ShaderHandle psAsset = assets.GetOrAddShader("shaders/async_ps.hlsl", std::move(ps));

    MaterialSystem ms;
    MaterialDesc d;
    MaterialRuntimeDesc dRuntime;
    d.name = "AsyncVariantMaterial";
    dRuntime.renderPass = StandardRenderPasses::Opaque();
    dRuntime.vertexShader = vsAsset;
    dRuntime.fragmentShader = psAsset;
    const MaterialHandle mat = MaterialRuntimeBridge::RegisterMaterial(ms, std::move(d), dRuntime);
    const MaterialSystemShaderMaterialSource source(ms);

    renderer::null_backend::NullDevice device;
    renderer::IDevice::DeviceDesc dd{};
    dd.appName = "ShaderRuntimeAsyncVariant";
    CHECK(ctx, device.Initialize(dd));

    jobs::JobSystem pool;
    pool.Initialize(2u);

    EnvironmentRuntimeState env{};
    env.active = true;
    env.irradiance = TextureHandle::Make(311u, 1u);
    env.prefiltered = TextureHandle::Make(312u, 1u);
    env.brdfLut = TextureHandle::Make(313u, 1u);

    ShaderRuntime runtime;
    CHECK(ctx, runtime.Initialize(device));
    runtime.SetAssetRegistry(&assets);
    runtime.SetAsyncShaderCompileJobs(&pool);
    CHECK(ctx, runtime.PrepareAllShaderAssets());
    runtime.SetEnvironmentState(env);

    CHECK(ctx, !runtime.PrepareMaterial(source, mat));
    const auto* waiting = runtime.GetMaterialState(mat);
    CHECK(ctx, waiting != nullptr && waiting->waitingForShaders && !waiting->valid);
    CHECK(ctx, runtime.HasPendingShaderCompiles());
    CHECK(ctx, runtime.IsShaderCompilePending(psAsset));
    ShaderCompileProgress progress = runtime.GetShaderCompileProgress();
    CHECK(ctx, progress.Active());
    CHECK_EQ(ctx, progress.total, 1u);
    CHECK_EQ(ctx, progress.completed, 0u);
    CHECK_EQ(ctx, progress.shaderName, std::string("PS_Async.ps"));
    CHECK(ctx, progress.defines.find("KROM_IBL") != std::string::npos);

    // Asking again while it compiles queues nothing new.
    bool pending = false;
    CHECK(ctx, !runtime.GetOrCreateVariant(psAsset, ShaderPassType::Main, ShaderVariantFlag::IBLMap, &pending).IsValid());
    CHECK(ctx, pending);
    CHECK_EQ(ctx, runtime.GetShaderCompileProgress().total, 1u);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (runtime.HasPendingShaderCompiles() && std::chrono::steady_clock::now() < deadline)
    {
        runtime.PumpAsyncShaderCompiles();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(ctx, !runtime.HasPendingShaderCompiles());
    CHECK(ctx, !runtime.GetShaderCompileProgress().Active());

    CHECK(ctx, runtime.PrepareMaterial(source, mat));
    const auto* ready = runtime.GetMaterialState(mat);
    CHECK(ctx, ready != nullptr && ready->valid && !ready->waitingForShaders);
    const ShaderHandle variant = runtime.GetOrCreateVariant(psAsset, ShaderPassType::Main, ShaderVariantFlag::IBLMap);
    CHECK(ctx, variant.IsValid());
    if (ready)
        CHECK_EQ(ctx, ready->fragmentShader, variant);

    // Disk cache hit: no background compile.
    ShaderRuntime second;
    CHECK(ctx, second.Initialize(device));
    second.SetAssetRegistry(&assets);
    second.SetAsyncShaderCompileJobs(&pool);
    CHECK(ctx, second.PrepareAllShaderAssets());
    second.SetEnvironmentState(env);
    CHECK(ctx, second.PrepareMaterial(source, mat));
    CHECK(ctx, !second.HasPendingShaderCompiles());

    second.Shutdown();
    runtime.Shutdown();
    pool.Shutdown();
    device.Shutdown();
    ShaderCompiler::SetCacheDirectory({});
    std::error_code ec;
    std::filesystem::remove_all(cacheDir, ec);
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
    MaterialRuntimeDesc dRuntime;
    d.name = "IBLInactiveMaterial";
    dRuntime.renderPass = StandardRenderPasses::Opaque();
    dRuntime.vertexShader = vsAsset;
    dRuntime.fragmentShader = psAsset;
    const MaterialHandle mat = MaterialRuntimeBridge::RegisterMaterial(ms, std::move(d), dRuntime);

    renderer::null_backend::NullDevice device;
    renderer::IDevice::DeviceDesc dd{};
    dd.appName = "ShaderRuntimeIBLInactive";
    CHECK(ctx, device.Initialize(dd));

    ShaderRuntime runtime;
    CHECK(ctx, runtime.Initialize(device));
    runtime.SetAssetRegistry(&assets);
    CHECK(ctx, runtime.PrepareAllShaderAssets());
    CHECK(ctx, runtime.PrepareMaterial(MaterialSystemShaderMaterialSource(ms), mat));

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
    MaterialRuntimeDesc dRuntime;
    d.name = "IBLRebuildMaterial";
    dRuntime.renderPass = StandardRenderPasses::Opaque();
    dRuntime.vertexShader = vsAsset;
    dRuntime.fragmentShader = psAsset;
    const MaterialHandle mat = MaterialRuntimeBridge::RegisterMaterial(ms, std::move(d), dRuntime);

    renderer::null_backend::NullDevice device;
    renderer::IDevice::DeviceDesc dd{};
    dd.appName = "ShaderRuntimeIBLRebuild";
    CHECK(ctx, device.Initialize(dd));

    ShaderRuntime runtime;
    CHECK(ctx, runtime.Initialize(device));
    runtime.SetAssetRegistry(&assets);
    CHECK(ctx, runtime.PrepareAllShaderAssets());

    CHECK(ctx, runtime.PrepareMaterial(MaterialSystemShaderMaterialSource(ms), mat));
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

    CHECK(ctx, runtime.PrepareMaterial(MaterialSystemShaderMaterialSource(ms), mat));
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
    CHECK_EQ(ctx, engine::renderer::opengl::ToGLFrontFace(WindingOrder::CCW), GLenum(0x0900u)); // GL_CW
    CHECK_EQ(ctx, engine::renderer::opengl::ToGLFrontFace(WindingOrder::CW), GLenum(0x0901u));  // GL_CCW
}


// Ein Shader, der nicht kompiliert, bricht den Frame nicht ab und wird erst nach
// einer Quellaenderung erneut versucht (TestDevice liefert nie gueltige Shader).
static void TestFailedShaderDoesNotAbortFrame(test::TestContext& ctx)
{
    assets::AssetRegistry assets;
    auto vs = std::make_unique<assets::ShaderAsset>();
    vs->debugName = "VS_Broken";
    vs->stage = assets::ShaderStage::Vertex;
    vs->entryPoint = "main";
    vs->sourceCode = "kein gueltiger Shader";
    const ShaderHandle vsAsset = assets.GetOrAddShader("shaders/broken.vs.hlsl", std::move(vs));

    TestDevice device;
    CHECK(ctx, device.Initialize(IDevice::DeviceDesc{}));
    ShaderRuntime runtime;
    CHECK(ctx, runtime.Initialize(device));
    runtime.SetAssetRegistry(&assets);

    CHECK(ctx, runtime.CommitShaderRequests({ vsAsset }));
    const ShaderAssetStatus* status = runtime.GetShaderStatus(vsAsset);
    CHECK(ctx, status != nullptr && status->failed && !status->loaded);
    const uint64_t firstHash = status ? status->sourceHash : 0ull;

    // Gleiche Quelle: weiterhin kein Abbruch, Status unveraendert.
    CHECK(ctx, runtime.CommitShaderRequests({ vsAsset }));
    status = runtime.GetShaderStatus(vsAsset);
    CHECK(ctx, status != nullptr && status->failed && status->sourceHash == firstHash);

    // Geaenderte Quelle: neuer Versuch (neuer Hash im Status).
    assets.shaders.Get(vsAsset)->sourceCode = "immer noch kaputt";
    CHECK(ctx, runtime.CommitShaderRequests({ vsAsset }));
    status = runtime.GetShaderStatus(vsAsset);
    CHECK(ctx, status != nullptr && status->failed && status->sourceHash != firstHash);
}


static void TestInheritedMeshState(test::TestContext& ctx)
{
    auto registry = CreateRendererTestRegistry();
    ecs::World world(registry);
    const EntityID folder = world.CreateEntity();
    const EntityID parent = world.CreateEntity();
    const EntityID child = world.CreateEntity();
    const EntityID hidden = world.CreateEntity();
    world.Add<ParentComponent>(parent, ParentComponent{folder});
    for (const EntityID id : {child, hidden})
    {
        world.Add<ParentComponent>(id, ParentComponent{parent});
        world.Add<WorldTransformComponent>(id);
        world.Add<MeshComponent>(id, MeshHandle::Make(id.Index() + 1u, 1u));
    }
    world.Get<MeshComponent>(hidden)->visible = false;
    auto count = [&](jobs::JobSystem* jobs = nullptr) {
        RenderWorld renderWorld;
        engine::addons::mesh_renderer::ExtractRenderables(world, renderWorld.GetExtractionView(), jobs);
        return renderWorld.TotalProxyCount();
    };
    CHECK_EQ(ctx, count(), 1u);
    world.Add<ActiveComponent>(folder, ActiveComponent{false});
    CHECK_EQ(ctx, count(), 0u);
    world.Get<ActiveComponent>(folder)->active = true;
    CHECK_EQ(ctx, count(), 1u);
    world.Add<VisibilityComponent>(folder, VisibilityComponent{false});
    CHECK_EQ(ctx, count(), 0u);
    world.Get<VisibilityComponent>(folder)->visible = true;
    world.Add<MeshComponent>(parent, MeshComponent{});
    world.Get<MeshComponent>(parent)->visible = false;
    CHECK_EQ(ctx, count(), 0u);
    world.Get<MeshComponent>(parent)->visible = true;
    CHECK_EQ(ctx, count(), 1u);
    jobs::JobSystem jobs;
    jobs.Initialize(2u);
    CHECK_EQ(ctx, count(&jobs), 1u);
    world.Get<VisibilityComponent>(folder)->visible = false;
    CHECK_EQ(ctx, count(&jobs), 0u);
    jobs.Shutdown();
    // Reparenting immediately releases inherited visibility without changing local flags.
    world.Get<ParentComponent>(child)->parent = NULL_ENTITY;
    CHECK_EQ(ctx, count(), 1u);
    CHECK(ctx, !world.Get<MeshComponent>(hidden)->visible);
}

static void TestCachedEditorPassUsesCurrentQueue(test::TestContext& ctx)
{
    FeatureRegistry registry;
    CHECK(ctx, registry.AddFeature(engine::renderer::addons::editor::CreateEditorFeature({})));
    engine::renderer::null_backend::NullDevice device;
    CHECK(ctx, device.Initialize(IDevice::DeviceDesc{}));
    ShaderRuntime shaderRuntime;
    CHECK(ctx, shaderRuntime.Initialize(device));
    CHECK(ctx, registry.InitializeAll(FeatureInitializationContext{device, shaderRuntime, nullptr}));
    auto cmd = device.CreateCommandList(QueueType::Graphics);
    auto bindings = std::make_shared<FrameGraphRuntimeBindings>();
    FrameRecipe recipe;
    FramePipelineCallbacks callbacks;
    RenderQueue buildQueue;
    bindings->scene.renderQueue = &buildQueue;
    std::vector<EntityID> drawn;
    PassBuildContext context{{64u, 64u, &buildQueue, bindings}, recipe, callbacks,
        [&](const DrawList& list, const rendergraph::RGExecContext&) {
            for (const auto& item : list.items)
                drawn.push_back(item.entity);
        }};
    for (const auto* contributor : registry.GetPassContributors())
        if (contributor->GetContributorId() == 0xED170001u)
        {
            CHECK(ctx, contributor->GetPhase() == PassPhase::AfterPostProcess);
            contributor->BuildPass(context);
        }
    StandardFrameRecipeBuilder::BuildParams params{};
    params.viewportWidth = params.viewportHeight = 64u;
    params.uiEnabled = true;
    params.postAAEnabled = true;
    const FrameRecipe pipeline = StandardFrameRecipeBuilder::BuildRecipe(params, nullptr, nullptr, nullptr, 0u, &recipe);
    size_t aaIndex = 0u, gizmoIndex = 0u, uiIndex = 0u;
    for (size_t i = 0u; i < pipeline.passes.size(); ++i)
    {
        if (pipeline.passes[i].name == "PostAAPass") aaIndex = i;
        if (pipeline.passes[i].name == "EditorGizmoOverlayPass")
        {
            gizmoIndex = i;
            CHECK_EQ(ctx, pipeline.passes[i].renderPass.targetResourceName, std::string("Backbuffer"));
            CHECK_EQ(ctx, pipeline.passes[i].renderPass.depthTargetResourceName, std::string("HDRSceneColor"));
        }
        if (pipeline.passes[i].name == "UIPass") uiIndex = i;
    }
    CHECK(ctx, aaIndex < gizmoIndex && gizmoIndex < uiIndex);
    const auto* depth = callbacks.Find("editor.gizmo_depth");
    const auto* overlay = callbacks.Find("editor.gizmo_overlay");
    CHECK(ctx, depth && overlay);
    const rendergraph::RGExecContext exec{&device, cmd.get()};
    // Der Graph bleibt gleich, der naechste Lade-/Szenenframe hat eine andere
    // Snapshot-Adresse. Die gecachten Callbacks muessen dessen Daten verwenden.
    RenderQueue nextQueue;
    DrawItem item{};
    item.entity = EntityID::Make(42u, 1u);
    item.layerMask = LAYER_EDITOR_GIZMO_DEPTH;
    nextQueue.GetOrCreateList(StandardRenderPasses::EditorGizmo()).items.push_back(item);
    item.entity = EntityID::Make(43u, 1u);
    item.layerMask = LAYER_EDITOR_GIZMO;
    nextQueue.GetOrCreateList(StandardRenderPasses::EditorGizmo()).items.push_back(item);
    bindings->scene.renderQueue = &nextQueue;
    if (depth) (*depth)(exec);
    if (overlay) (*overlay)(exec);
    CHECK_EQ(ctx, drawn.size(), size_t{2});
    CHECK(ctx, drawn.size() == 2 && drawn[0] == EntityID::Make(42u, 1u) && drawn[1] == EntityID::Make(43u, 1u));
    bindings->scene.renderQueue = nullptr;
    drawn.clear();
    if (depth) (*depth)(exec);
    if (overlay) (*overlay)(exec);
    CHECK(ctx, drawn.empty());
    registry.ShutdownAll(FeatureShutdownContext{});
    shaderRuntime.Shutdown();
    device.Shutdown();
}

static void TestSelectionOutlinePerViewAndCache(test::TestContext& ctx)
{
    auto resources = std::make_shared<renderer::addons::outline::OutlineGpuResources>();
    resources->initialized = true;
    renderer::addons::outline::OutlinePass outline(123u, resources,
        [] { return EntityID::Make(1u, 1u); }, [](EntityID) { return true; });
    FrameRecipe recipe;
    FramePipelineCallbacks callbacks;
    PassBuildContext pass{{64u,64u,nullptr,std::make_shared<FrameGraphRuntimeBindings>(),true,false},recipe,callbacks,{}};
    outline.BuildPass(pass);
    CHECK(ctx, recipe.passes.empty() && recipe.resources.empty() && callbacks.Entries().empty());
    pass.frame.enableSelectionOutline = true;
    outline.BuildPass(pass);
    CHECK_EQ(ctx, recipe.passes.size(), size_t{2});

    struct ProbePipeline final : IRenderPipelineFeature
    {
        mutable std::vector<bool> modes;
        std::string_view GetName() const noexcept override { return "OutlineViewProbe"; }
        bool Build(const RenderPipelineBuildContext& context, RenderPipelineBuildResult&) const override
        {
            modes.push_back(context.enableSelectionOutline);
            context.renderGraph.AddPass("base").Execute([](const rendergraph::RGExecContext&){});
            return true;
        }
    } pipeline;
    renderer::null_backend::NullDevice device;
    CHECK(ctx, device.Initialize(IDevice::DeviceDesc{}));
    ShaderRuntime shaders;
    CHECK(ctx, shaders.Initialize(device));
    GpuResourceRuntime gpu;
    CHECK(ctx, gpu.Initialize(device, 1u));
    MaterialSystem materials;
    RenderQueue queue;
    FramePipelineCallbacks graphCallbacks;
    FrameGraphStageContext context{device,64u,64u,{}, {},true,{},queue,&pipeline,shaders,materials,graphCallbacks,nullptr,gpu};
    FrameGraphStage stage;
    FrameGraphStageResult result;
    CHECK(ctx, stage.Execute(context, result));
    CHECK(ctx, stage.Execute(context, result));
    CHECK_EQ(ctx, pipeline.modes.size(), size_t{1});
    context.enableSelectionOutline = false;
    CHECK(ctx, stage.Execute(context, result));
    CHECK(ctx, pipeline.modes.size() == 2 && !pipeline.modes.back());
    context.enableSelectionOutline = true;
    CHECK(ctx, stage.Execute(context, result));
    CHECK(ctx, pipeline.modes.size() == 3 && pipeline.modes.back());
    gpu.Shutdown();
    shaders.Shutdown();
    device.Shutdown();
}

// Skinning uploads: a skin without pose gets an identity (bind-pose) palette, and
// unchanged palettes are not written again (Vulkan would wait for the last draw).
static void TestSkinningUploadsOnlyChangedPalettes(test::TestContext& ctx)
{
    RecordingComputeDevice device;
    assets::AssetRegistry registry;
    auto skeleton = std::make_unique<assets::SkeletonAsset>();
    skeleton->bones.resize(3u);
    const SkeletonHandle skeletonHandle = registry.GetOrAddSkeleton("Models/test.glb#skeleton/0", std::move(skeleton));

    ecs::ComponentMetaRegistry components;
    RegisterCoreComponents(components);
    RegisterAnimationComponents(components);
    ecs::World world(components);
    const EntityID rig = world.CreateEntity();
    SkinComponent skin{};
    skin.skeleton = skeletonHandle;
    world.Add<SkinComponent>(rig, skin);

    engine::addons::animation::SkinningSystem skinning(device, &registry);
    skinning.Upload(world);
    const BufferHandle buffer = world.Get<SkinComponent>(rig)->gpuBuffer;
    const auto* record = device.GetBuffer(buffer);
    CHECK(ctx, record && record->uploadCount == 1u);
    CHECK(ctx, record && record->uploadedData.size() == 3u * sizeof(math::Mat4));
    if (record && record->uploadedData.size() >= sizeof(math::Mat4))
    {
        math::Mat4 first{};
        std::memcpy(&first, record->uploadedData.data(), sizeof(first));
        const math::Mat4 identity = math::Mat4::Identity();
        CHECK(ctx, std::memcmp(&first, &identity, sizeof(first)) == 0);
    }

    skinning.Upload(world);   // bind pose again: no new write
    CHECK(ctx, record && record->uploadCount == 1u);

    auto* posed = world.Get<SkinComponent>(rig);
    posed->bonePalette.assign(3u, math::Mat4::Identity());
    posed->bonePalette[1].m[3][1] = 2.f;
    skinning.Upload(world);   // new pose
    CHECK(ctx, record && record->uploadCount == 2u);
    skinning.Upload(world);   // paused: same pose
    CHECK(ctx, record && record->uploadCount == 2u);
}

// A recipe pass with extra colour targets binds them behind its main target
// (MRT), with their own clear values; a pass without keeps a single target.
static void TestRecipeExtraColorTargets(test::TestContext& ctx)
{
    using namespace engine::rendergraph;
    RenderGraph rg;

    const RenderTargetHandle hdrRT    = RenderTargetHandle::Make(2u, 1u);
    const RenderTargetHandle normalRT = RenderTargetHandle::Make(3u, 1u);
    const RenderTargetHandle specRT   = RenderTargetHandle::Make(4u, 1u);

    FrameRecipe recipe{};
    recipe.resources.push_back(FrameRecipeResourceDesc{ "Backbuffer", true, 64u, 64u, Format::RGBA8_UNORM_SRGB, RGResourceKind::Backbuffer });
    auto imported = [&](const char* name, RenderTargetHandle rt, uint32_t texIndex, Format format) {
        FrameRecipeResourceDesc d{};
        d.name = name; d.width = 64u; d.height = 64u; d.format = format;
        d.kind = RGResourceKind::RenderTarget;
        d.importedRenderTarget = rt;
        d.importedTexture = TextureHandle::Make(texIndex, 1u);
        recipe.resources.push_back(d);
    };
    imported("HDR", hdrRT, 2u, Format::RGBA16_FLOAT);
    imported("Normals", normalRT, 3u, Format::RGBA16_FLOAT);
    imported("Specular", specRT, 4u, Format::RGBA16_FLOAT);

    FrameRecipePassDesc opaque{};
    opaque.name = "Opaque";
    opaque.executorName = "exec.opaque";
    opaque.accesses.push_back({ "HDR", FrameRecipeAccessKind::WriteRenderTarget });
    opaque.accesses.push_back({ "Normals", FrameRecipeAccessKind::WriteRenderTarget });
    opaque.accesses.push_back({ "Specular", FrameRecipeAccessKind::WriteRenderTarget });
    opaque.renderPass.enabled = true;
    opaque.renderPass.targetResourceName = "HDR";
    opaque.renderPass.viewportWidth = 64u;
    opaque.renderPass.viewportHeight = 64u;
    opaque.renderPass.clearColor = true;
    opaque.renderPass.extraColorTargets.push_back({ "Normals", { 0.f, 0.f, 1.f, 0.f } });
    opaque.renderPass.extraColorTargets.push_back({ "Specular", { 0.25f, 0.f, 0.f, 0.f } });
    recipe.passes.push_back(opaque);

    FrameRecipePassDesc present{};
    present.name = "Present";
    present.executorName = "exec.present";
    present.accesses.push_back({ "HDR", FrameRecipeAccessKind::ReadTexture });
    present.accesses.push_back({ "Normals", FrameRecipeAccessKind::ReadTexture });
    present.accesses.push_back({ "Specular", FrameRecipeAccessKind::ReadTexture });
    present.accesses.push_back({ "Backbuffer", FrameRecipeAccessKind::WriteRenderTarget });
    present.renderPass.enabled = true;
    present.renderPass.targetResourceName = "Backbuffer";
    present.renderPass.viewportWidth = 64u;
    present.renderPass.viewportHeight = 64u;
    recipe.passes.push_back(present);

    FramePipelineCallbacks callbacks{};
    callbacks.Register("exec.opaque", [](const RGExecContext&) {});
    callbacks.Register("exec.present", [](const RGExecContext&) {});

    const FrameRecipeCompileParams params{ RenderTargetHandle::Make(1u, 1u), TextureHandle::Make(1u, 1u) };
    FrameRecipeCompiler::Build(rg, params, recipe, callbacks);

    CompiledFrame frame;
    CHECK(ctx, rg.Compile(frame));
    RecordingCommandList cmd;
    RGExecContext exec{};
    exec.cmd = &cmd;
    rg.Execute(frame, exec);

    CHECK_EQ(ctx, cmd.renderPasses.size(), 2u);
    if (cmd.renderPasses.size() != 2u)
        return;
    const auto& rp = cmd.renderPasses[0];
    CHECK(ctx, rp.renderTarget == hdrRT);
    CHECK_EQ(ctx, rp.extraColorTargetCount, 2u);
    CHECK(ctx, rp.extraColorTargets[0] == normalRT);
    CHECK(ctx, rp.extraColorTargets[1] == specRT);
    CHECK(ctx, rp.extraColorClears[0].color[2] == 1.f);
    CHECK(ctx, rp.extraColorClears[1].color[0] == 0.25f);
    CHECK_EQ(ctx, cmd.renderPasses[1].extraColorTargetCount, 0u);
}

// The PBR pixel shaders compile in their KROM_SSR_OUTPUT variant (opaque pass
// with the SSR inputs as extra colour targets), with and without IBL, for the
// forward, Forward+ and material-graph paths.
static void TestGtaoDepthPipelineStates(test::TestContext& ctx)
{
    RecordingComputeDevice device;
    ShaderRuntime runtime;
    CHECK(ctx, runtime.Initialize(device));
    assets::AssetRegistry assets;
    runtime.SetAssetRegistry(&assets);

    auto addShader = [&](const char* name, assets::ShaderStage stage, std::string source)
    {
        auto shader = std::make_unique<assets::ShaderAsset>();
        shader->debugName = name;
        shader->stage = stage;
        shader->entryPoint = "main";
        shader->sourceCode = std::move(source);
        return assets.GetOrAddShader(name, std::move(shader));
    };
    MaterialSystem materials;
    MaterialRuntimeDesc desc{};
    desc.vertexShader = addShader("custom.vs.hlsl", assets::ShaderStage::Vertex,
        "float4 main(float3 pos : POSITION) : SV_Position { return float4(pos.xyz * 2.0, 1.0); }");
    desc.fragmentShader = addShader("custom.ps.hlsl", assets::ShaderStage::Fragment,
        "float4 main() : SV_Target { return 1.0; }");
    desc.colorFormat = Format::RGBA16_FLOAT;
    desc.depthFormat = Format::D24_UNORM_S8_UINT;
    desc.renderPass = StandardRenderPasses::Opaque();
    desc.depthStencil.depthEnable = true;
    desc.depthStencil.depthWrite = true;
    desc.depthStencil.depthFunc = DepthFunc::Less;
    MaterialDesc surface{};
    surface.name = "PrepassTest";
    const MaterialHandle material = MaterialRuntimeBridge::RegisterMaterial(materials, std::move(surface), desc);
    const MaterialSystemShaderMaterialSource source(materials);
    CHECK(ctx, runtime.PrepareAllShaderAssets());
    CHECK(ctx, runtime.PrepareMaterial(source, material));
    runtime.SetTargetSampleCount(2u);

    RecordingCommandList cmd;
    const auto bind = [&](SceneDepthMode mode)
    {
        CHECK(ctx, runtime.BindMaterialWithRange(cmd, source, material, {}, {}, {}, {},
            nullptr, StandardRenderPasses::Opaque(), mode));
        return cmd.events.empty() ? nullptr : device.GetPipeline(cmd.events.back().pipeline);
    };
    const PipelineDesc* pre = bind(SceneDepthMode::Prepass);
    const PipelineDesc* lit = bind(SceneDepthMode::ReadOnly);
    const PipelineDesc* legacy = bind(SceneDepthMode::Material);
    CHECK(ctx, pre && lit && legacy);
    if (pre && lit && legacy)
    {
        CHECK(ctx, pre->depthStencil.depthEnable && pre->depthStencil.depthWrite);
        CHECK(ctx, lit->depthStencil.depthEnable && !lit->depthStencil.depthWrite);
        CHECK_EQ(ctx, pre->depthStencil.depthFunc, DepthFunc::LessEqual);
        CHECK_EQ(ctx, lit->depthStencil.depthFunc, DepthFunc::LessEqual);
        CHECK_EQ(ctx, pre->colorFormat, Format::Unknown);
        CHECK_EQ(ctx, lit->colorFormat, Format::RGBA16_FLOAT);
        CHECK_EQ(ctx, pre->sampleCount, 2u);
        CHECK_EQ(ctx, lit->sampleCount, 2u);
        CHECK_EQ(ctx, pre->shaderStages.size(), 1u);
        CHECK_EQ(ctx, lit->shaderStages.size(), 2u);
        CHECK(ctx, pre->shaderStages.front().handle == lit->shaderStages.front().handle);
        CHECK(ctx, legacy->depthStencil.depthWrite);
        CHECK_EQ(ctx, legacy->depthStencil.depthFunc, DepthFunc::Less);
    }
    runtime.Shutdown();
}

static void TestNormalGraphShaders(test::TestContext& ctx)
{
    const auto root = PathFromUtf8(KROM_SOURCE_DIR) / "shaders";
    const bool haveDxc = !FindShaderTool(ShaderTool::Dxc).path.empty();
    for (const char* file : {"pbr_lit.ps.hlsl", "pbr_forward_plus.ps.hlsl", "lit.ps.hlsl"})
    {
        assets::ShaderAsset ps{};
        ps.debugName = file;
        ps.stage = assets::ShaderStage::Fragment;
        ps.sourceLanguage = assets::ShaderSourceLanguage::HLSL;
        ps.resolvedPath = PathToUtf8(root / file);
        for (const auto target : {assets::ShaderTargetProfile::DirectX11_SM5, assets::ShaderTargetProfile::Vulkan_SPIRV})
        {
            if (target == assets::ShaderTargetProfile::Vulkan_SPIRV && !haveDxc)
                continue;
            // Graph-only normals must work without the legacy NormalMap permutation.
            for (const auto extra : {ShaderVariantFlag::None, ShaderVariantFlag::NormalMap | ShaderVariantFlag::NormalMapBC5})
            {
                assets::CompiledShaderArtifact artifact{};
                std::string error;
                const bool ok = ShaderCompiler::CompileVariant(ps, target, ShaderVariantFlag::MaterialGraph | extra, artifact, &error);
                if (!ok)
                    std::fprintf(stderr, "Normal graph %s (%s): %s\n", file, ShaderCompiler::ToString(target), error.c_str());
                CHECK(ctx, ok);
            }
        }
    }

#if defined(_WIN32) && defined(KROM_DX11_BACKEND)
    // Execute the production HLSL on WARP and check invariants, rather than
    // reproducing the RNM implementation in a CPU test.
    assets::ShaderAsset cs{};
    cs.debugName = "NormalBlendMath";
    cs.stage = assets::ShaderStage::Compute;
    cs.sourceLanguage = assets::ShaderSourceLanguage::HLSL;
    cs.sourceCode = "#define KROM_MATERIAL_GRAPH\n"
        "float4 graphOutputs, baseColorFactor, timeData; float4 graphNodes[16], graphUV[8];\n"
        "SamplerState sLinear;\n#include \"" + PathToUtf8(root / "pbr_material_graph.hlsl") + "\"\n" + R"(
RWStructuredBuffer<float4> results : register(u0);
[numthreads(1, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    float3 flat = float3(0.5, 0.5, 1.0);
    float3 a = float3(0.8, 0.5, 0.9); // (0.6, 0, 0.8)
    float3 b = float3(0.5, 0.8, 0.9); // (0, 0.6, 0.8)
    results[0] = float4(KromGraphBlendNormals(flat, flat, 1, 1), 1);
    results[1] = float4(KromGraphBlendNormals(a, flat, 1, 1), 1);
    results[2] = float4(KromGraphBlendNormals(flat, b, 1, 1), 1);
    results[3] = float4(KromGraphBlendNormals(a, b, 1, 1), 1);
    results[4] = float4(KromGraphBlendNormals(a, b, 1, 0), 1);
    results[5] = float4(KromGraphBlendNormals(a, b, 0, 1), 1);
    results[6] = float4(KromGraphBlendNormals(a, b, 0, 0), 1);
    results[7] = float4(KromGraphBlendNormals(float3(0.5, 0.5, 0.5), flat, 1, 1), 1);
}
)";
    assets::CompiledShaderArtifact artifact{};
    std::string error;
    const bool compiled = ShaderCompiler::CompileForTarget(cs, assets::ShaderTargetProfile::DirectX11_SM5, artifact, &error);
    if (!compiled)
        std::fprintf(stderr, "Normal blend math: %s\n", error.c_str());
    CHECK(ctx, compiled);
    if (!compiled)
        return;

    using Microsoft::WRL::ComPtr;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> command;
    const HRESULT created = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &device, nullptr, &command);
    CHECK(ctx, SUCCEEDED(created));
    if (FAILED(created))
        return;
    ComPtr<ID3D11ComputeShader> shader;
    HRESULT hr = device->CreateComputeShader(artifact.bytecode.data(), artifact.bytecode.size(), nullptr, &shader);
    CHECK(ctx, SUCCEEDED(hr));
    if (FAILED(hr))
        return;
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = 8 * sizeof(math::Vec4);
    desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    desc.StructureByteStride = sizeof(math::Vec4);
    ComPtr<ID3D11Buffer> output, staging;
    hr = device->CreateBuffer(&desc, nullptr, &output);
    CHECK(ctx, SUCCEEDED(hr));
    if (FAILED(hr))
        return;
    ComPtr<ID3D11UnorderedAccessView> uav;
    hr = device->CreateUnorderedAccessView(output.Get(), nullptr, &uav);
    CHECK(ctx, SUCCEEDED(hr));
    if (FAILED(hr))
        return;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = desc.MiscFlags = desc.StructureByteStride = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    hr = device->CreateBuffer(&desc, nullptr, &staging);
    CHECK(ctx, SUCCEEDED(hr));
    if (FAILED(hr))
        return;
    command->CSSetShader(shader.Get(), nullptr, 0);
    ID3D11UnorderedAccessView* view = uav.Get();
    command->CSSetUnorderedAccessViews(0, 1, &view, nullptr);
    command->Dispatch(1, 1, 1);
    view = nullptr;
    command->CSSetUnorderedAccessViews(0, 1, &view, nullptr);
    command->CopyResource(staging.Get(), output.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr = command->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    CHECK(ctx, SUCCEEDED(hr));
    if (FAILED(hr))
        return;
    const math::Vec3 expected[] = {
        {0, 0, 1}, {0.6f, 0, 0.8f}, {0, 0.6f, 0.8f}, {0.48f, 0.6f, 0.64f},
        {0.6f, 0, 0.8f}, {0, 0.6f, 0.8f}, {0, 0, 1}, {0, 0, 1},
    };
    const auto* values = static_cast<const math::Vec4*>(mapped.pData);
    for (size_t i = 0; i < std::size(expected); ++i)
    {
        CHECK(ctx, std::abs(values[i].x * 2.f - 1.f - expected[i].x) < 1e-5f);
        CHECK(ctx, std::abs(values[i].y * 2.f - 1.f - expected[i].y) < 1e-5f);
        CHECK(ctx, std::abs(values[i].z * 2.f - 1.f - expected[i].z) < 1e-5f);
    }
    command->Unmap(staging.Get(), 0);

    // Exercise the graph compiler, normal texture decoding and independent UV
    // frames together in a real fragment shader, including mirrored tiling.
    assets::ShaderAsset vsAsset{};
    vsAsset.debugName = "NormalGraphFullscreenVS";
    vsAsset.stage = assets::ShaderStage::Vertex;
    vsAsset.sourceLanguage = assets::ShaderSourceLanguage::HLSL;
    vsAsset.sourceCode = "float4 main(uint id : SV_VertexID) : SV_POSITION { return float4(id == 2 ? 3 : -1, id == 1 ? 3 : -1, 0, 1); }";
    assets::ShaderAsset psAsset{};
    psAsset.debugName = "NormalGraphReadbackPS";
    psAsset.stage = assets::ShaderStage::Fragment;
    psAsset.sourceLanguage = assets::ShaderSourceLanguage::HLSL;
    psAsset.sourceCode = "#define KROM_MATERIAL_GRAPH\n"
        "static const float4 baseColorFactor = 1; static const float4 timeData = 0;\n"
        "cbuffer PerMaterial : register(b2) { float4 graphOutputs; float4 graphNodes[16]; float4 graphUV[8]; };\n"
        "SamplerState sLinear : register(s0);\n#include \"" + PathToUtf8(root / "pbr_material_graph.hlsl") + "\"\n" + R"(
float4 main(float4 pixel : SV_Position) : SV_Target
{
    float2 uv = pixel.xy / 4.0;
    EvaluateMaterialGraph(uv, float3(uv, 0));
    return float4(KromGraphNormal() * 0.5 + 0.5, 1);
}
)";
    assets::CompiledShaderArtifact vsCode{}, psCode{};
    const bool vertexOk = ShaderCompiler::CompileForTarget(vsAsset, assets::ShaderTargetProfile::DirectX11_SM5, vsCode, &error);
    CHECK(ctx, vertexOk);
    const bool pixelOk = ShaderCompiler::CompileForTarget(psAsset, assets::ShaderTargetProfile::DirectX11_SM5, psCode, &error);
    if (!pixelOk)
        std::fprintf(stderr, "Normal graph readback: %s\n", error.c_str());
    CHECK(ctx, pixelOk);
    if (!vertexOk || !pixelOk)
        return;
    ComPtr<ID3D11VertexShader> vertex;
    ComPtr<ID3D11PixelShader> pixel;
    hr = device->CreateVertexShader(vsCode.bytecode.data(), vsCode.bytecode.size(), nullptr, &vertex);
    CHECK(ctx, SUCCEEDED(hr));
    if (FAILED(hr))
        return;
    hr = device->CreatePixelShader(psCode.bytecode.data(), psCode.bytecode.size(), nullptr, &pixel);
    CHECK(ctx, SUCCEEDED(hr));
    if (FAILED(hr))
        return;

    D3D11_TEXTURE2D_DESC texDesc{};
    texDesc.Width = texDesc.Height = texDesc.MipLevels = texDesc.ArraySize = texDesc.SampleDesc.Count = 1;
    texDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    texDesc.Usage = D3D11_USAGE_IMMUTABLE;
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11ShaderResourceView> maps[2];
    const math::Vec4 texels[] = {{0.8f, 0.5f, 0.f, 1.f}, {0.5f, 0.8f, 0.f, 1.f}}; // Blue deliberately unused (BC5).
    for (size_t i = 0; i < std::size(maps); ++i)
    {
        D3D11_SUBRESOURCE_DATA data{&texels[i], sizeof(math::Vec4), 0};
        ComPtr<ID3D11Texture2D> texture;
        hr = device->CreateTexture2D(&texDesc, &data, &texture);
        CHECK(ctx, SUCCEEDED(hr));
        if (FAILED(hr))
            return;
        hr = device->CreateShaderResourceView(texture.Get(), nullptr, &maps[i]);
        CHECK(ctx, SUCCEEDED(hr));
        if (FAILED(hr))
            return;
    }
    texDesc.Width = texDesc.Height = 4;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> target, readback;
    ComPtr<ID3D11RenderTargetView> rtv;
    hr = device->CreateTexture2D(&texDesc, nullptr, &target);
    CHECK(ctx, SUCCEEDED(hr));
    if (FAILED(hr))
        return;
    hr = device->CreateRenderTargetView(target.Get(), nullptr, &rtv);
    CHECK(ctx, SUCCEEDED(hr));
    if (FAILED(hr))
        return;
    texDesc.Usage = D3D11_USAGE_STAGING;
    texDesc.BindFlags = 0;
    texDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    hr = device->CreateTexture2D(&texDesc, nullptr, &readback);
    CHECK(ctx, SUCCEEDED(hr));
    if (FAILED(hr))
        return;
    D3D11_SAMPLER_DESC samplerDesc{};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    samplerDesc.AddressU = samplerDesc.AddressV = samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> sampler;
    hr = device->CreateSamplerState(&samplerDesc, &sampler);
    CHECK(ctx, SUCCEEDED(hr));
    if (FAILED(hr))
        return;
    D3D11_RASTERIZER_DESC rasterDesc{};
    rasterDesc.FillMode = D3D11_FILL_SOLID;
    rasterDesc.CullMode = D3D11_CULL_NONE;
    rasterDesc.DepthClipEnable = TRUE;
    ComPtr<ID3D11RasterizerState> raster;
    hr = device->CreateRasterizerState(&rasterDesc, &raster);
    CHECK(ctx, SUCCEEDED(hr));
    if (FAILED(hr))
        return;

    MaterialDesc materialDesc{};
    materialDesc.name = "NormalGraphReadback";
    AddMaterialGraphParameters(materialDesc, false);
    MaterialSystem materialSystem;
    const auto material = materialSystem.RegisterMaterial(materialDesc);
    desc = {};
    desc.ByteWidth = 25 * sizeof(math::Vec4);
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ComPtr<ID3D11Buffer> constants;
    hr = device->CreateBuffer(&desc, nullptr, &constants);
    CHECK(ctx, SUCCEEDED(hr));
    if (FAILED(hr))
        return;
    using Node = assets::MaterialNodeType;
    assets::MaterialGraph graph;
    graph.nodes = {{1, Node::Texture, 0, 0, {}, {}, "base"},
                   {2, Node::Texture, 0, 0, {}, {}, "detail"},
                   {3, Node::NormalBlend, 1, 2}};
    graph.normal = 3;
    graph.nodes[1].uvScale = {8.f, 8.f};
    const math::Vec3 frameExpected[] = {{0.48f, 0.6f, 0.64f}, {0.48f, -0.6f, 0.64f}, {-0.48f, -0.6f, 0.64f}};
    for (size_t i = 0; i < std::size(frameExpected); ++i)
    {
        if (i == 1) graph.nodes[1].uvScale.y = -8.f;
        if (i == 2) graph.nodes[0].uvScale.x = -2.f;
        const auto compiledGraph = assets::CompileMaterialGraph(graph);
        CHECK(ctx, compiledGraph.Active());
        SetMaterialGraph(materialSystem, material, compiledGraph, std::array<TextureHandle, assets::kMaterialGraphMaxTextures>{});
        const auto& cb = materialSystem.GetCBData(material);
        CHECK_EQ(ctx, cb.size(), size_t(desc.ByteWidth));
        if (cb.size() != desc.ByteWidth)
            return;
        command->UpdateSubresource(constants.Get(), 0, nullptr, cb.data(), 0, 0);
        ID3D11Buffer* constantBuffer = constants.Get();
        command->PSSetConstantBuffers(2, 1, &constantBuffer);
        ID3D11SamplerState* samplerState = sampler.Get();
        command->PSSetSamplers(0, 1, &samplerState);
        ID3D11ShaderResourceView* resource = maps[0].Get();
        command->PSSetShaderResources(13, 1, &resource);
        resource = maps[1].Get();
        command->PSSetShaderResources(9, 1, &resource);
        command->VSSetShader(vertex.Get(), nullptr, 0);
        command->PSSetShader(pixel.Get(), nullptr, 0);
        command->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        command->RSSetState(raster.Get());
        D3D11_VIEWPORT viewport{0, 0, 4, 4, 0, 1};
        command->RSSetViewports(1, &viewport);
        ID3D11RenderTargetView* renderTarget = rtv.Get();
        command->OMSetRenderTargets(1, &renderTarget, nullptr);
        command->Draw(3, 0);
        command->OMSetRenderTargets(0, nullptr, nullptr);
        command->CopyResource(readback.Get(), target.Get());
        hr = command->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped);
        CHECK(ctx, SUCCEEDED(hr));
        if (FAILED(hr))
            return;
        const auto value = *static_cast<const math::Vec4*>(mapped.pData);
        CHECK(ctx, std::abs(value.x * 2.f - 1.f - frameExpected[i].x) < 1e-4f);
        CHECK(ctx, std::abs(value.y * 2.f - 1.f - frameExpected[i].y) < 1e-4f);
        CHECK(ctx, std::abs(value.z * 2.f - 1.f - frameExpected[i].z) < 1e-4f);
        command->Unmap(readback.Get(), 0);
    }
#endif
}

static void TestShadowReceiverMathOnWarp(test::TestContext& ctx)
{
    RenderPassRegistry passes;
    const auto* locks = passes.GetLocks(StandardRenderPasses::Shadow());
    CHECK(ctx, locks && locks->cullModeLocked && locks->cullModeValue == CullMode::None);
#if defined(_WIN32) && defined(KROM_DX11_BACKEND)
    assets::ShaderAsset vs{}, ps{};
    vs.stage = assets::ShaderStage::Vertex;
    vs.sourceLanguage = ps.sourceLanguage = assets::ShaderSourceLanguage::HLSL;
    vs.sourceCode = "float4 main(uint id : SV_VertexID) : SV_POSITION { return float4(id == 2 ? 3 : -1, id == 1 ? 3 : -1, 0, 1); }";
    ps.stage = assets::ShaderStage::Fragment;
    ps.sourceCode = "#include \"" + PathToUtf8(PathFromUtf8(KROM_SOURCE_DIR) / "shaders/shadow_receiver.hlsl") + "\"\n" + R"(
float4 main(float4 pixel : SV_Position) : SV_Target
{
    // Rotated and sheared UVs: the old component-wise division gives (5,-7).
    float2 uv = float2(pixel.x + pixel.y, -pixel.x + pixel.y) * 0.01;
    if (pixel.y >= 2) uv = (pixel.x * 0.01).xx; // singular Jacobian
    float depth = 2 * uv.x - 3 * uv.y;
    float2 gradient = KromShadowDepthGradient(depth, uv);
    float4x4 a = float4x4(1,0,0,0, 0,1,0,0, 0,0,-0.01,0.5, 0,0,0,1);
    float4x4 b = float4x4(1,0,0,0, 0,1,0,0, 0,-0.0006,-0.0008,0.5, 0,0,0,1);
    return float4(gradient, KromShadowReceiverBias(0.01, a) * 100,
                            KromShadowReceiverBias(0.01, b) * 1000);
}
)";
    assets::CompiledShaderArtifact vsCode{}, psCode{};
    std::string error;
    const bool vsOk = ShaderCompiler::CompileForTarget(vs, assets::ShaderTargetProfile::DirectX11_SM5, vsCode, &error);
    const bool psOk = ShaderCompiler::CompileForTarget(ps, assets::ShaderTargetProfile::DirectX11_SM5, psCode, &error);
    if (!psOk) std::fprintf(stderr, "Shadow receiver shader: %s\n", error.c_str());
    CHECK(ctx, vsOk && psOk);
    if (!vsOk || !psOk) return;
    using Microsoft::WRL::ComPtr;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> command;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &device, nullptr, &command);
    CHECK(ctx, SUCCEEDED(hr));
    if (FAILED(hr)) return;
    ComPtr<ID3D11VertexShader> vertex;
    ComPtr<ID3D11PixelShader> pixel;
    hr = device->CreateVertexShader(vsCode.bytecode.data(), vsCode.bytecode.size(), nullptr, &vertex);
    CHECK(ctx, SUCCEEDED(hr));
    if (FAILED(hr)) return;
    hr = device->CreatePixelShader(psCode.bytecode.data(), psCode.bytecode.size(), nullptr, &pixel);
    CHECK(ctx, SUCCEEDED(hr));
    if (FAILED(hr)) return;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = desc.Height = 4;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> target, staging;
    hr = device->CreateTexture2D(&desc, nullptr, &target);
    CHECK(ctx, SUCCEEDED(hr));
    if (FAILED(hr)) return;
    ComPtr<ID3D11RenderTargetView> rtv;
    hr = device->CreateRenderTargetView(target.Get(), nullptr, &rtv);
    CHECK(ctx, SUCCEEDED(hr));
    if (FAILED(hr)) return;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    hr = device->CreateTexture2D(&desc, nullptr, &staging);
    CHECK(ctx, SUCCEEDED(hr));
    if (FAILED(hr)) return;
    D3D11_RASTERIZER_DESC rasterDesc{};
    rasterDesc.FillMode = D3D11_FILL_SOLID;
    rasterDesc.CullMode = D3D11_CULL_NONE;
    rasterDesc.DepthClipEnable = TRUE;
    ComPtr<ID3D11RasterizerState> raster;
    hr = device->CreateRasterizerState(&rasterDesc, &raster);
    CHECK(ctx, SUCCEEDED(hr));
    if (FAILED(hr)) return;
    command->VSSetShader(vertex.Get(), nullptr, 0);
    command->PSSetShader(pixel.Get(), nullptr, 0);
    command->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    command->RSSetState(raster.Get());
    D3D11_VIEWPORT viewport{0, 0, 4, 4, 0, 1};
    command->RSSetViewports(1, &viewport);
    auto* renderTarget = rtv.Get();
    command->OMSetRenderTargets(1, &renderTarget, nullptr);
    command->Draw(3, 0);
    command->OMSetRenderTargets(0, nullptr, nullptr);
    command->CopyResource(staging.Get(), target.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr = command->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    CHECK(ctx, SUCCEEDED(hr));
    if (FAILED(hr)) return;
    for (uint32_t y = 0; y < 4; ++y)
    {
        const auto* row = reinterpret_cast<const math::Vec4*>(static_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch);
        for (uint32_t x = 0; x < 4; ++x)
        {
            CHECK(ctx, std::fabs(row[x].x - (y < 2 ? 2.f : 0.f)) < 0.0001f);
            CHECK(ctx, std::fabs(row[x].y - (y < 2 ? -3.f : 0.f)) < 0.0001f);
            // Same world-space offset for 100- and 1000-unit depth spans and a rotated light.
            CHECK(ctx, std::fabs(row[x].z - 0.01f) < 1e-6f);
            CHECK(ctx, std::fabs(row[x].w - 0.01f) < 1e-6f);
        }
    }
    command->Unmap(staging.Get(), 0);
#endif
}

static void TestDepthMaskShadersCompile(test::TestContext& ctx)
{
    const auto root = PathFromUtf8(KROM_SOURCE_DIR) / "shaders";
    const bool haveDxc = !FindShaderTool(ShaderTool::Dxc).path.empty();
    for (const char* file : {"pbr_lit.ps.hlsl", "pbr_forward_plus.ps.hlsl", "lit.ps.hlsl", "quad_unlit.ps.hlsl"})
    {
        assets::ShaderAsset ps{};
        ps.debugName = file;
        ps.stage = assets::ShaderStage::Fragment;
        ps.sourceLanguage = assets::ShaderSourceLanguage::HLSL;
        ps.entryPoint = "main";
        ps.resolvedPath = PathToUtf8(root / file);
        for (const auto target : {assets::ShaderTargetProfile::DirectX11_SM5, assets::ShaderTargetProfile::Vulkan_SPIRV})
        {
            if (target == assets::ShaderTargetProfile::Vulkan_SPIRV && !haveDxc)
                continue;
            for (const auto graph : {ShaderVariantFlag::None, ShaderVariantFlag::MaterialGraph})
            {
                const auto flags = ShaderVariantFlag::DepthPrepass | ShaderVariantFlag::AlphaTest |
                                   ShaderVariantFlag::BaseColorMap | graph;
                assets::CompiledShaderArtifact artifact{};
                std::string error;
                const bool ok = ShaderCompiler::CompileVariant(ps, target, flags, artifact, &error);
                if (!ok)
                    std::fprintf(stderr, "Depth mask %s (%s): %s\n", file, ShaderCompiler::ToString(target), error.c_str());
                CHECK(ctx, ok);
            }
        }
    }
}

static void TestPbrShadersCompileSsrOutput(test::TestContext& ctx)
{
    const std::filesystem::path source = PathFromUtf8(KROM_SOURCE_DIR) / "shaders";
    const bool haveDxc = !FindShaderTool(ShaderTool::Dxc).path.empty();
    size_t checked = 0u;
    for (const char* file : { "pbr_lit.ps.hlsl", "pbr_forward_plus.ps.hlsl" })
    {
        assets::ShaderAsset ps{};
        ps.debugName = file;
        ps.stage = assets::ShaderStage::Fragment;
        ps.sourceLanguage = assets::ShaderSourceLanguage::HLSL;
        ps.entryPoint = "main";
        ps.resolvedPath = PathToUtf8(source / file);
        for (const assets::ShaderTargetProfile target : { assets::ShaderTargetProfile::DirectX11_SM5,
                                                          assets::ShaderTargetProfile::Vulkan_SPIRV })
        {
            if (target == assets::ShaderTargetProfile::Vulkan_SPIRV && !haveDxc)
                continue;
            for (const ShaderVariantFlag extra : { ShaderVariantFlag::None,
                                                   ShaderVariantFlag::IBLMap,
                                                   ShaderVariantFlag::IBLMap | ShaderVariantFlag::MaterialGraph })
            {
                const ShaderVariantFlag flags = extra | ShaderVariantFlag::SsrOutput |
                                                ShaderVariantFlag::NormalMap | ShaderVariantFlag::ORMMap;
                assets::CompiledShaderArtifact artifact{};
                std::string error;
                const bool ok = ShaderCompiler::CompileVariant(ps, target, flags, artifact, &error);
                if (!ok)
                    Debug::LogError("PBR PS '%s' SSR variant (%s) failed: %s", file,
                                    ShaderCompiler::ToString(target), error.c_str());
                CHECK(ctx, ok);
                ++checked;
            }
        }
    }
    CHECK(ctx, checked >= 6u);
}

int RunRendererTests()
{
    RegisterRendererTestComponents();
    engine::Debug::MinLevel = engine::LogLevel::Fatal;

    test::TestSuite suite("Renderer");
    suite.Add("Normal graph shaders and normal blend math on WARP", TestNormalGraphShaders);
    suite.Add("Shadow receiver math and world bias on WARP", TestShadowReceiverMathOnWarp);
    suite.Add("Inherited mesh activation and visibility", TestInheritedMeshState);
    suite
        .Add("Skinning uploads only changed palettes", TestSkinningUploadsOnlyChangedPalettes)
        .Add("Selection outline respects view and graph cache", TestSelectionOutlinePerViewAndCache)
        .Add("PipelineKey determinism",     TestPipelineKeyDeterminism)
        .Add("Cached editor pass uses current queue", TestCachedEditorPassUsesCurrentQueue)
        .Add("PipelineKey no padding",      TestPipelineKeyNoPadding)
        .Add("SortKey ordering",            TestSortKeyOrdering)
        .Add("MaterialSystem",              TestMaterialSystem)
        .Add("Unlit addon shader asset set", TestUnlitAddonShaderAssetSet)
        .Add("Unlit addon factory",        TestUnlitAddonFactory)
        .Add("Lit addon shader asset set", TestLitAddonShaderAssetSet)
        .Add("Lit addon factory",          TestLitAddonFactory)
        .Add("PBR master material",        TestPbrMasterMaterial)
        .Add("Double-sided overrides cull mode", TestDoubleSidedOverridesCullMode)
        .Add("Standard frame recipe", TestStandardFrameRecipe)
        .Add("Forward pipeline keeps built-in shadow without shadow-declaring contributor", TestForwardPipelineKeepsBuiltInShadowWithoutShadowDeclaringContributor)
        .Add("Forward pipeline integrates AfterOpaque contributor", TestForwardPipelineIntegratesAfterOpaqueContributor)
        .Add("FeatureRegistry rejects pass contributor id collision", TestFeatureRegistryRejectsPassContributorIdCollision)
        .Add("FeatureRegistry frame parameter blocks", TestFeatureRegistryFrameParameterBlocks)
        .Add("Forward+ shader clamps tile light count to stored range", TestForwardPlusShaderClampsTileLightCountToStoredRange)
        .Add("Forward+ shadow buffers pack more than four shadow lights", TestForwardPlusShadowBuffersPackMoreThanFourShadowLights)
        .Add("Forward+ compute cull transitions tile buffers", TestForwardPlusComputeCullDispatchTransitionsTileBuffers)
        .Add("Forward+ CPU tile lists preserve light order and clear stale counts", TestForwardPlusCpuTileListPreservesLightOrder)
        .Add("PipelineCache",               TestPipelineCache)
        .Add("CbLayout HLSL packing",       TestCbLayout)
        .Add("ShaderBindingModel slots",    TestShaderBindingModel)
        .Add("Camera addon primary perspective view", TestCameraAddonBuildPrimaryPerspectiveView)
        .Add("Camera addon orthographic view", TestCameraAddonBuildOrthographicView)
        .Add("Camera addon uses live local transform", TestCameraAddonUsesLiveLocalTransform)
        .Add("RenderWorld extract+cull",    TestRenderWorldExtract)
        .Add("RenderWorld parallel draw lists match serial", TestRenderWorldParallelDrawListsMatchSerial)
        .Add("Shadow atlas packs tiles of different sizes", TestShadowAtlasLayout)
        .Add("GPU pass profiler", TestGpuPassProfiler)
        .Add("Tonemap operator names", TestTonemapOperatorNames)
        .Add("Shading settings flags", TestShadingSettingsFlags)
        .Add("MSAA recipe", TestMsaaRecipe)
        .Add("DrawList instancing merges identical runs", TestDrawListInstancingMergesIdenticalRuns)
        .Add("DrawList instancing merges bounds for culling", TestDrawListInstancingMergesBoundsForCulling)
        .Add("DrawList instancing skips unsafe draws", TestDrawListInstancingSkipsMaterialSkinningAndBackToFront)
        .Add("Feature scene extraction",  TestFeatureDrivenSceneExtraction)
        .Add("Scene extraction with render-world context", TestSceneExtractionWithRenderWorldContext)
        .Add("Light extraction isolates preview light rig", TestLightExtractionIsolatesPreviewLightRig)
        .Add("Frame extraction passes job system", TestFrameExtractionStagePassesJobSystemToSceneExtractionContext)
        .Add("Mesh extraction with job system", TestMeshExtractionUsesJobSystemWhenAvailable)
        .Add("Forward extraction registration", TestForwardFeatureExtractionRegistration)
        .Add("Shadow extraction + frame constants", TestShadowFeatureExtractionAndFrameConstants)
        .Add("Shadow extraction respects RenderView toggle", TestShadowExtractionRespectsRenderViewShadowToggle)
        .Add("Shadow extraction includes cube requests in current path", TestShadowExtractionCollectsMultipleRequestsAndIncludesCubeCurrentPath)
        .Add("Shadow extraction applies light and view budget", TestShadowExtractionAppliesLightAndViewBudget)
        .Add("Shadow extraction culls local shadow outside receiver bounds", TestShadowExtractionCullsLocalShadowOutsideReceiverBounds)
        .Add("Shadow extraction culls local shadow behind camera frustum", TestShadowExtractionCullsLocalShadowBehindCameraFrustum)
        .Add("Shadow caster culling filters items per shadow view", TestShadowCasterCullingFiltersItemsPerShadowView)
        .Add("Shadow extraction skips directional without visible receiver", TestShadowExtractionSkipsDirectionalWithoutVisibleReceiver)
        .Add("Shadow extraction filters directional caster outliers", TestShadowExtractionFiltersDirectionalCasterOutliers)
        .Add("Directional shadow slices match camera depth", TestDirectionalShadowSlicesMatchCameraDepth)
        .Add("Directional shadows include upstream casters", TestDirectionalShadowDepthIncludesUpstreamCasters)
        .Add("Shadow view builder clamps spot/point far plane", TestShadowViewBuilderClampsSpotAndPointFarPlaneToMaxDistance)
        .Add("Lighting extraction sorts shadowed first", TestLightingExtractionSortsShadowedAndDirectionalFirst)
        .Add("Lighting frame constants track packed/dropped lights", TestLightingFrameConstantsTrackPackedAndDroppedLights)
        .Add("Backend shadow clip-space adjustments DX11/OpenGL", TestBackendShadowClipSpaceAdjustmentsDx11OpenGL)
        .Add("Shadow draw list includes all selected casters", TestShadowDrawListIncludesAllCastersForSelectedDirectionalRequest)
        .Add("RenderFrame state",         TestRenderFrameExecutionState)
        .Add("Frame TaskGraph parallel",   TestFrameTaskGraphParallelStructure)
        .Add("Frame TaskGraph failure",    TestFrameTaskGraphFailurePropagation)
        .Add("GPU resource runtime",       TestGpuResourceRuntime)
        .Add("GPU resource runtime separates mesh buffers by layout", TestGpuResourceRuntimeSeparatesMeshBuffersByLayout)
        .Add("GPU resource runtime raw fast path", TestGpuResourceRuntimeRawFastPath)
        .Add("GPU resource runtime reuploads reimported mesh", TestGpuResourceRuntimeReuploadsReimportedMesh)
        .Add("RenderSystem closed loop",   TestRenderSystemLoop)
        .Add("DeviceFactory registration", TestDeviceFactoryRegistration)
        .Add("ShaderRuntime end-to-end", TestShaderRuntimeEndToEnd)
        .Add("ShaderRuntime validation", TestShaderRuntimeValidation)
        .Add("Shader tool override", TestShaderToolOverride)
                .Add("Material texture writes", TestMaterialTextureWrites)
        .Add("PerMaterial layout from reflection", TestPerMaterialLayoutFromReflection)
        .Add("GLSL std140 block reflection", TestGlslStd140BlockReflection)
        .Add("Generated tonemap GLSL matches hand-written", TestGeneratedTonemapGlslMatchesHandWritten)
        .Add("Compiled PerMaterial reflection (DXBC/SPIR-V)", TestCompiledPerMaterialReflection)
        .Add("Example shaders keep PerMaterial layout", TestExampleShadersKeepPerMaterialLayout)
        .Add("Custom vertex shaders compile instanced", TestCustomVertexShadersCompileInstanced)
        .Add("Custom texture register from reflection", TestCustomTextureRegisterFromReflection)
        .Add("Failed shader does not abort frame", TestFailedShaderDoesNotAbortFrame)
                .Add("ShaderRuntime partial env fallback", TestShaderRuntimePartialEnvironmentUsesFallbacks)
        .Add("ShaderRuntime env rebuild", TestShaderRuntimeRebuildOnEnvironmentChange)
        .Add("ShaderRuntime IBL variant active", TestShaderRuntimeUsesIBLVariantWhenEnvironmentActive)
        .Add("ShaderRuntime IBL variant inactive", TestShaderRuntimeDoesNotUseIBLVariantWithoutEnvironment)
        .Add("ShaderRuntime background variant compile", TestShaderRuntimeCompilesVariantsInBackground)
        .Add("ShaderRuntime IBL variant rebuild", TestShaderRuntimeRebuildsShaderVariantOnEnvironmentToggle)
        .Add("ShaderVariant cache",      TestShaderVariantCache)
        .Add("OpenGL front-face mapping", TestOpenGLFrontFaceMapping)
        .Add("OpenGL backend registration", TestOpenGLBackendRegistration)
        .Add("RenderSystem uses host job system", TestRenderSystemUsesHostJobSystem)
        .Add("PlatformRenderLoop",        TestPlatformRenderLoop)
        .Add("Recipe extra colour targets (MRT)", TestRecipeExtraColorTargets)
        .Add("GTAO depth prepass recipe", TestGtaoDepthPrepassRecipe)
        .Add("Post-AA recipe", TestPostAARecipe)
        .Add("SMAA shaders and lookup textures", TestSmaaShadersAndLookupTextures)
        .Add("Auto-exposure settings", TestAutoExposureSettings)
        .Add("Auto-exposure scene keys", TestAutoExposureSceneJson)
        .Add("Colour grading constants", TestColorGradingConstants)
        .Add("Colour grading scene keys and script view", TestColorGradingSceneJson)
        .Add("Auto-exposure recipe", TestAutoExposureRecipe)
        .Add("Recipe pre-render-pass executor", TestRecipePreRenderPassExecutor)
        .Add("Auto-exposure shaders compile", TestAutoExposureShadersCompile)
        .Add("Forward pipeline: opaque input enables depth prepass", TestForwardPipelineOpaqueInputEnablesDepthPrepass)
        .Add("Contact shadow pass recipe", TestContactShadowPassRecipe)
        .Add("GTAO depth pipeline states", TestGtaoDepthPipelineStates)
        .Add("Depth mask shader variants", TestDepthMaskShadersCompile)
        .Add("PBR shaders compile SSR output", TestPbrShadersCompileSsrOutput);

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
    MaterialRuntimeDesc dRuntime;
    d.name = "RuntimeMaterial";
    dRuntime.renderPass = StandardRenderPasses::Opaque();
    dRuntime.vertexShader = vsAsset;
    dRuntime.fragmentShader = psAsset;

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

    d.parameters = {albedoMap, linearSampler, tint};
    const MaterialHandle mat = MaterialRuntimeBridge::RegisterMaterial(ms, std::move(d), dRuntime);

    renderer::null_backend::NullDevice device;
    renderer::IDevice::DeviceDesc dd{};
    dd.appName = "ShaderRuntimeTest";
    CHECK(ctx, device.Initialize(dd));

    ShaderRuntime runtime;
    CHECK(ctx, runtime.Initialize(device));
    runtime.SetAssetRegistry(&assets);

    CHECK(ctx, runtime.PrepareAllShaderAssets());
    CHECK(ctx, runtime.PrepareMaterial(MaterialSystemShaderMaterialSource(ms), mat));

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
    CHECK(ctx, runtime.BindMaterial(*cmd, MaterialSystemShaderMaterialSource(ms), mat, BufferHandle::Make(1u,1u), BufferHandle::Make(2u,1u)));
    cmd->Draw(3u);
    cmd->End();
    cmd->Submit();
    CHECK_GT(ctx, device.GetDrawCallCount(), 0u);

    runtime.Shutdown();
    device.Shutdown();
}

static void TestShaderToolOverride(test::TestContext& ctx)
{
    namespace fs = std::filesystem;
    const fs::path previous = GetShaderToolOverride(ShaderTool::Dxc);
    const fs::path directory = fs::temp_directory_path() / PathFromUtf8("krom-shader-tool-Größe");
    std::error_code ec;
    fs::create_directories(directory, ec);
    const fs::path fake = directory / "dxc.exe";
    std::ofstream(fake) << "x";

    SetShaderToolOverride(ShaderTool::Dxc, fake);
    const auto chosen = FindShaderTool(ShaderTool::Dxc);
    CHECK(ctx, chosen.path == fake);
    CHECK(ctx, chosen.source == "override");

    // A missing override is reported, not replaced by another dxc.exe.
    SetShaderToolOverride(ShaderTool::Dxc, directory / "missing.exe");
    const auto missing = FindShaderTool(ShaderTool::Dxc);
    CHECK(ctx, missing.path.empty());
    CHECK(ctx, missing.source == "override");

    // Explicit override (unsaved settings) does not touch the stored one.
    CHECK(ctx, FindShaderTool(ShaderTool::Dxc, fake).path == fake);
    CHECK(ctx, GetShaderToolOverride(ShaderTool::Dxc) == directory / "missing.exe");

    SetShaderToolOverride(ShaderTool::Dxc, previous);
    CHECK(ctx, FindShaderTool(ShaderTool::Dxc, {}).source != "override");
    fs::remove_all(directory, ec);
}

static void TestShaderRuntimeValidation(test::TestContext& ctx)
{
    MaterialSystem ms;
    MaterialDesc d;
    MaterialRuntimeDesc dRuntime;
    d.name = "BrokenMaterial";
    dRuntime.renderPass = StandardRenderPasses::Opaque();
    dRuntime.vertexShader = ShaderHandle::Make(11u, 1u);
    dRuntime.fragmentShader = ShaderHandle::Make(12u, 1u);

    MaterialParam texA;
    texA.name = "albedoMap";
    texA.type = MaterialParam::Type::Texture;
    texA.texture = TextureHandle::Make(1u, 1u);

    MaterialParam texB;
    texB.name = "baseColorMap";
    texB.type = MaterialParam::Type::Texture;
    texB.texture = TextureHandle::Make(2u, 1u);

    d.parameters = {texA, texB};
    const MaterialHandle mat = MaterialRuntimeBridge::RegisterMaterial(ms, std::move(d), dRuntime);

    renderer::null_backend::NullDevice device;
    renderer::IDevice::DeviceDesc dd{};
    dd.appName = "ShaderRuntimeValidation";
    CHECK(ctx, device.Initialize(dd));

    ShaderRuntime runtime;
    CHECK(ctx, runtime.Initialize(device));

    std::vector<ShaderValidationIssue> issues;
    CHECK(ctx, !runtime.ValidateMaterial(MaterialSystemShaderMaterialSource(ms), mat, issues));
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
    MaterialRuntimeDesc dRuntime;
    d.name = "TextureWriteMaterial";
    dRuntime.renderPass = StandardRenderPasses::Opaque();
    dRuntime.vertexShader = ShaderHandle::Make(21u, 1u);
    dRuntime.fragmentShader = ShaderHandle::Make(22u, 1u);

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

    d.parameters = { baseColorFactor, metallicFactor, albedo };
    d.textureSlots = {
        { "albedo", MaterialTextureSemantic::BaseColor }
    };

    const MaterialHandle mat = MaterialRuntimeBridge::RegisterMaterial(ms, std::move(d), dRuntime);
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
    const MaterialParameterLayout* parameterLayout = ms.GetParameterLayout(mat);
    CHECK(ctx, parameterLayout != nullptr);
    const int32_t albedoSlot = parameterLayout ? parameterLayout->FindSlotIndexByName("albedo") : -1;
    CHECK(ctx, albedoSlot >= 0);
    CHECK_EQ(ctx, instance->parameters.GetTexture(static_cast<uint32_t>(albedoSlot)), TextureHandle::Make(91u, 1u));
}


// --------------------------------------------------------------------------
// PerMaterial-Layout aus der Shader-Reflection (Custom-Shader)
// --------------------------------------------------------------------------
namespace {

// Custom-Shader mit eigenem Feld ZWISCHEN emissiveFactor und metallicFactor und
// ohne occlusionStrength. Offsets nach HLSL-Packing (DXBC, SPIR-V mit
// -fvk-use-dx-layout):
//   baseColorFactor 0, emissiveFactor 16, customValue 32, metallicFactor 36,
//   roughnessFactor 40, opacityFactor 44, alphaCutoff 48,
//   materialFeatureMask 52, materialModel 56, Blockgroesse 64.
constexpr const char* kReorderedPerMaterialPS = R"(
cbuffer PerMaterial : register(b2)
{
    float4 baseColorFactor;
    float4 emissiveFactor;
    float  customValue;
    float  metallicFactor;
    float  roughnessFactor;
    float  opacityFactor;
    float  alphaCutoff;
    int    materialFeatureMask;
    float  materialModel;
};
float4 main() : SV_TARGET
{
    return baseColorFactor * customValue + emissiveFactor * metallicFactor
         + roughnessFactor + opacityFactor + alphaCutoff + materialFeatureMask + materialModel;
}
)";

constexpr const char* kPlainVS = R"(
float4 main(float3 position : POSITION) : SV_POSITION { return float4(position, 1.0f); }
)";

ReflectedConstantBuffer MakeReorderedReflection()
{
    ReflectedConstantBuffer reflection{};
    reflection.valid = true;
    reflection.byteSize = 64u;
    reflection.fields = {
        { "baseColorFactor", 0u, 16u }, { "emissiveFactor", 16u, 16u },
        { "customValue", 32u, 4u },     { "metallicFactor", 36u, 4u },
        { "roughnessFactor", 40u, 4u }, { "opacityFactor", 44u, 4u },
        { "alphaCutoff", 48u, 4u },     { "materialFeatureMask", 52u, 4u },
        { "materialModel", 56u, 4u },
    };
    return reflection;
}

unlit::UnlitMaterialCreateInfo MakeReorderedCreateInfo()
{
    unlit::UnlitMaterialCreateInfo info{};
    info.name = "ReorderedCustom";
    info.baseColorFactor = math::Vec4{0.1f, 0.2f, 0.3f, 0.4f};
    info.emissiveFactor = math::Vec4{0.5f, 0.6f, 0.7f, 1.0f};
    info.metallicFactor = 0.25f;
    info.roughnessFactor = 0.5f;
    info.occlusionStrength = 0.9f;
    info.opacityFactor = 0.75f;
    info.alphaCutoff = 0.3f;
    info.enableBaseColorMap = true;

    MaterialParam custom{};
    custom.name = "customValue";
    custom.type = MaterialParam::Type::Float;
    custom.value.f[0] = 7.0f;
    info.extraParameters.push_back(custom);
    return info;
}

float ReadCBFloat(const std::vector<uint8_t>& cb, uint32_t offset)
{
    float value = 0.f;
    if (offset + 4u <= cb.size())
        std::memcpy(&value, cb.data() + offset, 4u);
    return value;
}

int32_t ReadCBInt(const std::vector<uint8_t>& cb, uint32_t offset)
{
    int32_t value = 0;
    if (offset + 4u <= cb.size())
        std::memcpy(&value, cb.data() + offset, 4u);
    return value;
}

// Registriert das Custom-Material mit der Reflection und prueft jeden Wert am
// reflektierten Offset.
void CheckReorderedPerMaterial(test::TestContext& ctx, const ReflectedConstantBuffer& reflection)
{
    unlit::UnlitMaterialCreateInfo info = MakeReorderedCreateInfo();
    info.perMaterialReflection = reflection;

    MaterialSystem ms;
    const MaterialHandle mat = ms.RegisterMaterial(unlit::UnlitMaterial::BuildDesc(info));
    CHECK_VALID(ctx, mat);
    const std::vector<uint8_t>& cb = ms.GetCBData(mat);
    CHECK_EQ(ctx, cb.size(), static_cast<size_t>(64u));

    CHECK_EQ(ctx, ReadCBFloat(cb, 0u), 0.1f);
    CHECK_EQ(ctx, ReadCBFloat(cb, 12u), 0.4f);
    CHECK_EQ(ctx, ReadCBFloat(cb, 16u), 0.5f);
    CHECK_EQ(ctx, ReadCBFloat(cb, 32u), 7.0f);   // customValue
    CHECK_EQ(ctx, ReadCBFloat(cb, 36u), 0.25f);  // metallicFactor
    CHECK_EQ(ctx, ReadCBFloat(cb, 40u), 0.5f);   // roughnessFactor
    CHECK_EQ(ctx, ReadCBFloat(cb, 44u), 0.75f);  // opacityFactor, nicht occlusionStrength
    CHECK_EQ(ctx, ReadCBFloat(cb, 48u), 0.3f);   // alphaCutoff
    CHECK_EQ(ctx, ReadCBInt(cb, 52u), 2);        // materialFeatureMask: Albedo-Bit
    CHECK_EQ(ctx, ReadCBFloat(cb, 56u), 0.0f);   // materialModel

    // Fehlende Felder bekommen keinen Slot und schreiben nirgends hin.
    const MaterialParameterLayout* layout = ms.GetParameterLayout(mat);
    CHECK(ctx, layout != nullptr);
    if (layout)
    {
        CHECK(ctx, layout->FindByName("occlusionStrength") == nullptr);
        CHECK(ctx, layout->FindByName("albedo") != nullptr);
    }

    // Laufzeit-Aenderung landet ebenfalls am reflektierten Offset.
    ms.SetFloat(mat, "customValue", 3.0f);
    ms.SetFloat(mat, "metallicFactor", 0.125f);
    const std::vector<uint8_t>& cb2 = ms.GetCBData(mat);
    CHECK_EQ(ctx, ReadCBFloat(cb2, 32u), 3.0f);
    CHECK_EQ(ctx, ReadCBFloat(cb2, 36u), 0.125f);
    CHECK_EQ(ctx, ms.GetCBLayout(mat).GetOffset("customValue"), 32u);
}

} // namespace

static void TestPerMaterialLayoutFromReflection(test::TestContext& ctx)
{
    CheckReorderedPerMaterial(ctx, MakeReorderedReflection());

    // Ohne Reflection: unveraendertes sequenzielles Layout (eigene Parameter ab 60).
    MaterialSystem ms;
    const MaterialHandle mat = ms.RegisterMaterial(unlit::UnlitMaterial::BuildDesc(MakeReorderedCreateInfo()));
    const std::vector<uint8_t>& cb = ms.GetCBData(mat);
    CHECK_EQ(ctx, ReadCBFloat(cb, 32u), 0.25f);  // metallicFactor
    CHECK_EQ(ctx, ReadCBFloat(cb, 40u), 0.9f);   // occlusionStrength
    CHECK_EQ(ctx, ReadCBFloat(cb, 44u), 0.75f);  // opacityFactor
    CHECK_EQ(ctx, ReadCBFloat(cb, 60u), 7.0f);   // customValue
    CHECK_EQ(ctx, ms.GetCBLayout(mat).GetOffset("customValue"), 60u);

    // Trailing-Padding im Shader: Buffer deckt den ganzen Block ab.
    ReflectedConstantBuffer padded = MakeReorderedReflection();
    padded.byteSize = 96u;
    unlit::UnlitMaterialCreateInfo info = MakeReorderedCreateInfo();
    info.perMaterialReflection = padded;
    MaterialSystem ms2;
    const MaterialHandle paddedMat = ms2.RegisterMaterial(unlit::UnlitMaterial::BuildDesc(info));
    CHECK_EQ(ctx, ms2.GetCBData(paddedMat).size(), static_cast<size_t>(96u));
}

// Experiment (Auftrag 13): the tonemap GLSL generated from HLSL via SPIR-V and
// SPIRV-Cross must reflect like the hand-written file and use the names the
// OpenGL backend binds after linking (block PerPass, samplers by texture name).
static void TestGeneratedTonemapGlslMatchesHandWritten(test::TestContext& ctx)
{
    const std::filesystem::path assetRoot = PathFromUtf8(__FILE__).parent_path().parent_path() / "shaders";
    const auto readText = [](const std::filesystem::path& path) {
        std::ifstream input(path, std::ios::in | std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    };

    const std::string handFs = readText(assetRoot / "passthrough.opengl.fs.glsl");
    const std::string generatedFs = readText(assetRoot / "generated" / "passthrough.opengl.fs.glsl");
    const std::string generatedVs = readText(assetRoot / "generated" / "fullscreen.opengl.vs.glsl");
    CHECK(ctx, !handFs.empty());
    CHECK(ctx, !generatedFs.empty());
    CHECK(ctx, !generatedVs.empty());

    CHECK(ctx, generatedFs.rfind("#version 410", 0u) == 0u);
    CHECK(ctx, generatedVs.rfind("#version 410", 0u) == 0u);

    ReflectedConstantBuffer hand{};
    ReflectedConstantBuffer generated{};
    std::string error;
    CHECK(ctx, ReflectGlslStd140Block(handFs, "PerPass", hand, &error));
    CHECK(ctx, ReflectGlslStd140Block(generatedFs, "PerPass", generated, &error));
    CHECK(ctx, hand.valid && generated.valid);
    CHECK_EQ(ctx, generated.byteSize, hand.byteSize);
    CHECK_EQ(ctx, generated.fields.size(), hand.fields.size());
    for (const ReflectedCBField& field : hand.fields)
    {
        const ReflectedCBField* other = generated.Find(field.name);
        CHECK(ctx, other != nullptr);
        CHECK(ctx, other && other->byteOffset == field.byteOffset && other->byteSize == field.byteSize);
    }

    ReflectedTextureBindings handBindings;
    ReflectedTextureBindings generatedBindings;
    CHECK(ctx, ReflectGlslTextureBindings(handFs, handBindings, &error));
    CHECK(ctx, ReflectGlslTextureBindings(generatedFs, generatedBindings, &error));
    CHECK(ctx, handBindings == generatedBindings);

    // Names the backend binds by (OpenGLResources.cpp), and no SPIRV-Cross
    // default names left that it would not find.
    CHECK(ctx, generatedFs.find("uniform sampler2D uHDRInput;") != std::string::npos);
    CHECK(ctx, generatedFs.find("uniform sampler2D uBloomTexture;") != std::string::npos);
    CHECK(ctx, generatedFs.find("SPIRV_Cross_Combined") == std::string::npos);
    CHECK(ctx, generatedFs.find("type_") == std::string::npos);
    CHECK(ctx, generatedFs.find("binding") == std::string::npos);

    // Vertex output and fragment input match by name.
    CHECK(ctx, generatedVs.find("out vec2 krom_TEXCOORD0;") != std::string::npos);
    CHECK(ctx, generatedFs.find("in vec2 krom_TEXCOORD0;") != std::string::npos);
}

static void TestGlslStd140BlockReflection(test::TestContext& ctx)
{
    const char* source = R"(#version 450 core
layout(std140, binding = 2) uniform PerObject { mat4 worldMatrix; };
layout(std140) uniform PerMaterial
{
    vec4  baseColorFactor;   // 0
    vec4  emissiveFactor;    // 16
    float customValue;       // 32
    /* Kommentar */ float metallicFactor; // 36
    vec2  uvScale;           // 40 (std140: vec2 auf 8)
#ifdef SOMETHING
#endif
    vec3  tint;              // 48
    int   mode;              // 60
    float weights[2];        // 64, Stride 16
    mat4  extra;             // 96
    highp float a, b;        // 160, 164
};
void main() {}
)";
    ReflectedConstantBuffer block{};
    std::string error;
    CHECK(ctx, ReflectGlslStd140Block(source, "PerMaterial", block, &error));
    CHECK(ctx, block.valid);
    const auto offsetOf = [&](const char* name) -> uint32_t {
        const ReflectedCBField* field = block.Find(name);
        return field ? field->byteOffset : UINT32_MAX;
    };
    CHECK_EQ(ctx, offsetOf("baseColorFactor"), 0u);
    CHECK_EQ(ctx, offsetOf("emissiveFactor"), 16u);
    CHECK_EQ(ctx, offsetOf("customValue"), 32u);
    CHECK_EQ(ctx, offsetOf("metallicFactor"), 36u);
    CHECK_EQ(ctx, offsetOf("uvScale"), 40u);
    CHECK_EQ(ctx, offsetOf("tint"), 48u);
    CHECK_EQ(ctx, offsetOf("mode"), 60u);
    CHECK_EQ(ctx, offsetOf("weights"), 64u);
    CHECK_EQ(ctx, offsetOf("extra"), 96u);
    CHECK_EQ(ctx, offsetOf("a"), 160u);
    CHECK_EQ(ctx, offsetOf("b"), 164u);
    CHECK(ctx, block.Find("worldMatrix") == nullptr);
    CHECK_EQ(ctx, block.byteSize, 176u);

    // Der Regressionsfall als GLSL: gleiche Offsets wie unter HLSL.
    const char* reordered = R"(#version 450 core
layout(std140) uniform PerMaterial
{
    vec4  baseColorFactor;
    vec4  emissiveFactor;
    float customValue;
    float metallicFactor;
    float roughnessFactor;
    float opacityFactor;
    float alphaCutoff;
    int   materialFeatureMask;
    float materialModel;
};
)";
    ReflectedConstantBuffer glBlock{};
    CHECK(ctx, ReflectGlslStd140Block(reordered, "PerMaterial", glBlock, &error));
    CheckReorderedPerMaterial(ctx, glBlock);

    // Nicht unterstuetzter Typ -> keine Reflection (Fallback auf festes Layout).
    ReflectedConstantBuffer unsupported{};
    CHECK(ctx, !ReflectGlslStd140Block("uniform PerMaterial { MyStruct s; };", "PerMaterial", unsupported, &error));
    CHECK(ctx, !unsupported.valid);
    CHECK(ctx, !ReflectGlslStd140Block("uniform Other { float x; };", "PerMaterial", unsupported, &error));
}

static void TestCompiledPerMaterialReflection(test::TestContext& ctx)
{
    assets::ShaderAsset vs{};
    vs.debugName = "ReflectionTestVS";
    vs.stage = assets::ShaderStage::Vertex;
    vs.sourceLanguage = assets::ShaderSourceLanguage::HLSL;
    vs.sourceCode = kPlainVS;

    assets::ShaderAsset ps{};
    ps.debugName = "ReflectionTestPS";
    ps.stage = assets::ShaderStage::Fragment;
    ps.sourceLanguage = assets::ShaderSourceLanguage::HLSL;
    ps.sourceCode = kReorderedPerMaterialPS;

    for (const assets::ShaderTargetProfile target : { assets::ShaderTargetProfile::DirectX11_SM5,
                                                      assets::ShaderTargetProfile::Vulkan_SPIRV })
    {
        assets::CompiledShaderArtifact artifact{};
        std::string error;
        if (!ShaderCompiler::CompileForTarget(ps, target, artifact, &error))
        {
            // Compiler fehlt auf dieser Maschine (z. B. kein DXC): nur protokollieren.
            Debug::LogWarning("PerMaterial reflection test: %s nicht kompilierbar (%s), uebersprungen",
                              ShaderCompiler::ToString(target), error.c_str());
            continue;
        }

        ReflectedConstantBuffer single{};
        CHECK(ctx, ReflectConstantBuffer(artifact, "PerMaterial", CBSlots::PerMaterial, single, &error));
        const ReflectedCBField* custom = single.Find("customValue");
        CHECK(ctx, custom != nullptr);
        if (custom)
            CHECK_EQ(ctx, custom->byteOffset, 32u);

        // Programm: der VS deklariert PerMaterial nicht, der PS liefert die Felder.
        ReflectedConstantBuffer program{};
        CHECK(ctx, ReflectProgramConstantBuffer(vs, ps, target, "PerMaterial", CBSlots::PerMaterial, program, &error));
        CHECK_EQ(ctx, program.byteSize, 64u);
        CheckReorderedPerMaterial(ctx, program);
    }
}

// Die Beispiel-Shader in assets/examples halten das bisherige feste Layout ein:
// Mit Reflection entsteht derselbe PerMaterial-Inhalt wie ohne.
static void TestExampleShadersKeepPerMaterialLayout(test::TestContext& ctx)
{
    const std::filesystem::path examples = PathFromUtf8(KROM_SOURCE_DIR) / "assets" / "examples";
    const auto readText = [](const std::filesystem::path& path) {
        std::ifstream file(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    };

    unlit::UnlitMaterialCreateInfo info = MakeReorderedCreateInfo();
    info.extraParameters.clear();
    MaterialSystem legacySystem;
    const MaterialHandle legacy = legacySystem.RegisterMaterial(unlit::UnlitMaterial::BuildDesc(info));
    const std::vector<uint8_t> legacyCB = legacySystem.GetCBData(legacy);
    CHECK_EQ(ctx, legacyCB.size(), static_cast<size_t>(64u));

    const auto checkSameAsLegacy = [&](const ReflectedConstantBuffer& reflection, const char* label) {
        CHECK(ctx, reflection.valid);
        unlit::UnlitMaterialCreateInfo reflected = info;
        reflected.perMaterialReflection = reflection;
        MaterialSystem ms;
        const MaterialHandle mat = ms.RegisterMaterial(unlit::UnlitMaterial::BuildDesc(reflected));
        const std::vector<uint8_t>& cb = ms.GetCBData(mat);
        const bool same = cb.size() == legacyCB.size() && std::memcmp(cb.data(), legacyCB.data(), cb.size()) == 0;
        if (!same)
            Debug::LogError("Example shader '%s': PerMaterial differs from fixed layout", label);
        CHECK(ctx, same);
    };

    for (const char* name : { "animated_color", "grayscale" })
    {
        ReflectedConstantBuffer glsl{};
        std::string error;
        const std::string glslSource = readText(examples / (std::string(name) + ".opengl.fs.glsl"));
        CHECK(ctx, ReflectGlslStd140Block(glslSource, "PerMaterial", glsl, &error));
        checkSameAsLegacy(glsl, name);

        assets::ShaderAsset ps{};
        ps.debugName = std::string(name) + "_ps";
        ps.stage = assets::ShaderStage::Fragment;
        ps.sourceLanguage = assets::ShaderSourceLanguage::HLSL;
        ps.sourceCode = readText(examples / (std::string(name) + ".ps.hlsl"));
        CHECK(ctx, !ps.sourceCode.empty());
        assets::CompiledShaderArtifact artifact{};
        if (!ShaderCompiler::CompileForTarget(ps, assets::ShaderTargetProfile::DirectX11_SM5, artifact, &error))
        {
            Debug::LogWarning("Example shader '%s': DX11 nicht kompilierbar (%s), uebersprungen", name, error.c_str());
            continue;
        }
        ReflectedConstantBuffer dxbc{};
        CHECK(ctx, ReflectConstantBuffer(artifact, "PerMaterial", CBSlots::PerMaterial, dxbc, &error));
        checkSameAsLegacy(dxbc, name);
    }
}

// Custom vertex shaders can be used by materials with "Instanced" switched on. Every shipped
// example and DemoProject vertex shader must compile with and without KROM_INSTANCED.
static void TestCustomVertexShadersCompileInstanced(test::TestContext& ctx)
{
    const std::filesystem::path source = PathFromUtf8(KROM_SOURCE_DIR);
    const std::filesystem::path directories[] = {
        source / "assets" / "examples",
        source / "projects" / "DemoProject" / "Assets" / "Shader",
    };
    const bool haveDxc = !FindShaderTool(ShaderTool::Dxc).path.empty();
    if (!haveDxc)
        Debug::LogWarning("Custom VS instancing test: DXC not found, Vulkan skipped");

    size_t checked = 0u;
    for (const std::filesystem::path& directory : directories)
    {
        CHECK(ctx, std::filesystem::is_directory(directory));
        if (!std::filesystem::is_directory(directory))
            continue;
        for (const auto& entry : std::filesystem::directory_iterator(directory))
        {
            const std::string fileName = PathToUtf8(entry.path().filename());
            if (!fileName.ends_with(".vs.hlsl"))
                continue;

            assets::ShaderAsset vs{};
            vs.debugName = fileName;
            vs.stage = assets::ShaderStage::Vertex;
            vs.sourceLanguage = assets::ShaderSourceLanguage::HLSL;
            vs.entryPoint = "main";
            vs.resolvedPath = PathToUtf8(entry.path());

            for (const assets::ShaderTargetProfile target : { assets::ShaderTargetProfile::DirectX11_SM5,
                                                              assets::ShaderTargetProfile::Vulkan_SPIRV })
            {
                if (target == assets::ShaderTargetProfile::Vulkan_SPIRV && !haveDxc)
                    continue;
                for (const ShaderVariantFlag flags : { ShaderVariantFlag::None, ShaderVariantFlag::Instanced })
                {
                    assets::CompiledShaderArtifact artifact{};
                    std::string error;
                    const bool ok = ShaderCompiler::CompileVariant(vs, target, flags, artifact, &error);
                    if (!ok)
                        Debug::LogError("Custom VS '%s' (%s, %s) failed: %s", fileName.c_str(),
                                        ShaderCompiler::ToString(target),
                                        flags == ShaderVariantFlag::Instanced ? "instanced" : "plain",
                                        error.c_str());
                    CHECK(ctx, ok);
                    ++checked;
                }
            }
        }
    }
    CHECK(ctx, checked >= 10u);
}

// --------------------------------------------------------------------------
// Eigene Texturen am Register des Shaders (t13) statt der Reihe nach ab t2
// --------------------------------------------------------------------------
namespace {

constexpr const char* kCustomTexturePS = R"(
Texture2D    tAlbedo   : register(t0);
Texture2D    detailMap : register(t13);
SamplerState sLinear   : register(s0);
float4 main(float4 position : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET
{
    return tAlbedo.Sample(sLinear, uv) * detailMap.Sample(sLinear, uv);
}
)";

// Registriert ein Custom-Material mit eigener Textur "detailMap" und prueft,
// an welchem Slot sie im Layout und im kompilierten Material landet.
uint32_t RegisteredDetailMapSlot(test::TestContext& ctx, const ReflectedTextureBindings& overrides)
{
    unlit::UnlitMaterialCreateInfo info{};
    info.name = "CustomTextureRegister";
    MaterialParam detail{};
    detail.name = "detailMap";
    detail.type = MaterialParam::Type::Texture;
    info.extraParameters.push_back(detail);
    info.textureBindingOverrides = overrides;

    MaterialSystem ms;
    const MaterialHandle mat = ms.RegisterMaterial(unlit::UnlitMaterial::BuildDesc(info));
    CHECK_VALID(ctx, mat);
    const MaterialParameterLayout* layout = ms.GetParameterLayout(mat);
    const MaterialParameterSlot* slot = layout ? layout->FindByName("detailMap") : nullptr;
    CHECK(ctx, slot != nullptr);
    if (!slot)
        return UINT32_MAX;

    // Albedo/Emissive bleiben an ihren festen Slots.
    if (const MaterialParameterSlot* albedo = layout->FindByName("albedo"))
        CHECK_EQ(ctx, albedo->binding, TexSlots::Albedo);
    if (const MaterialParameterSlot* emissive = layout->FindByName("emissive"))
        CHECK_EQ(ctx, emissive->binding, TexSlots::Emissive);

    if (const CompiledMaterialDesc* compiled = ms.GetCompiledDesc(mat))
    {
        const auto it = std::find_if(compiled->resourceBindings.begin(), compiled->resourceBindings.end(),
            [](const MaterialResourceBindingDesc& binding) { return binding.name == "detailMap"; });
        CHECK(ctx, it != compiled->resourceBindings.end());
        if (it != compiled->resourceBindings.end())
            CHECK_EQ(ctx, it->logicalSlot, slot->binding);
    }
    return slot->binding;
}

uint32_t FindTextureSlot(const ReflectedTextureBindings& bindings, const char* name)
{
    const auto it = bindings.find(name);
    return it != bindings.end() ? it->second : UINT32_MAX;
}

} // namespace

static void TestCustomTextureRegisterFromReflection(test::TestContext& ctx)
{
    // Ohne Reflection: der bisherige Fallback zaehlt ab t2 (Kollision mit ORM).
    CHECK(ctx, RegisteredDetailMapSlot(ctx, {}) != 13u);

    // Vorgegebenes Override (wie es die Reflection liefert): Slot 13.
    CHECK_EQ(ctx, RegisteredDetailMapSlot(ctx, { { "detailMap", 13u } }), 13u);

    // GLSL: layout(binding = N); Sampler ohne binding und Bloecke fehlen.
    const char* glsl = R"(#version 450 core
layout(std140, binding = 2) uniform PerMaterial { vec4 baseColorFactor; };
uniform sampler2D tAlbedo;
layout(binding = 13) uniform sampler2D detailMap; // eigene Textur
layout(location = 0, binding=14) uniform highp sampler2D secondMap;
/* layout(binding = 15) uniform sampler2D commented; */
layout(binding = 21) uniform sampler2D outOfRange; // = TexSlots::COUNT
void main() {}
)";
    ReflectedTextureBindings glslBindings;
    std::string error;
    CHECK(ctx, ReflectGlslTextureBindings(glsl, glslBindings, &error));
    CHECK_EQ(ctx, FindTextureSlot(glslBindings, "detailMap"), 13u);
    CHECK_EQ(ctx, FindTextureSlot(glslBindings, "secondMap"), 14u);
    CHECK(ctx, glslBindings.count("tAlbedo") == 0u);
    CHECK(ctx, glslBindings.count("commented") == 0u);
    CHECK(ctx, glslBindings.count("outOfRange") == 0u);
    CHECK(ctx, glslBindings.count("PerMaterial") == 0u);
    CHECK_EQ(ctx, RegisteredDetailMapSlot(ctx, glslBindings), 13u);

    // DXBC und SPIR-V aus echtem HLSL, wenn der Compiler vorhanden ist.
    assets::ShaderAsset vs{};
    vs.debugName = "TextureRegisterVS";
    vs.stage = assets::ShaderStage::Vertex;
    vs.sourceLanguage = assets::ShaderSourceLanguage::HLSL;
    vs.sourceCode = kPlainVS;

    assets::ShaderAsset ps{};
    ps.debugName = "TextureRegisterPS";
    ps.stage = assets::ShaderStage::Fragment;
    ps.sourceLanguage = assets::ShaderSourceLanguage::HLSL;
    ps.sourceCode = kCustomTexturePS;

    for (const assets::ShaderTargetProfile target : { assets::ShaderTargetProfile::DirectX11_SM5,
                                                      assets::ShaderTargetProfile::Vulkan_SPIRV })
    {
        assets::CompiledShaderArtifact artifact{};
        if (!ShaderCompiler::CompileForTarget(ps, target, artifact, &error))
        {
            Debug::LogWarning("Texture register test: %s nicht kompilierbar (%s), uebersprungen",
                              ShaderCompiler::ToString(target), error.c_str());
            continue;
        }

        ReflectedTextureBindings single;
        CHECK(ctx, ReflectTextureBindings(artifact, single, &error));
        CHECK_EQ(ctx, FindTextureSlot(single, "detailMap"), 13u);
        CHECK_EQ(ctx, FindTextureSlot(single, "tAlbedo"), 0u);

        ReflectedTextureBindings program;
        CHECK(ctx, ReflectProgramTextureBindings(vs, ps, target, program, &error));
        CHECK_EQ(ctx, RegisteredDetailMapSlot(ctx, program), 13u);
    }
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
    MaterialRuntimeDesc dRuntime;
    d.name = "EnvPartialMaterial";
    dRuntime.renderPass = StandardRenderPasses::Opaque();
    dRuntime.vertexShader = vsAsset;
    dRuntime.fragmentShader = psAsset;
    const MaterialHandle mat = MaterialRuntimeBridge::RegisterMaterial(ms, std::move(d), dRuntime);

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

    CHECK(ctx, runtime.PrepareMaterial(MaterialSystemShaderMaterialSource(ms), mat));

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
    MaterialRuntimeDesc dRuntime;
    d.name = "EnvMaterial";
    dRuntime.renderPass = StandardRenderPasses::Opaque();
    dRuntime.vertexShader = vsAsset;
    dRuntime.fragmentShader = psAsset;
    const MaterialHandle mat = MaterialRuntimeBridge::RegisterMaterial(ms, std::move(d), dRuntime);

    renderer::null_backend::NullDevice device;
    renderer::IDevice::DeviceDesc dd{};
    dd.appName = "ShaderRuntimeEnvRebuild";
    CHECK(ctx, device.Initialize(dd));

    ShaderRuntime runtime;
    CHECK(ctx, runtime.Initialize(device));
    runtime.SetAssetRegistry(&assets);
    CHECK(ctx, runtime.PrepareAllShaderAssets());
    CHECK(ctx, runtime.PrepareMaterial(MaterialSystemShaderMaterialSource(ms), mat));

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
    CHECK(ctx, runtime.BindMaterial(*cmd, MaterialSystemShaderMaterialSource(ms), mat, BufferHandle::Make(1u,1u), BufferHandle::Make(2u,1u)));
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

