// =============================================================================
// KROM Engine - src/serialization/SceneSerializer.cpp
// BUGFIX: ForEachAlive statt View<NameComponent>
// NEU: JsonParser + SceneDeserializer
// =============================================================================
#include "serialization/SceneSerializer.hpp"
#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstring>

namespace engine::serialization {

// =============================================================================
// JsonWriter
// =============================================================================

static std::string FloatStr(float v)
{
    char buf[32];
    if (v == static_cast<float>(static_cast<int>(v)))
        std::snprintf(buf, sizeof(buf), "%g", static_cast<double>(v));
    else
        std::snprintf(buf, sizeof(buf), "%.6g", static_cast<double>(v));
    return buf;
}

std::string JsonWriter::FormatFloat(float value) const
{
    if (!m_roundTripFloats)
        return FloatStr(value);
    char buffer[32];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::general);
    return {buffer, result.ptr};
}

std::string JsonWriter::Escape(const std::string& s)
{
    std::string r; r.reserve(s.size());
    for (char c : s) {
        if      (c == '"')  r += "\\\"";
        else if (c == '\\') r += "\\\\";
        else if (c == '\n') r += "\\n";
        else if (c == '\r') r += "\\r";
        else if (c == '\t') r += "\\t";
        else                r += c;
    }
    return r;
}

void JsonWriter::Indent()
{ m_buf += '\n'; for (size_t i=0;i<m_stack.size();++i) m_buf += "  "; }

void JsonWriter::Prefix(const std::string& key)
{
    if (!m_stack.empty()) { Frame& f=m_stack.back(); if(f.hadItem) m_buf+=','; f.hadItem=true; }
    Indent();
    if (!key.empty()) { m_buf+='"'; m_buf+=Escape(key); m_buf+="\": "; }
}

void JsonWriter::BeginObject(const std::string& key)
{ Prefix(key); m_buf+='{'; m_stack.push_back({false,false}); }
void JsonWriter::EndObject()
{ m_stack.pop_back(); Indent(); m_buf+='}'; }
void JsonWriter::BeginArray(const std::string& key)
{ Prefix(key); m_buf+='['; m_stack.push_back({true,false}); }
void JsonWriter::EndArray()
{ m_stack.pop_back(); Indent(); m_buf+=']'; }

void JsonWriter::WriteString(const std::string& k,const std::string& v)
{ Prefix(k); m_buf+='"'; m_buf+=Escape(v); m_buf+='"'; }
void JsonWriter::WriteFloat(const std::string& k,float v)
{ Prefix(k); m_buf+=FormatFloat(v); }
void JsonWriter::WriteUint(const std::string& k,uint32_t v)
{ Prefix(k); char b[16]; std::snprintf(b,sizeof(b),"%u",v); m_buf+=b; }
void JsonWriter::WriteInt(const std::string& k,int32_t v)
{ Prefix(k); char b[16]; std::snprintf(b,sizeof(b),"%d",v); m_buf+=b; }
void JsonWriter::WriteBool(const std::string& k,bool v)
{ Prefix(k); m_buf+=(v?"true":"false"); }
void JsonWriter::WriteVec3(const std::string& k,const math::Vec3& v)
{ Prefix(k); m_buf+='['; m_buf+=FormatFloat(v.x)+", "+FormatFloat(v.y)+", "+FormatFloat(v.z); m_buf+=']'; }
void JsonWriter::WriteQuat(const std::string& k,const math::Quat& q)
{ Prefix(k); m_buf+='['; m_buf+=FormatFloat(q.x)+", "+FormatFloat(q.y)+", "+FormatFloat(q.z)+", "+FormatFloat(q.w); m_buf+=']'; }

std::string JsonWriter::GetString() const { return m_buf; }

// =============================================================================
// JsonValue
// =============================================================================

static JsonValue s_nullValue{};
const JsonValue& JsonValue::Null() noexcept { return s_nullValue; }

const JsonValue* JsonValue::Get(const std::string& key) const noexcept
{
    if (type != JsonType::Object) return nullptr;
    for (const auto& kv : objectVal)
        if (kv.first == key) return &kv.second;
    return nullptr;
}

