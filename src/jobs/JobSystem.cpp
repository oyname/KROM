// =============================================================================
// KROM Engine - src/jobs/JobSystem.cpp
// Job-System: Thread-Pool-Implementierung.
// =============================================================================
#include "jobs/JobSystem.hpp"
#include <algorithm>

namespace engine::jobs {

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

void JobSystem::Dispatch(std::function<void()> task)
{
    if (!task)
    {
        Debug::LogError("JobSystem.cpp: Dispatch - job has no task");
        return;
    }

    if (!m_initialized || m_workerCount == 0u)
    {
        task();
        return;
    }

    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_queue.push_back({ std::move(task), nullptr });
    }
    m_workCv.notify_one();
}

std::future<TaskResult> JobSystem::DispatchResult(std::function<TaskResult()> task)
{
    auto promise = std::make_shared<std::promise<TaskResult>>();
    std::future<TaskResult> future = promise->get_future();

    Dispatch([promise, task = std::move(task)]() mutable {
        if (!task)
        {
            promise->set_value(TaskResult::Fail("job has no task"));
            return;
        }

        promise->set_value(task());
    });

    return future;
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

        Dispatch([&, i, begin, end]() {
            if (firstFailedBatch.load(std::memory_order_acquire) == numBatches)
            {
                const TaskResult result = fn(begin, end);
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
    }

    std::unique_lock<std::mutex> lock(doneMtx);
    doneCv.wait(lock, [&remaining] { return remaining.load(std::memory_order_acquire) == 0u; });

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
    std::unique_lock<std::mutex> lock(m_mutex);
    m_doneCv.wait(lock, [this] {
        return m_queue.empty() && m_activeWorkers.load() == 0u;
    });
}

void JobSystem::WorkerLoop()
{
    while (true)
    {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_workCv.wait(lock, [this] { return m_stop || !m_queue.empty(); });
            if (m_stop && m_queue.empty()) break;
            if (m_queue.empty()) continue;
            task = std::move(m_queue.front().task);
            m_queue.pop_front();
            const uint32_t activeWorkers = ++m_activeWorkers;
            uint32_t peakWorkers = m_peakActiveWorkers.load(std::memory_order_acquire);
            while (activeWorkers > peakWorkers
                && !m_peakActiveWorkers.compare_exchange_weak(peakWorkers,
                                                              activeWorkers,
                                                              std::memory_order_acq_rel,
                                                              std::memory_order_acquire))
            {
            }
        }

        if (task)
            task();
        else
            Debug::LogError("JobSystem.cpp: WorkerLoop - job has no task");

        {
            std::unique_lock<std::mutex> lock(m_mutex);
            if (--m_activeWorkers == 0u && m_queue.empty())
                m_doneCv.notify_all();
        }
    }
}

} // namespace engine::jobs
