#pragma once
// =============================================================================
// KROM Engine - addons/forward/ForwardTonemapJson.hpp
// Scene keys of the exposure mode and the auto-exposure limits ("environment"
// block of editor scenes and .kscene files). Header only, for the editor and the
// krom.h runtime, which both read scene JSON.
// =============================================================================
#include "addons/forward/ForwardTonemap.hpp"
#include "serialization/SceneSerializer.hpp"

namespace engine::renderer::addons::forward {

inline constexpr const char* kSceneKeyExposureMode      = "exposureMode";
inline constexpr const char* kSceneKeyAutoExposureMinEv = "autoExposureMinEv";
inline constexpr const char* kSceneKeyAutoExposureMaxEv = "autoExposureMaxEv";
inline constexpr const char* kSceneKeyAutoExposureToBright = "autoExposureToBright";
inline constexpr const char* kSceneKeyAutoExposureToDark   = "autoExposureToDark";
// Object with the colour grading; missing = neutral, written only when not neutral.
inline constexpr const char* kSceneKeyColorGrading = "colorGrading";

// Reads mode and limits from a scene "environment" block. Missing keys give the
// defaults: scenes written before auto-exposure keep the fixed exposure.
inline void ReadSceneExposureJson(const serialization::JsonValue& env, ExposureMode& mode,
                                  AutoExposureSettings& autoExposure)
{
    mode = ExposureMode::Fixed;
    autoExposure = AutoExposureSettings{};
    if (const auto* value = env.Get(kSceneKeyExposureMode); value && value->IsString())
        mode = ParseExposureMode(value->AsString().c_str());
    const auto readFloat = [&env](const char* key, float& out)
    {
        if (const auto* value = env.Get(key); value && value->IsNumber())
            out = value->AsFloat();
    };
    readFloat(kSceneKeyAutoExposureMinEv, autoExposure.minEv);
    readFloat(kSceneKeyAutoExposureMaxEv, autoExposure.maxEv);
    readFloat(kSceneKeyAutoExposureToBright, autoExposure.toBrightSpeed);
    readFloat(kSceneKeyAutoExposureToDark, autoExposure.toDarkSpeed);
    autoExposure.Clamp();
}

// Reads the "colorGrading" object of a scene "environment" block. Missing object
// or keys give the neutral values: older scenes look unchanged.
inline void ReadSceneColorGradingJson(const serialization::JsonValue& env, ColorGradingSettings& out)
{
    out = ColorGradingSettings{};
    const serialization::JsonValue* g = env.Get(kSceneKeyColorGrading);
    if (!g || !g->IsObject())
        return;
    if (const auto* look = g->Get("look"); look && look->IsString())
        out.look = ParseAgxLook(look->AsString().c_str());
    const auto readFloat = [g](const char* key, float& value)
    {
        if (const auto* v = g->Get(key); v && v->IsNumber())
            value = v->AsFloat();
    };
    const auto readColor = [](const serialization::JsonValue* v, std::array<float, 3>& value)
    {
        if (v && v->IsArray() && v->arrayVal.size() >= 3u)
            for (size_t i = 0; i < 3u; ++i)
                if (v->At(i).IsNumber())
                    value[i] = v->At(i).AsFloat();
    };
    readFloat("temperature", out.temperature);
    readFloat("tint", out.tint);
    readColor(g->Get("colorFilter"), out.colorFilter);
    readFloat("hueShift", out.hueShift);
    readFloat("contrast", out.contrast);
    readFloat("saturation", out.saturation);
    // Wheels: [r, g, b, value].
    const auto readWheel = [&readColor](const serialization::JsonValue* v, ColorGradingWheel& wheel)
    {
        readColor(v, wheel.color);
        if (v && v->IsArray() && v->arrayVal.size() >= 4u && v->At(3).IsNumber())
            wheel.value = v->At(3).AsFloat();
    };
    readWheel(g->Get("lift"), out.lift);
    readWheel(g->Get("gamma"), out.gamma);
    readWheel(g->Get("gain"), out.gain);
    out.Clamp();
}

// Writes the "colorGrading" object (inside an open "environment" object), only
// when the grading is not neutral.
inline void WriteSceneColorGradingJson(serialization::JsonWriter& w, const ColorGradingSettings& g)
{
    if (g.IsNeutral())
        return;
    const auto color = [&w](const char* key, const std::array<float, 3>& c, const float* value)
    {
        w.BeginArray(key);
        for (float v : c)
            w.WriteFloat("", v);
        if (value)
            w.WriteFloat("", *value);
        w.EndArray();
    };
    w.BeginObject(kSceneKeyColorGrading);
    w.WriteString("look", AgxLookName(g.look));
    w.WriteFloat("temperature", g.temperature);
    w.WriteFloat("tint", g.tint);
    color("colorFilter", g.colorFilter, nullptr);
    w.WriteFloat("hueShift", g.hueShift);
    w.WriteFloat("contrast", g.contrast);
    w.WriteFloat("saturation", g.saturation);
    color("lift", g.lift.color, &g.lift.value);
    color("gamma", g.gamma.color, &g.gamma.value);
    color("gain", g.gain.color, &g.gain.value);
    w.EndObject();
}

} // namespace engine::renderer::addons::forward
