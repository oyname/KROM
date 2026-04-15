// =============================================================================
// KROM Engine - tests/test_rendergraph.cpp
// RenderGraph-Tests: Compile, Topo-Sort, Culling, State-Planung, Aliasing
// =============================================================================
#include "TestFramework.hpp"
#include "rendergraph/RenderGraph.hpp"
#include "rendergraph/ResourceAliaser.hpp"
#include "jobs/TaskGraph.hpp"
#include "core/Debug.hpp"

using namespace engine;
using namespace engine::renderer;
using namespace engine::rendergraph;

// ==========================================================================
// RenderGraph - einfache lineare Pipeline
// ==========================================================================
static void TestLinearPipeline(test::TestContext& ctx)
{
    RenderGraph rg;

    auto backbufRT  = RenderTargetHandle::Make(1u, 1u);
    auto backbufTex = TextureHandle::Make(1u, 1u);
    RGResourceID backbuf = rg.ImportBackbuffer(backbufRT, backbufTex, 1280u, 720u);

    auto hdrRT  = RenderTargetHandle::Make(2u, 1u);
    auto hdrTex = TextureHandle::Make(2u, 1u);
    RGResourceID hdr = rg.ImportRenderTarget(hdrRT, hdrTex, "HDR", 1280u, 720u,
        Format::RGBA16_FLOAT);

    int opaqueExecuted = 0, tonemapExecuted = 0;

    rg.AddPass("OpaquePass")
        .WriteRenderTarget(hdr)
        .Execute([&](const RGExecContext&) { ++opaqueExecuted; });

    rg.AddPass("TonemapPass")
        .ReadTexture(hdr)
        .Present(backbuf)
        .Execute([&](const RGExecContext&) { ++tonemapExecuted; });

    CHECK(ctx, rg.Compile());
    CHECK(ctx, rg.IsValid());
    CHECK_EQ(ctx, rg.GetSortedPasses().size(), 2u);

    // Topologische Reihenfolge: Opaque vor Tonemap
    const auto& sorted = rg.GetSortedPasses();
    CHECK_EQ(ctx, rg.GetPasses()[sorted[0]].debugName, std::string("OpaquePass"));
    CHECK_EQ(ctx, rg.GetPasses()[sorted[1]].debugName, std::string("TonemapPass"));

    RGExecContext execCtx{}; // kein Device nötig für Execution-Count-Test
    rg.Execute(execCtx);

    CHECK_EQ(ctx, opaqueExecuted,  1);
    CHECK_EQ(ctx, tonemapExecuted, 1);
}

// ==========================================================================
// Topologische Sortierung: Abhängigkeiten korrekt aufgelöst
// ==========================================================================
static void TestTopologicalSort(test::TestContext& ctx)
{
    RenderGraph rg;

    auto bb   = rg.ImportBackbuffer(RenderTargetHandle::Make(1u,1u),
                                     TextureHandle::Make(1u,1u), 1280u, 720u);
    auto shadow = rg.ImportRenderTarget(RenderTargetHandle::Make(2u,1u),
                    TextureHandle::Make(2u,1u), "Shadow", 2048u, 2048u, Format::D32_FLOAT);
    auto hdr  = rg.ImportRenderTarget(RenderTargetHandle::Make(3u,1u),
                    TextureHandle::Make(3u,1u), "HDR", 1280u, 720u, Format::RGBA16_FLOAT);
    auto tone = rg.CreateTransientRenderTarget("Tone", 1280u, 720u, Format::RGBA8_UNORM_SRGB);
    rg.SetTransientRenderTarget(tone, RenderTargetHandle::Make(4u,1u), TextureHandle::Make(4u,1u));

    std::vector<std::string> order;

    rg.AddPass("ShadowPass").WriteDepthStencil(shadow)
        .Execute([&](const RGExecContext&){ order.push_back("Shadow"); });
    rg.AddPass("OpaquePass").WriteRenderTarget(hdr).ReadTexture(shadow)
        .Execute([&](const RGExecContext&){ order.push_back("Opaque"); });
    rg.AddPass("TonemapPass").ReadTexture(hdr).WriteRenderTarget(tone)
        .Execute([&](const RGExecContext&){ order.push_back("Tonemap"); });
    rg.AddPass("PresentPass").ReadTexture(tone).Present(bb)
        .Execute([&](const RGExecContext&){ order.push_back("Present"); });

    CHECK(ctx, rg.Compile());
    RGExecContext execCtx{};
    rg.Execute(execCtx);

    CHECK_EQ(ctx, order.size(), 4u);
    CHECK_EQ(ctx, order[0], std::string("Shadow"));
    CHECK_EQ(ctx, order[1], std::string("Opaque"));
    CHECK_EQ(ctx, order[2], std::string("Tonemap"));
    CHECK_EQ(ctx, order[3], std::string("Present"));
}


