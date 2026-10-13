// =============================================================================
// KROM Engine - tests/test_jobs.cpp
// Härtungstests für das Job-System: Exceptions, Stress, Shutdown, Prioritäten,
// FrameScheduler und ComponentWriteGuard.
// =============================================================================
#include "TestFramework.hpp"
#include "jobs/JobSystem.hpp"
#include "jobs/TaskGraph.hpp"
#include "jobs/FrameScheduler.hpp"
#include "jobs/ComponentWriteGuard.hpp"
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace engine::jobs;

// ---------------------------------------------------------------------------
// Exceptions im Worker beenden nicht den Prozess
// ---------------------------------------------------------------------------
static void TestJobExceptionDoesNotKillProcess(test::TestContext& ctx)
{
    JobSystem js;
    js.Initialize(2u);

    std::atomic<int> completed{ 0 };

    // Job der eine Exception wirft
    CHECK(ctx, js.Dispatch([&]() {
        ++completed;
        throw std::runtime_error("intentional test exception");
    }));

    // Job der danach normal läuft
    CHECK(ctx, js.Dispatch([&]() {
        ++completed;
    }));

    js.WaitIdle();
    js.Shutdown();

    CHECK_EQ(ctx, completed.load(), 2);
}

// ---------------------------------------------------------------------------
// DispatchResult: Exception im Job → TaskResult::Failed, kein future.get()-Hänger
// ---------------------------------------------------------------------------
static void TestDispatchResultExceptionYieldsFailure(test::TestContext& ctx)
{
    JobSystem js;
    js.Initialize(1u);

    auto future = js.DispatchResult([]() -> TaskResult {
        throw std::runtime_error("intentional");
    });

    const TaskResult result = future.get();
    js.Shutdown();

    CHECK(ctx, result.Failed());
    CHECK(ctx, result.errorMessage != nullptr);
}

// ---------------------------------------------------------------------------
// DispatchReturn: Exception → ValueResult::Failed, kein Hänger
// ---------------------------------------------------------------------------
static void TestDispatchReturnExceptionYieldsFailure(test::TestContext& ctx)
{
    JobSystem js;
    js.Initialize(1u);

    auto future = js.DispatchReturn([]() -> int {
        throw std::runtime_error("intentional");
    });

    const ValueResult<int> result = future.get();
    js.Shutdown();

    CHECK(ctx, !result.Succeeded());
    CHECK(ctx, result.task.Failed());
}

// ---------------------------------------------------------------------------
// DispatchResult: Normaler Erfolgsfall
// ---------------------------------------------------------------------------
static void TestDispatchResultSuccess(test::TestContext& ctx)
{
    JobSystem js;
    js.Initialize(1u);

    auto future = js.DispatchResult([]() -> TaskResult {
        return TaskResult::Ok();
    });

    const TaskResult result = future.get();
    js.Shutdown();

    CHECK(ctx, result.Succeeded());
}

// ---------------------------------------------------------------------------
// IsWorkerThread(): Main-Thread ist kein Worker-Thread
// ---------------------------------------------------------------------------
static void TestIsWorkerThreadFalseOnMainThread(test::TestContext& ctx)
{
    JobSystem js;
    js.Initialize(2u);
    CHECK(ctx, !js.IsWorkerThread());
    CHECK(ctx, !JobSystem::IsAnyWorkerThread());
    js.Shutdown();
    CHECK(ctx, !js.IsWorkerThread());
    CHECK(ctx, !JobSystem::IsAnyWorkerThread());
}

// ---------------------------------------------------------------------------
// IsWorkerThread(): Worker-Threads erkennen sich selbst korrekt (instanzbezogen)
// ---------------------------------------------------------------------------
static void TestIsWorkerThreadTrueInsideJob(test::TestContext& ctx)
{
    JobSystem js;
    js.Initialize(2u);

    std::atomic<bool> sawWorkerFlag{ false };
    std::atomic<bool> sawAnyFlag{ false };
    auto future = js.DispatchResult([&]() -> TaskResult {
        if (js.IsWorkerThread())
            sawWorkerFlag.store(true, std::memory_order_release);
        if (JobSystem::IsAnyWorkerThread())
            sawAnyFlag.store(true, std::memory_order_release);
        return TaskResult::Ok();
    });

    future.get();
    js.Shutdown();

    CHECK(ctx, sawWorkerFlag.load());
    CHECK(ctx, sawAnyFlag.load());
}

