#pragma once

#include "core/Debug.hpp"
#include "renderer/RenderPipelineTypes.hpp"
#include "rendergraph/RenderGraph.hpp"
#include <array>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace engine::renderer {

using rendergraph::RGPassBuilder;
using rendergraph::RGPassType;
using rendergraph::RGResourceKind;
using rendergraph::RenderGraph;

enum class FrameRecipeAccessKind : uint8_t
{
    ReadTexture,
    ReadDepthStencil,
    WriteRenderTarget,
    WriteDepthStencil,
    Present,
};

struct FrameRecipeResourceAccess
{
    std::string resourceName;
    FrameRecipeAccessKind access = FrameRecipeAccessKind::ReadTexture;
};

struct FrameRecipeRenderPassDesc
{
    bool enabled = false;
    std::string targetResourceName;
    uint32_t viewportWidth = 0u;
    uint32_t viewportHeight = 0u;
    bool clearColor = false;
    bool clearDepth = false;
    std::array<float, 4> clearColorValue{0.f, 0.f, 0.f, 0.f};
    float clearDepthValue = 1.f;

    // Multiple render targets: colour-only resources bound behind the target
    // (ICommandList::RenderPassBeginInfo::extraColorTargets). The pass must
    // also list them as WriteRenderTarget accesses. Cleared with their own
    // value when clearExtraColors is set (independent of clearColor).
    struct ExtraColorTarget
    {
        std::string resourceName;
        std::array<float, 4> clearColorValue{0.f, 0.f, 0.f, 0.f};
    };
    std::vector<ExtraColorTarget> extraColorTargets;
    bool clearExtraColors = true;
    bool depthOnly = false;
    bool readOnlyDepth = false;
    std::string depthTargetResourceName;
};

// Colour-only resource a contributor wants the opaque scene pass to write as
// an extra render target (MRT), e.g. the SSR inputs. The contributor declares
// the resource itself (same size and sample count as the scene target) and
// lists it in FrameRecipe::sceneOutputs.
struct FrameRecipeSceneOutput
{
    std::string resourceName;
    Format format = Format::Unknown;
    std::array<float, 4> clearColorValue{0.f, 0.f, 0.f, 0.f};
};

// Texture a contributor of the AfterDepthPrepass phase produces for the opaque
// scene pass (e.g. GTAO). The contributor declares the resource itself and
// lists it in FrameRecipe::opaqueInputs; the forward pipeline then adds the
// depth prepass, a ReadTexture access on the opaque pass and binds the texture
// on textureSlot while the opaque list draws (white fallback when missing).
struct FrameRecipeOpaqueInput
{
    std::string resourceName;
    uint32_t textureSlot = 0u;
};

struct FrameRecipePassDesc
{
    std::string name;
    std::string executorName;
    // Optional executor run before the render pass opens, in the same graph pass
    // (e.g. compute dispatches whose results the pass reads). Uses the pass's
    // accesses and barriers; empty: none.
    std::string preRenderPassExecutorName;
    RGPassType type = RGPassType::Graphics;
    std::vector<FrameRecipeResourceAccess> accesses;
    FrameRecipeRenderPassDesc renderPass{};
};

struct FrameRecipeResourceDesc
{
    std::string name;
    bool importedBackbuffer = false;
    uint32_t width = 0u;
    uint32_t height = 0u;
    Format format = Format::Unknown;
    RGResourceKind kind = RGResourceKind::Unknown;
    uint32_t sampleCount = 1u;   // > 1: multisampled (transient render targets only)

    // Import an externally-owned render target instead of allocating a transient.
    // An external output keeps the producer alive across graph culling.
    RenderTargetHandle importedRenderTarget;
    TextureHandle      importedTexture;
    bool               externalOutput = false;
    std::string        depthAttachmentOf;
};

struct FrameRecipe
{
    std::vector<FrameRecipeResourceDesc> resources;
    std::vector<FrameRecipePassDesc> passes;
    // Extra outputs of the opaque scene pass (see FrameRecipeSceneOutput).
    std::vector<FrameRecipeSceneOutput> sceneOutputs;
    // Inputs of the opaque scene pass (see FrameRecipeOpaqueInput). Only read
    // from contributors of the AfterDepthPrepass phase.
    std::vector<FrameRecipeOpaqueInput> opaqueInputs;
};

struct FrameRecipeCompileParams
{
    RenderTargetHandle backbufferRT;
    TextureHandle backbufferTex;
};

class FrameRecipeCompiler
{
public:
    static std::unordered_map<std::string, RGResourceID> Build(
        RenderGraph& rg,
        const FrameRecipeCompileParams& params,
        const FrameRecipe& recipe,
        const FramePipelineCallbacks& executors)
    {
        std::unordered_map<std::string, RGResourceID> materializedResources;
        materializedResources.reserve(recipe.resources.size());

        ValidateRecipe(recipe, executors);
        MaterializeResources(rg, params, recipe, materializedResources);
        MaterializePasses(rg, recipe, executors, materializedResources);
        return materializedResources;
    }

private:
    [[nodiscard]] static FramePipelinePassCallback ResolveExecutor(const FramePipelineCallbacks& executors,
                                                                   std::string_view name)
    {
        const FramePipelinePassCallback* found = executors.Find(name);
        return found ? *found : FramePipelinePassCallback{};
    }

