#include "OpenGLDevice.hpp"
#include "core/Debug.hpp"

#ifdef _WIN32
#   define WIN32_LEAN_AND_MEAN
#   define NOMINMAX
#   include <windows.h>
#   include <dxgi.h>
#endif

#ifdef __linux__
#   include <algorithm>
#   include <cctype>
#   include <filesystem>
#   include <fstream>
#endif

namespace engine::renderer::opengl {

#ifdef _WIN32
static std::string WCharToUtf8GL(const WCHAR* src) noexcept
{
    char buf[256] = {};
    WideCharToMultiByte(CP_UTF8, 0, src, -1, buf, static_cast<int>(sizeof(buf) - 1u), nullptr, nullptr);
    return buf;
}

static std::vector<AdapterInfo> EnumeratePlatform()
{
    std::vector<AdapterInfo> out;
    IDXGIFactory* factory = nullptr;
    if (FAILED(CreateDXGIFactory(__uuidof(IDXGIFactory), reinterpret_cast<void**>(&factory))))
        return out;

    IDXGIAdapter* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
    {
        DXGI_ADAPTER_DESC desc = {};
        adapter->GetDesc(&desc);
        adapter->Release();

        AdapterInfo info{};
        info.index = static_cast<uint32_t>(out.size());
        info.name = WCharToUtf8GL(desc.Description);
        info.dedicatedVRAM = static_cast<size_t>(desc.DedicatedVideoMemory);
        info.isDiscrete = desc.DedicatedVideoMemory != 0;
        info.featureLevel = 0;
        out.push_back(std::move(info));
    }
    factory->Release();
    if (out.empty())
        out.push_back({0u, "Default OpenGL Adapter", 0u, true, 0});
    return out;
}
#elif defined(__linux__)
static std::vector<AdapterInfo> EnumeratePlatform()
{
    namespace fs = std::filesystem;
    std::vector<AdapterInfo> out;
    std::error_code ec;
    const fs::path drmRoot{"/sys/class/drm"};
    if (!fs::exists(drmRoot, ec))
        return {{0u, "Default OpenGL Adapter", 0u, true, 0}};

    std::vector<fs::path> cards;
    for (const auto& entry : fs::directory_iterator(drmRoot, ec))
    {
        const std::string fname = entry.path().filename().string();
        if (fname.rfind("card", 0) != 0)
            continue;
        bool digits = fname.size() > 4;
        for (size_t i = 4; i < fname.size() && digits; ++i)
            digits = std::isdigit(static_cast<unsigned char>(fname[i])) != 0;
        if (digits)
            cards.push_back(entry.path());
    }
    std::sort(cards.begin(), cards.end());
    for (const auto& cardPath : cards)
    {
        const auto devPath = cardPath / "device";
        if (!fs::exists(devPath, ec))
            continue;
        std::string vendorHex;
        if (std::ifstream vf(devPath / "vendor"); vf)
            vf >> vendorHex;
        std::string gpuName;
        if (std::ifstream lf(devPath / "label"); lf)
            std::getline(lf, gpuName);
        if (gpuName.empty())
        {
            if (vendorHex == "0x10de") gpuName = "NVIDIA GPU";
            else if (vendorHex == "0x1002") gpuName = "AMD GPU";
            else if (vendorHex == "0x8086") gpuName = "Intel GPU";
            else gpuName = "Unknown GPU";
        }
        size_t vram = 0u;
        if (std::ifstream mf(devPath / "mem_info_vram_total"); mf)
            mf >> vram;
        out.push_back({static_cast<uint32_t>(out.size()), gpuName, vram, vendorHex == "0x10de" || vendorHex == "0x1002", 0});
    }
    if (out.empty())
        out.push_back({0u, "Default OpenGL Adapter", 0u, true, 0});
    return out;
}
#else
static std::vector<AdapterInfo> EnumeratePlatform()
{
    return {{0u, "Default OpenGL Adapter", 0u, true, 0}};
}
#endif

std::vector<AdapterInfo> OpenGLDevice::EnumerateAdaptersImpl()
{
    return EnumeratePlatform();
}

} // namespace engine::renderer::opengl