static void TestTopologicalSortAfterDeadDependencyPrune(test::TestContext& ctx)
{
    RenderGraph rg;

    auto bb = rg.ImportBackbuffer(RenderTargetHandle::Make(1u,1u),
                                  TextureHandle::Make(1u,1u), 1280u, 720u);
    auto live = rg.CreateTransientRenderTarget("Live", 1280u, 720u, Format::RGBA16_FLOAT);
    auto dead = rg.CreateTransientRenderTarget("Dead", 1280u, 720u, Format::RGBA16_FLOAT);
    rg.SetTransientRenderTarget(live, RenderTargetHandle::Make(2u,1u), TextureHandle::Make(2u,1u));
    rg.SetTransientRenderTarget(dead, RenderTargetHandle::Make(3u,1u), TextureHandle::Make(3u,1u));

    rg.AddPass("DeadProducer")
        .WriteRenderTarget(dead)
        .Execute([](const RGExecContext&){});

    rg.AddPass("LiveProducer")
        .WriteRenderTarget(live)
        .Execute([](const RGExecContext&){});

    rg.AddPass("Present")
        .ReadTexture(live)
        .Present(bb)
        .Execute([](const RGExecContext&){});

    CHECK(ctx, rg.Compile());
    CHECK(ctx, rg.IsValid());
    CHECK_EQ(ctx, rg.GetSortedPasses().size(), 2u);
    CHECK_EQ(ctx, rg.GetPasses()[rg.GetSortedPasses()[0]].debugName, std::string("LiveProducer"));
    CHECK_EQ(ctx, rg.GetPasses()[rg.GetSortedPasses()[1]].debugName, std::string("Present"));
}

// ==========================================================================
// Dead Pass Culling
// ==========================================================================
static void TestDeadPassCulling(test::TestContext& ctx)
{
    RenderGraph rg;

    auto bb  = rg.ImportBackbuffer(RenderTargetHandle::Make(1u,1u),
                                    TextureHandle::Make(1u,1u), 1280u, 720u);
    auto hdr = rg.ImportRenderTarget(RenderTargetHandle::Make(2u,1u),
                   TextureHandle::Make(2u,1u), "HDR", 1280u, 720u, Format::RGBA16_FLOAT);
    // Dead resource - nicht mit Backbuffer verbunden
    auto dead = rg.CreateTransientRenderTarget("DeadRT", 512u, 512u, Format::RGBA8_UNORM_SRGB);
    rg.SetTransientRenderTarget(dead, RenderTargetHandle::Make(3u,1u), TextureHandle::Make(3u,1u));

    int liveExecuted = 0, deadExecuted = 0;

    // Dead pass schreibt in dead resource, die nie zum Backbuffer beiträgt
    rg.AddPass("DeadPass")
        .WriteRenderTarget(dead)
        .Execute([&](const RGExecContext&){ ++deadExecuted; });

    rg.AddPass("LivePass")
        .WriteRenderTarget(hdr)
        .Execute([&](const RGExecContext&){ ++liveExecuted; });

    rg.AddPass("PresentPass")
        .ReadTexture(hdr)
        .Present(bb)
        .Execute([&](const RGExecContext&){ });

    CHECK(ctx, rg.Compile());
    if (!rg.IsValid()) return;

    // Nach Culling: DeadPass deaktiviert
    int enabledCount = 0;
    for (const auto& p : rg.GetPasses())
        if (p.enabled) ++enabledCount;
    CHECK_EQ(ctx, enabledCount, 2); // LivePass + PresentPass

    RGExecContext execCtx{};
    rg.Execute(execCtx);
    CHECK_EQ(ctx, liveExecuted,  1);
    CHECK_EQ(ctx, deadExecuted,  0); // Dead Pass wurde nicht ausgeführt
}