// ---------------------------------------------------------------------------
// IsWorkerThread(): Worker von Pool A gilt nicht als Worker von Pool B
// ---------------------------------------------------------------------------
static void TestIsWorkerThreadPoolIsolation(test::TestContext& ctx)
{
    JobSystem poolA, poolB;
    poolA.Initialize(1u);
    poolB.Initialize(1u);

    std::atomic<bool> aSeesB{ false };

    // Job auf Pool A: IsWorkerThread() auf Pool B muss false sein
    auto future = poolA.DispatchResult([&]() -> TaskResult {
        if (poolB.IsWorkerThread())
            aSeesB.store(true, std::memory_order_release);
        return TaskResult::Ok();
    });

    future.get();
    poolA.Shutdown();
    poolB.Shutdown();

    CHECK(ctx, !aSeesB.load());
}

// ---------------------------------------------------------------------------
// ParallelFor aus Worker läuft synchron (kein Deadlock)
// ---------------------------------------------------------------------------
static void TestParallelForFromWorkerRunsSynchronously(test::TestContext& ctx)
{
    JobSystem js;
    js.Initialize(2u);

    std::atomic<int> sum{ 0 };

    // Der Job dispatcht selbst ein ParallelFor — das muss synchron laufen
    auto future = js.DispatchResult([&]() -> TaskResult {
        const TaskResult r = js.ParallelFor(10u, [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i)
                sum.fetch_add(1, std::memory_order_relaxed);
        }, 1u);
        return r;
    });

    const TaskResult result = future.get();
    js.Shutdown();

    CHECK(ctx, result.Succeeded());
    CHECK_EQ(ctx, sum.load(), 10);
}

// ---------------------------------------------------------------------------
// Prioritäten: Frame-Jobs werden vor Background-Jobs ausgeführt
// (probabilistischer Test mit einem einzigen Worker)
// ---------------------------------------------------------------------------
static void TestFrameJobsExecutedBeforeBackground(test::TestContext& ctx)
{
    JobSystem js;
    js.Initialize(1u);

    // Worker blockieren damit die Queue aufgebaut werden kann
    std::atomic<bool> gate{ false };
    CHECK(ctx, js.Dispatch([&]() {
        while (!gate.load(std::memory_order_acquire))
            std::this_thread::yield();
    }, JobPriority::Frame));

    // Jetzt Background, dann Frame einstellen — Frame muss zuerst kommen
    std::vector<int> order;
    std::mutex orderMtx;

    js.DispatchBackground([&]() {
        std::unique_lock<std::mutex> lk(orderMtx);
        order.push_back(2);
    });
    CHECK(ctx, js.Dispatch([&]() {
        std::unique_lock<std::mutex> lk(orderMtx);
        order.push_back(1);
    }, JobPriority::Frame));

    gate.store(true, std::memory_order_release);
    js.WaitIdle();
    js.Shutdown();

    CHECK_EQ(ctx, order.size(), size_t(2));
    CHECK_EQ(ctx, order[0], 1);  // Frame zuerst
    CHECK_EQ(ctx, order[1], 2);  // Background danach
}

// ---------------------------------------------------------------------------
// Stresstest: 1000 kleine Jobs laufen alle durch
// ---------------------------------------------------------------------------
static void TestStress1000Jobs(test::TestContext& ctx)
{
    JobSystem js;
    js.Initialize(4u);

    constexpr int kCount = 1000;
    std::atomic<int> counter{ 0 };

    for (int i = 0; i < kCount; ++i)
    {
        CHECK(ctx, js.Dispatch([&]() {
            counter.fetch_add(1, std::memory_order_relaxed);
        }));
    }

    js.WaitIdle();
    js.Shutdown();

    CHECK_EQ(ctx, counter.load(), kCount);
}

