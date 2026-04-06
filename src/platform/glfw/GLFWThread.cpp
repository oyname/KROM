#include "GLFWInternal.hpp"

#include <chrono>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#elif defined(__APPLE__)
#include <pthread.h>
#include <TargetConditionals.h>
#elif defined(__linux__)
#include <pthread.h>
#include <sched.h>
#include <sys/prctl.h>
#endif

namespace engine::platform {

namespace {

#if defined(_WIN32)
int TranslatePriority(ThreadPriority p)
{
    switch (p)
    {
    case ThreadPriority::Low: return THREAD_PRIORITY_BELOW_NORMAL;
    case ThreadPriority::High: return THREAD_PRIORITY_ABOVE_NORMAL;
    case ThreadPriority::Critical: return THREAD_PRIORITY_HIGHEST;
    case ThreadPriority::Normal:
    default: return THREAD_PRIORITY_NORMAL;
    }
}
#elif defined(__linux__)
int TranslatePriority(ThreadPriority p)
{
    switch (p)
    {
    case ThreadPriority::Low: return 1;
    case ThreadPriority::High: return 50;
    case ThreadPriority::Critical: return 80;
    case ThreadPriority::Normal:
    default: return 20;
    }
}
#elif defined(__APPLE__)
qos_class_t TranslateQos(ThreadPriority p)
{
    switch (p)
    {
    case ThreadPriority::Low: return QOS_CLASS_UTILITY;
    case ThreadPriority::High: return QOS_CLASS_USER_INITIATED;
    case ThreadPriority::Critical: return QOS_CLASS_USER_INTERACTIVE;
    case ThreadPriority::Normal:
    default: return QOS_CLASS_DEFAULT;
    }
}
#endif

} // namespace

GLFWThread::~GLFWThread()
{
    if (m_thread.joinable())
        m_thread.join();
}

void GLFWThread::Start(const std::function<void()>& entryPoint)
{
    if (m_running.load())
        Join();

    m_entryPoint = entryPoint;
    m_running = true;
    m_thread = std::thread([this]() {
        ApplyCurrentThreadSettings();
        if (m_entryPoint)
            m_entryPoint();
        m_running = false;
    });
}

void GLFWThread::Join()
{
    if (m_thread.joinable())
        m_thread.join();
    m_running = false;
}

void GLFWThread::SetPriority(ThreadPriority priority)
{
    m_priority = priority;
}

void GLFWThread::SetAffinity(const ThreadAffinity& affinity)
{
    m_affinity = affinity;
    if (!m_thread.joinable() || affinity.coreIndex < 0)
        return;

#if defined(__linux__)
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(static_cast<unsigned>(affinity.coreIndex), &cpuset);
    pthread_setaffinity_np(m_thread.native_handle(), sizeof(cpu_set_t), &cpuset);
#elif defined(_WIN32)
    SetThreadAffinityMask(m_thread.native_handle(), 1ull << affinity.coreIndex);
#else
    (void)affinity;
#endif
}

void GLFWThread::SetName(const char* name)
{
    m_name = name ? name : "";
#if defined(_WIN32)
    if (m_thread.joinable())
    {
        std::wstring wide(m_name.begin(), m_name.end());
        SetThreadDescription(m_thread.native_handle(), wide.c_str());
    }
#elif defined(__linux__)
    if (m_thread.joinable())
        pthread_setname_np(m_thread.native_handle(), m_name.c_str());
#elif defined(__APPLE__)
    if (pthread_self())
        pthread_setname_np(m_name.c_str());
#endif
}

void GLFWThread::ApplyCurrentThreadSettings()
{
#if defined(_WIN32)
    SetThreadPriority(GetCurrentThread(), TranslatePriority(m_priority));
#elif defined(__APPLE__)
    pthread_set_qos_class_self_np(TranslateQos(m_priority), 0);
#elif defined(__linux__)
    sched_param param{};
    param.sched_priority = TranslatePriority(m_priority);
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);
    if (!m_name.empty())
        prctl(PR_SET_NAME, m_name.c_str(), 0, 0, 0);
#endif
}

IThread* GLFWThreadFactory::CreateThread()
{
    return new GLFWThread();
}

void GLFWThreadFactory::DestroyThread(IThread* thread)
{
    delete thread;
}

int GLFWThreadFactory::GetHardwareConcurrency() const
{
    const auto hc = std::thread::hardware_concurrency();
    return hc > 0 ? static_cast<int>(hc) : 1;
}

void GLFWThreadFactory::SleepMs(int milliseconds) const
{
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

IMutex* GLFWThreadFactory::CreateMutex()
{
    return new StdMutex();
}

IJobSystem* GLFWThreadFactory::CreateJobSystem(uint32_t numWorkerThreads)
{
    auto* js = new NullJobSystem(numWorkerThreads);
    js->Initialize(numWorkerThreads);
    return js;
}

} // namespace engine::platform
