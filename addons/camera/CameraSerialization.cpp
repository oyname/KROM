#include "addons/camera/CameraSerialization.hpp"

#include "addons/camera/CameraComponents.hpp"

namespace engine::addons::camera {

void RegisterCameraSerializationHandlers(serialization::SceneSerializer& serializer)
{
    serializer.RegisterSerializer<CameraComponent>([](serialization::JsonWriter& w, const CameraComponent& c) {
        w.BeginObject();
        w.WriteString("type", "CameraComponent");
        w.WriteUint("projection", static_cast<uint32_t>(c.projection));
        w.WriteFloat("fovYDeg", c.fovYDeg);
        w.WriteFloat("nearPlane", c.nearPlane);
        w.WriteFloat("farPlane", c.farPlane);
        w.WriteFloat("orthoSize", c.orthoSize);
        w.WriteFloat("aspectRatio", c.aspectRatio);
        w.WriteUint("renderLayer", c.renderLayer);
        w.WriteBool("isMain", c.isMainCamera);
        w.EndObject();
    });
}

void RegisterCameraDeserializationHandlers(serialization::SceneDeserializer& deserializer)
{
    deserializer.RegisterHandler<CameraComponent>([](const serialization::JsonValue& v, ecs::World& w, EntityID id) {
        CameraComponent cc{};
        if (const auto* p = v.Get("projection")) cc.projection = static_cast<ProjectionType>(p->AsUint());
        if (const auto* f = v.Get("fovYDeg")) cc.fovYDeg = f->AsFloat();
        if (const auto* n = v.Get("nearPlane")) cc.nearPlane = n->AsFloat();
        if (const auto* fa = v.Get("farPlane")) cc.farPlane = fa->AsFloat();
        if (const auto* o = v.Get("orthoSize")) cc.orthoSize = o->AsFloat();
        if (const auto* a = v.Get("aspectRatio")) cc.aspectRatio = a->AsFloat();
        if (const auto* l = v.Get("renderLayer")) cc.renderLayer = l->AsUint();
        if (const auto* m = v.Get("isMain")) cc.isMainCamera = m->AsBool();
        w.Add<CameraComponent>(id, cc);
    });
}

} // namespace engine::addons::camera
