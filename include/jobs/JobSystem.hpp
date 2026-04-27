#pragma once
// =============================================================================
// KROM Engine - jobs/JobSystem.hpp
// Einfaches Job-System / Thread-Pool für Frame- und Hintergrundarbeit.
// =============================================================================
#include "core/Debug.hpp"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace engine::jobs {

enum class TaskStatus : uint8_t
{
    Success = 0,
    Failed  = 1
};

struct TaskResult
{
    TaskStatus  status = TaskStatus::Success;
    const char* errorMessage = nullptr;

    [[nodiscard]] static TaskResult Ok() noexcept
    {
        return {};
    }

    [[nodiscard]] static TaskResult Fail(const char* message) noexcept
    {
        TaskResult result;
        result.status = TaskStatus::Failed;
        result.errorMessage = message;
        return result;
    }

    [[nodiscard]] bool Succeeded() const noexcept
    {
        return status == TaskStatus::Success;
    }

    [[nodiscard]] bool Failed() const noexcept
    {
        return status == TaskStatus::Failed;
    }
};

template<typename T>
struct ValueResult
{
    TaskResult       task = TaskResult::Ok();
    std::optional<T> value;

    [[nodiscard]] static ValueResult Success(T&& inValue)
    {
        ValueResult result;
        result.value.emplace(std::forward<T>(inValue));
        return result;
    }

    [[nodiscard]] static ValueResult Success(const T& inValue)
    {
        ValueResult result;
        result.value.emplace(inValue);
        return result;
    }

    [[nodiscard]] static ValueResult Fail(const char* message) noexcept
    {
        ValueResult result;
        result.task = TaskResult::Fail(message);
        return result;
    }

    [[nodiscard]] bool Succeeded() const noexcept
    {
        return task.Succeeded() && value.has_value();
    }
};

struct Job
{
    std::function<void()> task;
    std::atomic<int>*     dependencyCounter = nullptr;
};

class JobSystem
{
public:
     JobSystem() = default;
    ~JobSystem() { Shutdown(); }

    JobSystem(const JobSystem&)            = delete;
    JobSystem& operator=(const JobSystem&) = delete;

    // workerCount==0 → hardware_concurrency - 1
    void Initialize(uint32_t workerCount = 0u);
    void Shutdown();

    void Dispatch(std::function<void()> task);
    [[nodiscard]] std::future<TaskResult> DispatchResult(std::function<TaskResult()> task);
    void WaitIdle();

    template<typename F,
             typename Fn = std::decay_t<F>,
             typename R = std::invoke_result_t<Fn&, size_t, size_t>,
             typename = std::enable_if_t<std::is_same_v<R, void> || std::is_same_v<R, TaskResult>>>
    [[nodiscard]] TaskResult ParallelFor(size_t itemCount,
                                         F&& fn,
                                         size_t minBatchSize = 64u)
    {
        std::function<TaskResult(size_t, size_t)> wrapped;

        if constexpr (std::is_same_v<R, TaskResult>)
        {
            wrapped = std::forward<F>(fn);
        }
        else
        {
            wrapped = [callable = Fn(std::forward<F>(fn))](size_t begin, size_t end) mutable -> TaskResult {
                callable(begin, end);
                return TaskResult::Ok();
            };
        }

        return ParallelForImpl(itemCount, std::move(wrapped), minBatchSize);
    }

    template<typename F,
             typename Fn = std::decay_t<F>,
             typename R = std::invoke_result_t<Fn&>,
             typename = std::enable_if_t<!std::is_same_v<R, void> && !std::is_same_v<R, TaskResult>>>
    [[nodiscard]] std::future<ValueResult<R>> DispatchReturn(F&& task)
    {
        auto promise = std::make_shared<std::promise<ValueResult<R>>>();
        std::future<ValueResult<R>> future = promise->get_future();

        Dispatch([promise, task = Fn(std::forward<F>(task))]() mutable {
            promise->set_value(ValueResult<R>::Success(task()));
        });

        return future;
    }

    [[nodiscard]] uint32_t WorkerCount() const noexcept { return m_workerCount; }
    [[nodiscard]] bool     IsParallel()  const noexcept { return m_workerCount > 0u; }
    void ResetPeakActiveWorkers() noexcept { m_peakActiveWorkers.store(0u, std::memory_order_release); }
    [[nodiscard]] uint32_t PeakActiveWorkers() const noexcept { return m_peakActiveWorkers.load(std::memory_order_acquire); }

private:
    [[nodiscard]] TaskResult ParallelForImpl(size_t itemCount,
                                             std::function<TaskResult(size_t begin, size_t end)> fn,
                                             size_t minBatchSize);
    void WorkerLoop();

    uint32_t                  m_workerCount = 0u;
    std::vector<std::thread>  m_workers;
    mutable std::mutex        m_mutex;
    std::condition_variable   m_workCv;
    std::condition_variable   m_doneCv;
    std::deque<Job>           m_queue;
    std::atomic<uint32_t>     m_activeWorkers{ 0u };
    std::atomic<uint32_t>     m_peakActiveWorkers{ 0u };
    bool                      m_stop        = false;
    bool                      m_initialized = false;
};

} // namespace engine::jobs
