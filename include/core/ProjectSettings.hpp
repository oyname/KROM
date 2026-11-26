#pragma once

#include "core/RenderQuality.hpp"
#include "platform/IInput.hpp"
#include "platform/IWindow.hpp"
#include "serialization/SceneSerializer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace engine {

struct InputActionBinding
{
    std::string name;
    platform::Key key = platform::Key::Unknown;
    platform::GamepadButton button = platform::GamepadButton::None;
};

// Project-wide render options. Plain data: the add-ons that own the options
// (forward, GTAO, probes, SSR) convert them to their own settings types.
struct ProjectRenderSettings
{
    std::string tonemapOperator = "agx";   // reinhard, aces, agx, pbrNeutral
    uint32_t msaaSamples = 1u;             // 1 = off, 2, 4
    bool brdfHeightCorrelated = true;
    bool brdfPerLightFresnel = true;
    bool brdfMultiScatter = true;
    bool brdfBurleyDiffuse = true;
    bool specularAntiAliasing = true;      // geometric specular AA (HLSL backends)
    bool specularOcclusion = true;
    // Screen-space reflections (SSR add-on).
    bool  ssrEnabled = true;
    float ssrIntensity = 1.0f;
    float ssrMaxRoughness = 0.6f;
    float ssrMaxDistance = 30.0f;
    float ssrThickness = 0.3f;
    // Ambient occlusion (GTAO add-on); defaults = GtaoSettings.
    // Off also drops the depth prepass unless another add-on needs it.
    bool  gtaoEnabled = true;
    float gtaoRadius = 0.50f;      // world-space metres
    float gtaoIntensity = 0.65f;   // 0 = no AO
    float gtaoBias = 0.05f;        // horizon angle bias against self-occlusion
    // Screen-space contact shadows of the main directional light (contact shadow
    // add-on); defaults = ContactShadowSettings.
    bool     contactShadowsEnabled = true;
    float    contactShadowLength = 0.30f;     // world-space metres
    float    contactShadowThickness = 0.05f;  // world-space metres
    float    contactShadowIntensity = 1.0f;   // 0 = none
    uint32_t contactShadowSteps = 12u;
    // Bloom (forward add-on). Off forces the bloom intensity to 0, also over
    // post-process volumes. Post-process volumes override the intensity per area.
    bool     bloomEnabled = true;
    float    bloomIntensity = 0.12f;  // strength of the glow
    float    bloomThreshold = 1.0f;   // brightness on screen (after the exposure) from which bloom starts
    // Post-process AA after tonemapping: off, fxaa, smaa (forward add-on).
    std::string postAA = "off";
    // Shadow maps (shadow add-on): tile edge of each sun cascade and of each
    // point/spot light view, and the filter (engine::ShadowFilter id). Global
    // like in Unity/Unreal; lights keep only scene values (distance, bias, ...).
    uint32_t shadowSunResolution = 2048u;
    uint32_t shadowLocalResolution = 1024u;
    uint8_t  shadowFilter = 2u;   // Tent5x5
};

struct ProjectSettings
{
    std::string windowTitle;
    int width = 1280;
    int height = 720;
    bool useSystemResolution = false;
    platform::WindowMode windowMode = platform::WindowMode::Windowed;
    bool vsync = false;
    int testBuild = 0;   // Editor configuration, Debug, RelWithDebInfo, Release
    int exportBuild = 0; // Release, RelWithDebInfo
    math::Vec3 gravity{0.f, -9.81f, 0.f};
    std::array<uint32_t, 32> collisionMask = [] {
        std::array<uint32_t, 32> masks{};
        masks.fill(UINT32_MAX);
        return masks;
    }();
    std::vector<InputActionBinding> actions;
    ProjectRenderSettings rendering;
    // False for a project file without the "rendering" block: such projects kept
    // these options in the "environment" block of each scene (see
    // ReadLegacySceneRenderSettings). The block is always written.
    bool renderingStored = false;
};

