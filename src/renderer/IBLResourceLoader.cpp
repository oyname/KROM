#include "renderer/IBLResourceLoader.hpp"
#include "renderer/RendererTypes.hpp"
#include "FloatConvert.hpp"
#include "core/Debug.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

namespace engine::renderer {
namespace {

[[nodiscard]] Format SelectIBLFormat(IDevice& device) noexcept
{
    const ResourceUsage usage = ResourceUsage::ShaderResource | ResourceUsage::CopyDest;
    if (device.SupportsTextureFormat(Format::RGBA16_FLOAT, usage))
        return Format::RGBA16_FLOAT;
    if (device.SupportsTextureFormat(Format::R11G11B10_FLOAT, usage))
        return Format::R11G11B10_FLOAT;
    if (device.SupportsTextureFormat(Format::RGBA8_UNORM, usage))
        return Format::RGBA8_UNORM;
    return Format::Unknown;
}

[[nodiscard]] uint32_t PackUnsignedFloatComponent(float value, uint32_t mantissaBits) noexcept
{
    using internal::FloatToHalf;

    if (!(value > 0.0f))
        return 0u;

    const uint16_t half = FloatToHalf(value);
    uint32_t exp = (half >> 10u) & 0x1Fu;
    uint32_t mant = half & 0x03FFu;
    if (exp == 0u && mant == 0u)
        return 0u;

    const uint32_t shift = 10u - mantissaBits;
    mant += 1u << (shift - 1u);
    mant >>= shift;
    if (mant >= (1u << mantissaBits))
    {
        mant = 0u;
        ++exp;
    }

    const uint32_t maxExp = 0x1Eu;
    if (exp > maxExp)
    {
        exp = maxExp;
        mant = (1u << mantissaBits) - 1u;
    }

    return (exp << mantissaBits) | mant;
}

[[nodiscard]] uint32_t PackR11G11B10Pixel(float r, float g, float b) noexcept
{
    const uint32_t rp = PackUnsignedFloatComponent(r, 6u);
    const uint32_t gp = PackUnsignedFloatComponent(g, 6u);
    const uint32_t bp = PackUnsignedFloatComponent(b, 5u);
    return rp | (gp << 11u) | (bp << 22u);
}

[[nodiscard]] std::vector<uint8_t> TranscodeRGBA16FToR11G11B10(const uint8_t* srcBytes, size_t srcByteSize)
{
    using internal::HalfToFloat;

    if (!srcBytes || (srcByteSize % (4u * sizeof(uint16_t))) != 0u)
        return {};

    const size_t pixelCount = srcByteSize / (4u * sizeof(uint16_t));
    std::vector<uint8_t> out(pixelCount * sizeof(uint32_t), 0u);
    const auto* src = reinterpret_cast<const uint16_t*>(srcBytes);
    auto* dst = reinterpret_cast<uint32_t*>(out.data());
    for (size_t i = 0u; i < pixelCount; ++i)
    {
        dst[i] = PackR11G11B10Pixel(
            HalfToFloat(src[i * 4u + 0u]),
            HalfToFloat(src[i * 4u + 1u]),
            HalfToFloat(src[i * 4u + 2u]));
    }
    return out;
}

[[nodiscard]] std::vector<uint8_t> TranscodeRGBA16FToRGBA8(const uint8_t* srcBytes, size_t srcByteSize)
{
    using internal::HalfToFloat;

    if (!srcBytes || (srcByteSize % (4u * sizeof(uint16_t))) != 0u)
        return {};

    const size_t pixelCount = srcByteSize / (4u * sizeof(uint16_t));
    std::vector<uint8_t> out(pixelCount * 4u, 0u);
    const auto* src = reinterpret_cast<const uint16_t*>(srcBytes);
    for (size_t i = 0u; i < pixelCount; ++i)
    {
        const float r = std::max(0.0f, HalfToFloat(src[i * 4u + 0u]));
        const float g = std::max(0.0f, HalfToFloat(src[i * 4u + 1u]));
        const float b = std::max(0.0f, HalfToFloat(src[i * 4u + 2u]));
        const auto tonemap = [](float v) noexcept -> uint8_t
        {
            const float mapped = v / (1.0f + v);
            return static_cast<uint8_t>(std::clamp(mapped, 0.0f, 1.0f) * 255.0f + 0.5f);
        };
        out[i * 4u + 0u] = tonemap(r);
        out[i * 4u + 1u] = tonemap(g);
        out[i * 4u + 2u] = tonemap(b);
        out[i * 4u + 3u] = 255u;
    }
    return out;
}

[[nodiscard]] std::vector<uint8_t> TranscodeIBLSlice(const uint8_t* srcBytes, size_t srcByteSize, Format format)
{
    switch (format)
    {
    case Format::RGBA16_FLOAT:
        return std::vector<uint8_t>(srcBytes, srcBytes + srcByteSize);
    case Format::R11G11B10_FLOAT:
        return TranscodeRGBA16FToR11G11B10(srcBytes, srcByteSize);
    case Format::RGBA8_UNORM:
        return TranscodeRGBA16FToRGBA8(srcBytes, srcByteSize);
    default:
        return {};
    }
}

[[nodiscard]] bool UploadFace(IDevice& device,
                              TextureHandle texture,
                              Format format,
                              const uint8_t* srcBytes,
                              size_t srcByteSize,
                              uint32_t mipLevel,
                              uint32_t face,
                              const char* label)
{
    std::vector<uint8_t> uploadBytes = TranscodeIBLSlice(srcBytes, srcByteSize, format);
    if (uploadBytes.empty())
    {
        Debug::LogError("IBLResourceLoader: failed to transcode %s for mip=%u face=%u format=%u",
            label ? label : "IBL slice", mipLevel, face, static_cast<unsigned>(format));
        return false;
    }

    device.UploadTextureData(texture, uploadBytes.data(), uploadBytes.size(), mipLevel, face);
    return true;
}

} // namespace

