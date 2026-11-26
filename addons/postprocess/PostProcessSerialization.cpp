#include "addons/postprocess/PostProcessSerialization.hpp"

#include "addons/forward/ForwardTonemapJson.hpp"

#include <algorithm>
#include <cstring>

namespace engine::addons::postprocess {
namespace {

using renderer::addons::forward::kMaxExposureEv;
using renderer::addons::forward::kMinExposureEv;

PostProcessOverride OverrideAt(uint32_t index) noexcept
{
    return static_cast<PostProcessOverride>(1u << index);
}

} // namespace

void WritePostProcessVolumeFields(serialization::JsonWriter& w, const PostProcessVolumeComponent& c)
{
    w.WriteBool("global", c.global);
    w.WriteFloat("weight", c.weight);
    w.WriteInt("priority", c.priority);
    w.WriteVec3("extents", c.extents);
    w.WriteVec3("boxOffset", c.boxOffset);
    w.WriteFloat("blendDistance", c.blendDistance);
    w.BeginArray("overrides");
    for (uint32_t i = 0u; i < kPostProcessOverrideCount; ++i)
        if (c.Overrides(OverrideAt(i)))
            w.WriteString("", PostProcessOverrideName(OverrideAt(i)));
    w.EndArray();
    w.WriteFloat("exposureEv", c.exposureEv);
    w.WriteFloat("autoExposureMinEv", c.autoExposureMinEv);
    w.WriteFloat("autoExposureMaxEv", c.autoExposureMaxEv);
    // Same object as the scene's colour grading; not written when neutral.
    renderer::addons::forward::WriteSceneColorGradingJson(w, c.grading);
    w.WriteFloat("bloomIntensity", c.bloomIntensity);
    w.WriteFloat("bloomThreshold", c.bloomThreshold);
    w.WriteFloat("gtaoIntensity", c.gtaoIntensity);
    w.WriteFloat("ssrIntensity", c.ssrIntensity);
    w.WriteFloat("contactShadowIntensity", c.contactShadowIntensity);
}

PostProcessVolumeComponent ReadPostProcessVolumeFields(const serialization::JsonValue& v)
{
    PostProcessVolumeComponent c{};
    const auto readFloat = [&v](const char* key, float& out) {
        if (const auto* value = v.Get(key); value && value->IsNumber())
            out = value->AsFloat();
    };
    if (const auto* b = v.Get("global"); b && b->IsBool()) c.global = b->AsBool();
    readFloat("weight", c.weight);
    if (const auto* p = v.Get("priority"); p && p->IsNumber()) c.priority = p->AsInt();
    if (const auto* e = v.Get("extents")) c.extents = e->AsVec3();
    if (const auto* o = v.Get("boxOffset")) c.boxOffset = o->AsVec3();
    readFloat("blendDistance", c.blendDistance);
    if (const auto* list = v.Get("overrides"); list && list->IsArray())
    {
        for (size_t i = 0u; i < list->arrayVal.size(); ++i)
        {
            const auto& name = list->At(i);
            if (!name.IsString())
                continue;
            for (uint32_t bit = 0u; bit < kPostProcessOverrideCount; ++bit)
                if (name.AsString() == PostProcessOverrideName(OverrideAt(bit)))
                    c.SetOverride(OverrideAt(bit), true);
        }
    }
    readFloat("exposureEv", c.exposureEv);
    readFloat("autoExposureMinEv", c.autoExposureMinEv);
    readFloat("autoExposureMaxEv", c.autoExposureMaxEv);
    renderer::addons::forward::ReadSceneColorGradingJson(v, c.grading);
    readFloat("bloomIntensity", c.bloomIntensity);
    readFloat("bloomThreshold", c.bloomThreshold);
    readFloat("gtaoIntensity", c.gtaoIntensity);
    readFloat("ssrIntensity", c.ssrIntensity);
    readFloat("contactShadowIntensity", c.contactShadowIntensity);

    c.weight = std::clamp(c.weight, 0.f, 1.f);
    c.extents.x = std::max(c.extents.x, 0.05f);
    c.extents.y = std::max(c.extents.y, 0.05f);
    c.extents.z = std::max(c.extents.z, 0.05f);
    c.blendDistance = std::max(c.blendDistance, 0.f);
    c.exposureEv = std::clamp(c.exposureEv, kMinExposureEv, kMaxExposureEv);
    c.autoExposureMinEv = std::clamp(c.autoExposureMinEv, kMinExposureEv, kMaxExposureEv);
    c.autoExposureMaxEv = std::clamp(c.autoExposureMaxEv, c.autoExposureMinEv, kMaxExposureEv);
    c.bloomIntensity = std::clamp(c.bloomIntensity, 0.f, 4.f);
    c.bloomThreshold = std::clamp(c.bloomThreshold, 0.f, 16.f);
    c.gtaoIntensity = std::clamp(c.gtaoIntensity, 0.f, 2.f);
    c.ssrIntensity = std::clamp(c.ssrIntensity, 0.f, 1.f);
    c.contactShadowIntensity = std::clamp(c.contactShadowIntensity, 0.f, 1.f);
    return c;
}

void RegisterPostProcessSerializationHandlers(serialization::SceneSerializer& serializer)
{
    serializer.RegisterSerializer<PostProcessVolumeComponent>(
        [](serialization::JsonWriter& w, const PostProcessVolumeComponent& c) {
            w.BeginObject();
            w.WriteString("type", "PostProcessVolumeComponent");
            WritePostProcessVolumeFields(w, c);
            w.EndObject();
        });
}

void UnregisterPostProcessSerializationHandlers(serialization::SceneSerializer& serializer)
{
    serializer.UnregisterSerializer<PostProcessVolumeComponent>();
}

void RegisterPostProcessDeserializationHandlers(serialization::SceneDeserializer& deserializer)
{
    deserializer.RegisterHandler("PostProcessVolumeComponent",
        [](const serialization::JsonValue& v, ecs::World& w, EntityID id,
           const std::unordered_map<uint32_t, EntityID>&) {
            w.SetComponent<PostProcessVolumeComponent>(id, ReadPostProcessVolumeFields(v));
        });
}

void UnregisterPostProcessDeserializationHandlers(serialization::SceneDeserializer& deserializer)
{
    deserializer.UnregisterHandler("PostProcessVolumeComponent");
}

} // namespace engine::addons::postprocess