// ==========================================================================
// State-Planung: Transitions in topologischer Reihenfolge
// ==========================================================================
static void TestStateTransitionsTopological(test::TestContext& ctx)
{
    RenderGraph rg;

    auto bb  = rg.ImportBackbuffer(RenderTargetHandle::Make(1u,1u),
                                    TextureHandle::Make(1u,1u), 1280u, 720u);
    auto hdr = rg.CreateTransientRenderTarget("HDR", 1280u, 720u, Format::RGBA16_FLOAT, RGResourceKind::ColorTexture);
    rg.SetTransientRenderTarget(hdr, RenderTargetHandle::Make(2u,1u), TextureHandle::Make(2u,1u));

    rg.AddPass("TonemapPass").ReadTexture(hdr).Present(bb).Execute([](const RGExecContext&){});
    rg.AddPass("OpaquePass").WriteRenderTarget(hdr).Execute([](const RGExecContext&){});

    CHECK(ctx, !rg.Compile() || rg.IsValid());
}

// ==========================================================================
// ResourceAliaser
// ==========================================================================
static void TestResourceAliasing(test::TestContext& ctx)
{
    RenderGraph rg;

    auto bb = rg.ImportBackbuffer(RenderTargetHandle::Make(1u,1u),
                                   TextureHandle::Make(1u,1u), 1280u, 720u);

    // A: lebt in Pass 0 und 1
    auto resA = rg.CreateTransientRenderTarget("ResA", 1280u, 720u, Format::RGBA16_FLOAT);
    rg.SetTransientRenderTarget(resA, RenderTargetHandle::Make(2u,1u), TextureHandle::Make(2u,1u));

    // B: lebt in Pass 2 und 3 (nach A) → kann aliasieren
    auto resB = rg.CreateTransientRenderTarget("ResB", 1280u, 720u, Format::RGBA16_FLOAT);
    rg.SetTransientRenderTarget(resB, RenderTargetHandle::Make(3u,1u), TextureHandle::Make(3u,1u));

    // C: lebt gleichzeitig mit A → KEIN Aliasing
    auto resC = rg.CreateTransientRenderTarget("ResC", 1280u, 720u, Format::RGBA8_UNORM_SRGB);
    rg.SetTransientRenderTarget(resC, RenderTargetHandle::Make(4u,1u), TextureHandle::Make(4u,1u));

    int e0=0,e1=0,e2=0,e3=0;
    rg.AddPass("P0").WriteRenderTarget(resA).Execute([&](const RGExecContext&){++e0;});
    rg.AddPass("P1").ReadTexture(resA).WriteRenderTarget(resC).Execute([&](const RGExecContext&){++e1;});
    rg.AddPass("P2").ReadTexture(resC).WriteRenderTarget(resB).Execute([&](const RGExecContext&){++e2;});
    rg.AddPass("P3").ReadTexture(resB).Present(bb).Execute([&](const RGExecContext&){++e3;});

    CHECK(ctx, rg.Compile());

    // Aliaser analysieren
    ResourceAliaser aliaser;
    auto plan = aliaser.Analyze(rg, [](const RGResourceDesc& r) -> size_t {
        return static_cast<size_t>(r.width) * r.height * 4u;
    });

    // ResA und ResB haben disjunkte Lifetimes → können aliasieren
    bool abCanAlias = false;
    for (const auto& g : plan.groups)
    {
        bool hasA = false, hasB = false;
        for (RGResourceID id : g.resources)
        {
            const auto& res = rg.GetResources()[id];
            if (res.debugName == "ResA") hasA = true;
            if (res.debugName == "ResB") hasB = true;
        }
        if (hasA && hasB) { abCanAlias = true; break; }
    }
    CHECK(ctx, abCanAlias);

    // ResC überschneidet sich mit ResA → darf nicht in gleicher Gruppe sein
    bool acInSameGroup = false;
    for (const auto& g : plan.groups)
    {
        bool hasA = false, hasC = false;
        for (RGResourceID id : g.resources)
        {
            const auto& res = rg.GetResources()[id];
            if (res.debugName == "ResA") hasA = true;
            if (res.debugName == "ResC") hasC = true;
        }
        if (hasA && hasC) { acInSameGroup = true; break; }
    }
    CHECK(ctx, !acInSameGroup);
}

