#pragma once
// =============================================================================
// KROM Engine - jobs/TaskGraph.hpp
// Frame-Task-Graph mit deklarierten Abhängigkeiten.
//
// Konzept:
//   - Tasks werden deklariert mit optionalen "muss nach X kommen"-Abhängigkeiten
//   - Build() sortiert topologisch und ermittelt Parallelisierungsebenen
//   - Execute() dispatcht parallele Ebenen über JobSystem, wartet pro Ebene
// =============================================================================
#include "jobs/JobSystem.hpp"
#include "core/Debug.hpp"
#include <vector>
#include <string>
#include <functional>
#include <cstdint>
#include <cassert>
#include <type_traits>
#include <utility>

namespace engine::jobs {

using TaskHandle = uint32_t;
static constexpr TaskHandle INVALID_TASK = UINT32_MAX;

class TaskGraph
{
public:
    struct TaskDesc
    {
        std::string                  name;
        std::vector<TaskHandle>      deps;     // Vorgänger
        std::function<TaskResult()>  fn;
        uint32_t                     level = 0u; // Ausführungsebene (nach Build())
    };

    template<typename F,
             typename Fn = std::decay_t<F>,
             typename R = std::invoke_result_t<Fn&>,
             typename = std::enable_if_t<std::is_same_v<R, void> || std::is_same_v<R, TaskResult>>>
    TaskHandle Add(std::string name,
                   std::vector<TaskHandle> deps,
                   F&& fn)
    {
        std::function<TaskResult()> wrapped;

        if constexpr (std::is_same_v<R, TaskResult>)
        {
            wrapped = std::forward<F>(fn);
        }
        else
        {
            wrapped = [callable = Fn(std::forward<F>(fn))]() mutable -> TaskResult {
                callable();
                return TaskResult::Ok();
            };
        }

        const TaskHandle h = static_cast<TaskHandle>(m_tasks.size());
        m_tasks.push_back({ std::move(name), std::move(deps), std::move(wrapped), 0u });
        m_built = false;
        return h;
    }

    bool Build()
    {
        const size_t n = m_tasks.size();
        if (n == 0u) { m_built = true; m_levels.clear(); return true; }

        for (uint32_t i = 0; i < static_cast<uint32_t>(n); ++i)
            for (TaskHandle dep : m_tasks[i].deps)
                if (dep >= n)
                {
                    Debug::LogError("TaskGraph.cpp: Build - Task '%s': "
                        "ungültiger Dep-Handle %u (max=%zu)",
                        m_tasks[i].name.c_str(), dep, n-1u);
                    return false;
                }

        enum Color : uint8_t { WHITE = 0, GRAY = 1, BLACK = 2 };
        std::vector<Color>    color(n, WHITE);
        std::vector<int>      longest(n, -1);

        bool hasCycle = false;

        struct Frame { uint32_t node; size_t depIdx; int parentDepth; };
        std::vector<Frame> stack;
        stack.reserve(n);

        auto launch = [&](uint32_t root) {
            if (color[root] == BLACK) return;
            stack.push_back({ root, 0u, -1 });
            color[root] = GRAY;
        };

        for (uint32_t i = 0; i < static_cast<uint32_t>(n); ++i)
        {
            if (color[i] != WHITE) continue;
            launch(i);

            while (!stack.empty() && !hasCycle)
            {
                Frame& f = stack.back();
                const uint32_t u = f.node;
                const auto& deps = m_tasks[u].deps;

                if (f.depIdx < deps.size())
                {
                    const TaskHandle v = deps[f.depIdx++];
                    if (color[v] == GRAY)
                    {
                        Debug::LogError(
                            "TaskGraph.cpp: Build - Zyklus: Task '%s' → Task '%s'",
                            m_tasks[u].name.c_str(), m_tasks[v].name.c_str());
                        hasCycle = true;
                    }
                    else if (color[v] == WHITE)
                    {
                        color[v] = GRAY;
                        stack.push_back({ v, 0u, static_cast<int>(f.depIdx-1u) });
                    }
                }
                else
                {
                    int maxDep = -1;
                    for (TaskHandle dep : deps)
                        maxDep = std::max(maxDep, longest[dep]);
                    longest[u] = maxDep + 1;
                    m_tasks[u].level = static_cast<uint32_t>(longest[u]);
                    color[u] = BLACK;
                    stack.pop_back();
                }
            }
            if (hasCycle) return false;
        }

        if (hasCycle) return false;

        uint32_t maxLevel = 0u;
        for (const auto& t : m_tasks)
            maxLevel = std::max(maxLevel, t.level);

        m_levels.assign(maxLevel + 1u, {});
        for (uint32_t i = 0; i < static_cast<uint32_t>(n); ++i)
            m_levels[m_tasks[i].level].push_back(i);

        m_built = true;

        Debug::Log("TaskGraph.cpp: Build - %zu tasks, %zu levels", n, m_levels.size());
        for (size_t lvl = 0; lvl < m_levels.size(); ++lvl)
        {
            std::string names;
            for (TaskHandle h : m_levels[lvl])
                names += m_tasks[h].name + " ";
            Debug::Log("TaskGraph.cpp:   Level %zu: %s", lvl, names.c_str());
        }
        return true;
    }