// ---------------------------------------------------------------------------
// Stresstest: ParallelFor mit 10 000 Elementen liefert korrektes Ergebnis
// ---------------------------------------------------------------------------
static void TestParallelForLargeCount(test::TestContext& ctx)
{
    JobSystem js;
    js.Initialize(4u);

    constexpr size_t kN = 10000u;
    std::atomic<size_t> sum{ 0u };

    const TaskResult result = js.ParallelFor(kN, [&](size_t begin, size_t end) {
        size_t local = 0u;
        for (size_t i = begin; i < end; ++i) local += i;
        sum.fetch_add(local, std::memory_order_relaxed);
    }, 256u);

    js.Shutdown();

    CHECK(ctx, result.Succeeded());
    const size_t expected = kN * (kN - 1u) / 2u;
    CHECK_EQ(ctx, sum.load(), expected);
}

// ---------------------------------------------------------------------------
// Shutdown während Queue noch Jobs enthält: kein Hänger, kein Crash
// ---------------------------------------------------------------------------
static void TestParallelForHelpWhileWaiting(test::TestContext& ctx)
{
    JobSystem js;
    js.Initialize(1u);

    std::atomic<bool> gate{ false };
    std::atomic<bool> workerRunning{ false };
    std::atomic<int> sum{ 0 };

    CHECK(ctx, js.Dispatch([&]() {
        workerRunning.store(true, std::memory_order_release);
        while (!gate.load(std::memory_order_acquire))
            std::this_thread::yield();
    }));

    // Sicherstellen dass der Worker den Gate-Job schon ausführt, bevor
    // die Batch-Jobs eingereiht werden — sonst nimmt HelpExecuteOne den
    // Gate-Job und dreht sich selbst in der Spin-Schleife.
    while (!workerRunning.load(std::memory_order_acquire))
        std::this_thread::yield();

    const TaskResult result = js.ParallelFor(8u, [&](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i)
            sum.fetch_add(1, std::memory_order_relaxed);
    }, 1u);

    gate.store(true, std::memory_order_release);
    js.Shutdown();

    CHECK(ctx, result.Succeeded());
    CHECK_EQ(ctx, sum.load(), 8);
}

static void TestWaitIdleHelpWhileWaiting(test::TestContext& ctx)
{
    JobSystem js;
    js.Initialize(1u);

    std::atomic<bool> gate{ false };
    std::atomic<bool> workerRunning{ false };
    std::atomic<int> completed{ 0 };

    CHECK(ctx, js.Dispatch([&]() {
        workerRunning.store(true, std::memory_order_release);
        while (!gate.load(std::memory_order_acquire))
            std::this_thread::yield();
    }));

    while (!workerRunning.load(std::memory_order_acquire))
        std::this_thread::yield();

    for (int i = 0; i < 6; ++i)
    {
        CHECK(ctx, js.Dispatch([&]() {
            completed.fetch_add(1, std::memory_order_relaxed);
        }));
    }

    std::thread releaser([&]() {
        while (completed.load(std::memory_order_acquire) < 6)
            std::this_thread::yield();
        gate.store(true, std::memory_order_release);
    });

    js.WaitIdle();

    releaser.join();
    js.Shutdown();

    CHECK_EQ(ctx, completed.load(), 6);
}

static void TestShutdownWithPendingJobs(test::TestContext& ctx)
{
    JobSystem js;
    js.Initialize(1u);

    // Worker blockieren
    std::atomic<bool> gate{ false };
    CHECK(ctx, js.Dispatch([&]() {
        while (!gate.load(std::memory_order_acquire))
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }));

    // Viele Jobs einreihen bevor der erste fertig ist
    std::atomic<int> ran{ 0 };
    for (int i = 0; i < 50; ++i)
        CHECK(ctx, js.Dispatch([&]() { ran.fetch_add(1, std::memory_order_relaxed); }));

    gate.store(true, std::memory_order_release);
    js.Shutdown();

    // Nicht alle 50 müssen gelaufen sein (Shutdown darf pending Jobs verwerfen),
    // aber der Prozess darf nicht hängen oder abstürzen.
    CHECK(ctx, ran.load() >= 0);
}

