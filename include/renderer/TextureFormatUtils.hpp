#pragma once

#include "assets/AssetRegistry.hpp"
#include "renderer/RendererTypes.hpp"
#include <algorithm>
#include <cstdint>

namespace engine::renderer {

struct TextureFormatInfo
{
    bool     isCompressed = false;
    bool     isBlockCompressed = false;
    uint32_t bytesPerPixel = 0u;
    uint32_t blockWidth = 1u;
    uint32_t blockHeight = 1u;
    uint32_t bytesPerBlock = 0u;
};

struct TextureUploadLayout
{
    uint32_t width = 0u;
    uint32_t height = 0u;
    uint32_t depth = 0u;
    uint32_t blocksWide = 0u;
    uint32_t blocksHigh = 0u;
    uint32_t rowPitch = 0u;
    uint32_t sliceSize = 0u;
    uint32_t byteSize = 0u;
};

[[nodiscard]] inline TextureFormatInfo GetTextureFormatInfo(assets::TextureFormat format) noexcept
{
    using assets::TextureFormat;
    switch (format)
    {
    case TextureFormat::R8_UNORM:
        return { false, false, 1u, 1u, 1u, 0u };
    case TextureFormat::RG8_UNORM:
        return { false, false, 2u, 1u, 1u, 0u };
    case TextureFormat::RGBA8_UNORM:
    case TextureFormat::RGBA8_SRGB:
    case TextureFormat::R11G11B10F:
    case TextureFormat::DEPTH32F:
        return { false, false, 4u, 1u, 1u, 0u };
    case TextureFormat::RGBA16F:
    case TextureFormat::DEPTH24_STENCIL8:
        return { false, false, 8u, 1u, 1u, 0u };
    case TextureFormat::BC1:
    case TextureFormat::BC4:
        return { true, true, 0u, 4u, 4u, 8u };
    case TextureFormat::BC3:
    case TextureFormat::BC5:
    case TextureFormat::BC7:
        return { true, true, 0u, 4u, 4u, 16u };
    default:
        return {};
    }
}

[[nodiscard]] inline TextureFormatInfo GetTextureFormatInfo(Format format) noexcept
{
    switch (format)
    {
    case Format::R8_UNORM:
        return { false, false, 1u, 1u, 1u, 0u };
    case Format::RG8_UNORM:
        return { false, false, 2u, 1u, 1u, 0u };
    case Format::RGBA8_UNORM:
    case Format::RGBA8_UNORM_SRGB:
    case Format::BGRA8_UNORM:
    case Format::BGRA8_UNORM_SRGB:
    case Format::R11G11B10_FLOAT:
    case Format::D32_FLOAT:
        return { false, false, 4u, 1u, 1u, 0u };
    case Format::RGBA16_FLOAT:
    case Format::D24_UNORM_S8_UINT:
        return { false, false, 8u, 1u, 1u, 0u };
    case Format::BC1_UNORM:
    case Format::BC1_UNORM_SRGB:
    case Format::BC4_UNORM:
        return { true, true, 0u, 4u, 4u, 8u };
    case Format::BC3_UNORM:
    case Format::BC3_UNORM_SRGB:
    case Format::BC5_UNORM:
    case Format::BC5_SNORM:
    case Format::BC7_UNORM:
    case Format::BC7_UNORM_SRGB:
        return { true, true, 0u, 4u, 4u, 16u };
    default:
        return {};
    }
}

template<typename TFormat>
[[nodiscard]] inline TextureUploadLayout ComputeTextureUploadLayoutImpl(
    TFormat format,
    uint32_t width,
    uint32_t height,
    uint32_t depth = 1u) noexcept
{
    const TextureFormatInfo info = GetTextureFormatInfo(format);
    TextureUploadLayout layout{};
    layout.width = std::max(1u, width);
    layout.height = std::max(1u, height);
    layout.depth = std::max(1u, depth);

    if (info.isBlockCompressed)
    {
        layout.blocksWide = std::max(1u, (layout.width + info.blockWidth - 1u) / info.blockWidth);
        layout.blocksHigh = std::max(1u, (layout.height + info.blockHeight - 1u) / info.blockHeight);
        layout.rowPitch = layout.blocksWide * info.bytesPerBlock;
        layout.sliceSize = layout.rowPitch * layout.blocksHigh;
        layout.byteSize = layout.sliceSize * layout.depth;
        return layout;
    }

    if (info.bytesPerPixel == 0u)
        return layout;

    layout.blocksWide = layout.width;
    layout.blocksHigh = layout.height;
    layout.rowPitch = layout.width * info.bytesPerPixel;
    layout.sliceSize = layout.rowPitch * layout.height;
    layout.byteSize = layout.sliceSize * layout.depth;
    return layout;
}

[[nodiscard]] inline TextureUploadLayout ComputeTextureUploadLayout(
    assets::TextureFormat format,
    uint32_t width,
    uint32_t height,
    uint32_t depth = 1u) noexcept
{
    return ComputeTextureUploadLayoutImpl(format, width, height, depth);
}

[[nodiscard]] inline TextureUploadLayout ComputeTextureUploadLayout(
    Format format,
    uint32_t width,
    uint32_t height,
    uint32_t depth = 1u) noexcept
{
    return ComputeTextureUploadLayoutImpl(format, width, height, depth);
}

} // namespace engine::renderer
