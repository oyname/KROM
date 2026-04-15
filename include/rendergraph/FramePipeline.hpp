#pragma once
// =============================================================================
// KROM Engine - rendergraph/FramePipeline.hpp
// Frame-Pipeline: registriert Passes und Ressourcen im RenderGraph.
//
// Minimalpfad (alle Flags false):
//   OpaquePass → TonemapPass(→Backbuffer) → PresentPass(State-Transition)
//
// Erweiterter Pfad:
//   ShadowPass → OpaquePass → TransparentPass → ParticlesPass →
//   BloomExtract → BloomBlurH → BloomBlurV →
//   TonemapPass(→Backbuffer) → UIPass → PresentPass
//
// Jeder Pass ruft BeginRenderPass/EndRenderPass auf - Draws landen immer
// in einem aktiven Render-Target.
//
// hdrSceneColor wird als RGResourceKind::RenderTarget erstellt, was in
// GpuResourceRuntime automatisch einen eingebetteten Depth-Buffer erzeugt.
// =============================================================================
#include "rendergraph/RenderGraph.hpp"
#include "core/Debug.hpp"

namespace engine::rendergraph {

struct FramePipelineResources
{
    RGResourceID shadowMap       = RG_INVALID_RESOURCE;
    // hdrSceneColor ist RenderTarget (hat eingebetteten Depth) - kein separater depthBuffer
    RGResourceID hdrSceneColor   = RG_INVALID_RESOURCE;

    RGResourceID bloomInput      = RG_INVALID_RESOURCE;
    RGResourceID bloomExtracted  = RG_INVALID_RESOURCE;
    RGResourceID bloomBlurH      = RG_INVALID_RESOURCE;
    RGResourceID bloomBlurV      = RG_INVALID_RESOURCE;

    // tonemapped: nur wenn bloomEnabled - sonst schreibt TonemapPass direkt in backbuffer
    RGResourceID tonemapped      = RG_INVALID_RESOURCE;
    RGResourceID uiOverlay       = RG_INVALID_RESOURCE;
    RGResourceID backbuffer      = RG_INVALID_RESOURCE;

    // Alias für abwärtskompatiblen Code der depthBuffer erwartet
    RGResourceID depthBuffer     = RG_INVALID_RESOURCE; // == hdrSceneColor (embedded depth)
};

struct FramePipelineCallbacks
{
    FramePipelineCallbacks() = default;
    std::function<void(const RGExecContext&)> onShadowPass;
    std::function<void(const RGExecContext&)> onOpaquePass;
    std::function<void(const RGExecContext&)> onTransparentPass;
    std::function<void(const RGExecContext&)> onParticlesPass;
    std::function<void(const RGExecContext&)> onBloomExtract;
    std::function<void(const RGExecContext&)> onBloomBlurH;
    std::function<void(const RGExecContext&)> onBloomBlurV;
    std::function<void(const RGExecContext&)> onTonemap;
    std::function<void(const RGExecContext&)> onUI;
    std::function<void(const RGExecContext&)> onPresent;
};

class FramePipelineBuilder
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

