// =============================================================================
// KROM Engine - src/serialization/SceneSerializer.cpp
// BUGFIX: ForEachAlive statt View<NameComponent>
// NEU: JsonParser + SceneDeserializer
// =============================================================================
#include "serialization/SceneSerializer.hpp"
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
{ Prefix(k); m_buf+=FloatStr(v); }
void JsonWriter::WriteUint(const std::string& k,uint32_t v)
{ Prefix(k); char b[16]; std::snprintf(b,sizeof(b),"%u",v); m_buf+=b; }
void JsonWriter::WriteInt(const std::string& k,int32_t v)
{ Prefix(k); char b[16]; std::snprintf(b,sizeof(b),"%d",v); m_buf+=b; }
void JsonWriter::WriteBool(const std::string& k,bool v)
{ Prefix(k); m_buf+=(v?"true":"false"); }
void JsonWriter::WriteVec3(const std::string& k,const math::Vec3& v)
{ Prefix(k); m_buf+='['; m_buf+=FloatStr(v.x)+", "+FloatStr(v.y)+", "+FloatStr(v.z); m_buf+=']'; }
void JsonWriter::WriteQuat(const std::string& k,const math::Quat& q)
{ Prefix(k); m_buf+='['; m_buf+=FloatStr(q.x)+", "+FloatStr(q.y)+", "+FloatStr(q.z)+", "+FloatStr(q.w); m_buf+=']'; }

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
    JsonValue v; v.type=JsonType::String; v.strVal=std::move(s); return v;
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
    RegisterSerializer<TransformComponent>([](JsonWriter& w,const TransformComponent& c){
        w.BeginObject(); w.WriteString("type","TransformComponent");
        w.WriteVec3("localPosition",c.localPosition); w.WriteQuat("localRotation",c.localRotation);
        w.WriteVec3("localScale",c.localScale); w.EndObject();
    });
    RegisterSerializer<ParentComponent>([](JsonWriter& w,const ParentComponent& c){
        w.BeginObject(); w.WriteString("type","ParentComponent");
        w.WriteUint("parentId",c.parent.value); w.EndObject();
    });
    RegisterSerializer<MeshComponent>([](JsonWriter& w,const MeshComponent& c){
        w.BeginObject(); w.WriteString("type","MeshComponent");
        w.WriteUint("meshHandle",c.mesh.value); w.WriteBool("castShadows",c.castShadows);
        w.WriteUint("layerMask",c.layerMask); w.EndObject();
    });
    RegisterSerializer<MaterialComponent>([](JsonWriter& w,const MaterialComponent& c){
        w.BeginObject(); w.WriteString("type","MaterialComponent");
        w.WriteUint("materialHandle",c.material.value); w.WriteUint("submeshIndex",c.submeshIndex);
        w.EndObject();
    });
    RegisterSerializer<CameraComponent>([](JsonWriter& w,const CameraComponent& c){
        w.BeginObject(); w.WriteString("type","CameraComponent");
        w.WriteFloat("fovYDeg",c.fovYDeg); w.WriteFloat("nearPlane",c.nearPlane);
        w.WriteFloat("farPlane",c.farPlane); w.WriteBool("isMain",c.isMainCamera);
        w.EndObject();
    });
    RegisterSerializer<LightComponent>([](JsonWriter& w,const LightComponent& c){
        w.BeginObject(); w.WriteString("type","LightComponent");
        w.WriteUint("lightType",static_cast<uint32_t>(c.type));
        w.WriteVec3("color",c.color); w.WriteFloat("intensity",c.intensity);
        w.WriteFloat("range",c.range); w.WriteBool("castShadows",c.castShadows);
        w.EndObject();
    });
    RegisterSerializer<ActiveComponent>([](JsonWriter& w,const ActiveComponent& c){
        w.BeginObject(); w.WriteString("type","ActiveComponent");
        w.WriteBool("active",c.active); w.EndObject();
    });
}

// BUGFIX: ForEachAlive statt View<NameComponent>
std::string SceneSerializer::SerializeToJson(const std::string& sceneName) const
{
    JsonWriter w;
    w.BeginObject();
    w.WriteString("scene",     sceneName);
    w.WriteUint("entityCount", static_cast<uint32_t>(m_world.EntityCount()));
    w.BeginArray("entities");

    m_world.ForEachAlive([&](EntityID id)
    {
        w.BeginObject();
        w.WriteUint("entityId",         id.value);
        w.WriteUint("entityIndex",      id.Index());
        w.WriteUint("entityGeneration", id.Generation());
        w.BeginArray("components");
        for (const auto& [typeId, handler] : m_handlers)
            handler(w, m_world, id);
        w.EndArray();
        w.EndObject();
    });

    w.EndArray();
    w.EndObject();
    return w.GetString();
}