const JsonValue& JsonValue::At(size_t i) const noexcept
{
    if (type != JsonType::Array || i >= arrayVal.size()) return Null();
    return arrayVal[i];
}

math::Vec3 JsonValue::AsVec3() const noexcept
{
    if (type!=JsonType::Array||arrayVal.size()<3) return {};
    return {arrayVal[0].AsFloat(),arrayVal[1].AsFloat(),arrayVal[2].AsFloat()};
}

math::Quat JsonValue::AsQuat() const noexcept
{
    if (type!=JsonType::Array||arrayVal.size()<4) return math::Quat::Identity();
    return {arrayVal[0].AsFloat(),arrayVal[1].AsFloat(),
            arrayVal[2].AsFloat(),arrayVal[3].AsFloat()};
}

// =============================================================================
// JsonParser
// =============================================================================

char JsonParser::Peek()    const noexcept { return m_src[m_pos]; }
char JsonParser::Consume()       noexcept { return m_src[m_pos++]; }

void JsonParser::SkipWs()
{
    while (m_src[m_pos]==' '||m_src[m_pos]=='\t'||m_src[m_pos]=='\n'||m_src[m_pos]=='\r')
        ++m_pos;
}

JsonValue JsonParser::ParseString()
{
    const size_t start = m_pos;
    if (Peek()=='"') Consume();
    std::string s;
    while (m_src[m_pos] && m_src[m_pos]!='"') {
        if (m_src[m_pos]=='\\') {
            ++m_pos;
            switch(m_src[m_pos]) {
            case '"': s+='"';  break; case '\\': s+='\\'; break;
            case '/': s+='/';  break; case 'n':  s+='\n'; break;
            case 'r': s+='\r'; break; case 't':  s+='\t'; break;
            default:  s+=m_src[m_pos]; break;
            }
        } else s+=m_src[m_pos];
        ++m_pos;
    }
    if (m_src[m_pos]=='"') ++m_pos;
    JsonValue v;
    v.type = JsonType::String;
    v.strVal = std::move(s);
    v.stringOffset = start;
    v.stringLength = m_pos - start;
    return v;
}

JsonValue JsonParser::ParseNumber()
{
    const size_t start=m_pos;
    if (Peek()=='-') ++m_pos;
    while (m_src[m_pos]>='0'&&m_src[m_pos]<='9') ++m_pos;
    if (m_src[m_pos]=='.') { ++m_pos; while(m_src[m_pos]>='0'&&m_src[m_pos]<='9') ++m_pos; }
    if (m_src[m_pos]=='e'||m_src[m_pos]=='E') {
        ++m_pos; if(m_src[m_pos]=='+'||m_src[m_pos]=='-') ++m_pos;
        while(m_src[m_pos]>='0'&&m_src[m_pos]<='9') ++m_pos;
    }
    JsonValue v; v.type=JsonType::Number;
    v.numVal=std::stod(std::string(m_src+start, m_pos-start));
    return v;
}

JsonValue JsonParser::ParseLiteral()
{
    JsonValue v;
    if      (std::strncmp(m_src+m_pos,"true", 4)==0) { v.type=JsonType::Bool; v.boolVal=true;  m_pos+=4; }
    else if (std::strncmp(m_src+m_pos,"false",5)==0) { v.type=JsonType::Bool; v.boolVal=false; m_pos+=5; }
    else if (std::strncmp(m_src+m_pos,"null", 4)==0) { v.type=JsonType::Null;                   m_pos+=4; }
    else m_error="Unbekanntes Literal bei "+std::to_string(m_pos);
    return v;
}

JsonValue JsonParser::ParseObject()
{
    ++m_pos; // {
    JsonValue v; v.type=JsonType::Object;
    SkipWs();
    if (Peek()=='}') { ++m_pos; return v; }
    while (m_src[m_pos] && m_error.empty()) {
        SkipWs();
        if (Peek()!='"') { m_error="Erwarte Key-String"; break; }
        JsonValue key=ParseString();
        SkipWs();
        if (Peek()!=':') { m_error="Erwarte ':'"; break; }
        ++m_pos; SkipWs();
        JsonValue val=ParseValue();
        v.objectVal.emplace_back(key.strVal, std::move(val));
        SkipWs();
        if (Peek()==',') { ++m_pos; SkipWs(); continue; }
        if (Peek()=='}') { ++m_pos; break; }
        m_error="Erwarte ',' oder '}'"; break;
    }
    return v;
}

