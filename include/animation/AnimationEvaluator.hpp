#pragma once
// =============================================================================
// KROM Engine - animation/AnimationEvaluator.hpp
// Pure sampling math: no ECS, no importer, no renderer, no backend.
// =============================================================================
#include "assets/AnimationClip.hpp"
#include "core/Math.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <vector>

namespace engine::animation {

[[nodiscard]] inline size_t FindKeyframeIndex(
    const std::vector<float>& times, float t) noexcept
{
    assert(!times.empty());
    const size_t n = times.size();
    if (n == 1u || t <= times[0]) return 0u;
    if (t >= times[n - 1u]) return n - 2u;

    const auto it = std::upper_bound(times.begin(), times.end(), t);
    return static_cast<size_t>(it - times.begin()) - 1u;
}

[[nodiscard]] inline float KeyframeAlpha(
    const std::vector<float>& times, size_t i, float t) noexcept
{
    const float dt = times[i + 1u] - times[i];
    return dt > 1e-7f ? (t - times[i]) / dt : 0.f;
}

[[nodiscard]] inline math::Vec3 CubicHermite(
    const math::Vec3& p0, const math::Vec3& m0,
    const math::Vec3& p1, const math::Vec3& m1,
    float t) noexcept
{
    const float t2  = t * t;
    const float t3  = t2 * t;
    const float h00 =  2.f * t3 - 3.f * t2 + 1.f;
    const float h10 =        t3 - 2.f * t2 + t;
    const float h01 = -2.f * t3 + 3.f * t2;
    const float h11 =        t3 -       t2;
    return p0 * h00 + m0 * h10 + p1 * h01 + m1 * h11;
}

[[nodiscard]] inline math::Quat CubicHermite(
    const math::Quat& p0, const math::Quat& m0,
    const math::Quat& p1, const math::Quat& m1,
    float t) noexcept
{
    const float t2  = t * t;
    const float t3  = t2 * t;
    const float h00 =  2.f * t3 - 3.f * t2 + 1.f;
    const float h10 =        t3 - 2.f * t2 + t;
    const float h01 = -2.f * t3 + 3.f * t2;
    const float h11 =        t3 -       t2;
    return math::Quat{
        p0.x * h00 + m0.x * h10 + p1.x * h01 + m1.x * h11,
        p0.y * h00 + m0.y * h10 + p1.y * h01 + m1.y * h11,
        p0.z * h00 + m0.z * h10 + p1.z * h01 + m1.z * h11,
        p0.w * h00 + m0.w * h10 + p1.w * h01 + m1.w * h11,
    }.Normalized();
}

[[nodiscard]] inline math::Vec3 SampleVec3(
    const std::vector<float>&      times,
    const std::vector<math::Vec3>& values,
    assets::AnimationInterpolation interpolation,
    float t) noexcept
{
    assert(!times.empty() && !values.empty());

    if (times.size() == 1u)
        return values[0];

    const size_t i = FindKeyframeIndex(times, t);

    switch (interpolation)
    {
    case assets::AnimationInterpolation::Step:
        return values[i];

    case assets::AnimationInterpolation::Linear:
    {
        const float alpha = KeyframeAlpha(times, i, t);
        return math::Vec3::Lerp(values[i], values[i + 1u], alpha);
    }

    case assets::AnimationInterpolation::CubicSpline:
    {
        const float dt = times[i + 1u] - times[i];
        const float alpha = KeyframeAlpha(times, i, t);
        const math::Vec3& v0 = values[3u * i + 1u];
        const math::Vec3  m0 = values[3u * i + 2u] * dt;
        const math::Vec3& v1 = values[3u * (i + 1u) + 1u];
        const math::Vec3  m1 = values[3u * (i + 1u)] * dt;
        return CubicHermite(v0, m0, v1, m1, alpha);
    }
    }
    return values[0];
}

[[nodiscard]] inline math::Quat SampleQuat(
    const std::vector<float>&      times,
    const std::vector<math::Quat>& values,
    assets::AnimationInterpolation interpolation,
    float t) noexcept
{
    assert(!times.empty() && !values.empty());

    if (times.size() == 1u)
        return values[0];

    const size_t i = FindKeyframeIndex(times, t);

    switch (interpolation)
    {
    case assets::AnimationInterpolation::Step:
        return values[i];

    case assets::AnimationInterpolation::Linear:
    {
        const float alpha = KeyframeAlpha(times, i, t);
        return math::Quat::Slerp(values[i], values[i + 1u], alpha);
    }

    case assets::AnimationInterpolation::CubicSpline:
    {
        const float dt = times[i + 1u] - times[i];
        const float alpha = KeyframeAlpha(times, i, t);
        const math::Quat& v0 = values[3u * i + 1u];
        const math::Quat  m0 = {
            values[3u * i + 2u].x * dt,
            values[3u * i + 2u].y * dt,
            values[3u * i + 2u].z * dt,
            values[3u * i + 2u].w * dt,
        };
        const math::Quat& v1 = values[3u * (i + 1u) + 1u];
        const math::Quat  m1 = {
            values[3u * (i + 1u)].x * dt,
            values[3u * (i + 1u)].y * dt,
            values[3u * (i + 1u)].z * dt,
            values[3u * (i + 1u)].w * dt,
        };
        return CubicHermite(v0, m0, v1, m1, alpha);
    }
    }
    return values[0];
}

[[nodiscard]] inline float NormalizeTime(
    float time, float duration, bool looping) noexcept
{
    if (duration <= 0.f)
        return 0.f;

    if (looping)
    {
        float wrapped = std::fmod(time, duration);
        if (wrapped < 0.f) wrapped += duration;
        return wrapped;
    }

    return time < 0.f ? 0.f : (time > duration ? duration : time);
}

inline bool EvaluateChannel(
    const assets::AnimationChannel& ch,
    float t,
    math::Vec3& outTranslation,
    math::Quat& outRotation,
    math::Vec3& outScale) noexcept
{
    if (ch.times.empty()) return false;

    switch (ch.property)
    {
    case assets::AnimationTargetProperty::Translation:
        if (ch.vec3Values.empty()) return false;
        outTranslation = SampleVec3(ch.times, ch.vec3Values, ch.interpolation, t);
        return true;

    case assets::AnimationTargetProperty::Rotation:
        if (ch.quatValues.empty()) return false;
        outRotation = SampleQuat(ch.times, ch.quatValues, ch.interpolation, t);
        return true;

    case assets::AnimationTargetProperty::Scale:
        if (ch.vec3Values.empty()) return false;
        outScale = SampleVec3(ch.times, ch.vec3Values, ch.interpolation, t);
        return true;
    }
    return false;
}

} // namespace engine::animation