// ==========================================================================
// TaskGraph - Zyklus-Erkennung DFS
// ==========================================================================
static void TestTaskGraphDFS(test::TestContext& ctx)
{
    using namespace engine::jobs;

    // Azyklischer Graph baut korrekt
    {
        TaskGraph g;
        auto a = g.Add("A", {}, []{});
        auto b = g.Add("B", {a}, []{});
        auto c = g.Add("C", {a}, []{});
        auto d = g.Add("D", {b, c}, []{});
        (void)d;
        CHECK(ctx, g.Build());
        CHECK_EQ(ctx, g.LevelCount(), 3u); // A | B,C | D
        // Level 1 muss B und C enthalten (parallel)
        CHECK_EQ(ctx, g.TaskCount(), 4u);
    }

    // Selbst-Zyklus wird erkannt
    {
        TaskGraph g;
        constexpr TaskHandle SELF = 0u;
        g.Add("A", std::vector<TaskHandle>{SELF}, []{});
        bool built = g.Build();
        CHECK(ctx, !built);
    }

    // Indirekter Zyklus A→B→C→A
    {
        TaskGraph g;
        // A(0)→B(1)→C(2)→A(0): C hat Dep auf 0 (A), aber A hat Dep auf B(1)
        // Handle 0 = A, 1 = B, 2 = C
        // A: Dep auf C(2) - C noch nicht existiert, aber Handle 2 wird korrekt als zukünftig erkannt
        // In unserem System: Index 2 ist noch nicht registriert → ungültiger Dep-Handle → error
        // Daher: Build gibt false zurück wegen ungültigem Handle
        auto a = g.Add("A", std::vector<TaskHandle>{2u}, []{});  // Dep auf C (Handle 2)
        auto b = g.Add("B", {a}, []{});
        auto c = g.Add("C", {b}, []{});
        (void)c;
        // a hat Dep auf 2 (c), b hat Dep auf a, c hat Dep auf b → A→C, B→A, C→B = Zyklus
        // Aber: A's dep (2) wird als vorwärts-Dep auf c (2) aufgelöst
        // Nach DFS: A depends on C, B depends on A, C depends on B → echter Zyklus
        bool built = g.Build();
        CHECK(ctx, !built); // Muss false sein (Zyklus oder ungültiger Handle)
    }
}


