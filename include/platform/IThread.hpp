#pragma once

#include <cstdint>
#include <functional>

namespace engine::platform {

enum class ThreadPriority
{
    Low,
    Normal,
    High,
    Critical
};

struct ThreadAffinity
{
    int coreIndex = -1;
    bool preferPerformanceCores = false;
};

class IMutex
{
public:
    virtual ~IMutex() = default;
    virtual void Lock() = 0;
    virtual void Unlock() = 0;
    [[nodiscard]] virtual bool TryLock() = 0;
};

class IJobSystem
{
public:
    virtual ~IJobSystem() = default;
    virtual void Initialize(uint32_t numWorkerThreads) = 0;
    virtual void Shutdown() = 0;
};

class IThread
{
public:
    virtual ~IThread() = default;
    virtual void Start(const std::function<void()>& entryPoint) = 0;
    virtual void Join() = 0;
    virtual void SetPriority(ThreadPriority priority) = 0;
    virtual void SetAffinity(const ThreadAffinity& affinity) = 0;
    virtual void SetName(const char* name) = 0;
    [[nodiscard]] virtual bool IsRunning() const = 0;
};

class IThreadFactory
{
public:
    virtual ~IThreadFactory() = default;
    [[nodiscard]] virtual IThread* CreateThread() = 0;
    virtual void DestroyThread(IThread* thread) = 0;
    [[nodiscard]] virtual int GetHardwareConcurrency() const = 0;
    virtual void SleepMs(int milliseconds) const = 0;
    [[nodiscard]] virtual IMutex* CreateMutex() = 0;
    [[nodiscard]] virtual IJobSystem* CreateJobSystem(uint32_t numWorkerThreads) = 0;
};

} // namespace engine::platform
