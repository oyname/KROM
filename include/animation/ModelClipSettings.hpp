#pragma once
// =============================================================================
// KROM Engine - animation/ModelClipSettings.hpp
// Clip settings of a model file (*.glb/*.gltf), kept beside it as
// "<model>.clips" (JSON). The model itself is never changed:
//
//   {
//     "kromclips": 1,
//     "fps": 24,
//     "clips": [
//       { "id": "3f9a01c2", "name": "Gallop", "source": "Gallop",
//         "loop": true, "rootMotion": true },
//       { "id": "b71e4d08", "name": "Attack_End", "source": "Attack",
//         "start": 30, "end": 55, "loop": false, "rootMotion": false,
//         "events": [ { "frame": 42, "name": "Hit", "value": 1, "text": "" } ] }
//     ]
//   }
//
// events      optional; "frame" counts in source frames like start/end
// source      name of the animation in the model (the Blender action name)
// start/end   optional frame range (at "fps") cut from the source
// id          stable: scenes reference a clip as "<model>#clip/<id>", so
//             renaming a clip breaks nothing
//
// Every animation of the model has one entry without a range (added by
// SyncModelClipSettings); cut clips are additional entries.
// =============================================================================
#include "animation/AnimationEvaluator.hpp"
#include "assets/AnimationClip.hpp"
#include "assets/SkeletonAsset.hpp"
#include "serialization/SceneSerializer.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace engine::animation {

inline constexpr const char* kModelClipSettingsExtension = ".clips";
inline constexpr const char* kModelClipReference = "#clip/";
inline constexpr float kDefaultModelClipFps = 24.f;

// A clip event as stored in the settings: at a frame of the source animation.
struct ModelClipEvent
{
    int         frame = 0;
    std::string name;
    float       value = 0.f;
    std::string text;

    bool operator==(const ModelClipEvent&) const = default;
};

struct ModelClipEntry
{
    std::string id;
    std::string name;
    std::string source;
    bool        hasRange   = false;
    int         startFrame = 0;
    int         endFrame   = 0;
    bool        looping    = true;
    bool        rootMotion = false;
    std::vector<ModelClipEvent> events;   // sorted by frame

    bool operator==(const ModelClipEntry&) const = default;
};

struct ModelClipSettings
{
    float                       fps = kDefaultModelClipFps;
    std::vector<ModelClipEntry> clips;

    bool operator==(const ModelClipSettings&) const = default;

    [[nodiscard]] const ModelClipEntry* Find(std::string_view id) const noexcept
    {
        for (const ModelClipEntry& entry : clips)
            if (entry.id == id)
                return &entry;
        return nullptr;
    }
    [[nodiscard]] ModelClipEntry* Find(std::string_view id) noexcept
    {
        for (ModelClipEntry& entry : clips)
            if (entry.id == id)
                return &entry;
        return nullptr;
    }
    [[nodiscard]] bool HasName(std::string_view name) const noexcept
    {
        return std::any_of(clips.begin(), clips.end(), [&](const ModelClipEntry& e) { return e.name == name; });
    }
    [[nodiscard]] std::string UniqueName(const std::string& base) const
    {
        if (!HasName(base))
            return base;
        for (int i = 2;; ++i)
        {
            std::string candidate = base + " " + std::to_string(i);
            if (!HasName(candidate))
                return candidate;
        }
    }
    [[nodiscard]] std::string NewId() const
    {
        static std::mt19937 generator{std::random_device{}()};
        for (;;)
        {
            char buffer[9];
            std::snprintf(buffer, sizeof(buffer), "%08x", static_cast<unsigned>(generator()));
            if (!Find(buffer))
                return buffer;
        }
    }
};

// "Models/Horse.glb" -> "Models/Horse.glb.clips"
[[nodiscard]] inline std::string ModelClipSettingsPath(const std::string& modelPath)
{
    return modelPath + kModelClipSettingsExtension;
}

// "Models/Horse.glb#clip/3f9a01c2"
[[nodiscard]] inline std::string ModelClipAssetPath(const std::string& modelPath, const std::string& id)
{
    return modelPath + kModelClipReference + id;
}

// Splits "<model>#clip/<id>"; false for any other reference.
[[nodiscard]] inline bool SplitModelClipAssetPath(const std::string& assetPath, std::string& modelPath, std::string& id)
{
    const size_t pos = assetPath.rfind(kModelClipReference);
    if (pos == std::string::npos || pos == 0u)
        return false;
    modelPath = assetPath.substr(0, pos);
    id = assetPath.substr(pos + std::char_traits<char>::length(kModelClipReference));
    return !id.empty();
}

[[nodiscard]] inline bool IsModelFilePath(std::string_view path)
{
    const auto endsWith = [&](std::string_view ext) {
        if (path.size() < ext.size())
            return false;
        for (size_t i = 0; i < ext.size(); ++i)
            if (std::tolower(static_cast<unsigned char>(path[path.size() - ext.size() + i])) != ext[i])
                return false;
        return true;
    };
    return endsWith(".glb") || endsWith(".gltf");
}

