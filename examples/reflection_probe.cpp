// =============================================================================
// KROM Engine - examples/reflection_probe.cpp
//
// A room open to the sky on one side, a metal box inside, and two local
// reflection probes: one for the whole room and a smaller one with higher
// priority for the back half (they blend over one metre). P toggles the
// probes (global IBL everywhere vs. the baked room), B bakes again.
// KROM_PROBE_SO=1 turns on specular occlusion, KROM_PROBE_DEBUG=1..3 shows a
// probe debug view.
//
// KROM_PROBE_SELFTEST=1: no geometry; bakes a probe in the empty sky and
// compares its prefiltered cube with the global IBL cache. Prints the error of
// the capture and of a mirrored capture and exits with 0 if the capture
// matches the global IBL clearly better (checks the face orientation).
// KROM_PROBES_OFF=1 starts with the probes switched off.
// =============================================================================
#include "krom.h"
#include "ExampleBackend.hpp"
#include "addons/camera/CameraComponents.hpp"
#include "addons/probes/ProbeComponents.hpp"
#include "addons/probes/ProbeData.hpp"
#include "core/Debug.hpp"
#include "core/PathUtf8.hpp"
#include "renderer/IBLCacheSerializer.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace {

void AddBox(LPENTITY entity, float sx, float sy, float sz)
{
    LPSURFACE surf = Krom::CreateSurface(entity);
    struct Face { float n[3]; float u[3]; float v[3]; };
    const Face faces[6] = {
        { { 1, 0, 0 }, { 0, 0,-1 }, { 0, 1, 0 } }, { {-1, 0, 0 }, { 0, 0, 1 }, { 0, 1, 0 } },
        { { 0, 1, 0 }, { 1, 0, 0 }, { 0, 0,-1 } }, { { 0,-1, 0 }, { 1, 0, 0 }, { 0, 0, 1 } },
        { { 0, 0, 1 }, { 1, 0, 0 }, { 0, 1, 0 } }, { { 0, 0,-1 }, {-1, 0, 0 }, { 0, 1, 0 } },
    };
    const float h[3] = { sx * 0.5f, sy * 0.5f, sz * 0.5f };
    int base = 0;
    for (const Face& f : faces)
    {
        const float corners[4][2] = { {-1,-1}, { 1,-1}, { 1, 1}, {-1, 1} };
        for (const auto& c : corners)
        {
            float p[3];
            for (int i = 0; i < 3; ++i)
                p[i] = (f.n[i] + f.u[i] * c[0] + f.v[i] * c[1]) * h[i];
            Krom::AddVertex(surf, p[0], p[1], p[2], (c[0] + 1.f) * 0.5f, (1.f - c[1]) * 0.5f, f.n[0], f.n[1], f.n[2]);
        }
        Krom::AddTriangle(surf, base, base + 1, base + 2);
        Krom::AddTriangle(surf, base, base + 2, base + 3);
        base += 4;
    }
}

LPENTITY Wall(const char* name, float x, float y, float z, float sx, float sy, float sz, float r, float g, float b)
{
    LPENTITY e = Krom::CreateMesh(name);
    AddBox(e, sx, sy, sz);
    Krom::SetPosition(e, x, y, z);
    Krom::SetPBR(e, Krom::PBR{ .color = Krom::Color(r, g, b), .roughness = 0.9f, .metallic = 0.f });
    return e;
}

float HalfToFloat(uint16_t hv)
{
    const uint32_t exp = (hv >> 10u) & 0x1Fu;
    const uint32_t mant = hv & 0x3FFu;
    float v = exp == 0u ? std::ldexp(static_cast<float>(mant), -24)
                        : std::ldexp(static_cast<float>(mant | 0x400u), static_cast<int>(exp) - 25);
    return (hv & 0x8000u) ? -v : v;
}