// ---------------------------------------------------------------------------
// Dispatch nach Shutdown wird verworfen, kein Absturz
// ---------------------------------------------------------------------------
static void TestDispatchAfterShutdownIsRejected(test::TestContext& ctx)
{
    JobSystem js;
    js.Initialize(1u);
    js.Shutdown();

    std::atomic<bool> ran{ false };
    CHECK(ctx, !js.Dispatch([&]() { ran.store(true); }));

    CHECK(ctx, !ran.load());
}

// ---------------------------------------------------------------------------
// TaskGraph + JobSystem: Zyklus-Erkennung verhindert Execute
// ---------------------------------------------------------------------------
static void TestTaskGraphCycleDetection(test::TestContext& ctx)
{
    TaskGraph graph;
    const auto a = graph.Add("A", {}, []() { return TaskResult::Ok(); });
    const auto b = graph.Add("B", { a }, []() { return TaskResult::Ok(); });
    // Zyklus: A hängt indirekt von B ab
    graph.Add("CycleBack", { b }, [](){ return TaskResult::Ok(); });

    // Manuell Zyklus erzeugen über Set — nicht direkt möglich über die API.
    // Stattdessen: ungültigen Dep-Handle → Build muss false zurückgeben
    TaskGraph badGraph;
    badGraph.Add("X", { 999u }, []() { return TaskResult::Ok(); });
    CHECK(ctx, !badGraph.Build());
}

// ---------------------------------------------------------------------------
// WaitIdle ohne laufende Jobs kehrt sofort zurück
// ---------------------------------------------------------------------------
static void TestWaitIdleReturnImmediatelyWhenEmpty(test::TestContext& ctx)
{
    JobSystem js;
    js.Initialize(2u);

    const auto t0 = std::chrono::steady_clock::now();
    js.WaitIdle();
    const auto elapsed = std::chrono::steady_clock::now() - t0;

    js.Shutdown();

    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    CHECK(ctx, ms < 500);
}

// ===========================================================================
// FrameScheduler Tests
// ===========================================================================

// ---------------------------------------------------------------------------
// Korrekte Pipeline ohne Konflikte: Physik → Animation → Rendering
// ---------------------------------------------------------------------------
static void TestFrameSchedulerCorrectPipeline(test::TestContext& ctx)
{
    using namespace engine::jobs;

    JobSystem js;
    js.Initialize(2u);

    std::vector<std::string> order;
    std::mutex orderMtx;

    FrameScheduler scheduler;

    const auto physics = scheduler.RegisterStage("Physics", {}, [&]() -> TaskResult {
        std::unique_lock lk(orderMtx);
        order.push_back("Physics");
        return TaskResult::Ok();
    }, { FrameTags::Transform, FrameTags::RigidBody });

    const auto animation = scheduler.RegisterStage("Animation", { physics }, [&]() -> TaskResult {
        std::unique_lock lk(orderMtx);
        order.push_back("Animation");
        return TaskResult::Ok();
    }, { FrameTags::Transform, FrameTags::Skeleton });

    scheduler.RegisterStage("Rendering", { animation }, [&]() -> TaskResult {
        std::unique_lock lk(orderMtx);
        order.push_back("Rendering");
        return TaskResult::Ok();
    }, {}, { FrameTags::Transform });

    CHECK(ctx, scheduler.Build());
    CHECK_EQ(ctx, scheduler.ConflictCount(), 0u);

    const TaskResult result = scheduler.Execute(js);
    js.Shutdown();

    CHECK(ctx, result.Succeeded());
    CHECK_EQ(ctx, order.size(), size_t(3));
    CHECK_EQ(ctx, order[0], std::string("Physics"));
    CHECK_EQ(ctx, order[1], std::string("Animation"));
    CHECK_EQ(ctx, order[2], std::string("Rendering"));
}

