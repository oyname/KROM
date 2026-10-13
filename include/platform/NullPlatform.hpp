#pragma once
// =============================================================================
// KROM Engine - platform/NullPlatform.hpp
//
// Header-only: absichtlich. Alle hier definierten Klassen (NullFilesystem,
// NullTiming und FixedTiming) sind ausschließlich für Unit-Tests und
// headless-Builds gedacht. Ihre Implementierungen sind trivial (no-op oder
// reines in-memory) und haben keine externen Abhängigkeiten — eine eigene
// Translation Unit wäre reiner Overhead ohne Mehrwert.
//
// Produktiv-Plattformen (GLFW, Win32) haben stets eine eigene .cpp, da sie
// OS-APIs einbinden und nicht in jeden Test-Build gezogen werden sollen.
//
// StdFilesystem (fstream-basiert, produktionsreif) ist ausgelagert:
//   Deklaration:     include/platform/StdFilesystem.hpp
//   Implementierung: src/platform/StdFilesystem.cpp
// =============================================================================
#include "platform/IFilesystem.hpp"
#include "platform/IPlatform.hpp"
#include "platform/IPlatformTiming.hpp"
#include "platform/PlatformInput.hpp"
#include "platform/StdTiming.hpp"
#include <unordered_map>
#include <chrono>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <cmath>
#include <vector>
#include <memory>
#include "platform/StdFilesystem.hpp"

namespace engine::platform {

// =============================================================================
// NullFilesystem - in-memory für Tests
// =============================================================================
class NullFilesystem final : public IFilesystem
{
public:
    void InjectFile(std::string path, std::vector<uint8_t> data) { m_files[std::move(path)] = std::move(data); }
    void InjectText(std::string path, const std::string& text)   { m_files[std::move(path)] = {text.begin(), text.end()}; }

    bool ReadFile(const char* path, std::vector<uint8_t>& out) override
    {
        auto it = m_files.find(path ? path : "");
        if (it == m_files.end()) return false;
        out = it->second; return true;
    }
    bool ReadText(const char* path, std::string& out) override
    {
        std::vector<uint8_t> d; if (!ReadFile(path, d)) return false;
        out.assign(d.begin(), d.end()); return true;
    }
    bool WriteFile(const char* path, const void* data, size_t size) override
    {
        auto& b = m_files[path ? path : ""]; b.resize(size);
        if (size && data) std::memcpy(b.data(), data, size);
        return true;
    }
    bool WriteText(const char* path, const std::string& text) override
    { return WriteFile(path, text.data(), text.size()); }

    bool      FileExists  (const char* path) const override { return m_files.count(path ? path : "") > 0; }
    FileStats GetFileStats(const char* path) const override
    {
        auto it = m_files.find(path ? path : "");
        if (it == m_files.end()) return {};
        FileStats stats{};
        stats.sizeBytes = static_cast<uint64_t>(it->second.size());
        stats.exists = true;
        stats.isDirectory = false;
        return stats;
    }
    std::string ResolveAssetPath(const char* rel) const override { return m_root + (rel ? rel : ""); }
    void SetAssetRoot(const char* root) override { m_root = root ? root : ""; }
    bool CreateDirectories(const char*) override { return true; }
    void ListFiles(const char* dir, const char*, std::vector<std::string>& out) override
    {
        std::string d = dir ? dir : "";
        for (auto& kv : m_files) if (kv.first.substr(0, d.size()) == d) out.push_back(kv.first);
    }

private:
    std::unordered_map<std::string, std::vector<uint8_t>> m_files;
    std::string m_root;
};

// =============================================================================
// NullTiming
// =============================================================================
class NullTiming final : public IPlatformTiming
{
public:
    void     BeginFrame() override {}
    void     EndFrame()   override {}
    double   GetTimeSeconds()         const override { return 0.0; }
    double   GetDeltaSeconds()        const override { return 0.0; }
    float    GetDeltaSecondsF()       const override { return 0.f; }
    float    GetTimeSecondsF()        const override { return 0.f; }
    uint64_t GetFrameCount()          const override { return 0ull; }
    float    GetSmoothedFPS()         const override { return 0.f; }
    double   GetRawTimestampSeconds() const override { return 0.0; }
    void     SetMaxDeltaSeconds(double) override {}
};

// =============================================================================
// FixedTiming - deterministisch, für Tests
// =============================================================================
class FixedTiming final : public IPlatformTiming
{
public:
    explicit FixedTiming(double delta = 1.0/60.0) : m_delta(delta) {}
    void     BeginFrame() override { m_time += m_delta; ++m_frame; }
    void     EndFrame()   override {}
    double   GetTimeSeconds()         const override { return m_time; }
    double   GetDeltaSeconds()        const override { return m_delta; }
    float    GetDeltaSecondsF()       const override { return static_cast<float>(m_delta); }
    float    GetTimeSecondsF()        const override { return static_cast<float>(m_time); }
    uint64_t GetFrameCount()          const override { return m_frame; }
    float    GetSmoothedFPS()         const override { return static_cast<float>(1.0/m_delta); }
    double   GetRawTimestampSeconds() const override { return m_time; }
    void     SetMaxDeltaSeconds(double) override {}
private:
    double   m_delta; double m_time = 0.0; uint64_t m_frame = 0ull;
};


class NullThreadFactory final : public IThreadFactory
{
public:
    [[nodiscard]] IThread* CreateThread() override { return nullptr; }
    void DestroyThread(IThread* thread) override { delete thread; }
    [[nodiscard]] int GetHardwareConcurrency() const override { return 1; }
    void SleepMs(int) const override {}
    [[nodiscard]] IMutex* CreateMutex() override { return nullptr; }
    [[nodiscard]] IJobSystem* CreateJobSystem(uint32_t) override { return nullptr; }
};

class NullPlatform final : public IPlatform
{
public:
    bool Initialize() override
    {
        m_initialized = true;
        return true;
    }

    void Shutdown() override
    {
        for (auto& window : m_windows)
            if (window)
                window->Destroy();
        m_windows.clear();
        m_initialized = false;
    }

    void PumpEvents() override
    {
        m_timing.BeginFrame();
        m_input.BeginFrame();
    }

    [[nodiscard]] double GetTimeSeconds() const override
    {
        return m_timing.GetRawTimestampSeconds();
    }

    [[nodiscard]] IWindow* CreateWindow(const WindowDesc& desc) override
    {
        auto window = std::make_unique<HeadlessWindow>();
        if (!window->Create(desc))
            return nullptr;
        auto* out = window.get();
        m_windows.push_back(std::move(window));
        return out;
    }

    [[nodiscard]] IInput* GetInput() override { return &m_input; }
    [[nodiscard]] IThreadFactory* GetThreadFactory() override { return &m_threadFactory; }

private:
    bool m_initialized = false;
    StdTiming m_timing{};
    NullInput m_input{};
    NullThreadFactory m_threadFactory{};
    std::vector<std::unique_ptr<IWindow>> m_windows{};
};

} // namespace engine::platform