// Mean |log(1+a) - log(1+b)| over mip 0 (RGB). mirror: compare a's texel x
// with b's texel size-1-x.
double CompareMip0(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, uint32_t size, bool mirror)
{
    double sum = 0.0;
    size_t count = 0u;
    for (uint32_t f = 0u; f < 6u; ++f)
        for (uint32_t y = 0u; y < size; ++y)
            for (uint32_t x = 0u; x < size; ++x)
            {
                const size_t ia = ((static_cast<size_t>(f) * size + y) * size + x) * 4u;
                const size_t ib = ((static_cast<size_t>(f) * size + y) * size + (mirror ? size - 1u - x : x)) * 4u;
                for (uint32_t c = 0u; c < 3u; ++c)
                {
                    uint16_t ha = 0u, hb = 0u;
                    std::memcpy(&ha, a.data() + (ia + c) * 2u, 2u);
                    std::memcpy(&hb, b.data() + (ib + c) * 2u, 2u);
                    sum += std::fabs(std::log1p(std::max(0.f, HalfToFloat(ha))) - std::log1p(std::max(0.f, HalfToFloat(hb))));
                    ++count;
                }
            }
    return count ? sum / static_cast<double>(count) : 1e9;
}

int RunSelfTest(LPENTITY probe)
{
    if (!Krom::BakeReflectionProbe(probe, "SelfTest"))
        return 2;
    const auto* comp = Krom::GetComponent<engine::ReflectionProbeComponent>(probe);
    engine::addons::probes::ProbeBakedData baked{};
    std::string error;
    if (!comp || !engine::addons::probes::ReadProbeFile(
            engine::PathToUtf8(std::filesystem::path("assets") / engine::PathFromUtf8(comp->bakedDataPath)), baked, &error))
    {
        std::printf("probe selftest: cannot read baked probe: %s\n", error.c_str());
        return 3;
    }

    // The global IBL of this sky is one of the cached files; take the best match.
    double bestDirect = 1e9, bestMirrored = 1e9;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(".krom/ibl", ec))
    {
        if (entry.path().extension() != ".iblcache")
            continue;
        engine::renderer::IBLBakedData global{};
        if (!engine::renderer::IBLCacheSerializer::Read(engine::PathToUtf8(entry.path()), global) ||
            global.prefilterBaseSize != baked.prefilterBaseSize)
            continue;
        const double direct = CompareMip0(baked.prefilterData, global.prefilterData, baked.prefilterBaseSize, false);
        if (direct < bestDirect)
        {
            bestDirect = direct;
            bestMirrored = CompareMip0(baked.prefilterData, global.prefilterData, baked.prefilterBaseSize, true);
        }
    }
    std::printf("probe selftest: mean log error capture %.4f, mirrored capture %.4f\n", bestDirect, bestMirrored);
    return (bestDirect < 0.5 * bestMirrored) ? 0 : 4;
}

} // namespace

