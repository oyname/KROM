// =============================================================================
// KROM Engine - src/jobs/JobSystem.cpp
// Job-System: Thread-Pool-Implementierung.
// =============================================================================
#include "jobs/JobSystem.hpp"
#include <algorithm>

namespace engine::jobs {

// Zeigt auf den Pool dessen Worker dieser Thread gerade ist, sonst nullptr.
// Pointer statt bool: Worker von Pool A gelten nicht fälschlich als Worker von Pool B.
static thread_local const JobSystem* s_ownerPool = nullptr;

bool JobSystem::IsWorkerThread() const noexcept
{
    return s_ownerPool == this;
}

/*static*/ bool JobSystem::IsAnyWorkerThread() noexcept
{
    return s_ownerPool != nullptr;
}

namespace {

void UpdatePeakWorkerCount(std::atomic<uint32_t>& peakCounter, uint32_t activeWorkers) noexcept
{
    uint32_t peakWorkers = peakCounter.load(std::memory_order_acquire);
    while (activeWorkers > peakWorkers
        && !peakCounter.compare_exchange_weak(peakWorkers,
                                              activeWorkers,
                                              std::memory_order_acq_rel,
                                              std::memory_order_acquire))
    {
    }
}

} // namespace

void JobSystem::Initialize(uint32_t workerCount)
{
    if (m_initialized) return;
    m_stop = false;
    m_peakActiveWorkers.store(0u, std::memory_order_release);

    const uint32_t hw = std::thread::hardware_concurrency();
    m_workerCount = (workerCount == 0u)
        ? (hw > 1u ? hw - 1u : 1u)
        : workerCount;

    Debug::Log("JobSystem.cpp: Initialize - %u worker threads", m_workerCount);

    m_workers.reserve(m_workerCount);
    for (uint32_t i = 0; i < m_workerCount; ++i)
        m_workers.emplace_back([this] { WorkerLoop(); });

    m_initialized = true;
}

void JobSystem::Shutdown()
{
    if (!m_initialized) return;
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_stop = true;
    }
    m_workCv.notify_all();
    for (auto& t : m_workers) t.join();
    m_workers.clear();
    m_workerCount = 0u;
    m_initialized = false;
    Debug::Log("JobSystem.cpp: Shutdown complete");
}

bool JobSystem::Dispatch(std::function<void()> task, JobPriority priority)
{
    if (!task)
    {
        Debug::LogError("JobSystem.cpp: Dispatch - job has no task");
        return false;
    }

    if (!m_initialized || m_workerCount == 0u)
    {
        if (m_stop)
        {
            Debug::LogWarning("JobSystem.cpp: Dispatch - shutdown in progress, job rejected");
            return false;
        }
        task();
        return true;
    }

    {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (m_stop)
        {
            Debug::LogWarning("JobSystem.cpp: Dispatch - shutdown in progress, job rejected");
            return false;
        }
        auto& queue = (priority == JobPriority::Frame) ? m_frameQueue : m_backgroundQueue;
        queue.push_back({ std::move(task), nullptr });
    }
    m_workCv.notify_one();
    m_doneCv.notify_all();
    return true;
}

std::future<TaskResult> JobSystem::DispatchResult(std::function<TaskResult()> task)
{
    auto promise = std::make_shared<std::promise<TaskResult>>();
    std::future<TaskResult> future = promise->get_future();

    const bool dispatched = Dispatch([promise, task = std::move(task)]() mutable {
        if (!task)
        {
            promise->set_value(TaskResult::Fail("job has no task"));
            return;
        }

        try
        {
            promise->set_value(task());
        }
        catch (const std::exception& e)
        {
            Debug::LogError("JobSystem.cpp: DispatchResult - exception: %s", e.what());
            promise->set_value(TaskResult::Fail("exception in dispatched job"));
        }
        catch (...)
        {
            Debug::LogError("JobSystem.cpp: DispatchResult - unknown exception");
            promise->set_value(TaskResult::Fail("unknown exception in dispatched job"));
        }
    });

    if (!dispatched)
        promise->set_value(TaskResult::Fail("job rejected: shutdown in progress"));

    return future;
}

