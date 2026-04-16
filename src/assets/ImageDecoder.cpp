#include "assets/ImageDecoder.hpp"
#include <cstring>
#include <limits>

#define STB_IMAGE_IMPLEMENTATION
#include "../../third_party/stb_image.h"

namespace engine::assets {

bool ImageDecoder::IsHdrFromMemory(const uint8_t* data, size_t size) noexcept
{
    if (!data || size == 0u || size > static_cast<size_t>(std::numeric_limits<int>::max()))
        return false;
    return stbi_is_hdr_from_memory(data, static_cast<int>(size)) != 0;
}

bool ImageDecoder::DecodeFromMemory(const uint8_t* data, size_t size, DecodedImage& outImage) noexcept
{
    outImage = {};
    if (!data || size == 0u || size > static_cast<size_t>(std::numeric_limits<int>::max()))
        return false;

    const int fileSize = static_cast<int>(size);
    int width = 0;
    int height = 0;
    int srcChannels = 0;

    if (IsHdrFromMemory(data, size))
    {
        float* pixels = stbi_loadf_from_memory(data, fileSize, &width, &height, &srcChannels, 4);
        if (!pixels || width <= 0 || height <= 0)
        {
            if (pixels) stbi_image_free(pixels);
            return false;
        }

        const size_t pixelCount = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
        outImage.width = static_cast<uint32_t>(width);
        outImage.height = static_cast<uint32_t>(height);
        outImage.channelCount = 4u;
        outImage.format = DecodedImageFormat::RGBA32F;
        outImage.bytes.resize(pixelCount * sizeof(float));
        std::memcpy(outImage.bytes.data(), pixels, outImage.bytes.size());
        stbi_image_free(pixels);
        return true;
    }

    stbi_uc* pixels = stbi_load_from_memory(data, fileSize, &width, &height, &srcChannels, 4);
    if (!pixels || width <= 0 || height <= 0)
    {
        if (pixels) stbi_image_free(pixels);
        return false;
    }

    const size_t pixelBytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
    outImage.width = static_cast<uint32_t>(width);
    outImage.height = static_cast<uint32_t>(height);
    outImage.channelCount = 4u;
    outImage.format = DecodedImageFormat::RGBA8;
    outImage.bytes.assign(pixels, pixels + pixelBytes);
    stbi_image_free(pixels);
    return true;
}

}
