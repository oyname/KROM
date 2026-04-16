#include "assets/TextureImporter.hpp"
#include "core/Debug.hpp"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <sstream>

namespace engine::assets {

namespace {
std::string ToLower(std::string s)
{
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
}

bool TextureImporter::Import(const TextureImportRequest& request, TextureAsset& outTexture)
{
    outTexture = {};
    if (request.sourcePath.extension() == ".tex")
        return ImportTextTexture(request, outTexture);

    DecodedImage decoded;
    if (!ImageDecoder::DecodeFromMemory(request.fileBytes.data(), request.fileBytes.size(), decoded))
    {
        Debug::LogError("TextureImporter: image decode failed for '%s'", request.sourcePath.filename().string().c_str());
        return false;
    }
    return ImportDecodedImage(request, decoded, outTexture);
}

bool TextureImporter::ImportTextTexture(const TextureImportRequest& request, TextureAsset& outTexture)
{
    std::string source(reinterpret_cast<const char*>(request.fileBytes.data()), request.fileBytes.size());
    std::istringstream iss(source);

    uint32_t w = 0u;
    uint32_t h = 0u;
    if (!(iss >> w >> h) || w == 0u || h == 0u)
    {
        Debug::LogError("TextureImporter: .tex header parse failed for '%s'", request.sourcePath.filename().string().c_str());
        return false;
    }

    outTexture.width = w;
    outTexture.height = h;
    outTexture.depth = 1u;
    outTexture.mipLevels = 1u;
    outTexture.arraySize = 1u;
    outTexture.format = TextureFormat::RGBA8_UNORM;
    outTexture.sRGB = false;
    outTexture.pixelData.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u, 255u);

    for (uint32_t i = 0u; i < w * h; ++i)
    {
        int r = 255;
        int g = 255;
        int b = 255;
        int a = 255;
        if (!(iss >> r >> g >> b >> a))
            break;
        outTexture.pixelData[i * 4u + 0u] = static_cast<uint8_t>(r);
        outTexture.pixelData[i * 4u + 1u] = static_cast<uint8_t>(g);
        outTexture.pixelData[i * 4u + 2u] = static_cast<uint8_t>(b);
        outTexture.pixelData[i * 4u + 3u] = static_cast<uint8_t>(a);
    }

    return true;
}

bool TextureImporter::ImportDecodedImage(const TextureImportRequest& request, const DecodedImage& image, TextureAsset& outTexture)
{
    if (!image.IsValid())
        return false;

    outTexture.width = image.width;
    outTexture.height = image.height;
    outTexture.depth = 1u;
    outTexture.mipLevels = 1u;
    outTexture.arraySize = 1u;

    switch (image.format)
    {
    case DecodedImageFormat::RGBA32F:
    {
        const size_t pixelCount = static_cast<size_t>(image.width) * static_cast<size_t>(image.height) * 4u;
        if (image.bytes.size() != pixelCount * sizeof(float))
            return false;

        outTexture.format = TextureFormat::RGBA16F;
        outTexture.sRGB = false;
        outTexture.pixelData.resize(pixelCount * sizeof(uint16_t));

        const float* src = reinterpret_cast<const float*>(image.bytes.data());
        uint16_t* dst = reinterpret_cast<uint16_t*>(outTexture.pixelData.data());
        for (size_t i = 0; i < pixelCount; ++i)
            dst[i] = Float32ToFloat16(src[i]);

        Debug::Log("TextureImporter: imported HDR texture '%s' %ux%u -> RGBA16F",
            request.sourcePath.filename().string().c_str(), outTexture.width, outTexture.height);
        return true;
    }
    case DecodedImageFormat::RGBA8:
        outTexture.format = TextureFormat::RGBA8_UNORM;
        outTexture.sRGB = InferSRGB(request.sourcePath);
        outTexture.pixelData = image.bytes;
        Debug::Log("TextureImporter: imported texture '%s' %ux%u sRGB=%d",
            request.sourcePath.filename().string().c_str(), outTexture.width, outTexture.height, outTexture.sRGB ? 1 : 0);
        return true;
    default:
        return false;
    }
}

bool TextureImporter::InferSRGB(const std::filesystem::path& path) noexcept
{
    const std::string lower = ToLower(path.filename().string());
    return lower.find("normal") == std::string::npos
        && lower.find("rough") == std::string::npos
        && lower.find("metal") == std::string::npos
        && lower.find("ao") == std::string::npos
        && lower.find("mask") == std::string::npos
        && lower.find("depth") == std::string::npos;
}

uint16_t TextureImporter::Float32ToFloat16(float value) noexcept
{
    uint32_t bits = 0u;
    std::memcpy(&bits, &value, sizeof(bits));
    const uint32_t sign = (bits >> 16) & 0x8000u;
    const int32_t exp = static_cast<int32_t>((bits >> 23) & 0xFFu) - 127 + 15;
    const uint32_t mant = (bits >> 13) & 0x3FFu;
    if (exp <= 0)
        return static_cast<uint16_t>(sign);
    if (exp >= 31)
        return static_cast<uint16_t>(sign | 0x7C00u);
    return static_cast<uint16_t>(sign | (static_cast<uint32_t>(exp) << 10) | mant);
}

}
