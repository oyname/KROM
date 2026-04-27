#pragma once
// =============================================================================
// KROM Engine - addons/forward/StandardFramePipeline.hpp
// Standard-Rezept fuer die Forward-Rendering-Frame-Abfolge.
// Lebt im Forward-Addon, nicht im Core.
// =============================================================================
#include "core/Debug.hpp"
#include "renderer/RenderPipelineRecipe.hpp"
#include <string>
#include <string_view>
#include <unordered_map>

namespace engine::renderer {

    enum class StandardFrameExecutorID : uint8_t
    {
        Shadow = 0,
        Opaque,
        Transparent,
        BloomExtract,
        BloomBlurH,
        BloomBlurV,
        Tonemap,
        UI,
        Present,
    };

    enum class StandardFrameResourceID : uint8_t
    {
        Backbuffer = 0,
        ShadowMap,
        HDRSceneColor,
        BloomExtracted,
        BloomBlurH,
        BloomBlurV,
        Tonemapped,
        UIOverlay,
    };

    enum class StandardFramePassID : uint8_t
    {
        Shadow = 0,
        MainOpaque,
        Transparent,
        BloomExtract,
        BloomBlurH,
        BloomBlurV,
        Tonemap,
        UI,
        Present,
    };

    namespace StandardFrameExecutors {

        [[nodiscard]] inline constexpr std::string_view Name(StandardFrameExecutorID id) noexcept
        {
            switch (id)
            {
            case StandardFrameExecutorID::Shadow:       return "frame.shadow";
            case StandardFrameExecutorID::Opaque:       return "frame.opaque";
            case StandardFrameExecutorID::Transparent:  return "frame.transparent";
            case StandardFrameExecutorID::BloomExtract: return "frame.bloom_extract";
            case StandardFrameExecutorID::BloomBlurH:   return "frame.bloom_blur_h";
            case StandardFrameExecutorID::BloomBlurV:   return "frame.bloom_blur_v";
            case StandardFrameExecutorID::Tonemap:      return "frame.tonemap";
            case StandardFrameExecutorID::UI:           return "frame.ui";
            case StandardFrameExecutorID::Present:      return "frame.present";
            }
            return {};
        }

        inline constexpr std::string_view Shadow       = Name(StandardFrameExecutorID::Shadow);
        inline constexpr std::string_view Opaque       = Name(StandardFrameExecutorID::Opaque);
        inline constexpr std::string_view Transparent  = Name(StandardFrameExecutorID::Transparent);
        inline constexpr std::string_view BloomExtract = Name(StandardFrameExecutorID::BloomExtract);
        inline constexpr std::string_view BloomBlurH   = Name(StandardFrameExecutorID::BloomBlurH);
        inline constexpr std::string_view BloomBlurV   = Name(StandardFrameExecutorID::BloomBlurV);
        inline constexpr std::string_view Tonemap      = Name(StandardFrameExecutorID::Tonemap);
        inline constexpr std::string_view UI           = Name(StandardFrameExecutorID::UI);
        inline constexpr std::string_view Present      = Name(StandardFrameExecutorID::Present);

    } // namespace StandardFrameExecutors

    namespace StandardFrameResources {

        [[nodiscard]] inline constexpr std::string_view Name(StandardFrameResourceID id) noexcept
        {
            switch (id)
            {
            case StandardFrameResourceID::Backbuffer:    return "Backbuffer";
            case StandardFrameResourceID::ShadowMap:     return "ShadowMap";
            case StandardFrameResourceID::HDRSceneColor: return "HDRSceneColor";
            case StandardFrameResourceID::BloomExtracted:return "BloomExtracted";
            case StandardFrameResourceID::BloomBlurH:    return "BloomBlurH";
            case StandardFrameResourceID::BloomBlurV:    return "BloomBlurV";
            case StandardFrameResourceID::Tonemapped:    return "Tonemapped";
            case StandardFrameResourceID::UIOverlay:     return "UIOverlay";
            }
            return {};
        }