// The options of ProjectRenderSettings a quality level sets (the project
// stores the post-AA mode by name; unknown names count as off).
[[nodiscard]] inline RenderQualityOptions GetRenderQualityOptions(const ProjectRenderSettings& settings)
{
    RenderQualityOptions options{};
    options.msaaSamples = settings.msaaSamples >= 4u ? 4u : (settings.msaaSamples >= 2u ? 2u : 1u);
    options.postAA = settings.postAA == "smaa" ? RenderPostAA::Smaa
                   : settings.postAA == "fxaa" ? RenderPostAA::Fxaa : RenderPostAA::Off;
    options.gtaoEnabled = settings.gtaoEnabled;
    options.ssrEnabled = settings.ssrEnabled;
    options.contactShadowsEnabled = settings.contactShadowsEnabled;
    options.sunShadowResolution = settings.shadowSunResolution;
    options.localShadowResolution = settings.shadowLocalResolution;
    options.shadowFilter = settings.shadowFilter;
    return options;
}

inline void SetRenderQualityOptions(ProjectRenderSettings& settings, const RenderQualityOptions& options)
{
    settings.msaaSamples = options.msaaSamples;
    settings.postAA = options.postAA == RenderPostAA::Smaa ? "smaa"
                    : options.postAA == RenderPostAA::Fxaa ? "fxaa" : "off";
    settings.gtaoEnabled = options.gtaoEnabled;
    settings.ssrEnabled = options.ssrEnabled;
    settings.contactShadowsEnabled = options.contactShadowsEnabled;
    settings.shadowSunResolution = options.sunShadowResolution;
    settings.shadowLocalResolution = options.localShadowResolution;
    settings.shadowFilter = options.shadowFilter;
}

// Sets the options of a level; Custom keeps the current options.
inline void ApplyRenderQualityLevel(ProjectRenderSettings& settings, RenderQualityLevel level)
{
    if (level != RenderQualityLevel::Custom)
        SetRenderQualityOptions(settings, RenderQualityPreset(level));
}

[[nodiscard]] inline RenderQualityLevel GetRenderQualityLevel(const ProjectRenderSettings& settings)
{
    return DetectRenderQualityLevel(GetRenderQualityOptions(settings));
}

// Per-computer settings of an exported game (user-settings.json, see
// UserGameSettingsFolderName): what a player picked, which overrides the project.
struct UserGameSettings
{
    std::optional<RenderQualityLevel> qualityLevel;   // never Custom
};

[[nodiscard]] inline UserGameSettings ReadUserGameSettings(const serialization::JsonValue& root)
{
    UserGameSettings result;
    RenderQualityLevel level = RenderQualityLevel::Custom;
    if (const auto* v = root.Get("qualityLevel"); v && v->IsString() &&
        ParseRenderQualityLevel(v->AsString().c_str(), level) && level != RenderQualityLevel::Custom)
        result.qualityLevel = level;
    return result;
}

[[nodiscard]] inline std::string WriteUserGameSettings(const UserGameSettings& settings)
{
    std::string text = "{\n";
    if (settings.qualityLevel && *settings.qualityLevel != RenderQualityLevel::Custom)
        text += std::string("  \"qualityLevel\": \"") + RenderQualityLevelName(*settings.qualityLevel) + "\"\n";
    text += "}\n";
    return text;
}

// Folder name for a game's user settings: the window title (UTF-8) with
// characters Windows does not allow in file names replaced by '_'; trailing
// dots and spaces removed. Reserved Windows device names get an '_' prefix.
// Empty if nothing usable is left.
[[nodiscard]] inline std::string UserGameSettingsFolderName(const std::string& title)
{
    std::string name;
    for (char c : title)
    {
        const unsigned char u = static_cast<unsigned char>(c);
        name += (u < 32u || std::string_view("<>:\"/\\|?*").find(c) != std::string_view::npos) ? '_' : c;
    }
    while (!name.empty() && (name.back() == '.' || name.back() == ' '))
        name.pop_back();
    const size_t first = name.find_first_not_of(' ');
    if (first == std::string::npos)
        return {};
    name.erase(0, first);

    // Device names stay reserved with an extension (e.g. con.txt). Windows
    // also recognises superscript 1, 2 and 3 in COM/LPT device names.
    std::string base = name.substr(0, name.find('.'));
    while (!base.empty() && base.back() == ' ')
        base.pop_back();
    for (char& c : base)
        if (c >= 'a' && c <= 'z')
            c = static_cast<char>(c - 'a' + 'A');
    bool reserved = base == "CON" || base == "PRN" || base == "AUX" || base == "NUL" ||
                    base == "CONIN$" || base == "CONOUT$";
    if (base.starts_with("COM") || base.starts_with("LPT"))
    {
        const std::string_view number = std::string_view(base).substr(3);
        reserved = (number.size() == 1 && number[0] >= '1' && number[0] <= '9') ||
                   number == "\xC2\xB9" || number == "\xC2\xB2" || number == "\xC2\xB3";
    }
    if (reserved)
        name.insert(name.begin(), '_');
    return name;
}