bool JobSystem::TryAcquireQueuedJob(Job& outJob)
{
    std::unique_lock<std::mutex> lock(m_mutex);
    if (m_frameQueue.empty() && m_backgroundQueue.empty())
        return false;

    auto& activeQueue = !m_frameQueue.empty() ? m_frameQueue : m_backgroundQueue;
    outJob = std::move(activeQueue.front());
    activeQueue.pop_front();

    const uint32_t activeWorkers = ++m_activeWorkers;
    UpdatePeakWorkerCount(m_peakActiveWorkers, activeWorkers);
    return true;
}

void JobSystem::ExecuteAcquiredJob(Job&& job)
{
    const JobSystem* const previousOwner = s_ownerPool;
    s_ownerPool = this;

    if (job.task)
    {
        try
        {
            job.task();
        }
        catch (const std::exception& e)
        {
            Debug::LogError("JobSystem.cpp: ExecuteAcquiredJob - unbehandelte Exception: %s", e.what());
        }
        catch (...)
        {
            Debug::LogError("JobSystem.cpp: ExecuteAcquiredJob - unbekannte Exception");
        }
    }
    else
    {
        Debug::LogError("JobSystem.cpp: ExecuteAcquiredJob - job has no task");
    }

    s_ownerPool = previousOwner;

    {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (--m_activeWorkers == 0u && m_frameQueue.empty() && m_backgroundQueue.empty())
            m_doneCv.notify_all();
    }
}

bool JobSystem::HelpExecuteOne()
{
    Job job;
    if (!TryAcquireQueuedJob(job))
        return false;

    ExecuteAcquiredJob(std::move(job));
    return true;
}

TaskResult JobSystem::ParallelForImpl(size_t itemCount,
                                      std::function<TaskResult(size_t, size_t)> fn,
                                      size_t minBatchSize)
{
    if (itemCount == 0u)
        return TaskResult::Ok();

    if (!fn)
    {
        Debug::LogError("JobSystem.cpp: ParallelFor - job has no task");
        return TaskResult::Fail("parallel job has no task");
    }

    // Aus einem Worker heraus würde das blockierende Wait den Pool erschöpfen.
    // Sicherste Variante: synchron ausführen.
    if (IsWorkerThread())
    {
        Debug::LogWarning("JobSystem.cpp: ParallelFor - aufgerufen aus Worker-Thread, "
                          "läuft synchron (Deadlock-Schutz)");
        return fn(0u, itemCount);
    }

    if (!m_initialized || m_workerCount == 0u || itemCount <= minBatchSize)
        return fn(0u, itemCount);

    const size_t numBatches = std::min(static_cast<size_t>(m_workerCount),
                                       (itemCount + minBatchSize - 1u) / minBatchSize);
    const size_t batchSize  = (itemCount + numBatches - 1u) / numBatches;

    std::atomic<size_t> remaining{ numBatches };
    std::atomic<size_t> firstFailedBatch{ numBatches };
    std::vector<TaskResult> batchResults(numBatches, TaskResult::Ok());
    std::mutex doneMtx;
    std::condition_variable doneCv;

    for (size_t i = 0; i < numBatches; ++i)
    {
        const size_t begin = i * batchSize;
        const size_t end   = std::min(begin + batchSize, itemCount);

        const bool dispatched = Dispatch([&, i, begin, end]() {
            if (firstFailedBatch.load(std::memory_order_acquire) == numBatches)
            {
                TaskResult result = TaskResult::Ok();
                try
                {
                    result = fn(begin, end);
                }
                catch (const std::exception& e)
                {
                    Debug::LogError("JobSystem.cpp: ParallelFor batch %zu - exception: %s",
                                    i, e.what());
                    result = TaskResult::Fail("exception in parallel batch");
                }
                catch (...)
                {
                    Debug::LogError("JobSystem.cpp: ParallelFor batch %zu - unknown exception", i);
                    result = TaskResult::Fail("unknown exception in parallel batch");
                }

                batchResults[i] = result;
                if (!result.Succeeded())
                {
                    size_t expected = numBatches;
                    firstFailedBatch.compare_exchange_strong(expected, i, std::memory_order_acq_rel);
                }
            }
            else
            {
                batchResults[i] = TaskResult::Fail("parallel batch skipped after earlier failure");
            }

            const bool last = (remaining.fetch_sub(1u, std::memory_order_acq_rel) == 1u);
            if (last)
            {
                std::unique_lock<std::mutex> lk(doneMtx);
                doneCv.notify_all();
            }
        });

        // Dispatch abgelehnt (Shutdown): Batch direkt als fehlgeschlagen zählen,
        // sonst hängt doneCv.wait() für immer.
        if (!dispatched)
        {
            batchResults[i] = TaskResult::Fail("job rejected: shutdown in progress");
            size_t expected = numBatches;
            firstFailedBatch.compare_exchange_strong(expected, i, std::memory_order_acq_rel);
            const bool last = (remaining.fetch_sub(1u, std::memory_order_acq_rel) == 1u);
            if (last)
            {
                std::unique_lock<std::mutex> lk(doneMtx);
                doneCv.notify_all();
            }
        }
    }

    while (remaining.load(std::memory_order_acquire) != 0u)
    {
        if (HelpExecuteOne())
            continue;

        std::unique_lock<std::mutex> lock(doneMtx);
        doneCv.wait(lock, [&remaining] { return remaining.load(std::memory_order_acquire) == 0u; });
    }

    const size_t failedBatch = firstFailedBatch.load(std::memory_order_acquire);
    if (failedBatch != numBatches)
    {
        const TaskResult& result = batchResults[failedBatch];
        Debug::LogError("JobSystem.cpp: ParallelFor - batch %zu failed%s%s",
                        failedBatch,
                        result.errorMessage ? ": " : "",
                        result.errorMessage ? result.errorMessage : "");
        return result.Succeeded()
            ? TaskResult::Fail("parallel batch failed")
            : result;
    }

    return TaskResult::Ok();
}

