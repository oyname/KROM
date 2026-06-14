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
        w.WriteUint("cullingMask", c.cullingMask);
        w.WriteBool("isMain", c.isMainCamera);
        w.WriteUint("backgroundMode", static_cast<uint32_t>(c.backgroundMode));
        w.BeginArray("clearColor");
        for (float v : c.clearColor) w.WriteFloat("", v);
        w.EndArray();
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
        if (const auto* cm = v.Get("cullingMask")) cc.cullingMask = cm->AsUint();
        cc.cullingMask &= ~renderer::LAYER_EDITOR_GIZMO;
        if (const auto* m  = v.Get("isMain"))            cc.isMainCamera    = m->AsBool();
        if (const auto* bm = v.Get("backgroundMode"))    cc.backgroundMode  = static_cast<BackgroundMode>(bm->AsUint());
        // "clearColor" ist der aktuelle Key; "solidColor" wird als Fallback fuer aeltere Szenen gelesen.
        const auto* colorNode = v.Get("clearColor");
        if (!colorNode) colorNode = v.Get("solidColor");
        if (colorNode && colorNode->IsArray() && colorNode->arrayVal.size() >= 4)
            for (size_t i = 0; i < 4; ++i) cc.clearColor[i] = colorNode->At(i).AsFloat();
        w.Add<CameraComponent>(id, cc);
    });
}

void UnregisterCameraSerializationHandlers(serialization::SceneSerializer& serializer)
{
    serializer.UnregisterSerializer<CameraComponent>();
}

void UnregisterCameraDeserializationHandlers(serialization::SceneDeserializer& deserializer)
{
    deserializer.UnregisterHandler("CameraComponent");
}

void RegisterCameraAddon(ecs::ComponentMetaRegistry* components,
                          serialization::SceneSerializer* serializer,
                          serialization::SceneDeserializer* deserializer)
{
    if (components)  RegisterCameraComponents(*components);
    if (serializer)  RegisterCameraSerializationHandlers(*serializer);
    if (deserializer) RegisterCameraDeserializationHandlers(*deserializer);
}

void UnregisterCameraAddon(ecs::ComponentMetaRegistry* components,
                            serialization::SceneSerializer* serializer,
                            serialization::SceneDeserializer* deserializer)
{
    if (deserializer) UnregisterCameraDeserializationHandlers(*deserializer);
    if (serializer)  UnregisterCameraSerializationHandlers(*serializer);
    if (components)  components->Unregister<CameraComponent>();
}

} // namespace engine::addons::camera