inline void ReadRenderSettingKeys(const serialization::JsonValue& object, ProjectRenderSettings& out)
{
    auto flag = [&](const char* key, bool& value) {
        if (const auto* v = object.Get(key); v && v->IsBool())
            value = v->AsBool();
    };
    if (const auto* op = object.Get("tonemapOperator"); op && op->IsString())
        out.tonemapOperator = op->AsString();
    if (const auto* msaa = object.Get("msaaSamples"); msaa && msaa->IsNumber())
        out.msaaSamples = msaa->numVal >= 4.0 ? 4u : (msaa->numVal >= 2.0 ? 2u : 1u);
    if (const auto* postAA = object.Get("postAA"); postAA && postAA->IsString())
        out.postAA = postAA->AsString();
    flag("brdfHeightCorrelated", out.brdfHeightCorrelated);
    flag("brdfPerLightFresnel", out.brdfPerLightFresnel);
    flag("brdfMultiScatter", out.brdfMultiScatter);
    flag("brdfBurleyDiffuse", out.brdfBurleyDiffuse);
    flag("specularAntiAliasing", out.specularAntiAliasing);
    flag("specularOcclusion", out.specularOcclusion);
    auto number = [&](const char* key, float& value, float lo, float hi) {
        if (const auto* v = object.Get(key); v && v->IsNumber())
            value = std::clamp(static_cast<float>(v->numVal), lo, hi);
    };
    flag("ssrEnabled", out.ssrEnabled);
    number("ssrIntensity", out.ssrIntensity, 0.0f, 1.0f);
    number("ssrMaxRoughness", out.ssrMaxRoughness, 0.05f, 1.0f);
    number("ssrMaxDistance", out.ssrMaxDistance, 0.5f, 500.0f);
    number("ssrThickness", out.ssrThickness, 0.01f, 5.0f);
    flag("gtaoEnabled", out.gtaoEnabled);
    number("gtaoRadius", out.gtaoRadius, 0.01f, 2.0f);
    number("gtaoIntensity", out.gtaoIntensity, 0.0f, 1.0f);
    number("gtaoBias", out.gtaoBias, 0.0f, 0.5f);
    flag("contactShadowsEnabled", out.contactShadowsEnabled);
    number("contactShadowLength", out.contactShadowLength, 0.02f, 2.0f);
    number("contactShadowThickness", out.contactShadowThickness, 0.005f, 0.5f);
    number("contactShadowIntensity", out.contactShadowIntensity, 0.0f, 1.0f);
    if (const auto* v = object.Get("contactShadowSteps"); v && v->IsNumber() && std::isfinite(v->numVal))
        out.contactShadowSteps = static_cast<uint32_t>(std::clamp(v->numVal, 4.0, 32.0));
    flag("bloomEnabled", out.bloomEnabled);
    number("bloomIntensity", out.bloomIntensity, 0.0f, 4.0f);
    number("bloomThreshold", out.bloomThreshold, 0.0f, 16.0f);

    // Shadow quality (since 2026-11-21). Files without it take the values of the
    // quality level their other options match, so the level stays the same.
    bool hasShadowQuality = false;
    if (const auto* v = object.Get("shadowSunResolution"); v && v->IsNumber() && std::isfinite(v->numVal))
    {
        out.shadowSunResolution = ClampShadowQualityResolution(static_cast<uint32_t>(std::max(v->numVal, 0.0)));
        hasShadowQuality = true;
    }
    if (const auto* v = object.Get("shadowLocalResolution"); v && v->IsNumber() && std::isfinite(v->numVal))
    {
        out.shadowLocalResolution = ClampShadowQualityResolution(static_cast<uint32_t>(std::max(v->numVal, 0.0)));
        hasShadowQuality = true;
    }
    if (const auto* v = object.Get("shadowFilter"); v && v->IsNumber() && std::isfinite(v->numVal))
    {
        out.shadowFilter = static_cast<uint8_t>(std::clamp(v->numVal, 0.0, static_cast<double>(kMaxShadowQualityFilter)));
        hasShadowQuality = true;
    }
    if (!hasShadowQuality)
    {
        for (uint8_t i = 0; i < static_cast<uint8_t>(RenderQualityLevel::Custom); ++i)
        {
            const RenderQualityOptions preset = RenderQualityPreset(static_cast<RenderQualityLevel>(i));
            RenderQualityOptions current = GetRenderQualityOptions(out);
            current.sunShadowResolution = preset.sunShadowResolution;
            current.localShadowResolution = preset.localShadowResolution;
            current.shadowFilter = preset.shadowFilter;
            if (current == preset)
            {
                SetRenderQualityOptions(out, preset);
                break;
            }
        }
    }
}

