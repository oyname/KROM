#include "DX11Device.hpp"
#include "core/Debug.hpp"

#ifdef _WIN32
#   define WIN32_LEAN_AND_MEAN
#   define NOMINMAX
#   include <windows.h>
#   include <d3d11.h>
#   include <d3dcompiler.h>
#   include <dxgi.h>
#endif

namespace engine::renderer::dx11 {

// =============================================================================
// Adapter-Enumeration (identisch zur vorherigen Version)
// =============================================================================

#ifdef _WIN32
static int FeatureLevelToInt(D3D_FEATURE_LEVEL fl) noexcept
{
    switch (fl) {
    case D3D_FEATURE_LEVEL_10_0: return 100;
    case D3D_FEATURE_LEVEL_10_1: return 101;
    case D3D_FEATURE_LEVEL_11_0: return 110;
    case D3D_FEATURE_LEVEL_11_1: return 111;
    case D3D_FEATURE_LEVEL_12_0: return 120;
    case D3D_FEATURE_LEVEL_12_1: return 121;
    default:                     return 0;
    }
}
static std::string WCharToUtf8(const WCHAR* src) noexcept
{
    char buf[256] = {};
    WideCharToMultiByte(CP_UTF8, 0, src, -1, buf, static_cast<int>(sizeof(buf)-1), nullptr, nullptr);
    return buf;
}
#endif

std::vector<engine::renderer::AdapterInfo> DX11Device::EnumerateAdaptersImpl()
{
    std::vector<engine::renderer::AdapterInfo> result;
#ifdef _WIN32
    IDXGIFactory* factory = nullptr;
    if (FAILED(CreateDXGIFactory(__uuidof(IDXGIFactory), reinterpret_cast<void**>(&factory))))
    { Debug::LogError("DX11Device.cpp: EnumerateAdapters -- CreateDXGIFactory failed"); return result; }

    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0,
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
    };
    IDXGIAdapter* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
    {
        DXGI_ADAPTER_DESC desc = {};
        adapter->GetDesc(&desc);
        const std::wstring wn = desc.Description;
        const bool soft = desc.DedicatedVideoMemory == 0 ||
                          wn.find(L"Microsoft Basic") != std::wstring::npos ||
                          wn.find(L"WARP") != std::wstring::npos;
        if (!soft) {
            D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_10_0;
            if (SUCCEEDED(D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0u,
                                            levels, static_cast<UINT>(std::size(levels)),
                                            D3D11_SDK_VERSION, nullptr, &fl, nullptr)))
            {
                engine::renderer::AdapterInfo info;
                info.index         = static_cast<uint32_t>(result.size());
                info.name          = WCharToUtf8(desc.Description);
                info.dedicatedVRAM = desc.DedicatedVideoMemory;
                info.isDiscrete    = true;
                info.featureLevel  = FeatureLevelToInt(fl);
                Debug::Log("DX11Device.cpp: Adapter[%u] %s FL=%d VRAM=%zuMB",
                    info.index, info.name.c_str(), info.featureLevel,
                    info.dedicatedVRAM / (1024u*1024u));
                result.push_back(std::move(info));
            }
        }
        adapter->Release();
    }
    factory->Release();
#endif
    return result;
}


} // namespace engine::renderer::dx11