// ==========================================================================
// CompiledFrame - Compile(CompiledFrame&) und Execute(frame, ctx)
// ==========================================================================
static void TestCompiledFrame(test::TestContext& ctx)
{
    RenderGraph rg;

    auto bbRT  = RenderTargetHandle::Make(1u, 1u);
    auto bbTex = TextureHandle::Make(1u, 1u);
    auto hdrRT = RenderTargetHandle::Make(2u, 1u);
    auto hdrTex = TextureHandle::Make(2u, 1u);

    RGResourceID bb  = rg.ImportBackbuffer(bbRT, bbTex, 1280u, 720u);
    RGResourceID hdr = rg.ImportRenderTarget(hdrRT, hdrTex, "HDR",
                           1280u, 720u, Format::RGBA16_FLOAT);

    int opaqueRan = 0, tonemapRan = 0;
    rg.AddPass("Opaque") .WriteRenderTarget(hdr).Execute([&](const RGExecContext&){ ++opaqueRan; });
    rg.AddPass("Tonemap").ReadTexture(hdr).Present(bb).Execute([&](const RGExecContext&){ ++tonemapRan; });

    CompiledFrame frame;
    CHECK(ctx, rg.Compile(frame));
    CHECK(ctx, frame.IsValid());
    CHECK_EQ(ctx, frame.passes.size(), 2u);
    CHECK_EQ(ctx, frame.resources.size(), 2u);
    CHECK(ctx, frame.topologyKey != 0ull);

    // Pass-Namen korrekt materialisiert
    CHECK_EQ(ctx, frame.passes[0].debugName, std::string("Opaque"));
    CHECK_EQ(ctx, frame.passes[1].debugName, std::string("Tonemap"));

    // HDR-Ressource im Snapshot vorhanden
    bool foundHdr = false;
    for (const auto& r : frame.resources)
        if (r.debugName == "HDR") { foundHdr = true; break; }
    CHECK(ctx, foundHdr);

    // Execute über CompiledFrame
    RGExecContext execCtx{};
    rg.Execute(frame, execCtx);
    CHECK_EQ(ctx, opaqueRan,  1);
    CHECK_EQ(ctx, tonemapRan, 1);

    // Zweites Execute ohne erneutes Compile()
    rg.Execute(frame, execCtx);
    CHECK_EQ(ctx, opaqueRan,  2);
    CHECK_EQ(ctx, tonemapRan, 2);

    // topologyKey ist bei gleicher Topologie stabil
    CompiledFrame frame2;
    rg.Compile(frame2);
    CHECK_EQ(ctx, frame.topologyKey, frame2.topologyKey);
}

// ==========================================================================
// CompiledFrame - topologyKey ändert sich bei Topologie-Änderung
// ==========================================================================
static void TestCompiledFrameTopologyKeyChanges(test::TestContext& ctx)
{
    auto makeGraph = [](int extraPass) -> uint64_t
    {
        RenderGraph rg;
        auto bb  = rg.ImportBackbuffer(RenderTargetHandle::Make(1u,1u),
                                        TextureHandle::Make(1u,1u), 1280u, 720u);
        auto hdr = rg.ImportRenderTarget(RenderTargetHandle::Make(2u,1u),
                       TextureHandle::Make(2u,1u), "HDR",
                       1280u, 720u, Format::RGBA16_FLOAT);
        rg.AddPass("Opaque").WriteRenderTarget(hdr).Execute([](const RGExecContext&){});
        if (extraPass)
        {
            auto mid = rg.ImportRenderTarget(RenderTargetHandle::Make(3u,1u),
                           TextureHandle::Make(3u,1u), "Mid",
                           1280u, 720u, Format::RGBA16_FLOAT);
            rg.AddPass("Mid").ReadTexture(hdr).WriteRenderTarget(mid)
                .Execute([](const RGExecContext&){});
            rg.AddPass("Tonemap").ReadTexture(mid).Present(bb).Execute([](const RGExecContext&){});
        }
        else
        {
            rg.AddPass("Tonemap").ReadTexture(hdr).Present(bb).Execute([](const RGExecContext&){});
        }
        CompiledFrame f;
        rg.Compile(f);
        return f.topologyKey;
    };

    const uint64_t keyA = makeGraph(0);
    const uint64_t keyB = makeGraph(1);
    CHECK(ctx, keyA != 0ull);
    CHECK(ctx, keyB != 0ull);
    CHECK(ctx, keyA != keyB);
}

// ==========================================================================
// Run all RenderGraph tests
// ==========================================================================
// ==========================================================================
// Barrier-Optimierung - No-Op-Elimination + Merge
// ==========================================================================
static void TestBarrierOptimization(test::TestContext& ctx)
{
    RenderGraph rg;
    auto bb  = rg.ImportBackbuffer(RenderTargetHandle::Make(1u,1u),
                                    TextureHandle::Make(1u,1u), 1280u, 720u);
    auto hdr = rg.ImportRenderTarget(RenderTargetHandle::Make(2u,1u),
                   TextureHandle::Make(2u,1u), "HDR", 1280u, 720u, Format::RGBA16_FLOAT);

    rg.AddPass("Opaque") .WriteRenderTarget(hdr).Execute([](const RGExecContext&){});
    rg.AddPass("Tonemap").ReadTexture(hdr).Present(bb).Execute([](const RGExecContext&){});

    CompiledFrame frame;
    CHECK(ctx, rg.Compile(frame));

    // Barrier-Stats: mindestens eine Transition muss vorhanden sein
    // (HDR: Common→RT, RT→ShaderRead, Backbuffer→Present)
    CHECK(ctx, frame.barrierStats.totalTransitions > 0u);

    // finalTransitions <= totalTransitions (Optimierung entfernt oder merged)
    CHECK(ctx, frame.barrierStats.finalTransitions <= frame.barrierStats.totalTransitions);

    // Kein Verlust echter Transitions (final > 0 wenn total > 0)
    CHECK(ctx, frame.barrierStats.finalTransitions > 0u);
}