// Render options from the "environment" block of a scene file written before
// they moved to the project. A missing key means the look before that option
// existed (Reinhard, everything off).
inline ProjectRenderSettings ReadLegacySceneRenderSettings(const serialization::JsonValue& sceneRoot)
{
    ProjectRenderSettings result{"reinhard", 1u, false, false, false, false, false, false};
    if (const auto* env = sceneRoot.Get("environment"); env && env->IsObject())
        ReadRenderSettingKeys(*env, result);
    return result;
}

inline int ProjectSettingInt(const serialization::JsonValue& object, const char* key,
                             int fallback, int min, int max)
{
    const auto* value = object.Get(key);
    if (!value || !value->IsNumber() || !std::isfinite(value->numVal))
        return fallback;
    return static_cast<int>(std::clamp(value->numVal, static_cast<double>(min), static_cast<double>(max)));
}

inline ProjectSettings ReadProjectSettings(const serialization::JsonValue& root)
{
    ProjectSettings result;
    const auto* settings = root.Get("settings");
    if (!settings || !settings->IsObject())
        return result;

    if (const auto* title = settings->Get("windowTitle"); title && title->IsString())
        result.windowTitle = title->AsString();
    result.width = ProjectSettingInt(*settings, "width", result.width, 320, 16384);
    result.height = ProjectSettingInt(*settings, "height", result.height, 200, 16384);
    if (const auto* value = settings->Get("useSystemResolution"); value && value->IsBool())
        result.useSystemResolution = value->AsBool();
    result.windowMode = static_cast<platform::WindowMode>(ProjectSettingInt(*settings, "windowMode", 0, 0, 3));
    if (const auto* vsync = settings->Get("vsync"); vsync && vsync->IsBool())
        result.vsync = vsync->AsBool();
    result.testBuild = ProjectSettingInt(*settings, "testBuild", 0, 0, 3);
    result.exportBuild = ProjectSettingInt(*settings, "exportBuild", 0, 0, 1);
    if (const auto* gravity = settings->Get("gravity"); gravity && gravity->IsArray() && gravity->arrayVal.size() == 3)
    {
        auto component = [&](size_t index, float fallback) {
            const auto& value = gravity->At(index);
            return value.IsNumber() && std::isfinite(value.numVal)
                ? static_cast<float>(std::clamp(value.numVal, -1000.0, 1000.0)) : fallback;
        };
        result.gravity = {component(0, 0.f), component(1, -9.81f), component(2, 0.f)};
    }
    if (const auto* masks = settings->Get("collisionMask"); masks && masks->IsArray())
    {
        for (size_t i = 0; i < std::min(masks->arrayVal.size(), result.collisionMask.size()); ++i)
        {
            const auto& value = masks->At(i);
            if (value.IsNumber() && value.numVal >= 0 && value.numVal <= UINT32_MAX)
                result.collisionMask[i] = value.AsUint();
        }
    }
    if (const auto* actions = settings->Get("actions"); actions && actions->IsArray())
    {
        for (const auto& action : actions->arrayVal)
        {
            const auto* name = action.Get("name");
            if (!name || !name->IsString() || name->AsString().empty() ||
                std::any_of(result.actions.begin(), result.actions.end(), [&](const auto& binding) {
                    return binding.name == name->AsString();
                }))
                continue;
            result.actions.push_back({name->AsString(),
                static_cast<platform::Key>(ProjectSettingInt(action, "key", 0, 0, static_cast<int>(platform::Key::Count) - 1)),
                static_cast<platform::GamepadButton>(ProjectSettingInt(action, "button", 0, 0, static_cast<int>(platform::GamepadButton::Count) - 1))});
        }
    }
    if (const auto* rendering = settings->Get("rendering"); rendering && rendering->IsObject())
    {
        ReadRenderSettingKeys(*rendering, result.rendering);
        result.renderingStored = true;
    }
    return result;
}

