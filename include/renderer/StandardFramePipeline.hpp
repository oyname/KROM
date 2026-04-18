#pragma once
// =============================================================================
// KROM Engine - renderer/StandardFramePipeline.hpp
// Standard-Rezept fuer die aktuelle Default-Frame-Abfolge. Die generischen
// Recipe-/Compile-Mechanismen leben in renderer/RenderPipelineRecipe.hpp.
// =============================================================================
#include "core/Debug.hpp"
#include "renderer/RenderPipelineRecipe.hpp"
#include <string>
#include <string_view>

namespace engine::renderer {

namespace StandardFrameExecutors {

inline constexpr std::string_view Shadow       = "frame.shadow";
inline constexpr std::string_view Opaque       = "frame.opaque";
inline constexpr std::string_view Transparent  = "frame.transparent";
inline constexpr std::string_view BloomExtract = "frame.bloom_extract";
inline constexpr std::string_view BloomBlurH   = "frame.bloom_blur_h";
inline constexpr std::string_view BloomBlurV   = "frame.bloom_blur_v";
inline constexpr std::string_view Tonemap      = "frame.tonemap";
inline constexpr std::string_view UI           = "frame.ui";
inline constexpr std::string_view Present      = "frame.present";

} // namespace StandardFrameExecutors

class StandardFrameRecipeBuilder
{
public:
    struct BuildParams
    {
        uint32_t viewportWidth   = 1280u;
        uint32_t viewportHeight  = 720u;
        uint32_t shadowMapSize   = 2048u;
        uint32_t bloomWidth      = 640u;
        uint32_t bloomHeight     = 360u;

        RenderTargetHandle backbufferRT;
        TextureHandle      backbufferTex;

        bool shadowEnabled      = false;
        bool bloomEnabled       = false;
        bool transparentEnabled = false;
        bool uiEnabled          = false;
    };