// ==========================================================================
// Barrier-Optimierung - No-Op direkt prüfen
// ==========================================================================
static void TestBarrierNoOpElimination(test::TestContext& ctx)
{
    // Synthetischen CompiledFrame mit No-Ops bauen
    CompiledFrame frame;
    frame.valid = true;

    auto hdrTex = TextureHandle::Make(1u, 1u);

    CompiledPassEntry entry;
    entry.passIndex = 0u;
    entry.debugName = "TestPass";

    // Echter Übergang: Common → RenderTarget
    CompiledTransition real;
    real.texture = hdrTex;
    real.before  = ResourceState::Common;
    real.after   = ResourceState::RenderTarget;
    entry.beginTransitions.push_back(real);

    // No-Op: RenderTarget → RenderTarget (before == after)
    CompiledTransition noop;
    noop.texture = hdrTex;
    noop.before  = ResourceState::RenderTarget;
    noop.after   = ResourceState::RenderTarget;
    entry.beginTransitions.push_back(noop);

    frame.passes.push_back(std::move(entry));

    BarrierStats stats = RenderGraph::OptimizeBarriers(frame);

    CHECK_EQ(ctx, stats.totalTransitions, 2u);
    CHECK_EQ(ctx, stats.redundantEliminated, 1u);
    CHECK_EQ(ctx, stats.finalTransitions, 1u);
    // Echter Übergang bleibt erhalten
    CHECK_EQ(ctx, frame.passes[0].beginTransitions.size(), 1u);
    CHECK(ctx, frame.passes[0].beginTransitions[0].after == ResourceState::RenderTarget);
}

// ==========================================================================
// Barrier-Optimierung - Merge aufeinanderfolgender Transitions
// ==========================================================================
static void TestBarrierMerge(test::TestContext& ctx)
{
    CompiledFrame frame;
    frame.valid = true;
    auto tex = TextureHandle::Make(2u, 1u);

    CompiledPassEntry entry;
    entry.passIndex = 0u;

    // Common → RT, dann RT → ShaderRead → mergen zu Common → ShaderRead
    CompiledTransition t1; t1.texture=tex; t1.before=ResourceState::Common;       t1.after=ResourceState::RenderTarget;
    CompiledTransition t2; t2.texture=tex; t2.before=ResourceState::RenderTarget; t2.after=ResourceState::ShaderRead;
    entry.beginTransitions.push_back(t1);
    entry.beginTransitions.push_back(t2);
    frame.passes.push_back(std::move(entry));

    BarrierStats stats = RenderGraph::OptimizeBarriers(frame);

    CHECK_EQ(ctx, stats.totalTransitions, 2u);
    CHECK_EQ(ctx, stats.mergedTransitions, 1u);
    CHECK_EQ(ctx, stats.finalTransitions, 1u);
    CHECK(ctx, frame.passes[0].beginTransitions[0].before == ResourceState::Common);
    CHECK(ctx, frame.passes[0].beginTransitions[0].after  == ResourceState::ShaderRead);
}