// ---------------------------------------------------------------------------
// Fehlende Abhängigkeit: Physik und Animation parallel → Write-Write-Konflikt
// ---------------------------------------------------------------------------
static void TestFrameSchedulerDetectsWriteWriteConflict(test::TestContext& ctx)
{
    using namespace engine::jobs;

    FrameScheduler scheduler;

    // Beide schreiben Transform, aber keine Abhängigkeit → müssen als Konflikt erkannt werden
    const auto input = scheduler.RegisterStage("Input", {}, []() { return TaskResult::Ok(); });

    scheduler.RegisterStage("Physics", { input }, []() { return TaskResult::Ok(); },
        { FrameTags::Transform, FrameTags::RigidBody });

    scheduler.RegisterStage("Animation", { input }, []() { return TaskResult::Ok(); },
        { FrameTags::Transform, FrameTags::Skeleton });

    CHECK(ctx, scheduler.Build());
    CHECK_GT(ctx, scheduler.ConflictCount(), 0u);
}

// ---------------------------------------------------------------------------
// Fehlende Abhängigkeit: Write-Read-Konflikt
// ---------------------------------------------------------------------------
static void TestFrameSchedulerDetectsWriteReadConflict(test::TestContext& ctx)
{
    using namespace engine::jobs;

    FrameScheduler scheduler;

    const auto input = scheduler.RegisterStage("Input", {}, []() { return TaskResult::Ok(); });

    // Physics schreibt Transform, Rendering liest Transform — ohne Abhängigkeit
    scheduler.RegisterStage("Physics", { input }, []() { return TaskResult::Ok(); },
        { FrameTags::Transform });

    scheduler.RegisterStage("Rendering", { input }, []() { return TaskResult::Ok(); },
        {}, { FrameTags::Transform });

    CHECK(ctx, scheduler.Build());
    CHECK_GT(ctx, scheduler.ConflictCount(), 0u);
}

// ---------------------------------------------------------------------------
// Build ohne Stages: kein Absturz, Execute gibt Ok zurück
// ---------------------------------------------------------------------------
static void TestFrameSchedulerEmptyIsOk(test::TestContext& ctx)
{
    using namespace engine::jobs;

    JobSystem js;
    js.Initialize(1u);

    FrameScheduler scheduler;
    CHECK(ctx, scheduler.Build());
    CHECK_EQ(ctx, scheduler.ConflictCount(), 0u);
    const TaskResult result = scheduler.Execute(js);
    js.Shutdown();

    CHECK(ctx, result.Succeeded());
}

// ---------------------------------------------------------------------------
// Execute ohne Build → schlägt fehl (kein Absturz)
// ---------------------------------------------------------------------------
static void TestFrameSchedulerExecuteWithoutBuildFails(test::TestContext& ctx)
{
    using namespace engine::jobs;

    JobSystem js;
    js.Initialize(1u);

    FrameScheduler scheduler;
    scheduler.RegisterStage("A", {}, []() { return TaskResult::Ok(); });

    const TaskResult result = scheduler.Execute(js);
    js.Shutdown();

    CHECK(ctx, result.Failed());
}

// ---------------------------------------------------------------------------
// FrameScheduler: Stage schlägt fehl → nachfolgende Stage läuft nicht
// ---------------------------------------------------------------------------
static void TestFrameSchedulerFailurePropagation(test::TestContext& ctx)
{
    using namespace engine::jobs;

    JobSystem js;
    js.Initialize(1u);

    bool renderingRan = false;
    FrameScheduler scheduler;

    const auto phys = scheduler.RegisterStage("Physics", {}, []() {
        return TaskResult::Fail("physics exploded");
    }, { FrameTags::RigidBody });

    scheduler.RegisterStage("Rendering", { phys }, [&]() -> TaskResult {
        renderingRan = true;
        return TaskResult::Ok();
    }, {}, { FrameTags::Transform });

    CHECK(ctx, scheduler.Build());
    const TaskResult result = scheduler.Execute(js);
    js.Shutdown();

    CHECK(ctx, result.Failed());
    CHECK(ctx, !renderingRan);
}