        inline constexpr std::string_view Backbuffer    = Name(StandardFrameResourceID::Backbuffer);
        inline constexpr std::string_view ShadowMap     = Name(StandardFrameResourceID::ShadowMap);
        inline constexpr std::string_view HDRSceneColor = Name(StandardFrameResourceID::HDRSceneColor);
        inline constexpr std::string_view BloomExtracted= Name(StandardFrameResourceID::BloomExtracted);
        inline constexpr std::string_view BloomBlurH    = Name(StandardFrameResourceID::BloomBlurH);
        inline constexpr std::string_view BloomBlurV    = Name(StandardFrameResourceID::BloomBlurV);
        inline constexpr std::string_view Tonemapped    = Name(StandardFrameResourceID::Tonemapped);
        inline constexpr std::string_view UIOverlay     = Name(StandardFrameResourceID::UIOverlay);

    } // namespace StandardFrameResources

    namespace StandardFramePassNames {

        [[nodiscard]] inline constexpr std::string_view Name(StandardFramePassID id) noexcept
        {
            switch (id)
            {
            case StandardFramePassID::Shadow:      return "ShadowPass";
            case StandardFramePassID::MainOpaque:  return "MainOpaquePass";
            case StandardFramePassID::Transparent: return "TransparentPass";
            case StandardFramePassID::BloomExtract:return "BloomExtractPass";
            case StandardFramePassID::BloomBlurH:  return "BloomBlurPassH";
            case StandardFramePassID::BloomBlurV:  return "BloomBlurPassV";
            case StandardFramePassID::Tonemap:     return "TonemapPass";
            case StandardFramePassID::UI:          return "UIPass";
            case StandardFramePassID::Present:     return "PresentPass";
            }
            return {};
        }

        inline constexpr std::string_view Shadow      = Name(StandardFramePassID::Shadow);
        inline constexpr std::string_view MainOpaque  = Name(StandardFramePassID::MainOpaque);
        inline constexpr std::string_view Transparent = Name(StandardFramePassID::Transparent);
        inline constexpr std::string_view BloomExtract= Name(StandardFramePassID::BloomExtract);
        inline constexpr std::string_view BloomBlurH  = Name(StandardFramePassID::BloomBlurH);
        inline constexpr std::string_view BloomBlurV  = Name(StandardFramePassID::BloomBlurV);
        inline constexpr std::string_view Tonemap     = Name(StandardFramePassID::Tonemap);
        inline constexpr std::string_view UI          = Name(StandardFramePassID::UI);
        inline constexpr std::string_view Present     = Name(StandardFramePassID::Present);

    } // namespace StandardFramePassNames

    struct StandardFrameBuildResult
    {
        rendergraph::RGResourceID backbuffer     = rendergraph::RG_INVALID_RESOURCE;
        rendergraph::RGResourceID shadowMap      = rendergraph::RG_INVALID_RESOURCE;
        rendergraph::RGResourceID hdrSceneColor  = rendergraph::RG_INVALID_RESOURCE;
        rendergraph::RGResourceID bloomInput     = rendergraph::RG_INVALID_RESOURCE;
        rendergraph::RGResourceID bloomExtracted = rendergraph::RG_INVALID_RESOURCE;
        rendergraph::RGResourceID bloomBlurH     = rendergraph::RG_INVALID_RESOURCE;
        rendergraph::RGResourceID bloomBlurV     = rendergraph::RG_INVALID_RESOURCE;
        rendergraph::RGResourceID tonemapped     = rendergraph::RG_INVALID_RESOURCE;
        rendergraph::RGResourceID uiOverlay      = rendergraph::RG_INVALID_RESOURCE;
    };

    class StandardFrameRecipeBuilder
    {
    public:
        struct BuildParams
        {
            uint32_t viewportWidth  = 1280u;
            uint32_t viewportHeight = 720u;
            uint32_t shadowMapSize  = 2048u;
            uint32_t bloomWidth     = 640u;
            uint32_t bloomHeight    = 360u;

            RenderTargetHandle backbufferRT;
            TextureHandle      backbufferTex;

            bool shadowEnabled      = false;
            bool bloomEnabled       = false;
            bool transparentEnabled = false;
            bool uiEnabled          = false;

            std::array<float, 4> clearColorValue = { 0.3f, 0.3f, 0.3f, 1.f };
        };