int main()
{
    engine::Debug::ResetMinLevelForBuild();
    Krom::SetAssetRoot("assets");
    if (!Krom::Graphics(ExampleRenderer(), 1280, 720, "Reflection Probe"))
        return 1;

    auto env = Krom::CreateEnvironmentFromHDR("autumn_field_puresky_2k.hdr", 1.f, true);
    Krom::SetActiveEnvironment(env);

    const char* selfTestEnv = std::getenv("KROM_PROBE_SELFTEST");
    const bool selfTest = selfTestEnv && selfTestEnv[0] == '1';

    LPENTITY camera = Krom::CreateCamera("MainCamera", { 0.f, 1.6f, 9.f }, 60.f, true);
    Krom::GetComponent<engine::CameraComponent>(camera)->backgroundMode = engine::BackgroundMode::Skybox;

    LPENTITY probe = Krom::CreateEntity("Probe");
    Krom::SetPosition(probe, 0.f, 1.5f, 0.f);
    {
        engine::ReflectionProbeComponent p{};
        // A bit larger than the room (by the blend distance), so the walls lie
        // fully inside the box and only the space outside fades out.
        p.extents = { 4.5f, 2.0f, 4.5f };
        p.blendDistance = 0.5f;
        Krom::AddComponent<engine::ReflectionProbeComponent>(probe, p);
    }
    LPENTITY backProbe = NULL_LPENTITY;
    if (!selfTest)
    {
        backProbe = Krom::CreateEntity("BackProbe");
        Krom::SetPosition(backProbe, 0.f, 1.5f, -2.25f);
        engine::ReflectionProbeComponent p{};
        p.extents = { 4.5f, 2.0f, 2.25f };
        p.blendDistance = 0.5f;
        p.priority = 1;
        Krom::AddComponent<engine::ReflectionProbeComponent>(backProbe, p);
    }

    if (!selfTest)
    {
        // Room 8 x 3 x 8, open towards +Z (the camera side).
        Wall("Floor",   0.f, -0.1f,  0.f, 8.4f, 0.2f, 8.4f, 0.6f, 0.55f, 0.5f);
        Wall("Ceiling", 0.f,  3.1f,  0.f, 8.4f, 0.2f, 8.4f, 0.8f, 0.8f, 0.8f);
        Wall("Back",    0.f,  1.5f, -4.1f, 8.4f, 3.2f, 0.2f, 0.7f, 0.2f, 0.15f);
        Wall("Left",   -4.1f, 1.5f,  0.f, 0.2f, 3.2f, 8.4f, 0.2f, 0.4f, 0.7f);
        Wall("Right",   4.1f, 1.5f,  0.f, 0.2f, 3.2f, 8.4f, 0.8f, 0.8f, 0.75f);

        LPENTITY ball = Krom::CreateMesh("Ball");
        AddBox(ball, 1.2f, 1.2f, 1.2f);
        Krom::SetPosition(ball, 0.f, 0.8f, 0.f);
        Krom::SetPBR(ball, Krom::PBR{ .color = Krom::Color(0.95f, 0.95f, 0.95f), .roughness = 0.15f, .metallic = 1.f });

        Krom::CreatePointLight("Lamp", { 0.f, 2.6f, -1.f }, { 1.f, 0.9f, 0.8f }, 6.f, 9.f);
    }

    int frame = 0;
    // KROM_PROBES_OFF=1 starts with probes off (for image comparisons).
    const char* probesOffEnv = std::getenv("KROM_PROBES_OFF");
    bool probesOn = !(probesOffEnv && probesOffEnv[0] == '1');
    Krom::SetReflectionProbesEnabled(probesOn);
    if (const char* so = std::getenv("KROM_PROBE_SO"); so && so[0] == '1')
        Krom::SetSpecularOcclusion(true);
    if (const char* view = std::getenv("KROM_PROBE_DEBUG"); view && view[0] >= '0' && view[0] <= '3')
        Krom::SetProbeDebugView(static_cast<uint32_t>(view[0] - '0'));
    int result = 5;   // stays if the self test never ran
    const int runResult = Krom::Run([&](float) mutable
    {
        ++frame;
        // Bake after a few frames so the environment and the meshes are uploaded.
        if (frame == 5)
        {
            if (selfTest)
            {
                result = RunSelfTest(probe);
                Krom::Quit();
                return;
            }
            Krom::BakeReflectionProbe(probe, "ReflectionProbeExample");
            Krom::BakeReflectionProbe(backProbe, "ReflectionProbeExample");
        }
        if (Krom::KeyHit(Krom::Key::B))
        {
            Krom::BakeReflectionProbe(probe, "ReflectionProbeExample");
            Krom::BakeReflectionProbe(backProbe, "ReflectionProbeExample");
        }
        if (Krom::KeyHit(Krom::Key::P))
        {
            probesOn = !probesOn;
            Krom::SetReflectionProbesEnabled(probesOn);
        }
        if (Krom::KeyHit(Krom::Key::Escape))
            Krom::Quit();
    });
    return selfTest ? result : runResult;
}