void JobSystem::WaitIdle()
{
    if (IsWorkerThread())
    {
        // WaitIdle aus einem Worker würde deadlocken, da m_activeWorkers nie 0 wird
        // solange der wartende Worker selbst noch zählt.
        Debug::LogError("JobSystem.cpp: WaitIdle - aufgerufen aus Worker-Thread, "
                        "würde deadlocken — wird ignoriert");
        return;
    }

    while (true)
    {
        if (HelpExecuteOne())
            continue;

        std::unique_lock<std::mutex> lock(m_mutex);
        if (m_frameQueue.empty()
            && m_backgroundQueue.empty()
            && m_activeWorkers.load(std::memory_order_acquire) == 0u)
        {
            return;
        }

        m_doneCv.wait(lock, [this] {
            return (!m_frameQueue.empty())
                || (!m_backgroundQueue.empty())
                || (m_activeWorkers.load(std::memory_order_acquire) == 0u);
        });
    }
}

void JobSystem::WorkerLoop()
{
    s_ownerPool = this;

    while (true)
    {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_workCv.wait(lock, [this] {
                return m_stop || !m_frameQueue.empty() || !m_backgroundQueue.empty();
            });

            if (m_stop && m_frameQueue.empty() && m_backgroundQueue.empty()) break;

            // Frame-Queue hat Vorrang
            auto& activeQueue = !m_frameQueue.empty() ? m_frameQueue : m_backgroundQueue;
            task = std::move(activeQueue.front().task);
            activeQueue.pop_front();

            const uint32_t activeWorkers = ++m_activeWorkers;
            UpdatePeakWorkerCount(m_peakActiveWorkers, activeWorkers);
        }

        ExecuteAcquiredJob(Job{ std::move(task), nullptr });
    }

    // Pointer zurücksetzen: Thread könnte danach einem anderen Pool übergeben werden.
    s_ownerPool = nullptr;
}

} // namespace engine::jobs