    [[nodiscard]] static RGResourceID LookupResource(const std::unordered_map<std::string, RGResourceID>& resources,
                                                     std::string_view name)
    {
        const auto it = resources.find(std::string(name));
        if (it != resources.end())
            return it->second;

        Debug::LogError("FrameRecipeCompiler: unresolved recipe resource '%s'",
                        std::string(name).c_str());
        return RG_INVALID_RESOURCE;
    }

    static void ValidateRecipe(const FrameRecipe& recipe,
                               const FramePipelineCallbacks& executors)
    {
        std::unordered_set<std::string> resourceNames;
        std::unordered_set<std::string> passNames;

        for (const FrameRecipeResourceDesc& resource : recipe.resources)
        {
            if (!resourceNames.insert(resource.name).second)
            {
                Debug::LogError("FrameRecipeCompiler: duplicate recipe resource '%s'",
                                resource.name.c_str());
            }
        }

        for (const FrameRecipePassDesc& pass : recipe.passes)
        {
            if (!passNames.insert(pass.name).second)
            {
                Debug::LogError("FrameRecipeCompiler: duplicate recipe pass '%s'",
                                pass.name.c_str());
            }

            if (executors.Find(pass.executorName) == nullptr)
            {
                Debug::LogWarning("FrameRecipeCompiler: pass '%s' has no executor '%s'",
                                  pass.name.c_str(),
                                  pass.executorName.c_str());
            }
            if (!pass.preRenderPassExecutorName.empty() &&
                executors.Find(pass.preRenderPassExecutorName) == nullptr)
            {
                Debug::LogWarning("FrameRecipeCompiler: pass '%s' has no pre-render-pass executor '%s'",
                                  pass.name.c_str(),
                                  pass.preRenderPassExecutorName.c_str());
            }

            if (pass.renderPass.enabled && resourceNames.find(pass.renderPass.targetResourceName) == resourceNames.end())
            {
                Debug::LogError("FrameRecipeCompiler: pass '%s' targets unknown resource '%s'",
                                pass.name.c_str(),
                                pass.renderPass.targetResourceName.c_str());
            }

            if (pass.renderPass.extraColorTargets.size() > kMaxExtraColorTargets)
            {
                Debug::LogError("FrameRecipeCompiler: pass '%s' has %zu extra colour targets (max %u)",
                                pass.name.c_str(), pass.renderPass.extraColorTargets.size(),
                                kMaxExtraColorTargets);
            }
            for (const auto& extra : pass.renderPass.extraColorTargets)
            {
                if (resourceNames.find(extra.resourceName) == resourceNames.end())
                {
                    Debug::LogError("FrameRecipeCompiler: pass '%s' binds unknown extra colour target '%s'",
                                    pass.name.c_str(), extra.resourceName.c_str());
                }
            }

            for (const FrameRecipeResourceAccess& access : pass.accesses)
            {
                if (resourceNames.find(access.resourceName) == resourceNames.end())
                {
                    Debug::LogError("FrameRecipeCompiler: pass '%s' references unknown resource '%s'",
                                    pass.name.c_str(),
                                    access.resourceName.c_str());
                }
            }
        }
    }

    static void MaterializeResources(RenderGraph& rg,
                                     const FrameRecipeCompileParams& params,
                                     const FrameRecipe& recipe,
                                     std::unordered_map<std::string, RGResourceID>& materializedResources)
    {
        for (const FrameRecipeResourceDesc& desc : recipe.resources)
        {
            RGResourceID id = RG_INVALID_RESOURCE;
            if (desc.importedBackbuffer)
            {
                id = rg.ImportBackbuffer(params.backbufferRT,
                                         params.backbufferTex,
                                         desc.width,
                                         desc.height);
            }
            else if (desc.importedRenderTarget.IsValid())
            {
                id = rg.ImportRenderTarget(desc.importedRenderTarget,
                                           desc.importedTexture,
                                           desc.name.c_str(),
                                           desc.width,
                                           desc.height,
                                           desc.format,
                                           desc.externalOutput);
            }
            else if (!desc.depthAttachmentOf.empty())
            {
                id = rg.UseDepthAttachment(LookupResource(materializedResources, desc.depthAttachmentOf),
                                           desc.name.c_str());
            }
            else
            {
                id = rg.CreateTransientRenderTarget(desc.name.c_str(),
                                                    desc.width,
                                                    desc.height,
                                                    desc.format,
                                                    desc.kind,
                                                    desc.sampleCount);
            }

            materializedResources.emplace(desc.name, id);
        }
    }