[[nodiscard]] inline std::string SerializeModelClipSettings(const ModelClipSettings& settings)
{
    serialization::JsonWriter out;
    out.BeginObject();
    out.WriteInt("kromclips", 1);
    out.WriteFloat("fps", settings.fps);
    out.BeginArray("clips");
    for (const ModelClipEntry& entry : settings.clips)
    {
        out.BeginObject();
        out.WriteString("id", entry.id);
        out.WriteString("name", entry.name);
        out.WriteString("source", entry.source);
        if (entry.hasRange)
        {
            out.WriteInt("start", entry.startFrame);
            out.WriteInt("end", entry.endFrame);
        }
        out.WriteBool("loop", entry.looping);
        out.WriteBool("rootMotion", entry.rootMotion);
        if (!entry.events.empty())
        {
            out.BeginArray("events");
            for (const ModelClipEvent& event : entry.events)
            {
                out.BeginObject();
                out.WriteInt("frame", event.frame);
                out.WriteString("name", event.name);
                out.WriteFloat("value", event.value);
                if (!event.text.empty())
                    out.WriteString("text", event.text);
                out.EndObject();
            }
            out.EndArray();
        }
        out.EndObject();
    }
    out.EndArray();
    out.EndObject();
    return out.GetString();
}

// Entries without id or source are dropped; a duplicate id gets a new one.
[[nodiscard]] inline bool ParseModelClipSettings(const std::string& json, ModelClipSettings& out,
                                                 std::string* error = nullptr)
{
    std::string parseError;
    const serialization::JsonValue root = serialization::JsonParser::Parse(json, parseError);
    const auto fail = [&](const std::string& message) {
        if (error)
            *error = message;
        return false;
    };
    if (!parseError.empty() || !root.IsObject())
        return fail(parseError.empty() ? "not a JSON object" : parseError);
    const auto* version = root.Get("kromclips");
    if (!version || !version->IsNumber())
        return fail("missing \"kromclips\"");
    if (version->AsInt() != 1)
        return fail("unsupported version " + std::to_string(version->AsInt()));

    ModelClipSettings result;
    if (const auto* fps = root.Get("fps"); fps && fps->IsNumber() && std::isfinite(fps->numVal) &&
                                           fps->numVal >= 1.0 && fps->numVal <= 1000.0)
        result.fps = fps->AsFloat();
    if (const auto* clips = root.Get("clips"); clips && clips->IsArray())
        for (const serialization::JsonValue& item : clips->arrayVal)
        {
            const auto* id = item.Get("id");
            const auto* source = item.Get("source");
            if (!id || !id->IsString() || id->AsString().empty() || !source || !source->IsString())
                continue;
            ModelClipEntry entry;
            entry.id = id->AsString();
            entry.source = source->AsString();
            if (const auto* name = item.Get("name"); name && name->IsString() && !name->AsString().empty())
                entry.name = name->AsString();
            else
                entry.name = entry.source;
            const auto* start = item.Get("start");
            const auto* end = item.Get("end");
            if (start && end && start->IsNumber() && end->IsNumber())
            {
                entry.hasRange = true;
                entry.startFrame = std::max(0, start->AsInt());
                entry.endFrame = std::max(entry.startFrame, end->AsInt());
            }
            if (const auto* loop = item.Get("loop"); loop && loop->IsBool())
                entry.looping = loop->AsBool();
            if (const auto* rootMotion = item.Get("rootMotion"); rootMotion && rootMotion->IsBool())
                entry.rootMotion = rootMotion->AsBool();
            if (const auto* events = item.Get("events"); events && events->IsArray())
            {
                for (const serialization::JsonValue& e : events->arrayVal)
                {
                    const auto* frame = e.Get("frame");
                    const auto* eventName = e.Get("name");
                    if (!frame || !frame->IsNumber() || !eventName || !eventName->IsString() || eventName->AsString().empty())
                        continue;
                    ModelClipEvent event;
                    event.frame = std::max(0, frame->AsInt());
                    event.name = eventName->AsString();
                    if (const auto* value = e.Get("value"); value && value->IsNumber() && std::isfinite(value->numVal))
                        event.value = value->AsFloat();
                    if (const auto* text = e.Get("text"); text && text->IsString())
                        event.text = text->AsString();
                    entry.events.push_back(std::move(event));
                }
                std::stable_sort(entry.events.begin(), entry.events.end(),
                                 [](const ModelClipEvent& a, const ModelClipEvent& b) { return a.frame < b.frame; });
            }
            if (result.Find(entry.id))
                entry.id = result.NewId();
            result.clips.push_back(std::move(entry));
        }
    out = std::move(result);
    return true;
}

