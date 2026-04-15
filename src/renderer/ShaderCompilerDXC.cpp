#include "ShaderCompilerInternal.hpp"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <vector>
#ifdef _WIN32
#   include <process.h>
#endif

namespace engine::renderer::internal {


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

    fs::path toolPath;
    if (const char* dxcPath = std::getenv("DXC_PATH"))
    {
        const fs::path candidate = fs::path(dxcPath);
        if (fs::exists(candidate))
            toolPath = candidate;
    }

    if (toolPath.empty())
    {
        if (const char* sdk = std::getenv("VULKAN_SDK"))
        {
            const fs::path bin = fs::path(sdk) / "Bin";
            const fs::path candidate = bin / "dxc.exe";
            if (fs::exists(candidate))
                toolPath = candidate;
        }
    }

    if (toolPath.empty())
    {
        const wchar_t* pathVar = _wgetenv(L"PATH");
        if (pathVar && *pathVar)
        {
            std::wstring pathList(pathVar);
            size_t start = 0u;
            while (start <= pathList.size())
            {
                const size_t end = pathList.find(L';', start);
                const std::wstring segment = pathList.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
                if (!segment.empty())
                {
                    const fs::path candidate = fs::path(segment) / "dxc.exe";
                    if (fs::exists(candidate))
                    {
                        toolPath = candidate;
                        break;
                    }
                }

                if (end == std::wstring::npos)
                    break;
                start = end + 1u;
            }
        }
    }

    if (toolPath.empty())
    {
        SetError(outError, "DirectX12 SM6/DXIL compilation requires dxc.exe (set DXC_PATH or install it in PATH/VULKAN_SDK/Bin)");
        return false;
    }

    const fs::path tempDir = fs::temp_directory_path() / "krom_dxc_fallback";
    std::error_code ec;
    fs::create_directories(tempDir, ec);
    const auto unique = Hex64(HashBytes(bundle.preprocessedSource.data(), bundle.preprocessedSource.size())) + "_" + Hex64(HashString(asset.entryPoint));
    const fs::path srcPath = tempDir / (unique + StageToToolExtension(asset.stage, assets::ShaderSourceLanguage::HLSL));
    const fs::path dxilPath = tempDir / (unique + ".dxil");

    {
        std::ofstream src(srcPath, std::ios::binary | std::ios::trunc);
        if (!src)
        {
            SetError(outError, "failed to create temporary HLSL shader source file");
            return false;
        }
        src << BuildShaderSource(bundle, defines);
    }

    const std::string entryPoint = asset.entryPoint.empty() ? "main" : asset.entryPoint;
    const std::string targetProfile = GetHlslTargetProfile(asset.stage, assets::ShaderTargetProfile::DirectX12_SM6);
    const std::wstring toolW = toolPath.wstring();
    const std::wstring srcW = srcPath.wstring();
    const std::wstring dxilW = dxilPath.wstring();
    const std::wstring entryW(entryPoint.begin(), entryPoint.end());
    const std::wstring targetW(targetProfile.begin(), targetProfile.end());

    std::vector<std::wstring> defineStorage;
    defineStorage.reserve(defines.size());
    for (const auto& define : defines)
    {
        std::wstring wide(define.begin(), define.end());
        wide += L"=1";
        defineStorage.push_back(std::move(wide));
    }

    std::vector<const wchar_t*> args;
    args.reserve(16u + defineStorage.size() * 2u);
    args.push_back(toolW.c_str());
    args.push_back(L"-T");
    args.push_back(targetW.c_str());
    args.push_back(L"-E");
    args.push_back(entryW.c_str());
    args.push_back(L"-Fo");
    args.push_back(dxilW.c_str());
    args.push_back(L"-HV");
    args.push_back(L"2021");
    args.push_back(L"-Ges");
#ifndef NDEBUG
    args.push_back(L"-Zi");
    args.push_back(L"-Od");
#else
    args.push_back(L"-O3");
#endif
    for (const auto& define : defineStorage)
    {
        args.push_back(L"-D");
        args.push_back(define.c_str());
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

} // namespace engine::renderer::internal
