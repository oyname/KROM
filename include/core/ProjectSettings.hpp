#pragma once

#include "platform/IInput.hpp"
#include "platform/IWindow.hpp"
#include "serialization/SceneSerializer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

namespace engine {

struct InputActionBinding
{
    std::string name;
    platform::Key key = platform::Key::Unknown;
    platform::GamepadButton button = platform::GamepadButton::None;
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
};

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
    out.EndObject();
}

} // namespace engine