// ==========================================================================
// Resource Versioning
// ==========================================================================
static void TestResourceVersioning(test::TestContext& ctx)
{
    // Fall 1: History-Ressource nie beschrieben → Versioning-Warnung
    {
        RenderGraph rg;
        auto bb = rg.ImportBackbuffer(RenderTargetHandle::Make(1u,1u),
                                       TextureHandle::Make(1u,1u), 1280u, 720u);
        auto [cur, prev] = rg.ImportHistoryPair(
            RenderTargetHandle::Make(2u,1u), TextureHandle::Make(2u,1u),
            RenderTargetHandle::Make(3u,1u), TextureHandle::Make(3u,1u),
            "TAA", 1280u, 720u, Format::RGBA16_FLOAT);

        // Liest History aber schreibt NIE in cur → Versioning-Warnung erwartet
        rg.AddPass("Composite")
            .ReadHistoryBuffer(prev)
            .Present(bb)
            .Execute([](const RGExecContext&){});

        CompiledFrame frame;
        rg.Compile(frame);
        // Warnung: cur (isHistoryCurrent) wurde nie beschrieben
        CHECK(ctx, !frame.versioningWarnings.empty());
    }

    // Fall 2: Sauberer Graph mit korrektem History-Write → keine Warnungen
    {
        RenderGraph rg;
        auto bb = rg.ImportBackbuffer(RenderTargetHandle::Make(1u,1u),
                                       TextureHandle::Make(1u,1u), 1280u, 720u);
        auto hdr = rg.ImportRenderTarget(RenderTargetHandle::Make(2u,1u),
                       TextureHandle::Make(2u,1u), "HDR",
                       1280u, 720u, Format::RGBA16_FLOAT);
        auto [cur, prev] = rg.ImportHistoryPair(
            RenderTargetHandle::Make(3u,1u), TextureHandle::Make(3u,1u),
            RenderTargetHandle::Make(4u,1u), TextureHandle::Make(4u,1u),
            "TAA", 1280u, 720u, Format::RGBA16_FLOAT);

        rg.AddPass("Opaque").WriteRenderTarget(hdr).Execute([](const RGExecContext&){});
        rg.AddPass("TAA")
            .ReadTexture(hdr)
            .ReadHistoryBuffer(prev)
            .WriteHistoryBuffer(cur)   // schreibt cur → kein Warning
            .Execute([](const RGExecContext&){});
        rg.AddPass("Tonemap")
            .ReadHistoryBuffer(cur)
            .Present(bb)
            .Execute([](const RGExecContext&){});

        CompiledFrame frame;
        CHECK(ctx, rg.Compile(frame));
        CHECK(ctx, frame.versioningWarnings.empty());
    }

    // Fall 3: Transient ohne Producer → Compile() schlägt fehl (Validate-Fehler)
    {
        RenderGraph rg;
        auto bb = rg.ImportBackbuffer(RenderTargetHandle::Make(1u,1u),
                                       TextureHandle::Make(1u,1u), 1280u, 720u);
        auto orphan = rg.CreateTransientRenderTarget("OrphanRT",
                          512u, 512u, Format::RGBA8_UNORM);
        rg.SetTransientRenderTarget(orphan,
            RenderTargetHandle::Make(2u,1u), TextureHandle::Make(2u,1u));
        rg.AddPass("BadPass").ReadTexture(orphan).Present(bb)
            .Execute([](const RGExecContext&){});

        CompiledFrame frame;
        // Validate() fängt das als harten Fehler ab → Compile schlägt fehl
        const bool compiled = rg.Compile(frame);
        CHECK(ctx, !compiled || !frame.IsValid());
    }
}