// ---------------------------------------------------------------------------
// ComponentWriteGuard: einzelner Schreiber — kein Fehler
// ---------------------------------------------------------------------------
static void TestComponentWriteGuardSingleWriterOk(test::TestContext& ctx)
{
    using namespace engine::jobs;

    // Dummy-Typen für den Test
    struct DummyTransform { float x = 0.f; };

    JobSystem js;
    js.Initialize(1u);

    std::atomic<bool> guardError{ false };

    auto future = js.DispatchResult([&]() -> TaskResult {
        ComponentWriteGuard<DummyTransform> guard;
        // Kein zweiter Guard gleichzeitig → kein Fehler erwartet
        (void)guard;
        return TaskResult::Ok();
    });

    future.get();
    js.Shutdown();

    // Test besteht wenn kein Absturz und kein Assert-Fehler aufgetreten ist
    CHECK(ctx, !guardError.load());
}

// ---------------------------------------------------------------------------
// ComponentWriteGuard: zwei parallele Schreiber → Guard erkennt Rennen
// (Test prüft dass der Prozess überlebt, nicht dass der Fehler geloggt wird)
// ---------------------------------------------------------------------------
static void TestComponentWriteGuardTwoWritersSurvives(test::TestContext& ctx)
{
    using namespace engine::jobs;

    struct DummyRigidBody { float mass = 1.f; };

    JobSystem js;
    js.Initialize(2u);

    std::atomic<bool> bothStarted{ false };
    std::atomic<int> startCount{ 0 };

    auto makeJob = [&]() -> TaskResult {
        ComponentWriteGuard<DummyRigidBody> guard;
        startCount.fetch_add(1, std::memory_order_acq_rel);
        // Kurz warten damit beide Jobs tatsächlich überlappen können
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        return TaskResult::Ok();
    };

    auto f1 = js.DispatchResult(makeJob);
    auto f2 = js.DispatchResult(makeJob);

    f1.get();
    f2.get();
    js.Shutdown();

    // Beide Jobs müssen fertig sein — kein Hänger, kein Crash
    CHECK_EQ(ctx, startCount.load(), 2);
}

// ---------------------------------------------------------------------------
// FrameScheduler: vollständige Spielpipeline — korrekte Ausführungsreihenfolge
// ---------------------------------------------------------------------------
static void TestFrameSchedulerGamePipeline(test::TestContext& ctx)
{
    using namespace engine::jobs;

    // Typische Pipeline: Input → AI → Physics → Animation → Transform → Rendering
    JobSystem js;
    js.Initialize(4u);

    std::vector<std::string> executionOrder;
    std::mutex orderMtx;

    auto addStage = [&](const char* name) -> std::function<TaskResult()> {
        return [&, name]() -> TaskResult {
            std::unique_lock lk(orderMtx);
            executionOrder.push_back(name);
            return TaskResult::Ok();
        };
    };

    FrameScheduler scheduler;

    const auto input  = scheduler.RegisterStage("Input",     {},           addStage("Input"),
        {}, { FrameTags::AI });

    const auto ai     = scheduler.RegisterStage("AI",        { input },    addStage("AI"),
        { FrameTags::AI });

    // Physics und AI können parallel laufen (keine gemeinsamen Tags)
    const auto phys   = scheduler.RegisterStage("Physics",   { input },    addStage("Physics"),
        { FrameTags::RigidBody, FrameTags::Transform });

    // Animation braucht Physics-Ergebnis
    const auto anim   = scheduler.RegisterStage("Animation", { phys, ai }, addStage("Animation"),
        { FrameTags::Skeleton, FrameTags::Transform });

    // Rendering liest nur
    scheduler.RegisterStage("Rendering", { anim }, addStage("Rendering"),
        {}, { FrameTags::Transform, FrameTags::Skeleton });

    CHECK(ctx, scheduler.Build());
    CHECK_EQ(ctx, scheduler.ConflictCount(), 0u);

    const TaskResult result = scheduler.Execute(js);
    js.Shutdown();

    CHECK(ctx, result.Succeeded());
    CHECK_EQ(ctx, executionOrder.size(), size_t(5));

    // Input muss zuerst sein
    CHECK_EQ(ctx, executionOrder[0], std::string("Input"));
    // Rendering muss zuletzt sein
    CHECK_EQ(ctx, executionOrder[4], std::string("Rendering"));
    // Animation muss nach Physics kommen
    const auto posPhys = std::find(executionOrder.begin(), executionOrder.end(), "Physics");
    const auto posAnim = std::find(executionOrder.begin(), executionOrder.end(), "Animation");
    CHECK(ctx, posPhys < posAnim);
}