// Adds an entry (whole clip, no range) for every model animation that has
// none yet. Returns true when something was added.
inline bool SyncModelClipSettings(ModelClipSettings& settings, const std::vector<std::string>& sourceNames)
{
    bool changed = false;
    for (const std::string& source : sourceNames)
    {
        const bool covered = std::any_of(settings.clips.begin(), settings.clips.end(), [&](const ModelClipEntry& e) {
            return !e.hasRange && e.source == source;
        });
        if (covered)
            continue;
        ModelClipEntry entry;
        entry.id = settings.NewId();
        entry.name = settings.UniqueName(source);
        entry.source = source;
        settings.clips.push_back(std::move(entry));
        changed = true;
    }
    return changed;
}

// Part [start, end] (seconds) of a clip, shifted to begin at 0. Keys inside the
// range are kept, the borders are sampled. Cubic-spline channels become linear
// (values at the source keys), because their tangents depend on the key spacing.
[[nodiscard]] inline assets::AnimationClip SliceAnimationClip(const assets::AnimationClip& source, float start, float end)
{
    using assets::AnimationInterpolation;
    using assets::AnimationTargetProperty;

    start = std::clamp(start, 0.f, source.duration);
    end = std::clamp(end, start, source.duration);
    constexpr float kEpsilon = 1e-5f;

    assets::AnimationClip result;
    result.name = source.name;
    result.looping = source.looping;
    result.rootMotion = source.rootMotion;
    result.duration = end - start;
    for (const assets::AnimationChannel& from : source.channels)
    {
        if (from.times.empty())
            continue;
        assets::AnimationChannel channel;
        channel.targetName = from.targetName;
        channel.targetIndex = from.targetIndex;
        channel.property = from.property;
        channel.interpolation = from.interpolation == AnimationInterpolation::Step ? AnimationInterpolation::Step
                                                                                     : AnimationInterpolation::Linear;
        const bool rotation = from.property == AnimationTargetProperty::Rotation;
        const auto addSample = [&](float time) {
            math::Vec3 translation{}, scale{1.f, 1.f, 1.f};
            math::Quat rotationValue = math::Quat::Identity();
            if (!EvaluateChannel(from, time, translation, rotationValue, scale))
                return;
            channel.times.push_back(time - start);
            if (rotation)
                channel.quatValues.push_back(rotationValue);
            else
                channel.vec3Values.push_back(from.property == AnimationTargetProperty::Scale ? scale : translation);
        };
        addSample(start);
        for (const float time : from.times)
            if (time > start + kEpsilon && time < end - kEpsilon)
                addSample(time);
        if (end > start + kEpsilon)
            addSample(end);
        result.channels.push_back(std::move(channel));
    }
    return result;
}

// The clip an entry stands for, built from the model's animations; nullopt
// (with a reason) when its source is missing.
[[nodiscard]] inline std::optional<assets::AnimationClip> BuildModelClip(
    const ModelClipEntry& entry, float fps, const std::vector<const assets::AnimationClip*>& sources,
    std::string* error = nullptr)
{
    const assets::AnimationClip* source = nullptr;
    for (const assets::AnimationClip* candidate : sources)
        if (candidate && candidate->name == entry.source)
        {
            source = candidate;
            break;
        }
    if (!source)
    {
        if (error)
            *error = "source animation '" + entry.source + "' is missing";
        return std::nullopt;
    }
    assets::AnimationClip clip = entry.hasRange
        ? SliceAnimationClip(*source, static_cast<float>(entry.startFrame) / fps, static_cast<float>(entry.endFrame) / fps)
        : *source;
    clip.name = entry.name;
    clip.looping = entry.looping;
    clip.rootMotion = entry.rootMotion;
    // Events: source frames -> seconds from the clip start; outside the range they are dropped.
    clip.events.clear();
    const int firstFrame = entry.hasRange ? entry.startFrame : 0;
    for (const ModelClipEvent& event : entry.events)
    {
        const float time = static_cast<float>(event.frame - firstFrame) / fps;
        if (time < -1e-4f || time > clip.duration + 1e-4f)
            continue;
        clip.events.push_back({std::clamp(time, 0.f, clip.duration), event.name, event.value, event.text});
    }
    return clip;
}

// First channel target that does not exist in the skeleton (bone index out of
// range or a different bone name there); empty when the clip fits.
[[nodiscard]] inline std::string FindSkeletonMismatch(const assets::AnimationClip& clip, const assets::SkeletonAsset& skeleton)
{
    for (const assets::AnimationChannel& channel : clip.channels)
    {
        if (channel.targetIndex < 0 || static_cast<size_t>(channel.targetIndex) >= skeleton.bones.size())
            return channel.targetName.empty() ? "#" + std::to_string(channel.targetIndex) : channel.targetName;
        const std::string& bone = skeleton.bones[static_cast<size_t>(channel.targetIndex)].name;
        if (!channel.targetName.empty() && !bone.empty() && bone != channel.targetName)
            return channel.targetName;
    }
    return {};
}

} // namespace engine::animation