    IBLGpuResources IBLResourceLoader::Upload(IDevice& device, const IBLBakedData& data)
    {
        if (!data.IsValid())
        {
            Debug::LogError("IBLResourceLoader: received invalid IBLBakedData");
            return {};
        }

        IBLGpuResources res{};
        const Format iblFormat = SelectIBLFormat(device);
        if (iblFormat == Format::Unknown)
        {
            Debug::LogError("IBLResourceLoader: no supported sampled format for IBL upload on backend '%s'",
                device.GetBackendName());
            return {};
        }
        if (iblFormat != Format::RGBA16_FLOAT)
        {
            Debug::LogWarning("IBLResourceLoader: RGBA16F not supported, using fallback format=%u on backend '%s'",
                static_cast<unsigned>(iblFormat), device.GetBackendName());
        }
        res.iblMode = (iblFormat == Format::RGBA8_UNORM) ? IBLRuntimeMode::LDRDiffuseOnly : IBLRuntimeMode::HDR;

        // -----------------------------------------------------------------
        // Environment cube (mip 0 only, used for skybox / reflection source)
        // -----------------------------------------------------------------
        {
            TextureDesc desc{};
            desc.width        = data.environmentSize;
            desc.height       = data.environmentSize;
            desc.arraySize    = 6u;
            desc.mipLevels    = 1u;
            desc.dimension    = TextureDimension::Cubemap;
            desc.format       = iblFormat;
            desc.usage        = ResourceUsage::ShaderResource | ResourceUsage::CopyDest;
            desc.initialState = ResourceState::ShaderRead;
            desc.debugName    = "IBL_Environment";
            res.environment = device.CreateTexture(desc);
            if (!res.environment.IsValid())
            {
                Debug::LogError("IBLResourceLoader: CreateTexture failed for environment cube");
                Destroy(device, res);
                return {};
            }
            const size_t faceBytes = static_cast<size_t>(data.environmentSize)
                                   * data.environmentSize * 4u * sizeof(uint16_t);
            for (uint32_t face = 0u; face < 6u; ++face)
            {
                if (!UploadFace(device, res.environment, iblFormat,
                        data.environmentData.data() + face * faceBytes, faceBytes, 0u, face, "environment cube"))
                {
                    Destroy(device, res);
                    return {};
                }
            }
        }

        // -----------------------------------------------------------------
        // Irradiance cube (diffuse IBL)
        // -----------------------------------------------------------------
        {
            TextureDesc desc{};
            desc.width        = data.irradianceSize;
            desc.height       = data.irradianceSize;
            desc.arraySize    = 6u;
            desc.mipLevels    = 1u;
            desc.dimension    = TextureDimension::Cubemap;
            desc.format       = iblFormat;
            desc.usage        = ResourceUsage::ShaderResource | ResourceUsage::CopyDest;
            desc.initialState = ResourceState::ShaderRead;
            desc.debugName    = "IBL_Irradiance";
            res.irradiance = device.CreateTexture(desc);
            if (!res.irradiance.IsValid())
            {
                Debug::LogError("IBLResourceLoader: CreateTexture failed for irradiance cube");
                Destroy(device, res);
                return {};
            }
            const size_t faceBytes = static_cast<size_t>(data.irradianceSize)
                                   * data.irradianceSize * 4u * sizeof(uint16_t);
            for (uint32_t face = 0u; face < 6u; ++face)
            {
                if (!UploadFace(device, res.irradiance, iblFormat,
                        data.irradianceData.data() + face * faceBytes, faceBytes, 0u, face, "irradiance cube"))
                {
                    Destroy(device, res);
                    return {};
                }
            }
        }

        // -----------------------------------------------------------------
        // Prefilter cube (specular IBL, mip chain)
        // Data layout: [mip0: 6 faces][mip1: 6 faces]...
        // -----------------------------------------------------------------
        {
            TextureDesc desc{};
            desc.width        = data.prefilterBaseSize;
            desc.height       = data.prefilterBaseSize;
            desc.arraySize    = 6u;
            desc.mipLevels    = data.prefilterMipCount;
            desc.dimension    = TextureDimension::Cubemap;
            desc.format       = iblFormat;
            desc.usage        = ResourceUsage::ShaderResource | ResourceUsage::CopyDest;
            desc.initialState = ResourceState::ShaderRead;
            desc.debugName    = "IBL_Prefilter";
            res.prefiltered = device.CreateTexture(desc);
            if (!res.prefiltered.IsValid())
            {
                Debug::LogError("IBLResourceLoader: CreateTexture failed for prefilter cube");
                Destroy(device, res);
                return {};
            }

            size_t   mipOffset = 0u;
            uint32_t mipSize   = data.prefilterBaseSize;
            for (uint32_t mip = 0u; mip < data.prefilterMipCount; ++mip)
            {
                const size_t faceBytes = static_cast<size_t>(mipSize) * mipSize * 4u * sizeof(uint16_t);
                for (uint32_t face = 0u; face < 6u; ++face)
                {
                    if (!UploadFace(device, res.prefiltered, iblFormat,
                            data.prefilterData.data() + mipOffset + face * faceBytes, faceBytes, mip, face, "prefilter cube"))
                    {
                        Destroy(device, res);
                        return {};
                    }
                }
                mipOffset += 6u * faceBytes;
                mipSize    = std::max(mipSize / 2u, 1u);
            }
        }

        return res;
    }

    void IBLResourceLoader::Destroy(IDevice& device, IBLGpuResources& res)
    {
        if (res.environment.IsValid()) { device.DestroyTexture(res.environment); res.environment = TextureHandle::Invalid(); }
        if (res.irradiance.IsValid())  { device.DestroyTexture(res.irradiance);  res.irradiance  = TextureHandle::Invalid(); }
        if (res.prefiltered.IsValid()) { device.DestroyTexture(res.prefiltered); res.prefiltered = TextureHandle::Invalid(); }
    }

} // namespace engine::renderer