    [[nodiscard]] static FrameRecipe BuildRecipe(const BuildParams& p)
    {
        FrameRecipe recipe{};

        recipe.resources.push_back(FrameRecipeResourceDesc{
            "Backbuffer", true, p.viewportWidth, p.viewportHeight, Format::RGBA8_UNORM_SRGB, RGResourceKind::Backbuffer
        });

        if (p.shadowEnabled)
        {
            recipe.resources.push_back(FrameRecipeResourceDesc{
                "ShadowMap", false, p.shadowMapSize, p.shadowMapSize, Format::D32_FLOAT, RGResourceKind::ShadowMap
            });
        }

        recipe.resources.push_back(FrameRecipeResourceDesc{
            "HDRSceneColor", false, p.viewportWidth, p.viewportHeight, Format::RGBA16_FLOAT, RGResourceKind::RenderTarget
        });

        if (p.bloomEnabled)
        {
            recipe.resources.push_back(FrameRecipeResourceDesc{
                "BloomExtracted", false, p.bloomWidth, p.bloomHeight, Format::RGBA16_FLOAT, RGResourceKind::ColorTexture
            });
            recipe.resources.push_back(FrameRecipeResourceDesc{
                "BloomBlurH", false, p.bloomWidth, p.bloomHeight, Format::RGBA16_FLOAT, RGResourceKind::ColorTexture
            });
            recipe.resources.push_back(FrameRecipeResourceDesc{
                "BloomBlurV", false, p.bloomWidth, p.bloomHeight, Format::RGBA16_FLOAT, RGResourceKind::ColorTexture
            });
            recipe.resources.push_back(FrameRecipeResourceDesc{
                "Tonemapped", false, p.viewportWidth, p.viewportHeight, Format::RGBA8_UNORM_SRGB, RGResourceKind::ColorTexture
            });
        }

        if (p.uiEnabled)
        {
            recipe.resources.push_back(FrameRecipeResourceDesc{
                "UIOverlay", false, p.viewportWidth, p.viewportHeight, Format::RGBA8_UNORM_SRGB, RGResourceKind::ColorTexture
            });
        }

        if (p.shadowEnabled)
        {
            FrameRecipePassDesc pass{};
            pass.name = "ShadowPass";
            pass.executorName = std::string(StandardFrameExecutors::Shadow);
            pass.accesses.push_back({"ShadowMap", FrameRecipeAccessKind::WriteDepthStencil});
            pass.renderPass.enabled = true;
            pass.renderPass.targetResourceName = "ShadowMap";
            pass.renderPass.viewportWidth = p.shadowMapSize;
            pass.renderPass.viewportHeight = p.shadowMapSize;
            pass.renderPass.clearDepth = true;
            recipe.passes.push_back(std::move(pass));
        }

        {
            FrameRecipePassDesc pass{};
            pass.name = "MainOpaquePass";
            pass.executorName = std::string(StandardFrameExecutors::Opaque);
            pass.accesses.push_back({"HDRSceneColor", FrameRecipeAccessKind::WriteRenderTarget});
            if (p.shadowEnabled)
                pass.accesses.push_back({"ShadowMap", FrameRecipeAccessKind::ReadTexture});
            pass.renderPass.enabled = true;
            pass.renderPass.targetResourceName = "HDRSceneColor";
            pass.renderPass.viewportWidth = p.viewportWidth;
            pass.renderPass.viewportHeight = p.viewportHeight;
            pass.renderPass.clearColor = true;
            pass.renderPass.clearDepth = true;
            pass.renderPass.clearColorValue = {0.3f, 0.3f, 0.3f, 1.f};
            recipe.passes.push_back(std::move(pass));
        }

        if (p.transparentEnabled)
        {
            FrameRecipePassDesc pass{};
            pass.name = "TransparentPass";
            pass.executorName = std::string(StandardFrameExecutors::Transparent);
            pass.accesses.push_back({"HDRSceneColor", FrameRecipeAccessKind::WriteRenderTarget});
            pass.renderPass.enabled = true;
            pass.renderPass.targetResourceName = "HDRSceneColor";
            pass.renderPass.viewportWidth = p.viewportWidth;
            pass.renderPass.viewportHeight = p.viewportHeight;
            recipe.passes.push_back(std::move(pass));
        }

        if (p.bloomEnabled)
        {
            {
                FrameRecipePassDesc pass{};
                pass.name = "BloomExtractPass";
                pass.executorName = std::string(StandardFrameExecutors::BloomExtract);
                pass.accesses.push_back({"HDRSceneColor", FrameRecipeAccessKind::ReadTexture});
                pass.accesses.push_back({"BloomExtracted", FrameRecipeAccessKind::WriteRenderTarget});
                pass.renderPass.enabled = true;
                pass.renderPass.targetResourceName = "BloomExtracted";
                pass.renderPass.viewportWidth = p.bloomWidth;
                pass.renderPass.viewportHeight = p.bloomHeight;
                pass.renderPass.clearColor = true;
                recipe.passes.push_back(std::move(pass));
            }

            {
                FrameRecipePassDesc pass{};
                pass.name = "BloomBlurPassH";
                pass.executorName = std::string(StandardFrameExecutors::BloomBlurH);
                pass.accesses.push_back({"BloomExtracted", FrameRecipeAccessKind::ReadTexture});
                pass.accesses.push_back({"BloomBlurH", FrameRecipeAccessKind::WriteRenderTarget});
                pass.renderPass.enabled = true;
                pass.renderPass.targetResourceName = "BloomBlurH";
                pass.renderPass.viewportWidth = p.bloomWidth;
                pass.renderPass.viewportHeight = p.bloomHeight;
                pass.renderPass.clearColor = true;
                recipe.passes.push_back(std::move(pass));
            }

            {
                FrameRecipePassDesc pass{};
                pass.name = "BloomBlurPassV";
                pass.executorName = std::string(StandardFrameExecutors::BloomBlurV);
                pass.accesses.push_back({"BloomBlurH", FrameRecipeAccessKind::ReadTexture});
                pass.accesses.push_back({"BloomBlurV", FrameRecipeAccessKind::WriteRenderTarget});
                pass.renderPass.enabled = true;
                pass.renderPass.targetResourceName = "BloomBlurV";
                pass.renderPass.viewportWidth = p.bloomWidth;
                pass.renderPass.viewportHeight = p.bloomHeight;
                pass.renderPass.clearColor = true;
                recipe.passes.push_back(std::move(pass));
            }
        }

        {
            FrameRecipePassDesc pass{};
            pass.name = "TonemapPass";
            pass.executorName = std::string(StandardFrameExecutors::Tonemap);
            pass.accesses.push_back({"HDRSceneColor", FrameRecipeAccessKind::ReadTexture});
            if (p.bloomEnabled)
            {
                pass.accesses.push_back({"BloomBlurV", FrameRecipeAccessKind::ReadTexture});
                pass.accesses.push_back({"Tonemapped", FrameRecipeAccessKind::WriteRenderTarget});
                pass.renderPass.targetResourceName = "Tonemapped";
            }
            else
            {
                pass.accesses.push_back({"Backbuffer", FrameRecipeAccessKind::WriteRenderTarget});
                pass.renderPass.targetResourceName = "Backbuffer";
            }
            pass.renderPass.enabled = true;
            pass.renderPass.viewportWidth = p.viewportWidth;
            pass.renderPass.viewportHeight = p.viewportHeight;
            pass.renderPass.clearColor = true;
            recipe.passes.push_back(std::move(pass));
        }

        if (p.uiEnabled)
        {
            FrameRecipePassDesc pass{};
            pass.name = "UIPass";
            pass.executorName = std::string(StandardFrameExecutors::UI);
            pass.accesses.push_back({p.bloomEnabled ? "Tonemapped" : "Backbuffer", FrameRecipeAccessKind::ReadTexture});
            pass.accesses.push_back({"UIOverlay", FrameRecipeAccessKind::WriteRenderTarget});
            pass.renderPass.enabled = true;
            pass.renderPass.targetResourceName = "UIOverlay";
            pass.renderPass.viewportWidth = p.viewportWidth;
            pass.renderPass.viewportHeight = p.viewportHeight;
            pass.renderPass.clearColor = true;
            recipe.passes.push_back(std::move(pass));
        }

        {
            FrameRecipePassDesc pass{};
            pass.name = "PresentPass";
            pass.executorName = std::string(StandardFrameExecutors::Present);
            if (p.uiEnabled)
                pass.accesses.push_back({"UIOverlay", FrameRecipeAccessKind::ReadTexture});
            else if (p.bloomEnabled)
                pass.accesses.push_back({"Tonemapped", FrameRecipeAccessKind::ReadTexture});
            pass.accesses.push_back({"Backbuffer", FrameRecipeAccessKind::Present});
            recipe.passes.push_back(std::move(pass));
        }

        return recipe;
    }

    static FramePipelineResources Build(RenderGraph& rg,
                                        const BuildParams& p,
                                        const FramePipelineCallbacks& executors)
    {
        const FrameRecipe recipe = BuildRecipe(p);
        const FrameRecipeCompileParams params{p.backbufferRT, p.backbufferTex};
        FramePipelineResources resources = FrameRecipeCompiler::Build(rg, params, recipe, executors);

        Debug::LogVerbose("rendergraph/StandardFramePipeline.cpp: recipe built - "
                          "shadow=%d transparent=%d bloom=%d ui=%d recipePasses=%zu recipeResources=%zu",
                          p.shadowEnabled, p.transparentEnabled, p.bloomEnabled,
                          p.uiEnabled, recipe.passes.size(), recipe.resources.size());
        return resources;
    }
};

} // namespace engine::renderer
