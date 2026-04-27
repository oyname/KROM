#include "assets/TextureImporter.hpp"
#include "core/Debug.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <limits>
#include <sstream>
#include <vector>

namespace engine::assets {

namespace {
std::string ToLower(std::string s)
{
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

[[nodiscard]] bool IsLikelyNormalMapName(const std::string& lower) noexcept
{
    return lower.find("normal") != std::string::npos
        || lower.find("_nor") != std::string::npos
        || lower.find("nor_") != std::string::npos
        || lower.find("_nrm") != std::string::npos
        || lower.find("nrm_") != std::string::npos;
}

[[nodiscard]] uint32_t ComputeMipLevelCount(uint32_t width, uint32_t height) noexcept
{
    uint32_t levels = 1u;
    while (width > 1u || height > 1u)
    {
        width = std::max(1u, width >> 1u);
        height = std::max(1u, height >> 1u);
        ++levels;
    }
    return levels;
}

[[nodiscard]] float SrgbToLinear(float v) noexcept
{
    v = std::clamp(v, 0.0f, 1.0f);
    return (v <= 0.04045f) ? (v / 12.92f) : std::pow((v + 0.055f) / 1.055f, 2.4f);
}

[[nodiscard]] float LinearToSrgb(float v) noexcept
{
    v = std::clamp(v, 0.0f, 1.0f);
    return (v <= 0.0031308f) ? (v * 12.92f) : (1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f);
}

struct Float3
{
    float x;
    float y;
    float z;
};

[[nodiscard]] Float3 Normalize3(Float3 v, Float3 fallback = {0.0f, 0.0f, 1.0f}) noexcept
{
    const float len2 = v.x * v.x + v.y * v.y + v.z * v.z;
    if (len2 <= 1e-12f)
        return fallback;
    const float invLen = 1.0f / std::sqrt(len2);
    return { v.x * invLen, v.y * invLen, v.z * invLen };
}

[[nodiscard]] size_t PixelOffset(uint32_t x, uint32_t y, uint32_t width) noexcept
{
    return static_cast<size_t>(y) * static_cast<size_t>(width) * 4u + static_cast<size_t>(x) * 4u;
}

[[nodiscard]] std::vector<uint8_t> GenerateMipChainRGBA8(const std::vector<uint8_t>& baseLevel,
                                                         uint32_t width,
                                                         uint32_t height,
                                                         const TextureMetadata& metadata,
                                                         uint32_t& outMipLevels)
{
    outMipLevels = 1u;
    if (width == 0u || height == 0u || baseLevel.size() != static_cast<size_t>(width) * static_cast<size_t>(height) * 4u)
        return baseLevel;

    if (!metadata.generateMipmaps || (width == 1u && height == 1u))
        return baseLevel;

    outMipLevels = ComputeMipLevelCount(width, height);
    std::vector<uint8_t> packed;
    packed.reserve(baseLevel.size() + baseLevel.size() / 3u);
    packed.insert(packed.end(), baseLevel.begin(), baseLevel.end());

    std::vector<uint8_t> prev = baseLevel;
    uint32_t prevWidth = width;
    uint32_t prevHeight = height;

    for (uint32_t mip = 1u; mip < outMipLevels; ++mip)
    {
        const uint32_t dstWidth = std::max(1u, prevWidth >> 1u);
        const uint32_t dstHeight = std::max(1u, prevHeight >> 1u);
        std::vector<uint8_t> next(static_cast<size_t>(dstWidth) * static_cast<size_t>(dstHeight) * 4u, 0u);

        for (uint32_t y = 0u; y < dstHeight; ++y)
        {
            for (uint32_t x = 0u; x < dstWidth; ++x)
            {
                const uint32_t sx0 = std::min(prevWidth - 1u, x * 2u + 0u);
                const uint32_t sx1 = std::min(prevWidth - 1u, x * 2u + 1u);
                const uint32_t sy0 = std::min(prevHeight - 1u, y * 2u + 0u);
                const uint32_t sy1 = std::min(prevHeight - 1u, y * 2u + 1u);
                const size_t s00 = PixelOffset(sx0, sy0, prevWidth);
                const size_t s10 = PixelOffset(sx1, sy0, prevWidth);
                const size_t s01 = PixelOffset(sx0, sy1, prevWidth);
                const size_t s11 = PixelOffset(sx1, sy1, prevWidth);
                const size_t d = PixelOffset(x, y, dstWidth);

                if (metadata.semantic == TextureSemantic::Normal)
                {
                    const auto decode = [&](size_t o) -> Float3 {
                        return {
                            prev[o + 0u] / 255.0f * 2.0f - 1.0f,
                            prev[o + 1u] / 255.0f * 2.0f - 1.0f,
                            prev[o + 2u] / 255.0f * 2.0f - 1.0f
                        };
                    };

                    const Float3 n00 = Normalize3(decode(s00));
                    const Float3 n10 = Normalize3(decode(s10));
                    const Float3 n01 = Normalize3(decode(s01));
                    const Float3 n11 = Normalize3(decode(s11));
                    const Float3 n = Normalize3({
                        0.25f * (n00.x + n10.x + n01.x + n11.x),
                        0.25f * (n00.y + n10.y + n01.y + n11.y),
                        0.25f * (n00.z + n10.z + n01.z + n11.z)
                    });

                    next[d + 0u] = static_cast<uint8_t>(std::clamp((n.x * 0.5f + 0.5f) * 255.0f + 0.5f, 0.0f, 255.0f));
                    next[d + 1u] = static_cast<uint8_t>(std::clamp((n.y * 0.5f + 0.5f) * 255.0f + 0.5f, 0.0f, 255.0f));
                    next[d + 2u] = static_cast<uint8_t>(std::clamp((n.z * 0.5f + 0.5f) * 255.0f + 0.5f, 0.0f, 255.0f));
                    next[d + 3u] = static_cast<uint8_t>((static_cast<uint32_t>(prev[s00 + 3u]) + static_cast<uint32_t>(prev[s10 + 3u])
                                                        + static_cast<uint32_t>(prev[s01 + 3u]) + static_cast<uint32_t>(prev[s11 + 3u]) + 2u) / 4u);
                }
                else
                {
                    for (uint32_t c = 0u; c < 4u; ++c)
                    {
                        if (metadata.colorSpace == ColorSpace::SRGB && c < 3u)
                        {
                            const float v00 = SrgbToLinear(prev[s00 + c] / 255.0f);
                            const float v10 = SrgbToLinear(prev[s10 + c] / 255.0f);
                            const float v01 = SrgbToLinear(prev[s01 + c] / 255.0f);
                            const float v11 = SrgbToLinear(prev[s11 + c] / 255.0f);
                            const float avg = 0.25f * (v00 + v10 + v01 + v11);
                            next[d + c] = static_cast<uint8_t>(std::clamp(LinearToSrgb(avg) * 255.0f + 0.5f, 0.0f, 255.0f));
                        }
                        else
                        {
                            next[d + c] = static_cast<uint8_t>((static_cast<uint32_t>(prev[s00 + c]) + static_cast<uint32_t>(prev[s10 + c])
                                                              + static_cast<uint32_t>(prev[s01 + c]) + static_cast<uint32_t>(prev[s11 + c]) + 2u) / 4u);
                        }
                    }
                }
            }
        }

        packed.insert(packed.end(), next.begin(), next.end());
        prev = std::move(next);
        prevWidth = dstWidth;
        prevHeight = dstHeight;
    }

    return packed;
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
    outTexture.arraySize = 1u;
    outTexture.format = TextureFormat::RGBA8_UNORM;
    outTexture.metadata.semantic = TextureSemantic::Data;
    outTexture.metadata.normalEncoding = NormalEncoding::None;
    outTexture.metadata.colorSpace = ColorSpace::Linear;
    std::vector<uint8_t> basePixels(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u, 255u);

    for (uint32_t i = 0u; i < w * h; ++i)
    {
        int r = 255;
        int g = 255;
        int b = 255;
        int a = 255;
        if (!(iss >> r >> g >> b >> a))
            break;
        basePixels[i * 4u + 0u] = static_cast<uint8_t>(r);
        basePixels[i * 4u + 1u] = static_cast<uint8_t>(g);
        basePixels[i * 4u + 2u] = static_cast<uint8_t>(b);
        basePixels[i * 4u + 3u] = static_cast<uint8_t>(a);
    }

    outTexture.pixelData = GenerateMipChainRGBA8(basePixels, w, h, outTexture.metadata, outTexture.mipLevels);

    return true;
}

bool TextureImporter::ImportDecodedImage(const TextureImportRequest& request, const DecodedImage& image, TextureAsset& outTexture)
{
    if (!image.IsValid())
        return false;

    outTexture.width = image.width;
    outTexture.height = image.height;
    outTexture.depth = 1u;
    outTexture.arraySize = 1u;

    switch (image.format)
    {
    case DecodedImageFormat::RGBA32F:
    {
        const size_t pixelCount = static_cast<size_t>(image.width) * static_cast<size_t>(image.height) * 4u;
        if (image.bytes.size() != pixelCount * sizeof(float))
            return false;

        outTexture.format = TextureFormat::RGBA16F;
        outTexture.metadata = InferMetadata(request.sourcePath, true);
        outTexture.pixelData.resize(pixelCount * sizeof(uint16_t));
        outTexture.mipLevels = 1u;

        const float* src = reinterpret_cast<const float*>(image.bytes.data());
        uint16_t* dst = reinterpret_cast<uint16_t*>(outTexture.pixelData.data());
        for (size_t i = 0; i < pixelCount; ++i)
            dst[i] = Float32ToFloat16(src[i]);

        Debug::Log("TextureImporter: imported HDR texture '%s' %ux%u -> RGBA16F",
            request.sourcePath.filename().string().c_str(), outTexture.width, outTexture.height);
        return true;
    }
    case DecodedImageFormat::RGBA8:
    {
        outTexture.format = TextureFormat::RGBA8_UNORM;
        outTexture.metadata = InferMetadata(request.sourcePath, false);
        outTexture.pixelData = GenerateMipChainRGBA8(image.bytes, image.width, image.height, outTexture.metadata, outTexture.mipLevels);
        Debug::Log("TextureImporter: imported texture '%s' %ux%u semantic=%u encoding=%u sRGB=%d",
            request.sourcePath.filename().string().c_str(), outTexture.width, outTexture.height,
            static_cast<unsigned>(outTexture.metadata.semantic),
            static_cast<unsigned>(outTexture.metadata.normalEncoding),
            IsSRGBColorSpace(outTexture.metadata) ? 1 : 0);
        return true;
    }
    default:
        return false;
    }
}

TextureMetadata TextureImporter::InferMetadata(const std::filesystem::path& path, bool isHdr) noexcept
{
    TextureMetadata metadata{};
    const std::string lower = ToLower(path.filename().string());

    if (isHdr)
    {
        metadata.semantic = TextureSemantic::HDR;
        metadata.colorSpace = ColorSpace::Linear;
        metadata.normalEncoding = NormalEncoding::None;
        return metadata;
    }

    if (IsLikelyNormalMapName(lower))
    {
        metadata.semantic = TextureSemantic::Normal;
        metadata.normalEncoding = NormalEncoding::RGB;
        metadata.colorSpace = ColorSpace::Linear;
        return metadata;
    }

    if (lower.find("rough") != std::string::npos
        || lower.find("metal") != std::string::npos
        || lower.find("ao") != std::string::npos
        || lower.find("mask") != std::string::npos
        || lower.find("depth") != std::string::npos)
    {
        metadata.semantic = TextureSemantic::Data;
        metadata.normalEncoding = NormalEncoding::None;
        metadata.colorSpace = ColorSpace::Linear;
        return metadata;
    }

    metadata.semantic = TextureSemantic::Color;
    metadata.normalEncoding = NormalEncoding::None;
    metadata.colorSpace = ColorSpace::SRGB;
    return metadata;
}

uint16_t TextureImporter::Float32ToFloat16(float value) noexcept
{
    if (std::isnan(value))
        return static_cast<uint16_t>(0x7E00u);

    if (std::isinf(value))
        return std::signbit(value) ? static_cast<uint16_t>(0xFC00u) : static_cast<uint16_t>(0x7C00u);

    constexpr float kHalfMax = 65504.0f;
    const float clamped = std::clamp(value, -kHalfMax, kHalfMax);
    uint32_t bits = 0u;
    std::memcpy(&bits, &clamped, sizeof(bits));

    const uint32_t sign = (bits >> 16u) & 0x8000u;
    const uint32_t exp32 = (bits >> 23u) & 0xFFu;
    const uint32_t mant32 = bits & 0x7FFFFFu;

    if (exp32 == 0u)
        return static_cast<uint16_t>(sign);

    int32_t exp16 = static_cast<int32_t>(exp32) - 127 + 15;
    if (exp16 <= 0)
        return static_cast<uint16_t>(sign);
    if (exp16 >= 31)
        return static_cast<uint16_t>(sign | 0x7BFFu);

    const uint32_t mantRounded = mant32 + 0x00001000u;
    uint32_t halfExp = static_cast<uint32_t>(exp16);
    uint32_t halfMant = mantRounded >> 13u;

    if (halfMant == 0x0400u)
    {
        halfMant = 0u;
        ++halfExp;
        if (halfExp >= 31u)
            return static_cast<uint16_t>(sign | 0x7BFFu);
    }

    return static_cast<uint16_t>(sign | (halfExp << 10u) | (halfMant & 0x03FFu));
}

}