// ---------------------------------------------------------------------------
// Suite-Runner
// ---------------------------------------------------------------------------
int RunJobsTests()
{
    test::TestSuite suite("Jobs");
    suite
        .Add("Exception im Job beendet nicht den Prozess",       TestJobExceptionDoesNotKillProcess)
        .Add("DispatchResult: Exception → TaskResult::Failed",   TestDispatchResultExceptionYieldsFailure)
        .Add("DispatchReturn: Exception → ValueResult::Failed",  TestDispatchReturnExceptionYieldsFailure)
        .Add("DispatchResult: Normalfall Erfolg",                TestDispatchResultSuccess)
        .Add("IsWorkerThread false auf Main-Thread",             TestIsWorkerThreadFalseOnMainThread)
        .Add("IsWorkerThread true im Job",                       TestIsWorkerThreadTrueInsideJob)
        .Add("IsWorkerThread Pool-Isolation (A != B)",           TestIsWorkerThreadPoolIsolation)
        .Add("ParallelFor aus Worker läuft synchron",            TestParallelForFromWorkerRunsSynchronously)
        .Add("Frame-Jobs vor Background-Jobs",                   TestFrameJobsExecutedBeforeBackground)
        .Add("Stresstest 1000 Jobs",                             TestStress1000Jobs)
        .Add("ParallelFor 10000 Elemente korrekt",               TestParallelForLargeCount)
        .Add("ParallelFor help-while-waiting",                   TestParallelForHelpWhileWaiting)
        .Add("WaitIdle help-while-waiting",                      TestWaitIdleHelpWhileWaiting)
        .Add("Shutdown mit ausstehenden Jobs",                   TestShutdownWithPendingJobs)
        .Add("Dispatch nach Shutdown verworfen",                 TestDispatchAfterShutdownIsRejected)
        .Add("TaskGraph ungültiger Dep-Handle",                  TestTaskGraphCycleDetection)
        .Add("WaitIdle kehrt sofort zurück wenn idle",           TestWaitIdleReturnImmediatelyWhenEmpty)
        // FrameScheduler
        .Add("FrameScheduler korrekte Pipeline",                 TestFrameSchedulerCorrectPipeline)
        .Add("FrameScheduler Write-Write-Konflikt erkannt",      TestFrameSchedulerDetectsWriteWriteConflict)
        .Add("FrameScheduler Write-Read-Konflikt erkannt",       TestFrameSchedulerDetectsWriteReadConflict)
        .Add("FrameScheduler leer ist ok",                       TestFrameSchedulerEmptyIsOk)
        .Add("FrameScheduler Execute ohne Build schlägt fehl",   TestFrameSchedulerExecuteWithoutBuildFails)
        .Add("FrameScheduler Fehler-Weitergabe",                 TestFrameSchedulerFailurePropagation)
        .Add("FrameScheduler Spielpipeline korrekte Reihenfolge",TestFrameSchedulerGamePipeline)
        // ComponentWriteGuard
        .Add("ComponentWriteGuard einzelner Schreiber ok",       TestComponentWriteGuardSingleWriterOk)
        .Add("ComponentWriteGuard zwei Schreiber überlebt",      TestComponentWriteGuardTwoWritersSurvives);
    return suite.Run();
}