JsonValue JsonParser::ParseArray()
{
    ++m_pos; // [
    JsonValue v; v.type=JsonType::Array;
    SkipWs();
    if (Peek()==']') { ++m_pos; return v; }
    while (m_src[m_pos] && m_error.empty()) {
        SkipWs();
        v.arrayVal.push_back(ParseValue());
        SkipWs();
        if (Peek()==',') { ++m_pos; continue; }
        if (Peek()==']') { ++m_pos; break; }
        m_error="Erwarte ',' oder ']'"; break;
    }
    return v;
}

JsonValue JsonParser::ParseValue()
{
    SkipWs();
    const char c=Peek();
    if (c=='{') return ParseObject();
    if (c=='[') return ParseArray();
    if (c=='"') return ParseString();
    if (c=='-'||(c>='0'&&c<='9')) return ParseNumber();
    return ParseLiteral();
}

JsonValue JsonParser::Parse(const std::string& json, std::string& outError)
{
    JsonParser p(json.c_str());
    JsonValue root=p.ParseValue();
    outError=p.m_error;
    return root;
}

// =============================================================================
// SceneSerializer::RegisterDefaultHandlers
// =============================================================================

void SceneSerializer::RegisterDefaultHandlers()
{
    RegisterSerializer<NameComponent>([](JsonWriter& w,const NameComponent& c){
        w.BeginObject(); w.WriteString("type","NameComponent"); w.WriteString("name",c.name); w.EndObject();
    });
    RegisterSerializer<GuidComponent>([](JsonWriter& w,const GuidComponent& c){
        w.BeginObject(); w.WriteString("type","GuidComponent"); w.WriteString("guid",c.guid); w.EndObject();
    });
    RegisterSerializer<TagComponent>([](JsonWriter& w,const TagComponent& c){
        w.BeginObject(); w.WriteString("type","TagComponent"); w.WriteString("tag",c.tag); w.EndObject();
    });
    RegisterSerializer<TransformComponent>([](JsonWriter& w,const TransformComponent& c){
        w.BeginObject(); w.WriteString("type","TransformComponent");
        w.WriteVec3("localPosition",c.localPosition); w.WriteQuat("localRotation",c.localRotation);
        w.WriteVec3("localScale",c.localScale); w.WriteBool("inheritParentScale", c.inheritParentScale); w.EndObject();
    });
    RegisterSerializer<ParentComponent>([](JsonWriter& w,const ParentComponent& c){
        w.BeginObject(); w.WriteString("type","ParentComponent");
        w.WriteUint("parentId",c.parent.value); w.EndObject();
    });
    RegisterSerializer<ActiveComponent>([](JsonWriter& w,const ActiveComponent& c){
        w.BeginObject(); w.WriteString("type","ActiveComponent");
        w.WriteBool("active",c.active); w.EndObject();
    });
    RegisterSerializer<VisibilityComponent>([](JsonWriter& w,const VisibilityComponent& c){
        w.BeginObject(); w.WriteString("type","VisibilityComponent");
        w.WriteBool("visible",c.visible); w.EndObject();
    });
    RegisterSerializer<FolderComponent>([](JsonWriter& w,const FolderComponent&){
        w.BeginObject(); w.WriteString("type","FolderComponent"); w.EndObject();
    });
    RegisterSerializer<BoundsComponent>([](JsonWriter& w,const BoundsComponent& c){
        w.BeginObject(); w.WriteString("type","BoundsComponent");
        w.WriteVec3("centerLocal",c.centerLocal); w.WriteVec3("extentsLocal",c.extentsLocal);
        w.EndObject();
    });
    RegisterSerializer<OBBComponent>([](JsonWriter& w,const OBBComponent& c){
        w.BeginObject(); w.WriteString("type","OBBComponent");
        w.WriteVec3("centerOffset",c.centerOffset);
        w.WriteVec3("halfExtents", c.halfExtents);
        w.WriteQuat("orientation",  c.orientation);
        w.WriteBool("showInEditor", c.showInEditor);
        w.WriteBool("isTrigger",    c.isTrigger);
        w.EndObject();
    });
    RegisterSerializer<ColliderComponent>([](JsonWriter& w,const ColliderComponent& c){
        w.BeginObject(); w.WriteString("type","ColliderComponent");
        WriteColliderFields(w, c);
        w.EndObject();
    });
}

