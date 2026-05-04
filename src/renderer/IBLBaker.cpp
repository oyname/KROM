#include "renderer/IBLBaker.hpp"
#include "FloatConvert.hpp"
#include "jobs/JobSystem.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace engine::renderer {
    namespace {

        // =====================================================================
        // Constants
        // =====================================================================
        constexpr float kPi    = 3.14159265358979323846f;
        constexpr float kTwoPi = 6.28318530717958647692f;
        constexpr uint32_t kCubeFaceCount = 6u;

        constexpr uint32_t kSkySourceWidth  = 256u;
        constexpr uint32_t kSkySourceHeight = 128u;

        // =====================================================================
        // Math types (internal to this TU)
        // =====================================================================
        struct Float2 { float x = 0.f; float y = 0.f; };

        struct Float3
        {
            float x = 0.f; float y = 0.f; float z = 0.f;
            Float3& operator+=(const Float3& r) noexcept { x += r.x; y += r.y; z += r.z; return *this; }
            Float3& operator*=(float s) noexcept { x *= s; y *= s; z *= s; return *this; }
        };

        static Float3 operator+(Float3 a, const Float3& b) noexcept { return a += b; }
        static Float3 operator-(const Float3& v) noexcept { return {-v.x, -v.y, -v.z}; }
        static Float3 operator-(const Float3& a, const Float3& b) noexcept { return {a.x-b.x, a.y-b.y, a.z-b.z}; }
        static Float3 operator*(const Float3& a, float s) noexcept { return {a.x*s, a.y*s, a.z*s}; }
        static Float3 operator/(const Float3& a, float s) noexcept { return {a.x/s, a.y/s, a.z/s}; }
        static Float3 Lerp(const Float3& a, const Float3& b, float t) noexcept
        { return a * (1.f - t) + b * t; }

        [[nodiscard]] static float Clamp01(float v) noexcept
        { return std::min(std::max(v, 0.f), 1.f); }
        [[nodiscard]] static float Dot(const Float3& a, const Float3& b) noexcept
        { return a.x*b.x + a.y*b.y + a.z*b.z; }
        [[nodiscard]] static Float3 Cross(const Float3& a, const Float3& b) noexcept
        { return { a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x }; }
        [[nodiscard]] static Float3 Normalize(const Float3& v) noexcept
        {
            const float len = std::sqrt(std::max(Dot(v,v), 0.f));
            return (len > 1e-12f) ? v / len : Float3{0.f, 0.f, 1.f};
        }
        [[nodiscard]] static Float3 Reflect(const Float3& i, const Float3& n) noexcept
        { return i - n * (2.f * Dot(n, i)); }
        [[nodiscard]] static float SmoothStep01(float t) noexcept
        { const float c = Clamp01(t); return c*c*(3.f-2.f*c); }
        [[nodiscard]] static float D_GGX(float NoH, float roughness) noexcept
        {
            const float a = roughness * roughness;
            const float a2 = a * a;
            const float d = (NoH * a2 - NoH) * NoH + 1.f;
            return a2 / std::max(kPi * d * d, 1e-6f);
        }
        [[nodiscard]] static bool IsFinite(float v) noexcept
        { return std::isfinite(v); }
        [[nodiscard]] static Float3 SanitizeFinite(const Float3& v) noexcept
        {
            return {
                IsFinite(v.x) ? v.x : 0.f,
                IsFinite(v.y) ? v.y : 0.f,
                IsFinite(v.z) ? v.z : 0.f
            };
        }
        // =====================================================================
        // Image containers
        // =====================================================================
        struct FloatImage
        {
            uint32_t width = 0u; uint32_t height = 0u;
            std::vector<Float3> pixels;
        };

        struct FloatCubeImage
        {
            uint32_t size = 0u;
            std::vector<Float3> pixels; // 6 * size * size, face-major
        };

        enum class CubeFace : uint32_t {
            PositiveX=0, NegativeX=1, PositiveY=2, NegativeY=3, PositiveZ=4, NegativeZ=5
        };

        [[nodiscard]] static size_t CubeIndex(uint32_t face, uint32_t x, uint32_t y, uint32_t sz) noexcept
        { return static_cast<size_t>(face)*sz*sz + static_cast<size_t>(y)*sz + x; }

        // =====================================================================
        // Direction / UV mapping
        // =====================================================================
        [[nodiscard]] static Float3 DirectionFromEquirect(float u, float v) noexcept
        {
            const float phi   = (u - 0.5f) * kTwoPi;
            const float theta = v * kPi;
            const float sinT  = std::sin(theta);
            return Normalize({ std::cos(phi)*sinT, std::cos(theta), std::sin(phi)*sinT });
        }

        [[nodiscard]] static Float2 EquirectFromDirection(const Float3& dir) noexcept
        {
            const Float3 n = Normalize(dir);
            const float u  = 0.5f + std::atan2(n.z, n.x) / kTwoPi;
            const float v  = std::acos(std::min(std::max(n.y, -1.f), 1.f)) / kPi;
            return { u - std::floor(u), Clamp01(v) };
        }

        [[nodiscard]] static Float3 DirectionFromCubemapFace(CubeFace face, float u, float v) noexcept
        {
            const float sx = 2.f*u-1.f, sy = 2.f*v-1.f;
            switch (face)
            {
            case CubeFace::PositiveX: return Normalize({  1.f, -sy,  -sx });
            case CubeFace::NegativeX: return Normalize({ -1.f, -sy,   sx });
            case CubeFace::PositiveY: return Normalize({   sx,  1.f,   sy });
            case CubeFace::NegativeY: return Normalize({   sx, -1.f,  -sy });
            case CubeFace::PositiveZ: return Normalize({   sx, -sy,  1.f  });
            case CubeFace::NegativeZ: return Normalize({  -sx, -sy, -1.f  });
            default:                  return Normalize({   sx, -sy,  1.f  });
            }
        }

        // =====================================================================
        // Sampling helpers
        // =====================================================================
        [[nodiscard]] static Float3 ReadPixel(const FloatImage& img, int x, int y) noexcept
        {
            const int w = static_cast<int>(img.width), h = static_cast<int>(img.height);
            x = (x % w + w) % w;
            y = std::min(std::max(y, 0), h-1);
            return img.pixels[static_cast<size_t>(y)*img.width + static_cast<size_t>(x)];
        }

        [[nodiscard]] static Float3 SampleBilinear(const FloatImage& img, float u, float v) noexcept
        {
            const float fx = (u-std::floor(u))*static_cast<float>(img.width)  - 0.5f;
            const float fy = Clamp01(v)        *static_cast<float>(img.height) - 0.5f;
            const int x0 = static_cast<int>(std::floor(fx));
            const int y0 = static_cast<int>(std::floor(fy));
            const float tx = fx-std::floor(fx), ty = fy-std::floor(fy);
            return SanitizeFinite(
                (ReadPixel(img,x0,y0)*(1.f-tx) + ReadPixel(img,x0+1,y0)*tx)*(1.f-ty)
              + (ReadPixel(img,x0,y0+1)*(1.f-tx) + ReadPixel(img,x0+1,y0+1)*tx)*ty);
        }

        [[nodiscard]] static Float3 ReadCubePixel(const FloatCubeImage& c, uint32_t face, int x, int y) noexcept
        {
            const int sz = static_cast<int>(c.size);
            return c.pixels[CubeIndex(face,
                static_cast<uint32_t>(std::min(std::max(x,0),sz-1)),
                static_cast<uint32_t>(std::min(std::max(y,0),sz-1)), c.size)];
        }

        [[nodiscard]] static Float3 SampleCubeBilinearFace(const FloatCubeImage& c, uint32_t face, float u, float v) noexcept
        {
            const float fx = Clamp01(u)*static_cast<float>(c.size)-0.5f;
            const float fy = Clamp01(v)*static_cast<float>(c.size)-0.5f;
            const int x0 = static_cast<int>(std::floor(fx));
            const int y0 = static_cast<int>(std::floor(fy));
            const float tx = fx-std::floor(fx), ty = fy-std::floor(fy);
            return SanitizeFinite(
                (ReadCubePixel(c,face,x0,y0)*(1.f-tx)+ReadCubePixel(c,face,x0+1,y0)*tx)*(1.f-ty)
              + (ReadCubePixel(c,face,x0,y0+1)*(1.f-tx)+ReadCubePixel(c,face,x0+1,y0+1)*tx)*ty);
        }

        [[nodiscard]] static Float3 SampleCubeBilinear(const FloatCubeImage& cube, const Float3& dir) noexcept
        {
            const Float3 n  = Normalize(dir);
            const float ax = std::abs(n.x), ay = std::abs(n.y), az = std::abs(n.z);
            CubeFace face = CubeFace::PositiveX;
            float uc = 0.f, vc = 0.f, ma = ax;
            if (ax >= ay && ax >= az)
            {
                if (n.x>=0.f) { face=CubeFace::PositiveX; uc=-n.z; vc=-n.y; ma=ax; }
                else          { face=CubeFace::NegativeX; uc= n.z; vc=-n.y; ma=ax; }
            }
            else if (ay >= ax && ay >= az)
            {
                if (n.y>=0.f) { face=CubeFace::PositiveY; uc= n.x; vc= n.z; ma=ay; }
                else          { face=CubeFace::NegativeY; uc= n.x; vc=-n.z; ma=ay; }
            }
            else
            {
                if (n.z>=0.f) { face=CubeFace::PositiveZ; uc= n.x; vc=-n.y; ma=az; }
                else          { face=CubeFace::NegativeZ; uc=-n.x; vc=-n.y; ma=az; }
            }
            const float inv = (ma > 1e-8f) ? (1.f/ma) : 0.f;
            return SanitizeFinite(SampleCubeBilinearFace(cube, static_cast<uint32_t>(face),
                uc*inv*0.5f+0.5f, vc*inv*0.5f+0.5f));
        }

        [[nodiscard]] static FloatCubeImage DownsampleCubeHalf(const FloatCubeImage& src) noexcept
        {
            const uint32_t dstSize = std::max(src.size / 2u, 1u);
            FloatCubeImage dst{ dstSize, std::vector<Float3>(static_cast<size_t>(kCubeFaceCount) * dstSize * dstSize) };

            for (uint32_t face = 0u; face < kCubeFaceCount; ++face)
            {
                for (uint32_t y = 0u; y < dstSize; ++y)
                {
                    for (uint32_t x = 0u; x < dstSize; ++x)
                    {
                        const int srcX = static_cast<int>(x * 2u);
                        const int srcY = static_cast<int>(y * 2u);
                        const Float3 p00 = ReadCubePixel(src, face, srcX + 0, srcY + 0);
                        const Float3 p10 = ReadCubePixel(src, face, srcX + 1, srcY + 0);
                        const Float3 p01 = ReadCubePixel(src, face, srcX + 0, srcY + 1);
                        const Float3 p11 = ReadCubePixel(src, face, srcX + 1, srcY + 1);
                        dst.pixels[CubeIndex(face, x, y, dstSize)] =
                            SanitizeFinite((p00 + p10 + p01 + p11) * 0.25f);
                    }
                }
            }

            return dst;
        }

        [[nodiscard]] static std::vector<FloatCubeImage> BuildCubeMipChain(const FloatCubeImage& base)
        {
            std::vector<FloatCubeImage> mips;
            mips.push_back(base);
            while (mips.back().size > 1u)
                mips.push_back(DownsampleCubeHalf(mips.back()));
            return mips;
        }

        [[nodiscard]] static Float3 SampleCubeBilinearLod(const std::vector<FloatCubeImage>& cubeMips,
                                                          const Float3& dir,
                                                          float lod) noexcept
        {
            if (cubeMips.empty())
                return {};

            const float maxLod = static_cast<float>(cubeMips.size() - 1u);
            const float clampedLod = std::min(std::max(lod, 0.f), maxLod);
            const uint32_t mip0 = static_cast<uint32_t>(std::floor(clampedLod));
            const uint32_t mip1 = std::min(mip0 + 1u, static_cast<uint32_t>(cubeMips.size() - 1u));
            const float frac = clampedLod - static_cast<float>(mip0);
            const Float3 c0 = SampleCubeBilinear(cubeMips[mip0], dir);
            const Float3 c1 = SampleCubeBilinear(cubeMips[mip1], dir);
            return SanitizeFinite(Lerp(c0, c1, frac));
        }

        // =====================================================================
        // Color helpers
        // =====================================================================
        [[nodiscard]] static float SrgbToLinear(float c) noexcept
        {
            const float s = std::max(c, 0.f);
            return (s <= 0.04045f) ? (s/12.92f) : std::pow((s+0.055f)/1.055f, 2.4f);
        }

        [[nodiscard]] static float Luminance(const Float3& c) noexcept
        {
            return std::max(0.0f, c.x * 0.2126f + c.y * 0.7152f + c.z * 0.0722f);
        }

        static void CompressDominantHighlightsForIBL(FloatImage& img) noexcept
        {
            if (img.pixels.empty())
                return;

            float avgLum = 0.0f;
            float maxLum = 0.0f;
            for (const Float3& px : img.pixels)
            {
                const float lum = Luminance(px);
                avgLum += lum;
                maxLum = std::max(maxLum, lum);
            }

            avgLum /= static_cast<float>(img.pixels.size());
            if (!(avgLum > 1e-6f) || maxLum <= avgLum * 12.0f)
                return;

            const float knee = std::max(avgLum * 6.0f, 1.0f);
            const float limit = std::max(avgLum * 10.0f, knee + 0.5f);

            for (Float3& px : img.pixels)
            {
                const float lum = Luminance(px);
                if (lum <= knee)
                    continue;

                const float compressedLum = (lum < limit)
                    ? lum
                    : (knee + (limit - knee) * (1.0f - std::exp(-(lum - limit) / std::max(limit, 1e-6f))));
                const float scale = compressedLum / std::max(lum, 1e-6f);
                px *= scale;
            }
        }

        static void RemoveDominantSunHotspotForIBL(FloatImage& img) noexcept
        {
            if (img.pixels.empty() || img.width == 0u || img.height == 0u)
                return;

            float avgLum = 0.0f;
            float maxLum = 0.0f;
            for (const Float3& px : img.pixels)
            {
                const float lum = Luminance(px);
                avgLum += lum;
                maxLum = std::max(maxLum, lum);
            }

            avgLum /= static_cast<float>(img.pixels.size());
            if (!(avgLum > 1e-6f) || maxLum <= avgLum * 8.0f)
                return;

            const float hotspotThreshold = std::max(avgLum * 4.0f, 1.0f);
            std::vector<Float3> filtered = img.pixels;

            for (uint32_t y = 0u; y < img.height; ++y)
            {
                for (uint32_t x = 0u; x < img.width; ++x)
                {
                    const size_t idx = static_cast<size_t>(y) * img.width + x;
                    const float lum = Luminance(img.pixels[idx]);
                    if (lum <= hotspotThreshold)
                        continue;

                    Float3 localAvg{};
                    uint32_t localCount = 0u;
                    for (int oy = -4; oy <= 4; ++oy)
                    {
                        const uint32_t sy = static_cast<uint32_t>(std::clamp<int>(static_cast<int>(y) + oy, 0, static_cast<int>(img.height) - 1));
                        for (int ox = -4; ox <= 4; ++ox)
                        {
                            const uint32_t sx = static_cast<uint32_t>((static_cast<int>(x) + ox + static_cast<int>(img.width)) % static_cast<int>(img.width));
                            const Float3 sample = img.pixels[static_cast<size_t>(sy) * img.width + sx];
                            if (Luminance(sample) > hotspotThreshold)
                                continue;
                            localAvg += sample;
                            ++localCount;
                        }
                    }

                    Float3 replacement = img.pixels[idx];
                    if (localCount > 0u)
                        replacement = localAvg / static_cast<float>(localCount);

                    const float targetLum = std::min(hotspotThreshold, lum);
                    const float replacementLum = std::max(Luminance(replacement), 1e-6f);
                    replacement *= targetLum / replacementLum;

                    const float blend = std::clamp((lum - hotspotThreshold) / std::max(maxLum - hotspotThreshold, 1e-6f), 0.65f, 1.0f);
                    filtered[idx] = Lerp(img.pixels[idx], replacement, blend);
                }
            }

            img.pixels = std::move(filtered);
        }

        // =====================================================================
        // Quasi-random sampling
        // =====================================================================
        [[nodiscard]] static float RadicalInverseVdC(uint32_t bits) noexcept
        {
            bits = (bits<<16u)|(bits>>16u);
            bits = ((bits&0x55555555u)<<1u)|((bits&0xAAAAAAAAu)>>1u);
            bits = ((bits&0x33333333u)<<2u)|((bits&0xCCCCCCCCu)>>2u);
            bits = ((bits&0x0F0F0F0Fu)<<4u)|((bits&0xF0F0F0F0u)>>4u);
            bits = ((bits&0x00FF00FFu)<<8u)|((bits&0xFF00FF00u)>>8u);
            return static_cast<float>(bits) * 2.3283064365386963e-10f;
        }
        [[nodiscard]] static Float2 Hammersley(uint32_t i, uint32_t n) noexcept
        { return { static_cast<float>(i)/static_cast<float>(n), RadicalInverseVdC(i) }; }

        // =====================================================================
        // GGX importance sampling
        // =====================================================================
        [[nodiscard]] static Float3 ImportanceSampleGGX(const Float2& xi, const Float3& n, float roughness) noexcept
        {
            const float a        = roughness * roughness;
            const float phi      = kTwoPi * xi.x;
            const float cosTheta = std::sqrt((1.f-xi.y) / std::max(1.f+(a*a-1.f)*xi.y, 1e-6f));
            const float sinTheta = std::sqrt(std::max(1.f-cosTheta*cosTheta, 0.f));
            const Float3 hT{ std::cos(phi)*sinTheta, std::sin(phi)*sinTheta, cosTheta };
            const Float3 up      = (std::abs(n.z)<0.999f) ? Float3{0.f,0.f,1.f} : Float3{1.f,0.f,0.f};
            const Float3 tangent = Normalize(Cross(up, n));
            const Float3 bitang  = Cross(n, tangent);
            return Normalize(tangent*hT.x + bitang*hT.y + n*hT.z);
        }

        // =====================================================================
        // Source image decoding
        // =====================================================================
        [[nodiscard]] static FloatImage DecodeAsset(const assets::TextureAsset& asset)
        {
            using internal::HalfToFloat;
            FloatImage img{ asset.width, asset.height,
                std::vector<Float3>(static_cast<size_t>(asset.width)*asset.height) };

            if (asset.format == assets::TextureFormat::RGBA16F)
            {
                const auto* src = reinterpret_cast<const uint16_t*>(asset.pixelData.data());
                for (size_t i = 0; i < img.pixels.size(); ++i)
                    img.pixels[i] = { HalfToFloat(src[i*4+0]), HalfToFloat(src[i*4+1]), HalfToFloat(src[i*4+2]) };
            }
            else
            {
                const auto* src = asset.pixelData.data();
                for (size_t i = 0; i < img.pixels.size(); ++i)
                {
                    const float r = static_cast<float>(src[i*4+0])/255.f;
                    const float g = static_cast<float>(src[i*4+1])/255.f;
                    const float b = static_cast<float>(src[i*4+2])/255.f;
                    img.pixels[i] = assets::IsSRGBColorSpace(asset.metadata)
                        ? Float3{ SrgbToLinear(r), SrgbToLinear(g), SrgbToLinear(b) }
                        : Float3{ r, g, b };
                }
            }
            return img;
        }

        [[nodiscard]] static FloatImage BuildProceduralSkyImage(const ProceduralSkyDesc& sky) noexcept
        {
            FloatImage img{ kSkySourceWidth, kSkySourceHeight,
                std::vector<Float3>(static_cast<size_t>(kSkySourceWidth)*kSkySourceHeight) };

            const Float3 zenith  { sky.zenithColor.x,  sky.zenithColor.y,  sky.zenithColor.z  };
            const Float3 horizon { sky.horizonColor.x, sky.horizonColor.y, sky.horizonColor.z };
            const Float3 ground  { sky.groundColor.x,  sky.groundColor.y,  sky.groundColor.z  };
            const Float3 sunTint { sky.sunColor.x,     sky.sunColor.y,     sky.sunColor.z     };
            const Float3 sunDir  = Normalize({ sky.sunDirection.x, sky.sunDirection.y, sky.sunDirection.z });
            const float cosSunR  = std::cos(std::max(sky.sunAngularRadius, 1e-4f));
            const float sharp    = std::max(sky.horizonSharpness, 0.1f);

            for (uint32_t y = 0u; y < kSkySourceHeight; ++y)
                for (uint32_t x = 0u; x < kSkySourceWidth; ++x)
                {
                    const float u  = (static_cast<float>(x)+0.5f)/static_cast<float>(kSkySourceWidth);
                    const float v  = (static_cast<float>(y)+0.5f)/static_cast<float>(kSkySourceHeight);
                    const Float3 d = DirectionFromEquirect(u, v);
                    Float3 color{};
                    if (d.y >= 0.f)
                    { const float t = 1.f-std::exp(-d.y*sharp); color = horizon*(1.f-t)+zenith*t; }
                    else
                    { const float t = SmoothStep01(-d.y*4.f); color = horizon*(1.f-t)+ground*t; }
                    const float cosA = Dot(d, sunDir);
                    if (cosA >= cosSunR)
                    {
                        const float rng = 1.f - cosSunR;
                        const float t   = (rng > 1e-6f) ? SmoothStep01((cosA-cosSunR)/rng) : 1.f;
                        color += sunTint * (sky.sunIntensity * t);
                    }
                    img.pixels[static_cast<size_t>(y)*kSkySourceWidth+x] = color;
                }
            return img;
        }

        // =====================================================================
        // Cube map building — inner pixel loop extracted for reuse
        // =====================================================================
        static void ProcessIrradianceFace(FloatCubeImage& out,
                                           const FloatCubeImage& env,
                                           uint32_t face, uint32_t size, uint32_t samples) noexcept
        {
            for (uint32_t y = 0u; y < size; ++y)
                for (uint32_t x = 0u; x < size; ++x)
                {
                    const float u  = (static_cast<float>(x)+0.5f)/static_cast<float>(size);
                    const float v  = (static_cast<float>(y)+0.5f)/static_cast<float>(size);
                    const Float3 N = DirectionFromCubemapFace(static_cast<CubeFace>(face), u, v);
                    const Float3 up      = (std::abs(N.z)<0.999f) ? Float3{0.f,0.f,1.f} : Float3{1.f,0.f,0.f};
                    const Float3 tangent = Normalize(Cross(up, N));
                    const Float3 bitang  = Cross(N, tangent);
                    Float3 accum{};
                    for (uint32_t i = 0u; i < samples; ++i)
                    {
                        const Float2 xi  = Hammersley(i, samples);
                        const float phi  = kTwoPi * xi.x;
                        const float cosT = std::sqrt(1.f - xi.y);
                        const float sinT = std::sqrt(xi.y);
                        const Float3 loc{ std::cos(phi)*sinT, std::sin(phi)*sinT, cosT };
                        accum += SanitizeFinite(SampleCubeBilinear(env, Normalize(tangent*loc.x + bitang*loc.y + N*loc.z)));
                    }
                    out.pixels[CubeIndex(face, x, y, size)] = SanitizeFinite(accum * (kPi / static_cast<float>(samples)));
                }
        }

        static void ProcessPrefilterFace(FloatCubeImage& out,
                                          const std::vector<FloatCubeImage>& envMips,
                                          uint32_t face, uint32_t size,
                                          float roughness, uint32_t samples) noexcept
        {
            if (envMips.empty())
                return;

            const float envResolution = static_cast<float>(envMips.front().size);
            const float saTexel = 4.f * kPi / std::max(6.f * envResolution * envResolution, 1e-6f);
            const float effectiveRoughness = std::max(roughness, 0.04f);

            for (uint32_t y = 0u; y < size; ++y)
                for (uint32_t x = 0u; x < size; ++x)
                {
                    const float u   = (static_cast<float>(x)+0.5f)/static_cast<float>(size);
                    const float v   = (static_cast<float>(y)+0.5f)/static_cast<float>(size);
                    const Float3 R  = DirectionFromCubemapFace(static_cast<CubeFace>(face), u, v);
                    const Float3 N  = R;
                    const Float3 V  = R;
                    Float3 acc{}; float totalW = 0.f;
                    for (uint32_t i = 0u; i < samples; ++i)
                    {
                        const Float2 xi = Hammersley(i, samples);
                        const Float3 H  = ImportanceSampleGGX(xi, N, effectiveRoughness);
                        const float HoV = std::max(Dot(H, V), 0.f);
                        if (HoV <= 1e-6f)
                            continue;

                        const Float3 L  = Normalize(Reflect(-V, H));
                        const float NoL = std::max(Dot(N, L), 0.f);
                        if (NoL <= 0.f) continue;

                        const float NoH = std::max(Dot(N, H), 0.f);
                        const float pdf = D_GGX(NoH, effectiveRoughness) * NoH / std::max(4.f * HoV, 1e-6f) + 1e-4f;
                        const float saSample = 1.f / (static_cast<float>(samples) * pdf + 1e-4f);
                        const float mipLevel = (roughness <= 0.f) ? 0.f
                            : std::max(0.f, 0.5f * std::log2(saSample / saTexel));

                        acc    += SanitizeFinite(SampleCubeBilinearLod(envMips, L, mipLevel)) * NoL;
                        totalW += NoL;
                    }
                    out.pixels[CubeIndex(face, x, y, size)] = (totalW > 0.f) ? SanitizeFinite(acc / totalW) : Float3{};
                }
        }

        // =====================================================================
        // Parallel cube builders
        // Each face writes to a non-overlapping slice of the preallocated pixel
        // buffer, so concurrent writes are safe without any mutex.
        // =====================================================================
        [[nodiscard]] static FloatCubeImage BuildEnvironmentCube(const FloatImage& src,
                                                               uint32_t size,
                                                               jobs::JobSystem* js)
        {
            FloatCubeImage cube{ size, std::vector<Float3>(static_cast<size_t>(kCubeFaceCount)*size*size) };

            auto processRange = [&](size_t faceBegin, size_t faceEnd)
            {
                for (size_t fi = faceBegin; fi < faceEnd; ++fi)
                {
                    const uint32_t face = static_cast<uint32_t>(fi);
                    for (uint32_t y = 0u; y < size; ++y)
                        for (uint32_t x = 0u; x < size; ++x)
                        {
                            const Float3 d  = DirectionFromCubemapFace(static_cast<CubeFace>(face),
                                (static_cast<float>(x)+0.5f)/static_cast<float>(size),
                                (static_cast<float>(y)+0.5f)/static_cast<float>(size));
                            const Float2 eq = EquirectFromDirection(d);
                            cube.pixels[CubeIndex(face,x,y,size)] = SanitizeFinite(SampleBilinear(src, eq.x, eq.y));
                        }
                }
            };

            if (js && js->IsParallel())
            {
                const jobs::TaskResult result = js->ParallelFor(kCubeFaceCount, processRange, 1u);
                if (result.Failed())
                    processRange(0u, kCubeFaceCount);
            }
            else
                processRange(0u, kCubeFaceCount);

            return cube;
        }

        [[nodiscard]] static FloatCubeImage BuildIrradianceCube(const FloatCubeImage& env,
                                                                  uint32_t size, uint32_t samples,
                                                                  jobs::JobSystem* js)
        {
            FloatCubeImage cube{ size, std::vector<Float3>(static_cast<size_t>(kCubeFaceCount)*size*size) };

            auto processRange = [&](size_t faceBegin, size_t faceEnd)
            {
                for (size_t fi = faceBegin; fi < faceEnd; ++fi)
                    ProcessIrradianceFace(cube, env, static_cast<uint32_t>(fi), size, samples);
            };

            if (js && js->IsParallel())
            {
                const jobs::TaskResult result = js->ParallelFor(kCubeFaceCount, processRange, 1u);
                if (result.Failed())
                    processRange(0u, kCubeFaceCount);
            }
            else
                processRange(0u, kCubeFaceCount);

            return cube;
        }

        [[nodiscard]] static FloatCubeImage BuildPrefilterCubeMip(const std::vector<FloatCubeImage>& envMips,
                                                                    uint32_t size, float roughness,
                                                                    uint32_t samples,
                                                                    jobs::JobSystem* js)
        {
            FloatCubeImage cube{ size, std::vector<Float3>(static_cast<size_t>(kCubeFaceCount)*size*size) };

            auto processRange = [&](size_t faceBegin, size_t faceEnd)
            {
                for (size_t fi = faceBegin; fi < faceEnd; ++fi)
                    ProcessPrefilterFace(cube, envMips, static_cast<uint32_t>(fi), size, roughness, samples);
            };

            if (js && js->IsParallel())
            {
                const jobs::TaskResult result = js->ParallelFor(kCubeFaceCount, processRange, 1u);
                if (result.Failed())
                    processRange(0u, kCubeFaceCount);
            }
            else
                processRange(0u, kCubeFaceCount);

            return cube;
        }

        // =====================================================================
        // RGBA16F packing — converts FloatCubeImage to raw bytes (face-major)
        // =====================================================================
        [[nodiscard]] static std::vector<uint8_t> PackCubeRGBA16F(const FloatCubeImage& cube)
        {
            using internal::FloatToHalf;
            const size_t numPixels = static_cast<size_t>(kCubeFaceCount) * cube.size * cube.size;
            std::vector<uint8_t> out(numPixels * 4u * sizeof(uint16_t));
            auto* dst = reinterpret_cast<uint16_t*>(out.data());
            for (size_t i = 0u; i < numPixels; ++i)
            {
                dst[i*4+0] = FloatToHalf(cube.pixels[i].x);
                dst[i*4+1] = FloatToHalf(cube.pixels[i].y);
                dst[i*4+2] = FloatToHalf(cube.pixels[i].z);
                dst[i*4+3] = FloatToHalf(1.f);
            }
            return out;
        }

        // =====================================================================
        // FNV-1a 64-bit — same prime/offset as ShaderRuntime
        // =====================================================================
        [[nodiscard]] static uint64_t FNV1a(const void* data, size_t size,
                                             uint64_t seed = 14695981039346656037ull) noexcept
        {
            const auto* b = static_cast<const uint8_t*>(data);
            uint64_t h = seed;
            for (size_t i = 0u; i < size; ++i) { h ^= b[i]; h *= 1099511628211ull; }
            return h;
        }

        // Convenience: hash a single float value, continuing an existing hash.
        [[nodiscard]] static uint64_t FNV1aFloat(float v, uint64_t seed) noexcept
        { return FNV1a(&v, sizeof(v), seed); }

        // =====================================================================
        // Core bake (shared entry point)
        // =====================================================================
        [[nodiscard]] static IBLBakedData BakeFromSource(const FloatImage& source,
                                                          const IBLBakeParams& params)
        {
            jobs::JobSystem* js = params.jobSystem;

            IBLBakedData data{};
            data.environmentSize   = params.environmentSize;
            data.irradianceSize    = params.irradianceSize;
            data.prefilterBaseSize = params.prefilterBaseSize;
            data.prefilterMipCount = params.prefilterMipCount;

            const FloatCubeImage envCube = BuildEnvironmentCube(source, params.environmentSize, js);
            const std::vector<FloatCubeImage> envMips = BuildCubeMipChain(envCube);
            const FloatCubeImage irrCube = BuildIrradianceCube(envCube,
                params.irradianceSize, params.irradianceSamples, js);

            data.environmentData = PackCubeRGBA16F(envCube);
            data.irradianceData  = PackCubeRGBA16F(irrCube);

            uint32_t mipSize = params.prefilterBaseSize;
            for (uint32_t mip = 0u; mip < params.prefilterMipCount; ++mip)
            {
                const float roughness = (params.prefilterMipCount <= 1u)
                    ? 0.f
                    : static_cast<float>(mip) / static_cast<float>(params.prefilterMipCount-1u);
                const FloatCubeImage mipCube = BuildPrefilterCubeMip(envMips, mipSize,
                    roughness, params.prefilterSamples, js);
                const std::vector<uint8_t> mipBytes = PackCubeRGBA16F(mipCube);
                data.prefilterData.insert(data.prefilterData.end(), mipBytes.begin(), mipBytes.end());
                mipSize = std::max(mipSize / 2u, 1u);
            }
            return data;
        }

    } // namespace

    // =========================================================================
    // IBLBaker — public API
    // =========================================================================

    uint64_t IBLBaker::HashTextureSource(const assets::TextureAsset& asset, float intensity) noexcept
    {
        uint64_t h = FNV1a(asset.pixelData.data(), asset.pixelData.size());
        h = FNV1a(&intensity,     sizeof(intensity),     h);
        h = FNV1a(&asset.width,   sizeof(asset.width),   h);
        h = FNV1a(&asset.height,  sizeof(asset.height),  h);
        const auto fmt = static_cast<uint32_t>(asset.format);
        h = FNV1a(&fmt,           sizeof(fmt),           h);
        return h;
    }

    uint64_t IBLBaker::HashProceduralSky(const ProceduralSkyDesc& sky, float intensity) noexcept
    {
        // Hash each field individually in a fixed order.
        // Never hashes raw struct bytes — padding-safe, layout-stable.
        constexpr uint64_t kSeed = 14695981039346656037ull;
        uint64_t h = kSeed;

        // Sky gradient colors
        h = FNV1aFloat(sky.zenithColor.x,  h); h = FNV1aFloat(sky.zenithColor.y,  h); h = FNV1aFloat(sky.zenithColor.z,  h);
        h = FNV1aFloat(sky.horizonColor.x, h); h = FNV1aFloat(sky.horizonColor.y, h); h = FNV1aFloat(sky.horizonColor.z, h);
        h = FNV1aFloat(sky.groundColor.x,  h); h = FNV1aFloat(sky.groundColor.y,  h); h = FNV1aFloat(sky.groundColor.z,  h);

        // Normalized sun direction (direction-only, magnitude doesn't affect IBL)
        {
            const float dx = sky.sunDirection.x, dy = sky.sunDirection.y, dz = sky.sunDirection.z;
            const float len = std::sqrt(dx*dx + dy*dy + dz*dz);
            const float inv = (len > 1e-12f) ? (1.f/len) : 1.f;
            h = FNV1aFloat(dx*inv, h); h = FNV1aFloat(dy*inv, h); h = FNV1aFloat(dz*inv, h);
        }

        // Sun properties
        h = FNV1aFloat(sky.sunColor.x,      h); h = FNV1aFloat(sky.sunColor.y,      h); h = FNV1aFloat(sky.sunColor.z, h);
        h = FNV1aFloat(sky.sunIntensity,     h);
        h = FNV1aFloat(sky.sunAngularRadius, h);

        // Atmospheric parameters
        h = FNV1aFloat(sky.horizonSharpness, h);

        // Intensity applied after source generation
        h = FNV1aFloat(intensity, h);

        return h;
    }

    IBLBakedData IBLBaker::BakeFromTexture(const assets::TextureAsset& asset,
                                            float intensity,
                                            const IBLBakeParams& params)
    {
        if (asset.pixelData.empty() || asset.width == 0u || asset.height == 0u)
            return {};
        FloatImage source = DecodeAsset(asset);
        if (intensity != 1.f)
            for (Float3& px : source.pixels) px *= intensity;
        CompressDominantHighlightsForIBL(source);
        RemoveDominantSunHotspotForIBL(source);
        return BakeFromSource(source, params);
    }

    IBLBakedData IBLBaker::BakeFromProceduralSky(const ProceduralSkyDesc& sky,
                                                   float intensity,
                                                   const IBLBakeParams& params)
    {
        FloatImage source = BuildProceduralSkyImage(sky);
        if (intensity != 1.f)
            for (Float3& px : source.pixels) px *= intensity;
        return BakeFromSource(source, params);
    }

} // namespace engine::renderer