        [[nodiscard]] static FrameRecipe BuildRecipe(const BuildParams& p)
        {
            FrameRecipe recipe{};
            AppendCoreResources(recipe, p);
            AppendOptionalResources(recipe, p);
            AppendCorePasses(recipe, p);
            AppendOptionalPasses(recipe, p);
            return recipe;
        }

        static StandardFrameBuildResult Build(RenderGraph& rg,
            const BuildParams& p,
            const FramePipelineCallbacks& executors)
        {
            const FrameRecipe recipe = BuildRecipe(p);
            const FrameRecipeCompileParams params{ p.backbufferRT, p.backbufferTex };
            const auto materialized = FrameRecipeCompiler::Build(rg, params, recipe, executors);
            StandardFrameBuildResult result = AssembleResources(materialized);

            Debug::LogVerbose("StandardFramePipeline: recipe built - "
                "shadow=%d transparent=%d bloom=%d ui=%d recipePasses=%zu recipeResources=%zu",
                p.shadowEnabled, p.transparentEnabled, p.bloomEnabled,
                p.uiEnabled, recipe.passes.size(), recipe.resources.size());
            return result;
        }

    private:
        [[nodiscard]] static StandardFrameBuildResult AssembleResources(
            const std::unordered_map<std::string, rendergraph::RGResourceID>& mat)
        {
            auto lookup = [&](StandardFrameResourceID id) -> rendergraph::RGResourceID {
                const auto it = mat.find(std::string(StandardFrameResources::Name(id)));
                return it != mat.end() ? it->second : rendergraph::RG_INVALID_RESOURCE;
            };

            StandardFrameBuildResult res{};
            res.backbuffer     = lookup(StandardFrameResourceID::Backbuffer);
            res.shadowMap      = lookup(StandardFrameResourceID::ShadowMap);
            res.hdrSceneColor  = lookup(StandardFrameResourceID::HDRSceneColor);
            res.bloomInput     = lookup(StandardFrameResourceID::HDRSceneColor);
            res.bloomExtracted = lookup(StandardFrameResourceID::BloomExtracted);
            res.bloomBlurH     = lookup(StandardFrameResourceID::BloomBlurH);
            res.bloomBlurV     = lookup(StandardFrameResourceID::BloomBlurV);
            res.tonemapped     = lookup(StandardFrameResourceID::Tonemapped);
            res.uiOverlay      = lookup(StandardFrameResourceID::UIOverlay);
            return res;
        }

        [[nodiscard]] static FrameRecipeResourceDesc MakeResource(StandardFrameResourceID id,
            bool importedBackbuffer,
            uint32_t width,
            uint32_t height,
            Format format,
            RGResourceKind kind)
        {
            return FrameRecipeResourceDesc{
                std::string(StandardFrameResources::Name(id)),
                importedBackbuffer,
                width,
                height,
                format,
                kind
            };
        }

        [[nodiscard]] static FrameRecipePassDesc MakePass(StandardFramePassID passId,
            StandardFrameExecutorID executorId)
        {
            FrameRecipePassDesc pass{};
            pass.name         = std::string(StandardFramePassNames::Name(passId));
            pass.executorName = std::string(StandardFrameExecutors::Name(executorId));
            return pass;
        }

        static void AddAccess(FrameRecipePassDesc& pass,
            StandardFrameResourceID resourceId,
            FrameRecipeAccessKind access)
        {
            pass.accesses.push_back(FrameRecipeResourceAccess{
                std::string(StandardFrameResources::Name(resourceId)),
                access
            });
        }

        static void ConfigureRenderPass(FrameRecipePassDesc& pass,
            StandardFrameResourceID targetId,
            uint32_t width,
            uint32_t height,
            bool clearColor = false,
            bool clearDepth = false,
            std::array<float, 4> clearColorValue = { 0.f, 0.f, 0.f, 0.f })
        {
            pass.renderPass.enabled            = true;
            pass.renderPass.targetResourceName = std::string(StandardFrameResources::Name(targetId));
            pass.renderPass.viewportWidth      = width;
            pass.renderPass.viewportHeight     = height;
            pass.renderPass.clearColor         = clearColor;
            pass.renderPass.clearDepth         = clearDepth;
            pass.renderPass.clearColorValue    = clearColorValue;
        }