    [[nodiscard]] TaskResult Execute(JobSystem& js)
    {
        if (!m_built)
        {
            Debug::LogError("TaskGraph.cpp: Execute - graph is not built");
            return TaskResult::Fail("task graph is not built");
        }

        for (const auto& level : m_levels)
        {
            if (level.size() == 1u)
            {
                const TaskDesc& task = m_tasks[level[0]];
                TaskResult result = ExecuteTask(task);
                if (!result.Succeeded())
                {
                    LogTaskFailure(task, result);
                    return result;
                }
            }
            else
            {
                std::vector<std::future<TaskResult>> futures;
                futures.reserve(level.size());

                for (TaskHandle handle : level)
                {
                    const TaskDesc& task = m_tasks[handle];
                    futures.push_back(js.DispatchResult([this, &task]() {
                        return ExecuteTask(task);
                    }));
                }

                for (size_t i = 0; i < level.size(); ++i)
                {
                    const TaskDesc& task = m_tasks[level[i]];
                    TaskResult result = futures[i].get();
                    if (!result.Succeeded())
                    {
                        LogTaskFailure(task, result);
                        return result;
                    }
                }
            }
        }

        return TaskResult::Ok();
    }


    void SetTaskFunction(TaskHandle h, std::function<TaskResult()> fn)
    {
        assert(h < m_tasks.size());
        m_tasks[h].fn = std::move(fn);
    }

    void Clear() { m_tasks.clear(); m_levels.clear(); m_built = false; }

    [[nodiscard]] size_t TaskCount()  const noexcept { return m_tasks.size(); }
    [[nodiscard]] size_t LevelCount() const noexcept { return m_levels.size(); }
    [[nodiscard]] bool   IsBuilt()    const noexcept { return m_built; }

    [[nodiscard]] const TaskDesc& GetTask(TaskHandle h) const
    {
        assert(h < m_tasks.size());
        return m_tasks[h];
    }

private:
    [[nodiscard]] static TaskResult ExecuteTask(const TaskDesc& task)
    {
        if (!task.fn)
            return TaskResult::Fail("task has no function");

        return task.fn();
    }

    static void LogTaskFailure(const TaskDesc& task, const TaskResult& result)
    {
        Debug::LogError("TaskGraph.cpp: Execute - Task '%s' failed%s%s",
                        task.name.c_str(),
                        result.errorMessage ? ": " : "",
                        result.errorMessage ? result.errorMessage : "");
    }

    std::vector<TaskDesc>                m_tasks;
    std::vector<std::vector<TaskHandle>> m_levels;
    bool                                 m_built = false;
};

} // namespace engine::jobs