    static void ApplyAccess(RGPassBuilder& builder,
                            const FrameRecipeResourceAccess& access,
                            const std::unordered_map<std::string, RGResourceID>& materializedResources)
    {
        const RGResourceID id = LookupResource(materializedResources, access.resourceName);
        switch (access.access)
        {
        case FrameRecipeAccessKind::ReadTexture:
            builder.ReadTexture(id);
            break;
        case FrameRecipeAccessKind::ReadDepthStencil:
            builder.ReadDepthStencil(id);
            break;
        case FrameRecipeAccessKind::WriteRenderTarget:
            builder.WriteRenderTarget(id);
            break;
        case FrameRecipeAccessKind::WriteDepthStencil:
            builder.WriteDepthStencil(id);
            break;
        case FrameRecipeAccessKind::Present:
            builder.Present(id);
            break;
        }
    }

    static void MaterializePasses(RenderGraph& rg,
                                  const FrameRecipe& recipe,
                                  const FramePipelineCallbacks& executors,
                                  const std::unordered_map<std::string, RGResourceID>& materializedResources)
    {
        for (const FrameRecipePassDesc& desc : recipe.passes)
        {
            RGPassBuilder builder = rg.AddPass(desc.name.c_str(), desc.type);
            for (const FrameRecipeResourceAccess& access : desc.accesses)
                ApplyAccess(builder, access, materializedResources);

            const FramePipelinePassCallback executor = ResolveExecutor(executors, desc.executorName);
            const FramePipelinePassCallback preExecutor = desc.preRenderPassExecutorName.empty()
                ? FramePipelinePassCallback{}
                : ResolveExecutor(executors, desc.preRenderPassExecutorName);

            if (!desc.renderPass.enabled)
            {
                builder.Execute([executor, preExecutor](const RGExecContext& ctx) {
                    if (preExecutor) preExecutor(ctx);
                    if (executor) executor(ctx);
                });
                continue;
            }

            const RGResourceID targetId = LookupResource(materializedResources, desc.renderPass.targetResourceName);
            const RGResourceID depthTargetId = desc.renderPass.depthTargetResourceName.empty()
                ? RG_INVALID_RESOURCE
                : LookupResource(materializedResources, desc.renderPass.depthTargetResourceName);
            const bool depthOnly = desc.renderPass.depthOnly;
            const bool readOnlyDepth = desc.renderPass.readOnlyDepth;
            const uint32_t viewportWidth = desc.renderPass.viewportWidth;
            const uint32_t viewportHeight = desc.renderPass.viewportHeight;
            const bool clearColor = desc.renderPass.clearColor;
            const bool clearDepth = desc.renderPass.clearDepth;
            const auto clearColorValue = desc.renderPass.clearColorValue;
            const float clearDepthValue = desc.renderPass.clearDepthValue;
            const bool clearExtraColors = desc.renderPass.clearExtraColors;
            std::array<RGResourceID, kMaxExtraColorTargets> extraIds{};
            std::array<std::array<float, 4>, kMaxExtraColorTargets> extraClears{};
            uint32_t extraCount = 0u;
            for (const auto& extra : desc.renderPass.extraColorTargets)
            {
                if (extraCount >= kMaxExtraColorTargets)
                    break;
                extraIds[extraCount] = LookupResource(materializedResources, extra.resourceName);
                extraClears[extraCount] = extra.clearColorValue;
                ++extraCount;
            }

            builder.Execute([executor,
                             preExecutor,
                             targetId,
                             depthTargetId,
                             depthOnly,
                             readOnlyDepth,
                             viewportWidth,
                             viewportHeight,
                             clearColor,
                             clearDepth,
                             clearColorValue,
                             clearDepthValue,
                             extraIds,
                             extraClears,
                             extraCount,
                             clearExtraColors](const RGExecContext& ctx) {
                if (preExecutor) preExecutor(ctx);
                ICommandList::RenderPassBeginInfo rp{};
                rp.renderTarget = ctx.GetRenderTarget(targetId);
                rp.depthTarget = ctx.GetRenderTarget(depthTargetId);
                rp.depthOnly = depthOnly;
                rp.readOnlyDepth = readOnlyDepth;
                rp.clearColor = clearColor;
                rp.clearDepth = clearDepth;
                rp.colorClear.color[0] = clearColorValue[0];
                rp.colorClear.color[1] = clearColorValue[1];
                rp.colorClear.color[2] = clearColorValue[2];
                rp.colorClear.color[3] = clearColorValue[3];
                rp.depthClear.depth = clearDepthValue;
                rp.clearExtraColors = clearExtraColors;
                for (uint32_t i = 0u; i < extraCount; ++i)
                {
                    const RenderTargetHandle extra = ctx.GetRenderTarget(extraIds[i]);
                    if (!extra.IsValid())
                        break;
                    rp.extraColorTargets[rp.extraColorTargetCount] = extra;
                    for (int c = 0; c < 4; ++c)
                        rp.extraColorClears[rp.extraColorTargetCount].color[c] = extraClears[i][c];
                    ++rp.extraColorTargetCount;
                }
                ctx.cmd->BeginRenderPass(rp);
                ctx.cmd->SetViewport(0.f, 0.f,
                                     static_cast<float>(viewportWidth),
                                     static_cast<float>(viewportHeight),
                                     0.f, 1.f);
                ctx.cmd->SetScissor(0, 0, viewportWidth, viewportHeight);
                if (executor) executor(ctx);
                ctx.cmd->EndRenderPass();
            });
        }
    }
};

} // namespace engine::renderer
