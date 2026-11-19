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

} // namespace engine::renderer::addons::forward
