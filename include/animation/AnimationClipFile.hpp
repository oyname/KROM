#pragma once
// =============================================================================
// KROM Engine - animation/AnimationClipFile.hpp
// Text format of an animation (.anim), a set of named clips:
//
//   kromanim 2
//   clip Wave
//   duration 2
//   looping 1
//   channel rotation linear 3 Arm_left      (property, interpolation, index, name)
//   k 0 0 0 0 1                             (time, then xyz or xyzw)
//
// Version 1 held a single clip and is still read ("name" instead of "clip").
// =============================================================================
#include "assets/AnimationClip.hpp"

#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>
#include <string>
#include <vector>

namespace engine::animation {

[[nodiscard]] inline std::string SerializeAnimationSet(const std::vector<assets::AnimationClip>& clips)
{
    using assets::AnimationInterpolation;
    using assets::AnimationTargetProperty;

    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(9);
    out << "kromanim 2\n";
    for (const assets::AnimationClip& clip : clips)
    {
    out << "clip " << clip.name << '\n';
    out << "duration " << clip.duration << '\n';
    out << "looping " << (clip.looping ? 1 : 0) << '\n';
    for (const assets::AnimationChannel& channel : clip.channels)
    {
        const char* property = channel.property == AnimationTargetProperty::Rotation ? "rotation"
            : channel.property == AnimationTargetProperty::Scale ? "scale" : "translation";
        const char* interpolation = channel.interpolation == AnimationInterpolation::Step ? "step" : "linear";
        out << "channel " << property << ' ' << interpolation << ' '
            << channel.targetIndex << ' ' << channel.targetName << '\n';
        for (size_t i = 0; i < channel.times.size(); ++i)
        {
            out << "k " << channel.times[i];
            if (channel.property == AnimationTargetProperty::Rotation)
            {
                const math::Quat& q = channel.quatValues[i];
                out << ' ' << q.x << ' ' << q.y << ' ' << q.z << ' ' << q.w << '\n';
            }
            else
            {
                const math::Vec3& v = channel.vec3Values[i];
                out << ' ' << v.x << ' ' << v.y << ' ' << v.z << '\n';
            }
        }
    }
    }
    return out.str();
}

[[nodiscard]] inline bool ParseAnimationSet(const std::string& source, std::vector<assets::AnimationClip>& outClips)
{
    using assets::AnimationInterpolation;
    using assets::AnimationTargetProperty;

    std::vector<assets::AnimationClip> clips;
    std::istringstream lines(source);
    std::string line;
    if (!std::getline(lines, line))
        return false;
    if (line.rfind("kromanim 1", 0) == 0)
        clips.emplace_back();
    else if (line.rfind("kromanim 2", 0) != 0)
        return false;

    while (std::getline(lines, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        std::istringstream in(line);
        in.imbue(std::locale::classic());
        std::string tag;
        if (!(in >> tag))
            continue;

        if (tag == "clip")
        {
            clips.emplace_back();
            std::getline(in >> std::ws, clips.back().name);
            continue;
        }
        if (clips.empty())
            return false;
        assets::AnimationClip& clip = clips.back();

        if (tag == "name")
        {
            std::getline(in >> std::ws, clip.name);
        }
        else if (tag == "duration")
        {
            if (!(in >> clip.duration) || !std::isfinite(clip.duration) || clip.duration < 0.f)
                return false;
        }
        else if (tag == "looping")
        {
            int looping = 0;
            if (!(in >> looping))
                return false;
            clip.looping = looping != 0;
        }
        else if (tag == "channel")
        {
            assets::AnimationChannel channel;
            std::string property, interpolation;
            if (!(in >> property >> interpolation >> channel.targetIndex))
                return false;
            if (property == "translation") channel.property = AnimationTargetProperty::Translation;
            else if (property == "rotation") channel.property = AnimationTargetProperty::Rotation;
            else if (property == "scale") channel.property = AnimationTargetProperty::Scale;
            else return false;
            if (interpolation == "linear") channel.interpolation = AnimationInterpolation::Linear;
            else if (interpolation == "step") channel.interpolation = AnimationInterpolation::Step;
            else return false;
            std::getline(in >> std::ws, channel.targetName);
            clip.channels.push_back(std::move(channel));
        }
        else if (tag == "k")
        {
            if (clip.channels.empty())
                return false;
            assets::AnimationChannel& channel = clip.channels.back();
            float time = 0.f;
            if (!(in >> time) || !std::isfinite(time) ||
                (!channel.times.empty() && time <= channel.times.back()))
                return false;
            if (channel.property == AnimationTargetProperty::Rotation)
            {
                math::Quat q;
                if (!(in >> q.x >> q.y >> q.z >> q.w))
                    return false;
                channel.quatValues.push_back(q);
            }
            else
            {
                math::Vec3 v;
                if (!(in >> v.x >> v.y >> v.z))
                    return false;
                channel.vec3Values.push_back(v);
            }
            channel.times.push_back(time);
        }
        else
        {
            return false;
        }
    }

    outClips = std::move(clips);
    return true;
}

} // namespace engine::animation