// =============================================================================
// ColliderComponent fields
// =============================================================================

namespace {

void ReadShapeFloat(const JsonValue& v, const char* key, float& out)
{
    if (const auto* n = v.Get(key); n && n->IsNumber())
        out = n->AsFloat();
}

void ReadShapeVec3(const JsonValue& v, const char* key, math::Vec3& out)
{
    if (const auto* n = v.Get(key); n && n->IsArray() && n->arrayVal.size() >= 3u)
        out = n->AsVec3();
}

int ReadShapeInt(const JsonValue& v, const char* key, int fallback, int maxValue)
{
    if (const auto* n = v.Get(key); n && n->IsNumber())
        return std::clamp(n->AsInt(), 0, maxValue);
    return fallback;
}

bool ReadShowInEditor(const JsonValue& v)
{
    const auto* n = v.Get("showInEditor");
    return n && n->IsBool() && n->AsBool();
}

} // namespace

void WriteColliderFields(JsonWriter& w, const ColliderComponent& c)
{
    w.WriteBool("showInEditor", c.showInEditor);
    w.BeginArray("shapes");
    for (const ColliderShape& s : c.shapes)
    {
        w.BeginObject();
        w.WriteInt("shape", static_cast<int>(s.type));
        w.WriteVec3("centerOffset", s.centerOffset);
        w.WriteQuat("orientation", s.orientation);
        switch (s.type)
        {
            case ColliderShapeType::Box:
                w.WriteVec3("halfExtents", s.halfExtents);
                break;
            case ColliderShapeType::Sphere:
                w.WriteFloat("radius", s.radius);
                break;
            case ColliderShapeType::Capsule:
                w.WriteFloat("radius", s.radius);
                w.WriteFloat("height", s.height);
                w.WriteInt("axis", static_cast<int>(s.axis));
                break;
        }
        w.EndObject();
    }
    w.EndArray();
}

void ReadColliderFields(const JsonValue& v, ColliderComponent& c)
{
    if (const auto* n = v.Get("showInEditor"); n && n->IsBool())
        c.showInEditor = n->AsBool();
    const auto* shapes = v.Get("shapes");
    if (!shapes || !shapes->IsArray())
        return;
    c.shapes.clear();
    for (const JsonValue& sv : shapes->arrayVal)
    {
        if (!sv.IsObject())
            continue;
        ColliderShape s{};
        s.type = static_cast<ColliderShapeType>(ReadShapeInt(sv, "shape", 0, 2));
        ReadShapeVec3(sv, "centerOffset", s.centerOffset);
        if (const auto* q = sv.Get("orientation"); q && q->IsArray())
            s.orientation = q->AsQuat().Normalized();
        ReadShapeVec3(sv, "halfExtents", s.halfExtents);
        ReadShapeFloat(sv, "radius", s.radius);
        ReadShapeFloat(sv, "height", s.height);
        s.axis = static_cast<ColliderAxis>(ReadShapeInt(sv, "axis", 1, 2));
        c.shapes.push_back(s);
    }
}

void AppendLegacyBoxCollider(const OBBComponent& obb, ColliderComponent& c)
{
    ColliderShape s{};
    s.type         = ColliderShapeType::Box;
    s.centerOffset = obb.centerOffset;
    s.orientation  = obb.orientation;
    s.halfExtents  = obb.halfExtents;
    if (c.shapes.empty())
        c.showInEditor = obb.showInEditor;
    else
        c.showInEditor = c.showInEditor || obb.showInEditor;
    c.shapes.push_back(s);
}

void AppendLegacySphereCollider(const JsonValue& v, ColliderComponent& c)
{
    ColliderShape s{};
    s.type = ColliderShapeType::Sphere;
    ReadShapeVec3(v, "centerOffset", s.centerOffset);
    ReadShapeFloat(v, "radius", s.radius);
    const bool show = ReadShowInEditor(v);
    c.showInEditor = c.shapes.empty() ? show : (c.showInEditor || show);
    c.shapes.push_back(s);
}

