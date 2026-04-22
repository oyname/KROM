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
};

struct FrameRecipePassDesc
{
    std::string name;
    std::string executorName;
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
};

struct FrameRecipe
{
    std::vector<FrameRecipeResourceDesc> resources;
    std::vector<FrameRecipePassDesc> passes;
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

            if (pass.renderPass.enabled && resourceNames.find(pass.renderPass.targetResourceName) == resourceNames.end())
            {
                Debug::LogError("FrameRecipeCompiler: pass '%s' targets unknown resource '%s'",
                                pass.name.c_str(),
                                pass.renderPass.targetResourceName.c_str());
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
            else
            {
                id = rg.CreateTransientRenderTarget(desc.name.c_str(),
                                                    desc.width,
                                                    desc.height,
                                                    desc.format,
                                                    desc.kind);
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

            if (!desc.renderPass.enabled)
            {
                builder.Execute([executor](const RGExecContext& ctx) {
                    if (executor) executor(ctx);
                });
                continue;
            }

            const RGResourceID targetId = LookupResource(materializedResources, desc.renderPass.targetResourceName);
            const uint32_t viewportWidth = desc.renderPass.viewportWidth;
            const uint32_t viewportHeight = desc.renderPass.viewportHeight;
            const bool clearColor = desc.renderPass.clearColor;
            const bool clearDepth = desc.renderPass.clearDepth;
            const auto clearColorValue = desc.renderPass.clearColorValue;
            const float clearDepthValue = desc.renderPass.clearDepthValue;

            builder.Execute([executor,
                             targetId,
                             viewportWidth,
                             viewportHeight,
                             clearColor,
                             clearDepth,
                             clearColorValue,
                             clearDepthValue](const RGExecContext& ctx) {
                ICommandList::RenderPassBeginInfo rp{};
                rp.renderTarget = ctx.GetRenderTarget(targetId);
                rp.clearColor = clearColor;
                rp.clearDepth = clearDepth;
                rp.colorClear.color[0] = clearColorValue[0];
                rp.colorClear.color[1] = clearColorValue[1];
                rp.colorClear.color[2] = clearColorValue[2];
                rp.colorClear.color[3] = clearColorValue[3];
                rp.depthClear.depth = clearDepthValue;
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