        static void AppendCoreResources(FrameRecipe& recipe, const BuildParams& p)
        {
            recipe.resources.push_back(MakeResource(StandardFrameResourceID::Backbuffer,
                true, p.viewportWidth, p.viewportHeight, Format::RGBA8_UNORM_SRGB, RGResourceKind::Backbuffer));
            recipe.resources.push_back(MakeResource(StandardFrameResourceID::HDRSceneColor,
                false, p.viewportWidth, p.viewportHeight, Format::RGBA16_FLOAT, RGResourceKind::RenderTarget));
        }

        static void AppendOptionalResources(FrameRecipe& recipe, const BuildParams& p)
        {
            if (p.shadowEnabled)
            {
                recipe.resources.push_back(MakeResource(StandardFrameResourceID::ShadowMap,
                    false, p.shadowMapSize, p.shadowMapSize, Format::D32_FLOAT, RGResourceKind::ShadowMap));
            }

            if (p.bloomEnabled)
            {
                recipe.resources.push_back(MakeResource(StandardFrameResourceID::BloomExtracted, false, p.bloomWidth,      p.bloomHeight,      Format::RGBA16_FLOAT,       RGResourceKind::ColorTexture));
                recipe.resources.push_back(MakeResource(StandardFrameResourceID::BloomBlurH,     false, p.bloomWidth,      p.bloomHeight,      Format::RGBA16_FLOAT,       RGResourceKind::ColorTexture));
                recipe.resources.push_back(MakeResource(StandardFrameResourceID::BloomBlurV,     false, p.bloomWidth,      p.bloomHeight,      Format::RGBA16_FLOAT,       RGResourceKind::ColorTexture));
                recipe.resources.push_back(MakeResource(StandardFrameResourceID::Tonemapped,     false, p.viewportWidth,   p.viewportHeight,   Format::RGBA8_UNORM_SRGB,   RGResourceKind::ColorTexture));
            }

            if (p.uiEnabled)
            {
                recipe.resources.push_back(MakeResource(StandardFrameResourceID::UIOverlay,
                    false, p.viewportWidth, p.viewportHeight, Format::RGBA8_UNORM_SRGB, RGResourceKind::ColorTexture));
            }
        }

        static void AppendCorePasses(FrameRecipe& recipe, const BuildParams& p)
        {
            if (p.shadowEnabled)
            {
                FrameRecipePassDesc shadow = MakePass(StandardFramePassID::Shadow, StandardFrameExecutorID::Shadow);
                AddAccess(shadow, StandardFrameResourceID::ShadowMap, FrameRecipeAccessKind::WriteDepthStencil);
                ConfigureRenderPass(shadow, StandardFrameResourceID::ShadowMap, p.shadowMapSize, p.shadowMapSize, false, true);
                recipe.passes.push_back(std::move(shadow));
            }

            FrameRecipePassDesc opaque = MakePass(StandardFramePassID::MainOpaque, StandardFrameExecutorID::Opaque);
            AddAccess(opaque, StandardFrameResourceID::HDRSceneColor, FrameRecipeAccessKind::WriteRenderTarget);
            if (p.shadowEnabled)
                AddAccess(opaque, StandardFrameResourceID::ShadowMap, FrameRecipeAccessKind::ReadDepthStencil);
            ConfigureRenderPass(opaque, StandardFrameResourceID::HDRSceneColor,
                p.viewportWidth, p.viewportHeight, true, true, p.clearColorValue);
            recipe.passes.push_back(std::move(opaque));

            if (p.transparentEnabled)
            {
                FrameRecipePassDesc transparent = MakePass(StandardFramePassID::Transparent, StandardFrameExecutorID::Transparent);
                AddAccess(transparent, StandardFrameResourceID::HDRSceneColor, FrameRecipeAccessKind::WriteRenderTarget);
                ConfigureRenderPass(transparent, StandardFrameResourceID::HDRSceneColor, p.viewportWidth, p.viewportHeight);
                recipe.passes.push_back(std::move(transparent));
            }

            FrameRecipePassDesc tonemap = MakePass(StandardFramePassID::Tonemap, StandardFrameExecutorID::Tonemap);
            AddAccess(tonemap, StandardFrameResourceID::HDRSceneColor, FrameRecipeAccessKind::ReadTexture);
            if (p.bloomEnabled)
            {
                AddAccess(tonemap, StandardFrameResourceID::BloomBlurV,  FrameRecipeAccessKind::ReadTexture);
                AddAccess(tonemap, StandardFrameResourceID::Tonemapped,  FrameRecipeAccessKind::WriteRenderTarget);
                ConfigureRenderPass(tonemap, StandardFrameResourceID::Tonemapped, p.viewportWidth, p.viewportHeight, true, false);
            }
            else
            {
                AddAccess(tonemap, StandardFrameResourceID::Backbuffer, FrameRecipeAccessKind::WriteRenderTarget);
                ConfigureRenderPass(tonemap, StandardFrameResourceID::Backbuffer, p.viewportWidth, p.viewportHeight, true, false);
            }
            recipe.passes.push_back(std::move(tonemap));

            FrameRecipePassDesc present = MakePass(StandardFramePassID::Present, StandardFrameExecutorID::Present);
            if (p.uiEnabled)
                AddAccess(present, StandardFrameResourceID::UIOverlay,  FrameRecipeAccessKind::ReadTexture);
            else if (p.bloomEnabled)
                AddAccess(present, StandardFrameResourceID::Tonemapped, FrameRecipeAccessKind::ReadTexture);
            AddAccess(present, StandardFrameResourceID::Backbuffer, FrameRecipeAccessKind::Present);
            recipe.passes.push_back(std::move(present));
        }