void AppendLegacyCapsuleCollider(const JsonValue& v, ColliderComponent& c)
{
    ColliderShape s{};
    s.type = ColliderShapeType::Capsule;
    ReadShapeVec3(v, "centerOffset", s.centerOffset);
    ReadShapeFloat(v, "radius", s.radius);
    ReadShapeFloat(v, "height", s.height);
    s.axis = static_cast<ColliderAxis>(ReadShapeInt(v, "axis", 1, 2));
    const bool show = ReadShowInEditor(v);
    c.showInEditor = c.shapes.empty() ? show : (c.showInEditor || show);
    c.shapes.push_back(s);
}

// BUGFIX: ForEachAlive statt View<NameComponent>
std::string SceneSerializer::SerializeToJson(const std::string& sceneName) const
{
    // Count only the entities that pass the filter, so the header matches the
    // "entities" array (the world also holds editor helpers and other scenes).
    uint32_t writtenCount = 0u;
    if (m_entityFilter)
        m_world.ForEachAlive([&](EntityID id) { if (m_entityFilter(id)) ++writtenCount; });
    else
        writtenCount = static_cast<uint32_t>(m_world.EntityCount());

    JsonWriter w;
    w.BeginObject();
    w.WriteString("scene",     sceneName);
    w.WriteUint("entityCount", writtenCount);
    if (m_rootMetadataWriter)
        m_rootMetadataWriter(w);
    w.BeginArray("entities");

    m_world.ForEachAlive([&](EntityID id)
    {
        // Filter: Entity überspringen wenn nicht für diese Szene vorgesehen.
        if (m_entityFilter && !m_entityFilter(id))
            return;

        w.BeginObject();
        w.WriteUint("entityId",         id.value);
        w.WriteUint("entityIndex",      id.Index());
        w.WriteUint("entityGeneration", id.Generation());
        w.BeginArray("components");
        for (const auto& [typeId, handler] : m_handlers)
            if (const auto* component = m_world.Get(id, typeId))
                handler(w, component);
        w.EndArray();
        w.EndObject();
    });

    w.EndArray();
    w.EndObject();
    return w.GetString();
}

std::string SceneSerializer::SerializeComponent(EntityID entity, uint32_t typeId) const
{
    return SerializeComponentValue(typeId, m_world.Get(entity, typeId));
}

std::string SceneSerializer::SerializeComponentValue(uint32_t typeId, const void* component) const
{
    const auto handler = m_handlers.find(typeId);
    if (!component || handler == m_handlers.end())
        return {};
    JsonWriter writer(true);
    handler->second(writer, component);
    return writer.GetString();
}

// =============================================================================
// SceneDeserializer
// =============================================================================

