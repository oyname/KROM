#include "renderer/EnvironmentSystem.hpp"
#include "core/Debug.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace engine::renderer {
    namespace {
        constexpr float kPi = 3.14159265358979323846f;
        constexpr float kTwoPi = 6.28318530717958647692f;
        constexpr uint32_t kBrdfLutSize = 256u;
        constexpr uint32_t kBrdfSamples = 256u;
        constexpr uint32_t kIrradianceWidth = 64u;
        constexpr uint32_t kIrradianceHeight = 32u;
        constexpr uint32_t kIrradianceSamples = 64u;
        constexpr uint32_t kPrefilterWidth = 128u;
        constexpr uint32_t kPrefilterHeight = 64u;
        constexpr uint32_t kPrefilterMipCount = 6u;
        constexpr uint32_t kPrefilterSamples = 128u;

        struct Float2
        {
            float x = 0.0f;
            float y = 0.0f;
        };

        struct Float3
        {
            float x = 0.0f;
            float y = 0.0f;
            float z = 0.0f;

            Float3& operator+=(const Float3& rhs) noexcept
            {
                x += rhs.x; y += rhs.y; z += rhs.z;
                return *this;
            }

            Float3& operator*=(float s) noexcept
            {
                x *= s; y *= s; z *= s;
                return *this;
            }
        };

        static Float3 operator+(Float3 a, const Float3& b) noexcept { return a += b; }
        static Float3 operator-(const Float3& v) noexcept { return { -v.x, -v.y, -v.z }; }
        static Float3 operator-(const Float3& a, const Float3& b) noexcept { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
        static Float3 operator*(const Float3& a, float s) noexcept { return { a.x * s, a.y * s, a.z * s }; }
        static Float3 operator/(const Float3& a, float s) noexcept { return { a.x / s, a.y / s, a.z / s }; }

        [[nodiscard]] static float Clamp01(float v) noexcept
        {
            return std::min(std::max(v, 0.0f), 1.0f);
        }

        [[nodiscard]] static float Dot(const Float3& a, const Float3& b) noexcept
        {
            return a.x * b.x + a.y * b.y + a.z * b.z;
        }

        [[nodiscard]] static Float3 Cross(const Float3& a, const Float3& b) noexcept
        {
            return {
                a.y * b.z - a.z * b.y,
                a.z * b.x - a.x * b.z,
                a.x * b.y - a.y * b.x,
            };
        }

        [[nodiscard]] static float Length(const Float3& v) noexcept
        {
            return std::sqrt(std::max(Dot(v, v), 0.0f));
        }

        [[nodiscard]] static Float3 Normalize(const Float3& v) noexcept
        {
            const float len = Length(v);
            if (len <= 1e-12f)
                return { 0.0f, 0.0f, 1.0f };
            return v / len;
        }

        [[nodiscard]] static Float3 Reflect(const Float3& i, const Float3& n) noexcept
        {
            return i - n * (2.0f * Dot(n, i));
        }

        [[nodiscard]] static float RadicalInverseVdC(uint32_t bits) noexcept
        {
            bits = (bits << 16u) | (bits >> 16u);
            bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
            bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
            bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
            bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
            return static_cast<float>(bits) * 2.3283064365386963e-10f;
        }

        [[nodiscard]] static Float2 Hammersley(uint32_t i, uint32_t n) noexcept
        {
            return { static_cast<float>(i) / static_cast<float>(n), RadicalInverseVdC(i) };
        }


        [[nodiscard]] static float GeometrySchlickGGX(float NoV, float roughness) noexcept
        {
            const float r = roughness + 1.0f;
            const float k = (r * r) / 8.0f;
            return NoV / std::max(NoV * (1.0f - k) + k, 1e-6f);
        }

        [[nodiscard]] static float GeometrySmith(float NoV, float NoL, float roughness) noexcept
        {
            return GeometrySchlickGGX(NoV, roughness) * GeometrySchlickGGX(NoL, roughness);
        }

        [[nodiscard]] static Float3 ImportanceSampleGGX(const Float2& xi, const Float3& n, float roughness) noexcept
        {
            const float a = roughness * roughness;
            const float phi = kTwoPi * xi.x;
            const float cosTheta = std::sqrt((1.0f - xi.y) / std::max(1.0f + (a * a - 1.0f) * xi.y, 1e-6f));
            const float sinTheta = std::sqrt(std::max(1.0f - cosTheta * cosTheta, 0.0f));

            const Float3 hTangent{ std::cos(phi) * sinTheta, std::sin(phi) * sinTheta, cosTheta };
            const Float3 up = std::abs(n.z) < 0.999f ? Float3{ 0.0f, 0.0f, 1.0f } : Float3{ 1.0f, 0.0f, 0.0f };
            const Float3 tangent = Normalize(Cross(up, n));
            const Float3 bitangent = Cross(n, tangent);
            return Normalize(tangent * hTangent.x + bitangent * hTangent.y + n * hTangent.z);
        }

        [[nodiscard]] static Float3 DirectionFromEquirect(float u, float v) noexcept
        {
            const float phi = (u - 0.5f) * kTwoPi;
            const float theta = v * kPi;
            const float sinTheta = std::sin(theta);
            return Normalize({ std::cos(phi) * sinTheta, std::cos(theta), std::sin(phi) * sinTheta });
        }

        [[nodiscard]] static Float2 EquirectFromDirection(const Float3& dir) noexcept
        {
            const Float3 n = Normalize(dir);
            const float u = 0.5f + std::atan2(n.z, n.x) / kTwoPi;
            const float v = std::acos(std::min(std::max(n.y, -1.0f), 1.0f)) / kPi;
            return { u - std::floor(u), Clamp01(v) };
        }

        [[nodiscard]] static float SrgbToLinear(float c) noexcept
        {
            const float s = std::max(c, 0.0f);
            if (s <= 0.04045f)
                return s / 12.92f;
            return std::pow((s + 0.055f) / 1.055f, 2.4f);
        }

        [[nodiscard]] static float HalfToFloat(uint16_t h) noexcept
        {
            const uint32_t sign = (static_cast<uint32_t>(h & 0x8000u)) << 16u;
            const uint32_t exp = (h >> 10u) & 0x1Fu;
            const uint32_t mant = h & 0x03FFu;

            uint32_t out = 0u;
            if (exp == 0u)
            {
                if (mant == 0u)
                {
                    out = sign;
                }
                else
                {
                    uint32_t m = mant;
                    int32_t e = -14;
                    while ((m & 0x0400u) == 0u)
                    {
                        m <<= 1u;
                        --e;
                    }
                    m &= 0x03FFu;
                    out = sign | static_cast<uint32_t>((e + 127) << 23) | (m << 13u);
                }
            }
            else if (exp == 31u)
            {
                out = sign | 0x7F800000u | (mant << 13u);
            }
            else
            {
                out = sign | ((exp + 112u) << 23u) | (mant << 13u);
            }

            float result = 0.0f;
            std::memcpy(&result, &out, sizeof(float));
            return result;
        }


        struct FloatImage
        {
            uint32_t width = 0u;
            uint32_t height = 0u;
            std::vector<Float3> pixels;
        };

        [[nodiscard]] static Float3 ReadPixel(const FloatImage& img, int x, int y) noexcept
        {
            const int w = static_cast<int>(img.width);
            const int h = static_cast<int>(img.height);
            x = (x % w + w) % w;
            y = std::min(std::max(y, 0), h - 1);
            return img.pixels[static_cast<size_t>(y) * img.width + static_cast<size_t>(x)];
        }

        [[nodiscard]] static Float3 SampleBilinear(const FloatImage& img, float u, float v) noexcept
        {
            const float fx = (u - std::floor(u)) * static_cast<float>(img.width) - 0.5f;
            const float fy = Clamp01(v) * static_cast<float>(img.height) - 0.5f;
            const int x0 = static_cast<int>(std::floor(fx));
            const int y0 = static_cast<int>(std::floor(fy));
            const int x1 = x0 + 1;
            const int y1 = y0 + 1;
            const float tx = fx - std::floor(fx);
            const float ty = fy - std::floor(fy);

            const Float3 c00 = ReadPixel(img, x0, y0);
            const Float3 c10 = ReadPixel(img, x1, y0);
            const Float3 c01 = ReadPixel(img, x0, y1);
            const Float3 c11 = ReadPixel(img, x1, y1);
            const Float3 c0 = c00 * (1.0f - tx) + c10 * tx;
            const Float3 c1 = c01 * (1.0f - tx) + c11 * tx;
            return c0 * (1.0f - ty) + c1 * ty;
        }

        [[nodiscard]] static FloatImage DecodeSourceTexture(const assets::TextureAsset& asset)
        {
            FloatImage image{};
            image.width = asset.width;
            image.height = asset.height;
            image.pixels.resize(static_cast<size_t>(asset.width) * asset.height);

            if (asset.format == assets::TextureFormat::RGBA16F)
            {
                const auto* src = reinterpret_cast<const uint16_t*>(asset.pixelData.data());
                for (size_t i = 0; i < image.pixels.size(); ++i)
                {
                    image.pixels[i] = {
                        HalfToFloat(src[i * 4u + 0u]),
                        HalfToFloat(src[i * 4u + 1u]),
                        HalfToFloat(src[i * 4u + 2u])
                    };
                }
            }
            else
            {
                const auto* src = asset.pixelData.data();
                for (size_t i = 0; i < image.pixels.size(); ++i)
                {
                    const float r = static_cast<float>(src[i * 4u + 0u]) / 255.0f;
                    const float g = static_cast<float>(src[i * 4u + 1u]) / 255.0f;
                    const float b = static_cast<float>(src[i * 4u + 2u]) / 255.0f;
                    image.pixels[i] = asset.sRGB
                        ? Float3{ SrgbToLinear(r), SrgbToLinear(g), SrgbToLinear(b) }
                    : Float3{ r, g, b };
                }
            }

            return image;
        }

        [[nodiscard]] static std::vector<float> BuildBrdfLut()
        {
            std::vector<float> lut(static_cast<size_t>(kBrdfLutSize) * kBrdfLutSize * 4u, 0.0f);

            for (uint32_t y = 0u; y < kBrdfLutSize; ++y)
            {
                for (uint32_t x = 0u; x < kBrdfLutSize; ++x)
                {
                    const float NoV = std::max((static_cast<float>(x) + 0.5f) / static_cast<float>(kBrdfLutSize), 1e-4f);
                    const float roughness = (static_cast<float>(y) + 0.5f) / static_cast<float>(kBrdfLutSize);
                    const Float3 V{ std::sqrt(std::max(1.0f - NoV * NoV, 0.0f)), 0.0f, NoV };
                    const Float3 N{ 0.0f, 0.0f, 1.0f };

                    float A = 0.0f;
                    float B = 0.0f;
                    for (uint32_t i = 0u; i < kBrdfSamples; ++i)
                    {
                        const Float2 xi = Hammersley(i, kBrdfSamples);
                        const Float3 H = ImportanceSampleGGX(xi, N, roughness);
                        const Float3 L = Normalize(Reflect(-V, H));
                        const float NoL = std::max(L.z, 0.0f);
                        const float NoH = std::max(H.z, 0.0f);
                        const float HoV = std::max(Dot(H, V), 0.0f);
                        if (NoL <= 0.0f)
                            continue;

                        const float G = GeometrySmith(NoV, NoL, roughness);
                        const float G_Vis = (G * HoV) / std::max(NoH * NoV, 1e-4f);
                        const float Fc = std::pow(1.0f - HoV, 5.0f);
                        A += (1.0f - Fc) * G_Vis;
                        B += Fc * G_Vis;
                    }

                    const size_t idx = (static_cast<size_t>(y) * kBrdfLutSize + x) * 4u;
                    lut[idx + 0u] = A / static_cast<float>(kBrdfSamples);
                    lut[idx + 1u] = B / static_cast<float>(kBrdfSamples);
                    lut[idx + 2u] = 0.0f;
                    lut[idx + 3u] = 1.0f;
                }
            }

            return lut;
        }

        [[nodiscard]] static std::vector<float> BuildIrradianceMap(const FloatImage& source)
        {
            std::vector<float> out(static_cast<size_t>(kIrradianceWidth) * kIrradianceHeight * 4u, 0.0f);

            for (uint32_t y = 0u; y < kIrradianceHeight; ++y)
            {
                for (uint32_t x = 0u; x < kIrradianceWidth; ++x)
                {
                    const Float3 N = DirectionFromEquirect(
                        (static_cast<float>(x) + 0.5f) / static_cast<float>(kIrradianceWidth),
                        (static_cast<float>(y) + 0.5f) / static_cast<float>(kIrradianceHeight));
                    const Float3 up = std::abs(N.y) < 0.999f ? Float3{ 0.0f, 1.0f, 0.0f } : Float3{ 1.0f, 0.0f, 0.0f };
                    const Float3 tangent = Normalize(Cross(up, N));
                    const Float3 bitangent = Cross(N, tangent);

                    Float3 accum{};
                    for (uint32_t i = 0u; i < kIrradianceSamples; ++i)
                    {
                        const Float2 xi = Hammersley(i, kIrradianceSamples);
                        const float phi = kTwoPi * xi.x;
                        const float cosTheta = std::sqrt(1.0f - xi.y);
                        const float sinTheta = std::sqrt(xi.y);
                        const Float3 local{ std::cos(phi) * sinTheta, std::sin(phi) * sinTheta, cosTheta };
                        const Float3 L = Normalize(tangent * local.x + bitangent * local.y + N * local.z);
                        const Float2 uv = EquirectFromDirection(L);
                        accum += SampleBilinear(source, uv.x, uv.y);
                    }

                    // Cosine-weighted sampling => Integral ≈ PI * average(sampled radiance)
                    accum *= (kPi / static_cast<float>(kIrradianceSamples));
                    const size_t idx = (static_cast<size_t>(y) * kIrradianceWidth + x) * 4u;
                    out[idx + 0u] = accum.x;
                    out[idx + 1u] = accum.y;
                    out[idx + 2u] = accum.z;
                    out[idx + 3u] = 1.0f;
                }
            }

            return out;
        }

        [[nodiscard]] static std::vector<float> BuildPrefilterMip(const FloatImage& source, uint32_t width, uint32_t height, float roughness)
        {
            std::vector<float> out(static_cast<size_t>(width) * height * 4u, 0.0f);

            for (uint32_t y = 0u; y < height; ++y)
            {
                for (uint32_t x = 0u; x < width; ++x)
                {
                    const Float3 R = DirectionFromEquirect(
                        (static_cast<float>(x) + 0.5f) / static_cast<float>(width),
                        (static_cast<float>(y) + 0.5f) / static_cast<float>(height));
                    const Float3 N = R;
                    const Float3 V = R;

                    Float3 prefiltered{};
                    float totalWeight = 0.0f;
                    for (uint32_t i = 0u; i < kPrefilterSamples; ++i)
                    {
                        const Float2 xi = Hammersley(i, kPrefilterSamples);
                        const Float3 H = ImportanceSampleGGX(xi, N, std::max(roughness, 0.04f));
                        const Float3 L = Normalize(Reflect(-V, H));
                        const float NoL = std::max(Dot(N, L), 0.0f);
                        if (NoL <= 0.0f)
                            continue;

                        const Float2 uv = EquirectFromDirection(L);
                        prefiltered += SampleBilinear(source, uv.x, uv.y) * NoL;
                        totalWeight += NoL;
                    }

                    if (totalWeight > 0.0f)
                        prefiltered = prefiltered / totalWeight;

                    const size_t idx = (static_cast<size_t>(y) * width + x) * 4u;
                    out[idx + 0u] = prefiltered.x;
                    out[idx + 1u] = prefiltered.y;
                    out[idx + 2u] = prefiltered.z;
                    out[idx + 3u] = 1.0f;
                }
            }

            return out;
        }

        [[nodiscard]] static TextureHandle UploadFloatTexture2D(IDevice& device,
            const char* debugName,
            uint32_t width,
            uint32_t height,
            const std::vector<float>& rgba)
        {
            TextureDesc desc{};
            desc.width = width;
            desc.height = height;
            desc.format = Format::RGBA32_FLOAT;
            desc.usage = ResourceUsage::ShaderResource | ResourceUsage::CopyDest;
            desc.initialState = ResourceState::ShaderRead;
            desc.debugName = debugName;
            const TextureHandle handle = device.CreateTexture(desc);
            if (handle.IsValid())
                device.UploadTextureData(handle, rgba.data(), rgba.size() * sizeof(float), 0u, 0u);
            return handle;
        }

        [[nodiscard]] static TextureHandle UploadPrefilterTexture(IDevice& device,
            const FloatImage& source,
            const char* debugName)
        {
            TextureDesc desc{};
            desc.width = kPrefilterWidth;
            desc.height = kPrefilterHeight;
            desc.mipLevels = kPrefilterMipCount;
            desc.format = Format::RGBA32_FLOAT;
            desc.usage = ResourceUsage::ShaderResource | ResourceUsage::CopyDest;
            desc.initialState = ResourceState::ShaderRead;
            desc.debugName = debugName;
            const TextureHandle handle = device.CreateTexture(desc);
            if (!handle.IsValid())
                return handle;

            uint32_t width = kPrefilterWidth;
            uint32_t height = kPrefilterHeight;
            for (uint32_t mip = 0u; mip < kPrefilterMipCount; ++mip)
            {
                const float roughness = (kPrefilterMipCount <= 1u) ? 0.0f : static_cast<float>(mip) / static_cast<float>(kPrefilterMipCount - 1u);
                const std::vector<float> data = BuildPrefilterMip(source, width, height, roughness);
                device.UploadTextureData(handle, data.data(), data.size() * sizeof(float), mip, 0u);
                width = std::max(width / 2u, 1u);
                height = std::max(height / 2u, 1u);
            }
            return handle;
        }

    } // namespace

    bool EnvironmentSystem::Initialize(IDevice& device, assets::AssetRegistry* assets)
    {
        m_device = &device;
        m_assets = assets;
        if (!m_sharedBrdfLut.IsValid())
            m_sharedBrdfLut = UploadFloatTexture2D(device, "IBL_BRDFLUT", kBrdfLutSize, kBrdfLutSize, BuildBrdfLut());
        return m_sharedBrdfLut.IsValid();
    }

    void EnvironmentSystem::DestroyEntry(EnvironmentEntry& entry)
    {
        if (!m_device)
            return;
        if (entry.irradiance.IsValid())
            m_device->DestroyTexture(entry.irradiance);
        if (entry.prefiltered.IsValid())
            m_device->DestroyTexture(entry.prefiltered);
        entry = {};
    }

    void EnvironmentSystem::Shutdown()
    {
        for (auto& [_, entry] : m_entries)
            DestroyEntry(entry);
        m_entries.clear();
        if (m_device && m_sharedBrdfLut.IsValid())
            m_device->DestroyTexture(m_sharedBrdfLut);
        m_device = nullptr;
        m_assets = nullptr;
        m_sharedBrdfLut = TextureHandle::Invalid();
        m_active = {};
        m_nextId = 1u;
    }

    EnvironmentHandle EnvironmentSystem::CreateEnvironment(const EnvironmentDesc& desc)
    {
        if (!m_device || !m_assets || !desc.sourceTexture.IsValid())
            return {};

        const assets::TextureAsset* sourceAsset = m_assets->textures.Get(desc.sourceTexture);
        if (!sourceAsset || sourceAsset->state != assets::AssetState::Loaded || sourceAsset->pixelData.empty())
            return {};

        FloatImage source = DecodeSourceTexture(*sourceAsset);
        if (desc.intensity != 1.0f)
        {
            for (Float3& px : source.pixels)
                px *= desc.intensity;
        }
        if (source.width == 0u || source.height == 0u)
            return {};

        EnvironmentEntry entry{};
        entry.desc = desc;
        entry.irradiance = UploadFloatTexture2D(*m_device, "Environment_Irradiance", kIrradianceWidth, kIrradianceHeight, BuildIrradianceMap(source));
        entry.prefiltered = UploadPrefilterTexture(*m_device, source, "Environment_Prefiltered");
        entry.brdfLut = m_sharedBrdfLut;

        if (!entry.irradiance.IsValid() || !entry.prefiltered.IsValid() || !entry.brdfLut.IsValid())
        {
            DestroyEntry(entry);
            Debug::LogError("EnvironmentSystem: failed to build prefiltered IBL resources");
            return {};
        }

        EnvironmentHandle handle{ m_nextId++ };
        m_entries.emplace(handle.id, std::move(entry));
        return handle;
    }

    void EnvironmentSystem::DestroyEnvironment(EnvironmentHandle handle)
    {
        const auto it = m_entries.find(handle.id);
        if (it == m_entries.end())
            return;
        if (m_active == handle)
            m_active = {};
        DestroyEntry(it->second);
        m_entries.erase(it);
    }

    void EnvironmentSystem::SetActiveEnvironment(EnvironmentHandle handle) noexcept
    {
        if (!handle.IsValid())
        {
            m_active = {};
            return;
        }
        if (m_entries.find(handle.id) != m_entries.end())
            m_active = handle;
    }

    EnvironmentRuntimeState EnvironmentSystem::ResolveRuntimeState() const noexcept
    {
        EnvironmentRuntimeState state{};
        const auto it = m_entries.find(m_active.id);
        if (it == m_entries.end())
            return state;

        state.irradiance = it->second.irradiance;
        state.prefiltered = it->second.prefiltered;
        state.brdfLut = it->second.brdfLut;
        state.intensity = it->second.desc.intensity;
        state.active = it->second.desc.enableIBL && state.irradiance.IsValid() && state.prefiltered.IsValid() && state.brdfLut.IsValid();
        return state;
    }

} // namespace engine::renderer