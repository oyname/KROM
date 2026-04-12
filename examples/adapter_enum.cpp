// adapter_enum.cpp
//
// KROM Engine — Adapter Enumeration
//
// Enumerates all available GPU adapters for the DX11 backend and prints
// their properties to the console. No window, renderer, or event loop needed.
//
// Build (Visual Studio, x64, Windows):
//   Add to a Win32 console project and link against engine_core + engine_dx11.
//   Use krom_link_self_registering_addon(my_target engine_dx11) in CMake.
//
// Expected output (example):
//
//   ========================================
//    KROM Adapter Enumeration
//   ========================================
//
//   Adapter 0
//     Name         : NVIDIA GeForce RTX 4090
//     Dedicated VRAM: 24576 MB
//     Discrete     : YES
//     Feature Level: 121
//     Best         : YES  <---
//
//   Adapter 1
//     Name         : Intel(R) UHD Graphics 770
//     Dedicated VRAM: 128 MB
//     Discrete     : NO
//     Feature Level: 121
//     Best         : NO
//
//   ========================================
//   Best adapter: 0  [NVIDIA GeForce RTX 4090]
//   ========================================

#include "renderer/IDevice.hpp"
#include "core/Debug.hpp"
#include <iostream>
#include <sstream>

using namespace engine::renderer;

static std::string FormatMB(size_t bytes)
{
    std::ostringstream oss;
    oss << (bytes / (1024ull * 1024ull)) << " MB";
    return oss.str();
}

int main()
{
    std::cout
        << "\n"
        << "========================================\n"
        << " KROM Adapter Enumeration\n"
        << "========================================\n\n";

    if (!DeviceFactory::IsRegistered(DeviceFactory::BackendType::DirectX11))
    {
        std::cout << "  DX11 backend not registered.\n"
                  << "  Make sure engine_dx11 is linked via "
                     "krom_link_self_registering_addon.\n\n";
        return 1;
    }

    const auto adapters = DeviceFactory::EnumerateAdapters(
        DeviceFactory::BackendType::DirectX11);

    if (adapters.empty())
    {
        std::cout << "  No hardware adapters found.\n\n";
        return 1;
    }

    const uint32_t bestIndex = DeviceFactory::FindBestAdapter(adapters);

    for (auto backend : {
        DeviceFactory::BackendType::DirectX11,
        DeviceFactory::BackendType::Vulkan })
    {
        const char* name = (backend == DeviceFactory::BackendType::DirectX11)
            ? "DirectX11" : "Vulkan";

        if (!DeviceFactory::IsRegistered(backend))
        {
            std::cout << name << ": not registered\n\n";
            continue;
        }

        const auto adapters = DeviceFactory::EnumerateAdapters(backend);
        std::cout << name << ":\n";

        for (const auto& a : adapters)
            std::cout << "  [" << a.index << "] " << a.name
            << "  " << (a.dedicatedVRAM / (1024 * 1024)) << " MB"
            << (a.isDiscrete ? "  discrete" : "  integrated")
            << "\n";

        std::cout << "\n";
    }

    const auto& best = adapters[bestIndex];
    std::cout
        << "========================================\n"
        << "Best adapter: " << best.index
        << "  [" << best.name << "]\n"
        << "========================================\n\n";

    return 0;
}