void SceneDeserializer::RegisterDefaultHandlers()
{
    RegisterHandler<NameComponent>([](const JsonValue& v,ecs::World& w,EntityID id){
        const auto* n=v.Get("name"); if(n&&n->IsString()) w.SetComponent<NameComponent>(id,NameComponent(n->AsString()));
    });
    RegisterHandler<GuidComponent>([](const JsonValue& v,ecs::World& w,EntityID id){
        const auto* g=v.Get("guid"); if(g&&g->IsString()) w.SetComponent<GuidComponent>(id,GuidComponent(g->AsString()));
    });
    RegisterHandler<TagComponent>([](const JsonValue& v,ecs::World& w,EntityID id){
        TagComponent tc{};
        if (const auto* t=v.Get("tag")) tc.tag=t->AsString();
        w.SetComponent<TagComponent>(id,tc);
    });
    RegisterHandler<TransformComponent>([](const JsonValue& v,ecs::World& w,EntityID id){
        TransformComponent tc{};
        if (const auto* p=v.Get("localPosition")) tc.localPosition=p->AsVec3();
        if (const auto* r=v.Get("localRotation")) tc.localRotation=r->AsQuat();
        if (const auto* s=v.Get("localScale"))    tc.localScale=s->AsVec3();
        else                                       tc.localScale=math::Vec3(1,1,1);
        if (const auto* inherit = v.Get("inheritParentScale")) tc.inheritParentScale = inherit->AsBool();
        if (const auto* existing = w.Get<TransformComponent>(id))
        {
            tc.localVersion = existing->localVersion + 1;
            tc.worldVersion = existing->worldVersion;
            tc.parentWorldVersion = existing->parentWorldVersion;
        }
        tc.dirty = true;
        w.SetComponent(id, tc);
    });
    RegisterHandler<ActiveComponent>([](const JsonValue& v,ecs::World& w,EntityID id){
        ActiveComponent ac{};
        if (const auto* a=v.Get("active")) ac.active=a->AsBool();
        w.SetComponent<ActiveComponent>(id,ac);
    });
    RegisterHandler<VisibilityComponent>([](const JsonValue& v,ecs::World& w,EntityID id){
        VisibilityComponent visibility{};
        if (const auto* value = v.Get("visible")) visibility.visible = value->AsBool();
        w.SetComponent(id, visibility);
    });
    RegisterHandler<FolderComponent>([](const JsonValue&,ecs::World& w,EntityID id){
        w.SetComponent(id, FolderComponent{});
    });
    RegisterHandler<BoundsComponent>([](const JsonValue& v,ecs::World& w,EntityID id){
        BoundsComponent bc{};
        if (const auto* c=v.Get("centerLocal"))  bc.centerLocal=c->AsVec3();
        if (const auto* e=v.Get("extentsLocal")) bc.extentsLocal=e->AsVec3();
        bc.localDirty=true;
        w.SetComponent<BoundsComponent>(id,bc);
    });
    RegisterHandler<OBBComponent>([](const JsonValue& v,ecs::World& w,EntityID id){
        OBBComponent obb{};
        if (const auto* c=v.Get("centerOffset")) obb.centerOffset=c->AsVec3();
        if (const auto* h=v.Get("halfExtents"))  obb.halfExtents =h->AsVec3();
        if (const auto* q=v.Get("orientation"))  obb.orientation =q->AsQuat();
        if (const auto* s=v.Get("showInEditor")) obb.showInEditor=s->AsBool();
        if (const auto* t=v.Get("isTrigger"))    obb.isTrigger   =t->AsBool();
        w.SetComponent<OBBComponent>(id,obb);
    });
    RegisterHandler<ColliderComponent>([](const JsonValue& v,ecs::World& w,EntityID id){
        // Appends to shapes a legacy component of the same entity may have added.
        ColliderComponent loaded{};
        ReadColliderFields(v, loaded);
        ColliderComponent collider = w.Get<ColliderComponent>(id) ? *w.Get<ColliderComponent>(id) : ColliderComponent{};
        if (collider.shapes.empty())
            collider.showInEditor = loaded.showInEditor;
        collider.shapes.insert(collider.shapes.end(), loaded.shapes.begin(), loaded.shapes.end());
        w.SetComponent<ColliderComponent>(id, std::move(collider));
    });
    // Older files: sphere and capsule were physics-addon components.
    m_handlers["SphereColliderComponent"]=[](const JsonValue& v,ecs::World& w,EntityID id,
                                              const std::unordered_map<uint32_t,EntityID>&){
        ColliderComponent collider = w.Get<ColliderComponent>(id) ? *w.Get<ColliderComponent>(id) : ColliderComponent{};
        AppendLegacySphereCollider(v, collider);
        w.SetComponent<ColliderComponent>(id, std::move(collider));
    };
    m_handlers["CapsuleColliderComponent"]=[](const JsonValue& v,ecs::World& w,EntityID id,
                                               const std::unordered_map<uint32_t,EntityID>&){
        ColliderComponent collider = w.Get<ColliderComponent>(id) ? *w.Get<ColliderComponent>(id) : ColliderComponent{};
        AppendLegacyCapsuleCollider(v, collider);
        w.SetComponent<ColliderComponent>(id, std::move(collider));
    };
    // ParentComponent: braucht ID-Remapping
    m_handlers["ParentComponent"]=[](const JsonValue& v,ecs::World& w,EntityID id,
                                      const std::unordered_map<uint32_t,EntityID>& remap){
        const auto* p=v.Get("parentId"); if(!p) return;
        auto it=remap.find(p->AsUint()); if(it==remap.end()) return;
        w.SetComponent<ParentComponent>(id,ParentComponent{it->second});
    };
}

