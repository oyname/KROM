#include "ShaderCompilerInternal.hpp"
#include "renderer/ShaderBindingModel.hpp"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <vector>
#ifdef _WIN32
#   include <process.h>
#endif

namespace engine::renderer::internal {

// -----------------------------------------------------------------------------
// Tool resolution — shared between DXIL and SPIR-V paths
// -----------------------------------------------------------------------------
#ifdef _WIN32
static std::filesystem::path FindDxcExe()
{
    namespace fs = std::filesystem;

    if (const char* explicit_path = std::getenv("DXC_PATH"))
    {
        const fs::path candidate(explicit_path);
        if (fs::exists(candidate))
            return candidate;
    }

    if (const char* sdk = std::getenv("VULKAN_SDK"))
    {
        const fs::path candidate = fs::path(sdk) / "Bin" / "dxc.exe";
        if (fs::exists(candidate))
            return candidate;
    }

    if (const wchar_t* pathVar = _wgetenv(L"PATH"))
    {
        std::wstring pathList(pathVar);
        size_t start = 0u;
        while (start <= pathList.size())
        {
            const size_t end = pathList.find(L';', start);
            const std::wstring segment = pathList.substr(start,
                end == std::wstring::npos ? std::wstring::npos : end - start);
            if (!segment.empty())
            {
                const fs::path candidate = fs::path(segment) / "dxc.exe";
                if (fs::exists(candidate))
                    return candidate;
            }
            if (end == std::wstring::npos)
                break;
            start = end + 1u;
        }
    }

    return {};
}
#endif

// -----------------------------------------------------------------------------
// HLSL → DXIL (DirectX 12 SM6)
// -----------------------------------------------------------------------------
bool CompileToDxilWithTool(const assets::ShaderAsset& asset,
                           const SourceBundle& bundle,
                           const std::vector<std::string>& defines,
                           assets::CompiledShaderArtifact& outCompiled,
                           std::string* outError)
{
#ifdef _WIN32
    if (asset.sourceLanguage != assets::ShaderSourceLanguage::HLSL)
    {
        SetError(outError, "DirectX12 SM6/DXIL compilation requires HLSL shader sources");
        return false;
    }

    namespace fs = std::filesystem;

    const fs::path toolPath = FindDxcExe();
    if (toolPath.empty())
    {
        SetError(outError, "DirectX12 SM6/DXIL compilation requires dxc.exe "
                           "(set DXC_PATH or install it in PATH/VULKAN_SDK/Bin)");
        return false;
    }

    const fs::path tempDir = fs::temp_directory_path() / "krom_dxc_dxil";
    std::error_code ec;
    fs::create_directories(tempDir, ec);

    const auto unique = Hex64(HashBytes(bundle.preprocessedSource.data(),
                                        bundle.preprocessedSource.size()))
                      + "_" + Hex64(HashString(asset.entryPoint));
    const fs::path srcPath  = tempDir / (unique + StageToToolExtension(asset.stage,
                                                     assets::ShaderSourceLanguage::HLSL));
    const fs::path dxilPath = tempDir / (unique + ".dxil");

    {
        std::ofstream src(srcPath, std::ios::binary | std::ios::trunc);
        if (!src)
        {
            SetError(outError, "failed to create temporary HLSL source file");
            return false;
        }
        src << BuildShaderSource(bundle, defines);
    }

    const std::string entryPoint    = asset.entryPoint.empty() ? "main" : asset.entryPoint;
    const std::string targetProfile = GetHlslTargetProfile(asset.stage,
                                                           assets::ShaderTargetProfile::DirectX12_SM6);
    const std::wstring toolW   = toolPath.wstring();
    const std::wstring srcW    = srcPath.wstring();
    const std::wstring dxilW   = dxilPath.wstring();
    const std::wstring entryW(entryPoint.begin(), entryPoint.end());
    const std::wstring targetW(targetProfile.begin(), targetProfile.end());

    std::vector<std::wstring> defineStorage;
    defineStorage.reserve(defines.size());
    for (const auto& d : defines)
    {
        std::wstring wide(d.begin(), d.end());
        wide += L"=1";
        defineStorage.push_back(std::move(wide));
    }

    std::vector<const wchar_t*> args;
    args.reserve(16u + defineStorage.size() * 2u);
    args.push_back(toolW.c_str());
    args.push_back(L"-T");  args.push_back(targetW.c_str());
    args.push_back(L"-E");  args.push_back(entryW.c_str());
    args.push_back(L"-Fo"); args.push_back(dxilW.c_str());
    args.push_back(L"-HV"); args.push_back(L"2021");
    args.push_back(L"-Ges");
#ifndef NDEBUG
    args.push_back(L"-Zi");
    args.push_back(L"-Od");
#else
    args.push_back(L"-O3");
#endif
    for (const auto& d : defineStorage)
    {
        args.push_back(L"-D");
        args.push_back(d.c_str());
    }
    args.push_back(srcW.c_str());
    args.push_back(nullptr);

    const intptr_t rc = _wspawnv(_P_WAIT, toolW.c_str(), args.data());
    if (rc != 0 || !fs::exists(dxilPath))
    {
        SetError(outError, "dxc.exe failed to compile DirectX12 SM6/DXIL artifact");
        fs::remove(srcPath, ec);
        fs::remove(dxilPath, ec);
        return false;
    }

    if (!ReadBinaryFile(dxilPath, outCompiled.bytecode))
    {
        SetError(outError, "failed to read compiled DXIL output");
        fs::remove(srcPath, ec);
        fs::remove(dxilPath, ec);
        return false;
    }

    outCompiled.sourceText.clear();
    fs::remove(srcPath, ec);
    fs::remove(dxilPath, ec);
    return true;
#else
    (void)asset; (void)bundle; (void)defines; (void)outCompiled;
    SetError(outError, "DirectX12 SM6/DXIL compilation is only available on Windows builds");
    return false;
#endif
}

// -----------------------------------------------------------------------------
// HLSL → SPIR-V (Vulkan via DXC -spirv)
//
// Register→binding mapping matches BindingRegisterRanges exactly:
//   CB  register(bN) → binding N+0   via -fvk-b-shift 0  0
//   SRV register(tN) → binding N+16  via -fvk-t-shift 16 0
//   SMP register(sN) → binding N+32  via -fvk-s-shift 32 0
//   UAV register(uN) → binding N+48  via -fvk-u-shift 48 0
// -----------------------------------------------------------------------------
bool CompileHlslToSpirvWithDxc(const assets::ShaderAsset& asset,
                                const SourceBundle& bundle,
                                const std::vector<std::string>& defines,
                                assets::CompiledShaderArtifact& outCompiled,
                                std::string* outError)
{
#ifdef _WIN32
    if (asset.sourceLanguage != assets::ShaderSourceLanguage::HLSL)
    {
        SetError(outError, "Vulkan SPIR-V compilation via DXC requires HLSL shader sources");
        return false;
    }

    namespace fs = std::filesystem;

    const fs::path toolPath = FindDxcExe();
    if (toolPath.empty())
    {
        SetError(outError, "Vulkan SPIR-V compilation requires dxc.exe "
                           "(set DXC_PATH or install it in PATH/VULKAN_SDK/Bin)");
        return false;
    }

    const fs::path tempDir = fs::temp_directory_path() / "krom_dxc_spirv";
    std::error_code ec;
    fs::create_directories(tempDir, ec);

    const auto unique = Hex64(HashBytes(bundle.preprocessedSource.data(),
                                        bundle.preprocessedSource.size()))
                      + "_" + Hex64(HashString(asset.entryPoint));
    const fs::path srcPath = tempDir / (unique + StageToToolExtension(asset.stage,
                                                    assets::ShaderSourceLanguage::HLSL));
    const fs::path spvPath = tempDir / (unique + ".spv");

    {
        std::ofstream src(srcPath, std::ios::binary | std::ios::trunc);
        if (!src)
        {
            SetError(outError, "failed to create temporary HLSL source file");
            return false;
        }
        src << BuildShaderSource(bundle, defines);
    }

    const std::string entryPoint    = asset.entryPoint.empty() ? "main" : asset.entryPoint;
    const std::string targetProfile = GetHlslTargetProfile(asset.stage,
                                                           assets::ShaderTargetProfile::DirectX12_SM6);
    const std::wstring toolW   = toolPath.wstring();
    const std::wstring srcW    = srcPath.wstring();
    const std::wstring spvW    = spvPath.wstring();
    const std::wstring entryW(entryPoint.begin(), entryPoint.end());
    const std::wstring targetW(targetProfile.begin(), targetProfile.end());

    std::vector<std::wstring> defineStorage;
    defineStorage.reserve(defines.size());
    for (const auto& d : defines)
    {
        std::wstring wide(d.begin(), d.end());
        wide += L"=1";
        defineStorage.push_back(std::move(wide));
    }

    // Binding register shift values from BindingRegisterRanges
    const std::wstring spaceW   = std::to_wstring(BindingRegisterRanges::RegisterSpace);
    const std::wstring cbShiftW = std::to_wstring(BindingRegisterRanges::ConstantBufferBase);
    const std::wstring srvShiftW= std::to_wstring(BindingRegisterRanges::ShaderResourceBase);
    const std::wstring smpShiftW= std::to_wstring(BindingRegisterRanges::SamplerBase);
    const std::wstring uavShiftW= std::to_wstring(BindingRegisterRanges::UnorderedAccessBase);

    std::vector<const wchar_t*> args;
    args.reserve(32u + defineStorage.size() * 2u);
    args.push_back(toolW.c_str());
    args.push_back(L"-T");       args.push_back(targetW.c_str());
    args.push_back(L"-E");       args.push_back(entryW.c_str());
    args.push_back(L"-spirv");
            args.push_back(L"-fvk-use-dx-layout");
args.push_back(L"-fvk-b-shift"); args.push_back(cbShiftW.c_str());  args.push_back(spaceW.c_str());
    args.push_back(L"-fvk-t-shift"); args.push_back(srvShiftW.c_str()); args.push_back(spaceW.c_str());
    args.push_back(L"-fvk-s-shift"); args.push_back(smpShiftW.c_str()); args.push_back(spaceW.c_str());
    args.push_back(L"-fvk-u-shift"); args.push_back(uavShiftW.c_str()); args.push_back(spaceW.c_str());
    args.push_back(L"-HV");     args.push_back(L"2021");
    args.push_back(L"-Ges");
    args.push_back(L"-Fo");     args.push_back(spvW.c_str());
#ifndef NDEBUG
    args.push_back(L"-Zi");
    args.push_back(L"-Od");
#else
    args.push_back(L"-O3");
#endif
    for (const auto& d : defineStorage)
    {
        args.push_back(L"-D");
        args.push_back(d.c_str());
    }
    args.push_back(srcW.c_str());
    args.push_back(nullptr);

    const intptr_t rc = _wspawnv(_P_WAIT, toolW.c_str(), args.data());
    if (rc != 0 || !fs::exists(spvPath))
    {
        SetError(outError, "dxc.exe failed to compile Vulkan SPIR-V artifact from HLSL");
        fs::remove(srcPath, ec);
        fs::remove(spvPath, ec);
        return false;
    }

    if (!ReadBinaryFile(spvPath, outCompiled.bytecode))
    {
        SetError(outError, "failed to read compiled SPIR-V output");
        fs::remove(srcPath, ec);
        fs::remove(spvPath, ec);
        return false;
    }

    outCompiled.sourceText.clear();
    fs::remove(srcPath, ec);
    fs::remove(spvPath, ec);
    return true;
#else
    (void)asset; (void)bundle; (void)defines; (void)outCompiled;
    SetError(outError, "Vulkan SPIR-V compilation via DXC is only available on Windows builds");
    return false;
#endif
}

} // namespace engine::renderer::internal