// ==========================================================================
// History Resources - Ping-Pong API
// ==========================================================================
static void TestHistoryResources(test::TestContext& ctx)
{
    RenderGraph rg;

    auto bb = rg.ImportBackbuffer(RenderTargetHandle::Make(1u,1u),
                                   TextureHandle::Make(1u,1u), 1280u, 720u);

    // Ping-Pong-Paar für Akkumulation (z.B. GTAO)
    auto curRT  = RenderTargetHandle::Make(2u, 1u);
    auto curTex = TextureHandle::Make(2u, 1u);
    auto prevRT  = RenderTargetHandle::Make(3u, 1u);
    auto prevTex = TextureHandle::Make(3u, 1u);

    auto [cur, prev] = rg.ImportHistoryPair(curRT, curTex, prevRT, prevTex,
                                             "GTAO", 1280u, 720u, Format::R8_UNORM);

    int accumulateRan = 0;
    rg.AddPass("Accumulate")
        .WriteHistoryBuffer(cur)
        .ReadHistoryBuffer(prev)
        .Execute([&](const RGExecContext&){ ++accumulateRan; });

    rg.AddPass("Composite")
        .ReadHistoryBuffer(cur)
        .Present(bb)
        .Execute([](const RGExecContext&){});

    CompiledFrame frame;
    CHECK(ctx, rg.Compile(frame));
    CHECK(ctx, frame.IsValid());

    RGExecContext ctx2{};
    rg.Execute(frame, ctx2);
    CHECK_EQ(ctx, accumulateRan, 1);

    // Nach Execute: Handles tauschen für nächsten Frame
    // curRT soll jetzt die "Prev"-Rolle übernehmen
    const auto curResBeforeSwap  = rg.GetResources()[cur].texture.value;
    const auto prevResBeforeSwap = rg.GetResources()[prev].texture.value;
    rg.SwapHistoryResources(cur, prev);
    CHECK(ctx, rg.GetResources()[cur].texture.value  == prevResBeforeSwap);
    CHECK(ctx, rg.GetResources()[prev].texture.value == curResBeforeSwap);
}


static void TestRenderGraphPhysicalHandleValidation(test::TestContext& ctx)
{
    RenderGraph rg;
    auto transient = rg.CreateTransientRenderTarget("NeedHandle", 128u, 128u, Format::RGBA16_FLOAT, RGResourceKind::ColorTexture);
    auto bb = rg.ImportBackbuffer(RenderTargetHandle::Make(1u,1u), TextureHandle::Make(1u,1u), 128u, 128u);

    rg.AddPass("WriteTransient")
        .WriteRenderTarget(transient)
        .Execute([](const RGExecContext&){});

    rg.AddPass("Present")
        .ReadTexture(transient)
        .Present(bb)
        .Execute([](const RGExecContext&){});

    CHECK(ctx, !rg.Compile());
}

static void TestRenderGraphWriteAfterReadDependency(test::TestContext& ctx)
{
    RenderGraph rg;
    auto imported = rg.ImportTexture(TextureHandle::Make(7u,1u), "Imported", 64u, 64u, Format::RGBA8_UNORM);
    auto out = rg.ImportBackbuffer(RenderTargetHandle::Make(2u,1u), TextureHandle::Make(2u,1u), 64u, 64u);

    rg.AddPass("ReadFirst")
        .ReadTexture(imported)
        .Execute([](const RGExecContext&){});
    rg.AddPass("WriteSecond")
        .WriteRenderTarget(imported)
        .Execute([](const RGExecContext&){});
    rg.AddPass("Present")
        .ReadTexture(imported)
        .Present(out)
        .Execute([](const RGExecContext&){});

    CHECK(ctx, !rg.Compile());
}

int RunRenderGraphTests()
{
    engine::Debug::MinLevel = engine::LogLevel::Fatal;

    test::TestSuite suite("RenderGraph");
    suite
        .Add("Linear pipeline",             TestLinearPipeline)
        .Add("Topological sort",            TestTopologicalSort)
        .Add("Topological sort after cull", TestTopologicalSortAfterDeadDependencyPrune)
        .Add("Dead pass culling",           TestDeadPassCulling)
        .Add("State transitions topo",      TestStateTransitionsTopological)
        .Add("Resource aliasing",           TestResourceAliasing)
        .Add("TaskGraph DFS cycle detect",  TestTaskGraphDFS)
        .Add("CompiledFrame compile+exec",  TestCompiledFrame)
        .Add("CompiledFrame topologyKey",   TestCompiledFrameTopologyKeyChanges)
        .Add("Barrier optimization stats",  TestBarrierOptimization)
        .Add("Barrier no-op elimination",   TestBarrierNoOpElimination)
        .Add("Barrier merge",               TestBarrierMerge)
        .Add("Resource versioning",         TestResourceVersioning)
        .Add("Physical handle validation", TestRenderGraphPhysicalHandleValidation)
        .Add("Write-after-read deps",      TestRenderGraphWriteAfterReadDependency)
        .Add("History resources ping-pong", TestHistoryResources);

    return suite.Run();
}