DeserializeResult SceneDeserializer::DeserializeFromJson(const std::string& json)
{
    DeserializeResult result;

    std::string err;
    const JsonValue root=JsonParser::Parse(json,err);
    if (!err.empty()) { result.error="JSON-Parse-Fehler: "+err; return result; }
    return DeserializeFromValue(root);
}

bool SceneDeserializer::DeserializeComponent(EntityID entity, const JsonValue& component,
    const std::unordered_map<uint32_t, EntityID>& remap)
{
    const auto* type = component.Get("type");
    if (!m_world.IsAlive(entity) || !type || !type->IsString())
        return false;
    const auto handler = m_handlers.find(type->AsString());
    if (handler == m_handlers.end())
        return false;
    handler->second(component, m_world, entity, remap);
    return true;
}

DeserializeResult SceneDeserializer::DeserializeFromValue(const JsonValue& root)
{
    DeserializeResult result;
    if (!root.IsObject()) { result.error="Root ist kein Objekt"; return result; }

    const JsonValue* entities=root.Get("entities");
    if (!entities||!entities->IsArray()) { result.error="Kein 'entities'-Array"; return result; }

    std::unordered_map<uint32_t,EntityID> remap;

    // Zwischenspeicher für ParentComponent-Daten
    struct Pending { EntityID newId; uint32_t oldParentId; };
    std::vector<Pending> pendingParents;

    // Pass 1: Entities + alle Komponenten außer ParentComponent
    for (const JsonValue& eVal : entities->arrayVal)
    {
        if (!eVal.IsObject()) { ++result.entitiesSkipped; continue; }
        const JsonValue* oldIdVal=eVal.Get("entityId");
        if (!oldIdVal) oldIdVal=eVal.Get("id");
        if (!oldIdVal) { ++result.entitiesSkipped; continue; }

        const EntityID newId=m_world.CreateEntity();
        remap[oldIdVal->AsUint()]=newId;
        ++result.entitiesRead;

        const JsonValue* comps=eVal.Get("components");
        if (!comps||!comps->IsArray()) continue;

        for (const JsonValue& cVal : comps->arrayVal)
        {
            if (!cVal.IsObject()) continue;
            const JsonValue* typeVal=cVal.Get("type");
            if (!typeVal||!typeVal->IsString()) continue;
            const std::string& typeName=typeVal->AsString();

            if (typeName=="ParentComponent") {
                const JsonValue* p=cVal.Get("parentId");
                if (p) pendingParents.push_back({newId,p->AsUint()});
                continue;
            }
            auto it=m_handlers.find(typeName);
            if (it==m_handlers.end()) continue;
            it->second(cVal,m_world,newId,remap);
            ++result.componentsRead;
        }
    }

    // Pass 2: ParentComponents mit vollständigem Remap
    if (auto it=m_handlers.find("ParentComponent"); it!=m_handlers.end())
    {
        for (const Pending& pp : pendingParents)
        {
            auto remapIt=remap.find(pp.oldParentId);
            if (remapIt==remap.end()) continue;
            JsonValue fake; fake.type=JsonType::Object;
            JsonValue pv; pv.type=JsonType::Number; pv.numVal=static_cast<double>(pp.oldParentId);
            fake.objectVal.emplace_back("parentId",pv);
            it->second(fake,m_world,pp.newId,remap);
            ++result.componentsRead;
        }
    }

    // Older scenes: a solid OBB was the box collider. Only whole scenes are
    // migrated; single components (editor undo) load exactly as stored.
    for (const auto& [oldId, id] : remap)
    {
        const auto* obb = m_world.Get<OBBComponent>(id);
        if (!obb || obb->isTrigger)
            continue;
        ColliderComponent collider{};
        AppendLegacyBoxCollider(*obb, collider);
        if (const auto* existing = m_world.Get<ColliderComponent>(id))
        {
            collider.showInEditor = collider.showInEditor || existing->showInEditor;
            collider.shapes.insert(collider.shapes.end(), existing->shapes.begin(), existing->shapes.end());
        }
        m_world.Remove<OBBComponent>(id);
        m_world.SetComponent<ColliderComponent>(id, std::move(collider));
    }

    result.entityRemap = std::move(remap);
    result.success=true;
    return result;
}

} // namespace engine::serialization
