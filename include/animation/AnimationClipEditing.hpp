#pragma once
// =============================================================================
// KROM Engine - animation/AnimationClipEditing.hpp
// Keyframe operations for editor-authored clips (Linear/Step channels, one value
// per key). Keys of a channel stay sorted and unique in time.
// =============================================================================
#include "assets/AnimationClip.hpp"

#include <algorithm>
#include <cmath>
#include <string_view>
#include <vector>

namespace engine::animation {

inline constexpr float kKeyTimeEpsilon = 1e-4f;

struct KeyRef
{
    int channel = -1;
    int key     = -1;
    [[nodiscard]] bool operator==(const KeyRef&) const noexcept = default;
};

[[nodiscard]] inline bool IsRotationChannel(const assets::AnimationChannel& channel) noexcept
{
    return channel.property == assets::AnimationTargetProperty::Rotation;
}

[[nodiscard]] inline int FindKey(const assets::AnimationChannel& channel, float time) noexcept
{
    for (size_t i = 0; i < channel.times.size(); ++i)
        if (std::fabs(channel.times[i] - time) <= kKeyTimeEpsilon)
            return static_cast<int>(i);
    return -1;
}

[[nodiscard]] inline int FindChannel(const assets::AnimationClip& clip, std::string_view target,
                                     assets::AnimationTargetProperty property) noexcept
{
    for (size_t i = 0; i < clip.channels.size(); ++i)
        if (clip.channels[i].property == property && clip.channels[i].targetName == target)
            return static_cast<int>(i);
    return -1;
}

// Inserts a key, or replaces the value of a key at the same time.
inline void SetKey(assets::AnimationChannel& channel, float time, const math::Vec3& vec, const math::Quat& quat)
{
    const bool rotation = IsRotationChannel(channel);
    if (const int existing = FindKey(channel, time); existing >= 0)
    {
        if (rotation) channel.quatValues[static_cast<size_t>(existing)] = quat;
        else          channel.vec3Values[static_cast<size_t>(existing)] = vec;
        return;
    }
    const auto at = std::lower_bound(channel.times.begin(), channel.times.end(), time);
    const auto index = at - channel.times.begin();
    channel.times.insert(at, time);
    if (rotation) channel.quatValues.insert(channel.quatValues.begin() + index, quat);
    else          channel.vec3Values.insert(channel.vec3Values.begin() + index, vec);
}

inline void RemoveKey(assets::AnimationChannel& channel, int key)
{
    channel.times.erase(channel.times.begin() + key);
    if (IsRotationChannel(channel)) channel.quatValues.erase(channel.quatValues.begin() + key);
    else                            channel.vec3Values.erase(channel.vec3Values.begin() + key);
}

// Removes the keys and every channel left without keys.
inline void DeleteKeys(assets::AnimationClip& clip, std::vector<KeyRef> keys)
{
    std::sort(keys.begin(), keys.end(), [](const KeyRef& a, const KeyRef& b) {
        return a.channel != b.channel ? a.channel < b.channel : a.key > b.key;
    });
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    for (const KeyRef& ref : keys)
        RemoveKey(clip.channels[static_cast<size_t>(ref.channel)], ref.key);
    clip.channels.erase(std::remove_if(clip.channels.begin(), clip.channels.end(),
        [](const assets::AnimationChannel& channel) { return channel.times.empty(); }), clip.channels.end());
}

// Shifts keys in time (never below 0). A moved key replaces a key it lands on.
// Returns the references of the moved keys after the move.
[[nodiscard]] inline std::vector<KeyRef> MoveKeys(assets::AnimationClip& clip, std::vector<KeyRef> keys, float delta)
{
    struct Moved { int channel; float time; math::Vec3 vec; math::Quat quat; };
    std::sort(keys.begin(), keys.end(), [](const KeyRef& a, const KeyRef& b) {
        return a.channel != b.channel ? a.channel < b.channel : a.key > b.key;
    });
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());

    std::vector<Moved> moved;
    moved.reserve(keys.size());
    for (const KeyRef& ref : keys)
    {
        assets::AnimationChannel& channel = clip.channels[static_cast<size_t>(ref.channel)];
        const size_t key = static_cast<size_t>(ref.key);
        Moved entry{ref.channel, std::max(0.f, channel.times[key] + delta), {}, {}};
        if (IsRotationChannel(channel)) entry.quat = channel.quatValues[key];
        else                            entry.vec  = channel.vec3Values[key];
        moved.push_back(entry);
        RemoveKey(channel, ref.key);
    }
    for (const Moved& entry : moved)
        SetKey(clip.channels[static_cast<size_t>(entry.channel)], entry.time, entry.vec, entry.quat);

    std::vector<KeyRef> result;
    result.reserve(moved.size());
    for (const Moved& entry : moved)
        result.push_back({entry.channel, FindKey(clip.channels[static_cast<size_t>(entry.channel)], entry.time)});
    return result;
}

[[nodiscard]] inline float LastKeyTime(const assets::AnimationClip& clip) noexcept
{
    float last = 0.f;
    for (const assets::AnimationChannel& channel : clip.channels)
        if (!channel.times.empty())
            last = std::max(last, channel.times.back());
    return last;
}

} // namespace engine::animation