inline void WriteProjectSettings(serialization::JsonWriter& out, const ProjectSettings& settings)
{
    out.BeginObject("settings");
    out.WriteString("windowTitle", settings.windowTitle);
    out.WriteInt("width", settings.width);
    out.WriteInt("height", settings.height);
    out.WriteBool("useSystemResolution", settings.useSystemResolution);
    out.WriteInt("windowMode", static_cast<int>(settings.windowMode));
    out.WriteBool("vsync", settings.vsync);
    out.WriteInt("testBuild", settings.testBuild);
    out.WriteInt("exportBuild", settings.exportBuild);
    out.WriteVec3("gravity", settings.gravity);
    out.BeginArray("collisionMask");
    for (uint32_t mask : settings.collisionMask)
        out.WriteUint("", mask);
    out.EndArray();
    out.BeginArray("actions");
    for (const auto& action : settings.actions)
    {
        out.BeginObject();
        out.WriteString("name", action.name);
        out.WriteInt("key", static_cast<int>(action.key));
        out.WriteInt("button", static_cast<int>(action.button));
        out.EndObject();
    }
    out.EndArray();
    out.BeginObject("rendering");
    out.WriteString("tonemapOperator", settings.rendering.tonemapOperator);
    out.WriteUint("msaaSamples", settings.rendering.msaaSamples);
    out.WriteString("postAA", settings.rendering.postAA);
    out.WriteBool("brdfHeightCorrelated", settings.rendering.brdfHeightCorrelated);
    out.WriteBool("brdfPerLightFresnel", settings.rendering.brdfPerLightFresnel);
    out.WriteBool("brdfMultiScatter", settings.rendering.brdfMultiScatter);
    out.WriteBool("brdfBurleyDiffuse", settings.rendering.brdfBurleyDiffuse);
    out.WriteBool("specularAntiAliasing", settings.rendering.specularAntiAliasing);
    out.WriteBool("specularOcclusion", settings.rendering.specularOcclusion);
    out.WriteBool("ssrEnabled", settings.rendering.ssrEnabled);
    out.WriteFloat("ssrIntensity", settings.rendering.ssrIntensity);
    out.WriteFloat("ssrMaxRoughness", settings.rendering.ssrMaxRoughness);
    out.WriteFloat("ssrMaxDistance", settings.rendering.ssrMaxDistance);
    out.WriteFloat("ssrThickness", settings.rendering.ssrThickness);
    out.WriteBool("gtaoEnabled", settings.rendering.gtaoEnabled);
    out.WriteFloat("gtaoRadius", settings.rendering.gtaoRadius);
    out.WriteFloat("gtaoIntensity", settings.rendering.gtaoIntensity);
    out.WriteFloat("gtaoBias", settings.rendering.gtaoBias);
    out.WriteBool("contactShadowsEnabled", settings.rendering.contactShadowsEnabled);
    out.WriteFloat("contactShadowLength", settings.rendering.contactShadowLength);
    out.WriteFloat("contactShadowThickness", settings.rendering.contactShadowThickness);
    out.WriteFloat("contactShadowIntensity", settings.rendering.contactShadowIntensity);
    out.WriteUint("contactShadowSteps", settings.rendering.contactShadowSteps);
    out.WriteBool("bloomEnabled", settings.rendering.bloomEnabled);
    out.WriteFloat("bloomIntensity", settings.rendering.bloomIntensity);
    out.WriteFloat("bloomThreshold", settings.rendering.bloomThreshold);
    out.WriteUint("shadowSunResolution", settings.rendering.shadowSunResolution);
    out.WriteUint("shadowLocalResolution", settings.rendering.shadowLocalResolution);
    out.WriteUint("shadowFilter", settings.rendering.shadowFilter);
    out.EndObject();
    out.EndObject();
}

} // namespace engine