// =============================================================================
// SceneDeserializer
// =============================================================================

void SceneDeserializer::RegisterDefaultHandlers()
{
    RegisterHandler<NameComponent>([](const JsonValue& v,ecs::World& w,EntityID id){
        const auto* n=v.Get("name"); if(n&&n->IsString()) w.Add<NameComponent>(id,NameComponent(n->AsString()));
    });
    RegisterHandler<TransformComponent>([](const JsonValue& v,ecs::World& w,EntityID id){
        TransformComponent tc{};
        if (const auto* p=v.Get("localPosition")) tc.localPosition=p->AsVec3();
        if (const auto* r=v.Get("localRotation")) tc.localRotation=r->AsQuat();
        if (const auto* s=v.Get("localScale"))    tc.localScale=s->AsVec3();
        else                                       tc.localScale=math::Vec3(1,1,1);
        tc.dirty=true; w.Add<TransformComponent>(id,tc);
    });
    RegisterHandler<MeshComponent>([](const JsonValue& v,ecs::World& w,EntityID id){
        MeshComponent mc{};
        if (const auto* h=v.Get("meshHandle"))  mc.mesh=MeshHandle(h->AsUint());
        if (const auto* s=v.Get("castShadows")) mc.castShadows=s->AsBool();
        if (const auto* l=v.Get("layerMask"))   mc.layerMask=l->AsUint();
        w.Add<MeshComponent>(id,mc);
    });
    RegisterHandler<MaterialComponent>([](const JsonValue& v,ecs::World& w,EntityID id){
        MaterialComponent mc{};
        if (const auto* h=v.Get("materialHandle")) mc.material=MaterialHandle(h->AsUint());
        if (const auto* s=v.Get("submeshIndex"))   mc.submeshIndex=s->AsUint();
        w.Add<MaterialComponent>(id,mc);
    });
    RegisterHandler<CameraComponent>([](const JsonValue& v,ecs::World& w,EntityID id){
        CameraComponent cc{};
        if (const auto* f=v.Get("fovYDeg"))   cc.fovYDeg=f->AsFloat();
        if (const auto* n=v.Get("nearPlane")) cc.nearPlane=n->AsFloat();
        if (const auto* fa=v.Get("farPlane")) cc.farPlane=fa->AsFloat();
        if (const auto* m=v.Get("isMain"))    cc.isMainCamera=m->AsBool();
        w.Add<CameraComponent>(id,cc);
    });
    RegisterHandler<LightComponent>([](const JsonValue& v,ecs::World& w,EntityID id){
        LightComponent lc{};
        if (const auto* t=v.Get("lightType"))   lc.type=static_cast<LightType>(t->AsUint());
        if (const auto* c=v.Get("color"))        lc.color=c->AsVec3();
        if (const auto* i=v.Get("intensity"))    lc.intensity=i->AsFloat();
        if (const auto* r=v.Get("range"))        lc.range=r->AsFloat();
        if (const auto* s=v.Get("castShadows"))  lc.castShadows=s->AsBool();
        w.Add<LightComponent>(id,lc);
    });
    RegisterHandler<ActiveComponent>([](const JsonValue& v,ecs::World& w,EntityID id){
        ActiveComponent ac{};
        if (const auto* a=v.Get("active")) ac.active=a->AsBool();
        w.Add<ActiveComponent>(id,ac);
    });
    // ParentComponent: braucht ID-Remapping
    m_handlers["ParentComponent"]=[](const JsonValue& v,ecs::World& w,EntityID id,
                                      const std::unordered_map<uint32_t,EntityID>& remap){
        const auto* p=v.Get("parentId"); if(!p) return;
        auto it=remap.find(p->AsUint()); if(it==remap.end()) return;
        w.Add<ParentComponent>(id,ParentComponent{it->second});
    };
}

DeserializeResult SceneDeserializer::DeserializeFromJson(const std::string& json)
{
    DeserializeResult result;

    std::string err;
    const JsonValue root=JsonParser::Parse(json,err);
    if (!err.empty()) { result.error="JSON-Parse-Fehler: "+err; return result; }
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
            JsonValue pv; pv.type=JsonType::Number; pv.numVal=static_cast<double>(remapIt->second.value);
            fake.objectVal.emplace_back("parentId",pv);
            it->second(fake,m_world,pp.newId,remap);
            ++result.componentsRead;
        }
    }

    result.success=true;
    return result;
}

} // namespace engine::serialization