        // Alle Features standardmäßig aus - Minimalpfad funktioniert ohne Callbacks
        bool shadowEnabled      = false;
        bool bloomEnabled       = false;
        bool transparentEnabled = false;
        bool particleEnabled    = false;
        bool uiEnabled          = false;
    };

    static FramePipelineResources Build(RenderGraph& rg,
                                        const BuildParams& p,
                                        const FramePipelineCallbacks& cb)
    {
        FramePipelineResources res;

        // --- Ressourcen ---------------------------------------------------

        res.backbuffer = rg.ImportBackbuffer(p.backbufferRT, p.backbufferTex,
                                              p.viewportWidth, p.viewportHeight);

        if (p.shadowEnabled)
        {
            res.shadowMap = rg.CreateTransientRenderTarget(
                "ShadowMap", p.shadowMapSize, p.shadowMapSize,
                renderer::Format::D32_FLOAT, RGResourceKind::ShadowMap);
        }

        // RenderTarget (nicht ColorTexture!) → GpuResourceRuntime erzeugt
        // einen RT mit eingebettetem D24-Depth-Buffer.
        res.hdrSceneColor = rg.CreateTransientRenderTarget(
            "HDRSceneColor", p.viewportWidth, p.viewportHeight,
            renderer::Format::RGBA16_FLOAT,
            RGResourceKind::RenderTarget);
        res.depthBuffer = res.hdrSceneColor; // Alias - Depth ist eingebettet

        if (p.bloomEnabled)
        {
            res.bloomExtracted = rg.CreateTransientRenderTarget(
                "BloomExtracted", p.bloomWidth, p.bloomHeight,
                renderer::Format::RGBA16_FLOAT, RGResourceKind::ColorTexture);
            res.bloomBlurH = rg.CreateTransientRenderTarget(
                "BloomBlurH", p.bloomWidth, p.bloomHeight,
                renderer::Format::RGBA16_FLOAT, RGResourceKind::ColorTexture);
            res.bloomBlurV = rg.CreateTransientRenderTarget(
                "BloomBlurV", p.bloomWidth, p.bloomHeight,
                renderer::Format::RGBA16_FLOAT, RGResourceKind::ColorTexture);
            // Mit Bloom brauchen wir einen Zwischenbuffer für TonemapPass
            res.tonemapped = rg.CreateTransientRenderTarget(
                "Tonemapped", p.viewportWidth, p.viewportHeight,
                renderer::Format::RGBA8_UNORM_SRGB, RGResourceKind::ColorTexture);
        }

        if (p.uiEnabled)
        {
            res.uiOverlay = rg.CreateTransientRenderTarget(
                "UIOverlay", p.viewportWidth, p.viewportHeight,
                renderer::Format::RGBA8_UNORM_SRGB, RGResourceKind::ColorTexture);
        }

        // --- Passes -------------------------------------------------------

        // 1. ShadowPass
        if (p.shadowEnabled)
        {
            auto fn = cb.onShadowPass;
            auto shadowRT = res.shadowMap;
            const uint32_t shadowSize = p.shadowMapSize;
            rg.AddPass("ShadowPass", RGPassType::Graphics)
                .WriteDepthStencil(res.shadowMap)
                .Execute([fn, shadowRT, shadowSize](const RGExecContext& ctx) {
                    renderer::ICommandList::RenderPassBeginInfo rp{};
                    rp.renderTarget = ctx.GetRenderTarget(shadowRT);
                    rp.clearColor   = false;
                    rp.clearDepth   = true;
                    rp.depthClear.depth = 1.f;
                    ctx.cmd->BeginRenderPass(rp);
                    ctx.cmd->SetViewport(0.f, 0.f, static_cast<float>(shadowSize), static_cast<float>(shadowSize), 0.f, 1.f);
                    ctx.cmd->SetScissor(0, 0, shadowSize, shadowSize);
                    if (fn) fn(ctx);
                    ctx.cmd->EndRenderPass();
                });
        }

        // 2. MainOpaquePass
        {
            auto fn    = cb.onOpaquePass;
            auto hdrID = res.hdrSceneColor;
            const uint32_t viewportWidth  = p.viewportWidth;
            const uint32_t viewportHeight = p.viewportHeight;
            auto builder = rg.AddPass("MainOpaquePass", RGPassType::Graphics)
                .WriteRenderTarget(res.hdrSceneColor);
            if (p.shadowEnabled) builder.ReadTexture(res.shadowMap);

            builder.Execute([fn, hdrID, viewportWidth, viewportHeight](const RGExecContext& ctx) {
                // hdrSceneColor hat eingebetteten Depth → BeginRenderPass
                // mit dem RT-Handle richtet Farb- UND Tiefenpuffer ein.
                renderer::ICommandList::RenderPassBeginInfo rp{};
                rp.renderTarget    = ctx.GetRenderTarget(hdrID);
                rp.clearColor      = true;
                rp.clearDepth      = true;
                rp.colorClear.color[0] = 0.3f;
                rp.colorClear.color[1] = 0.3f;
                rp.colorClear.color[2] = 0.3f;
                rp.colorClear.color[3] = 1.f;
                rp.depthClear.depth    = 1.f;
                ctx.cmd->BeginRenderPass(rp);
                ctx.cmd->SetViewport(0.f, 0.f, static_cast<float>(viewportWidth), static_cast<float>(viewportHeight), 0.f, 1.f);
                ctx.cmd->SetScissor(0, 0, viewportWidth, viewportHeight);
                if (fn) fn(ctx);
                ctx.cmd->EndRenderPass();
            });
        }

        // 3. TransparentPass - nur wenn explizit aktiviert.
        // WriteRenderTarget nach OpaquePass = Write-after-Write; ohne Transparent-
        // Objekte im Frame ist der Pass sinnlos und der Validator würde
        // bei ReadRenderTarget+WriteRenderTarget (beide → RenderTarget-State)
        // "mehrfache schreibende Zugriffe" melden.
        if (p.transparentEnabled)
        {
            auto fn    = cb.onTransparentPass;
            auto hdrID = res.hdrSceneColor;
            const uint32_t viewportWidth  = p.viewportWidth;
            const uint32_t viewportHeight = p.viewportHeight;
            rg.AddPass("TransparentPass", RGPassType::Graphics)
                .WriteRenderTarget(res.hdrSceneColor)
                .Execute([fn, hdrID, viewportWidth, viewportHeight](const RGExecContext& ctx) {
                    renderer::ICommandList::RenderPassBeginInfo rp{};
                    rp.renderTarget = ctx.GetRenderTarget(hdrID);
                    rp.clearColor   = false;
                    rp.clearDepth   = false;
                    ctx.cmd->BeginRenderPass(rp);
                    ctx.cmd->SetViewport(0.f, 0.f, static_cast<float>(viewportWidth), static_cast<float>(viewportHeight), 0.f, 1.f);
                    ctx.cmd->SetScissor(0, 0, viewportWidth, viewportHeight);
                    if (fn) fn(ctx);
                    ctx.cmd->EndRenderPass();
                });
        }

        // 4. ParticlesPass
        // Partikel blenden direkt in hdrSceneColor ein. In dieser Architektur
        // darf dieselbe Ressource innerhalb eines Passes nicht gleichzeitig als
        // ReadRenderTarget und WriteRenderTarget deklariert werden; das ist fuer
        // den Validator ein mehrfacher Schreibzugriff. Fuer den normalen
        // Blend-Renderpfad reicht der RT-Write vollstaendig aus.
        if (p.particleEnabled)
        {
            auto fn    = cb.onParticlesPass;
            auto hdrID = res.hdrSceneColor;
            const uint32_t viewportWidth  = p.viewportWidth;
            const uint32_t viewportHeight = p.viewportHeight;
            rg.AddPass("ParticlesPass", RGPassType::Graphics)
                .WriteRenderTarget(res.hdrSceneColor)
                .Execute([fn, hdrID, viewportWidth, viewportHeight](const RGExecContext& ctx) {
                    renderer::ICommandList::RenderPassBeginInfo rp{};
                    rp.renderTarget = ctx.GetRenderTarget(hdrID);
                    rp.clearColor   = false;
                    rp.clearDepth   = false;
                    ctx.cmd->BeginRenderPass(rp);
                    ctx.cmd->SetViewport(0.f, 0.f, static_cast<float>(viewportWidth), static_cast<float>(viewportHeight), 0.f, 1.f);
                    ctx.cmd->SetScissor(0, 0, viewportWidth, viewportHeight);
                    if (fn) fn(ctx);
                    ctx.cmd->EndRenderPass();
                });
        }

        // 5-7. Bloom-Passes
        if (p.bloomEnabled)
        {
            auto fnExtract = cb.onBloomExtract;
            auto hdrID  = res.hdrSceneColor;
            auto extID  = res.bloomExtracted;
            const uint32_t bloomWidth  = p.bloomWidth;
            const uint32_t bloomHeight = p.bloomHeight;
            rg.AddPass("BloomExtractPass", RGPassType::Graphics)
                .ReadTexture(res.hdrSceneColor)
                .WriteRenderTarget(res.bloomExtracted)
                .Execute([fnExtract, extID, bloomWidth, bloomHeight](const RGExecContext& ctx) {
                    renderer::ICommandList::RenderPassBeginInfo rp{};
                    rp.renderTarget = ctx.GetRenderTarget(extID);
                    rp.clearColor   = true;
                    rp.clearDepth   = false;
                    ctx.cmd->BeginRenderPass(rp);
                    ctx.cmd->SetViewport(0.f, 0.f, static_cast<float>(bloomWidth), static_cast<float>(bloomHeight), 0.f, 1.f);
                    ctx.cmd->SetScissor(0, 0, bloomWidth, bloomHeight);
                    if (fnExtract) fnExtract(ctx);
                    ctx.cmd->EndRenderPass();
                });

            auto fnH   = cb.onBloomBlurH;
            auto blurH = res.bloomBlurH;
            rg.AddPass("BloomBlurPassH", RGPassType::Graphics)
                .ReadTexture(res.bloomExtracted)
                .WriteRenderTarget(res.bloomBlurH)
                .Execute([fnH, blurH, bloomWidth, bloomHeight](const RGExecContext& ctx) {
                    renderer::ICommandList::RenderPassBeginInfo rp{};
                    rp.renderTarget = ctx.GetRenderTarget(blurH);
                    rp.clearColor   = true;
                    rp.clearDepth   = false;
                    ctx.cmd->BeginRenderPass(rp);
                    ctx.cmd->SetViewport(0.f, 0.f, static_cast<float>(bloomWidth), static_cast<float>(bloomHeight), 0.f, 1.f);
                    ctx.cmd->SetScissor(0, 0, bloomWidth, bloomHeight);
                    if (fnH) fnH(ctx);
                    ctx.cmd->EndRenderPass();
                });

            auto fnV   = cb.onBloomBlurV;
            auto blurV = res.bloomBlurV;
            rg.AddPass("BloomBlurPassV", RGPassType::Graphics)
                .ReadTexture(res.bloomBlurH)
                .WriteRenderTarget(res.bloomBlurV)
                .Execute([fnV, blurV, bloomWidth, bloomHeight](const RGExecContext& ctx) {
                    renderer::ICommandList::RenderPassBeginInfo rp{};
                    rp.renderTarget = ctx.GetRenderTarget(blurV);
                    rp.clearColor   = true;
                    rp.clearDepth   = false;
                    ctx.cmd->BeginRenderPass(rp);
                    ctx.cmd->SetViewport(0.f, 0.f, static_cast<float>(bloomWidth), static_cast<float>(bloomHeight), 0.f, 1.f);
                    ctx.cmd->SetScissor(0, 0, bloomWidth, bloomHeight);
                    if (fnV) fnV(ctx);
                    ctx.cmd->EndRenderPass();
                });
        }

        // 8. TonemapPass
        // Ohne Bloom: direkt in Backbuffer rendern (kein Intermediate nötig).
        // Mit Bloom: in tonemapped-RT, PresentPass blit folgt.
        {
            auto fn = cb.onTonemap;

            if (!p.bloomEnabled)
            {
                // Kein Bloom → Tonemap schreibt direkt in den Backbuffer.
                auto hdrID = res.hdrSceneColor;
                auto bbID  = res.backbuffer;
                const uint32_t viewportWidth  = p.viewportWidth;
                const uint32_t viewportHeight = p.viewportHeight;
                rg.AddPass("TonemapPass", RGPassType::Graphics)
                    .ReadTexture(res.hdrSceneColor)
                    .WriteRenderTarget(res.backbuffer)
                    .Execute([fn, hdrID, bbID, viewportWidth, viewportHeight](const RGExecContext& ctx) {
                        renderer::ICommandList::RenderPassBeginInfo rp{};
                        rp.renderTarget = ctx.GetRenderTarget(bbID);
                        rp.clearColor   = true;
                        rp.clearDepth   = false;
                        ctx.cmd->BeginRenderPass(rp);
                        ctx.cmd->SetViewport(0.f, 0.f, static_cast<float>(viewportWidth), static_cast<float>(viewportHeight), 0.f, 1.f);
                        ctx.cmd->SetScissor(0, 0, viewportWidth, viewportHeight);
                        if (fn) fn(ctx);
                        ctx.cmd->EndRenderPass();
                    });
            }
            else
            {
                // Mit Bloom → in Intermediate, dann PresentPass blit.
                auto hdrID = res.hdrSceneColor;
                auto blurV = res.bloomBlurV;
                auto tmID  = res.tonemapped;
                const uint32_t viewportWidth  = p.viewportWidth;
                const uint32_t viewportHeight = p.viewportHeight;
                rg.AddPass("TonemapPass", RGPassType::Graphics)
                    .ReadTexture(res.hdrSceneColor)
                    .ReadTexture(res.bloomBlurV)
                    .WriteRenderTarget(res.tonemapped)
                    .Execute([fn, hdrID, blurV, tmID, viewportWidth, viewportHeight](const RGExecContext& ctx) {
                        renderer::ICommandList::RenderPassBeginInfo rp{};
                        rp.renderTarget = ctx.GetRenderTarget(tmID);
                        rp.clearColor   = true;
                        rp.clearDepth   = false;
                        ctx.cmd->BeginRenderPass(rp);
                        ctx.cmd->SetViewport(0.f, 0.f, static_cast<float>(viewportWidth), static_cast<float>(viewportHeight), 0.f, 1.f);
                        ctx.cmd->SetScissor(0, 0, viewportWidth, viewportHeight);
                        if (fn) fn(ctx);
                        ctx.cmd->EndRenderPass();
                    });
            }
        }

        // 9. UIPass (optional)
        if (p.uiEnabled)
        {
            auto fn   = cb.onUI;
            auto uiID = res.uiOverlay;
            const uint32_t viewportWidth  = p.viewportWidth;
            const uint32_t viewportHeight = p.viewportHeight;
            // UI liest aus tonemapped (bloom) oder backbuffer (kein bloom)
            auto srcID = p.bloomEnabled ? res.tonemapped : res.backbuffer;
            rg.AddPass("UIPass", RGPassType::Graphics)
                .ReadTexture(srcID)
                .WriteRenderTarget(res.uiOverlay)
                .Execute([fn, uiID, viewportWidth, viewportHeight](const RGExecContext& ctx) {
                    renderer::ICommandList::RenderPassBeginInfo rp{};
                    rp.renderTarget = ctx.GetRenderTarget(uiID);
                    rp.clearColor   = true;
                    rp.clearDepth   = false;
                    ctx.cmd->BeginRenderPass(rp);
                    ctx.cmd->SetViewport(0.f, 0.f, static_cast<float>(viewportWidth), static_cast<float>(viewportHeight), 0.f, 1.f);
                    ctx.cmd->SetScissor(0, 0, viewportWidth, viewportHeight);
                    if (fn) fn(ctx);
                    ctx.cmd->EndRenderPass();
                });
        }

        // 10. PresentPass - nur State-Transition, kein Draw.
        // Die eigentliche Ausgabe ist bereits im Backbuffer (kein Bloom)
        // oder muss noch per onPresent geblit werden (Bloom + UI).
        // Solange die Runtime frameweit nur eine Graphics-CommandList ausführt,
        // muss auch der Present-Pfad auf Graphics klassifiziert bleiben.
        // Eine Transfer-Klassifikation wäre erst dann korrekt, wenn der Pass
        // tatsächlich über einen separaten Transfer-Command-/Submit-Pfad läuft.
        {
            auto fn = cb.onPresent;
            auto presentBuilder = rg.AddPass("PresentPass", RGPassType::Graphics)
                .Present(res.backbuffer);

            // Was präsentiert wird hängt von den aktiven Features ab
            if (p.uiEnabled)
                presentBuilder.ReadTexture(res.uiOverlay);
            else if (p.bloomEnabled)
                presentBuilder.ReadTexture(res.tonemapped);
            // Kein Bloom, kein UI → Backbuffer bereits befüllt durch TonemapPass

            presentBuilder.Execute([fn](const RGExecContext& ctx) {
                // Im Minimalpfad: kein Draw nötig, TonemapPass hat
                // bereits in den Backbuffer gerendert.
                if (fn) fn(ctx);
            });
        }

        Debug::LogVerbose("rendergraph/FramePipeline.cpp: FramePipeline built - "
                    "shadow=%d transparent=%d bloom=%d particles=%d ui=%d",
                    p.shadowEnabled, p.transparentEnabled, p.bloomEnabled,
                    p.particleEnabled, p.uiEnabled);

        return res;
    }
};

} // namespace engine::rendergraph
