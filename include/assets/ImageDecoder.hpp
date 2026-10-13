#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>

namespace engine::assets {

enum class DecodedImageFormat : uint8_t
{
    Unknown = 0,
    RGBA8,
    RGBA32F,
};

struct DecodedImage
{
    uint32_t width = 0u;
    uint32_t height = 0u;
    uint32_t channelCount = 4u;
    DecodedImageFormat format = DecodedImageFormat::Unknown;
    std::vector<uint8_t> bytes;

    [[nodiscard]] bool IsValid() const noexcept
    {
        return width > 0u && height > 0u && !bytes.empty() && format != DecodedImageFormat::Unknown;
    }
};

class ImageDecoder
{
public:
    [[nodiscard]] static bool DecodeFromMemory(const uint8_t* data, size_t size, DecodedImage& outImage) noexcept;
    [[nodiscard]] static bool IsHdrFromMemory(const uint8_t* data, size_t size) noexcept;
};

}
