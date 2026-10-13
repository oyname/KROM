#include "Win32Internal.hpp"

namespace engine::platform::win32 {

#ifdef _WIN32

namespace {
class NullJobSystem final : public IJobSystem
{
public:
    explicit NullJobSystem(uint32_t workers) : m_workers(workers) {}
    void Initialize(uint32_t numWorkerThreads) override { m_workers = numWorkerThreads; }
    void Shutdown() override {}
private:
    uint32_t m_workers = 0;
};

int TranslatePriority(ThreadPriority p)
{
    switch (p) {
    case ThreadPriority::Low: return THREAD_PRIORITY_BELOW_NORMAL;
    case ThreadPriority::High: return THREAD_PRIORITY_ABOVE_NORMAL;
    case ThreadPriority::Critical: return THREAD_PRIORITY_HIGHEST;
    case ThreadPriority::Normal:
    default: return THREAD_PRIORITY_NORMAL;
    }
}

std::wstring Widen(const char* text)
{
    if (!text || !*text)
        return {};
    const int count = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (count <= 0)
        return {};
    std::wstring out(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, -1, out.data(), count);
    if (!out.empty() && out.back() == L'\0')
        out.pop_back();
    return out;
}
}

Win32Mutex::Win32Mutex() { InitializeCriticalSection(&m_cs); }
Win32Mutex::~Win32Mutex() { DeleteCriticalSection(&m_cs); }
void Win32Mutex::Lock() { EnterCriticalSection(&m_cs); }
void Win32Mutex::Unlock() { LeaveCriticalSection(&m_cs); }
bool Win32Mutex::TryLock() { return TryEnterCriticalSection(&m_cs) != 0; }

Win32Thread::~Win32Thread()
{
    Join();
}

void Win32Thread::Start(const std::function<void()>& fn)
{
    Join();
    m_running = true;
    m_thread = std::thread([fn, this]() {
        fn();
        m_running = false;
    });
}

void Win32Thread::Join()
{
    if (m_thread.joinable())
        m_thread.join();
}

void Win32Thread::SetPriority(ThreadPriority p)
{
    if (m_thread.joinable())
        ::SetThreadPriority(m_thread.native_handle(), TranslatePriority(p));
}

void Win32Thread::SetAffinity(const ThreadAffinity& a)
{
    if (m_thread.joinable() && a.coreIndex >= 0)
        SetThreadAffinityMask(m_thread.native_handle(), 1ull << a.coreIndex);
}

void Win32Thread::SetName(const char* name)
{
    if (m_thread.joinable()) {
        const auto wide = Widen(name);
        if (!wide.empty())
            SetThreadDescription(m_thread.native_handle(), wide.c_str());
    }
}

bool Win32Thread::IsRunning() const
{
    return m_running.load();
}

IThread* Win32ThreadFactory::CreateThread() { return new Win32Thread(); }
void Win32ThreadFactory::DestroyThread(IThread* t) { delete t; }
int Win32ThreadFactory::GetHardwareConcurrency() const { return static_cast<int>(std::thread::hardware_concurrency() ? std::thread::hardware_concurrency() : 1u); }
void Win32ThreadFactory::SleepMs(int ms) const { if (ms > 0) Sleep(static_cast<DWORD>(ms)); }
IMutex* Win32ThreadFactory::CreateMutex() { return new Win32Mutex(); }
IJobSystem* Win32ThreadFactory::CreateJobSystem(uint32_t workers) { return new NullJobSystem(workers); }

#endif

} // namespace engine::platform::win32