        static void AppendOptionalPasses(FrameRecipe& recipe, const BuildParams& p)
        {
            if (p.bloomEnabled)
            {
                FrameRecipePassDesc bloomExtract = MakePass(StandardFramePassID::BloomExtract, StandardFrameExecutorID::BloomExtract);
                AddAccess(bloomExtract, StandardFrameResourceID::HDRSceneColor,  FrameRecipeAccessKind::ReadTexture);
                AddAccess(bloomExtract, StandardFrameResourceID::BloomExtracted, FrameRecipeAccessKind::WriteRenderTarget);
                ConfigureRenderPass(bloomExtract, StandardFrameResourceID::BloomExtracted, p.bloomWidth, p.bloomHeight, true, false);
                recipe.passes.push_back(std::move(bloomExtract));

                FrameRecipePassDesc bloomBlurH = MakePass(StandardFramePassID::BloomBlurH, StandardFrameExecutorID::BloomBlurH);
                AddAccess(bloomBlurH, StandardFrameResourceID::BloomExtracted, FrameRecipeAccessKind::ReadTexture);
                AddAccess(bloomBlurH, StandardFrameResourceID::BloomBlurH,     FrameRecipeAccessKind::WriteRenderTarget);
                ConfigureRenderPass(bloomBlurH, StandardFrameResourceID::BloomBlurH, p.bloomWidth, p.bloomHeight, true, false);
                recipe.passes.push_back(std::move(bloomBlurH));

                FrameRecipePassDesc bloomBlurV = MakePass(StandardFramePassID::BloomBlurV, StandardFrameExecutorID::BloomBlurV);
                AddAccess(bloomBlurV, StandardFrameResourceID::BloomBlurH, FrameRecipeAccessKind::ReadTexture);
                AddAccess(bloomBlurV, StandardFrameResourceID::BloomBlurV, FrameRecipeAccessKind::WriteRenderTarget);
                ConfigureRenderPass(bloomBlurV, StandardFrameResourceID::BloomBlurV, p.bloomWidth, p.bloomHeight, true, false);
                recipe.passes.push_back(std::move(bloomBlurV));
            }

            if (p.uiEnabled)
            {
                FrameRecipePassDesc ui = MakePass(StandardFramePassID::UI, StandardFrameExecutorID::UI);
                AddAccess(ui,
                    p.bloomEnabled ? StandardFrameResourceID::Tonemapped : StandardFrameResourceID::Backbuffer,
                    FrameRecipeAccessKind::ReadTexture);
                AddAccess(ui, StandardFrameResourceID::UIOverlay, FrameRecipeAccessKind::WriteRenderTarget);
                ConfigureRenderPass(ui, StandardFrameResourceID::UIOverlay, p.viewportWidth, p.viewportHeight, true, false);
                recipe.passes.push_back(std::move(ui));
            }
        }
    };

} // namespace engine::renderer
