#pragma once
#include "imgui.h"
#include "imgui_internal.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>
#include <utility>

namespace engine::renderer::addons::editor::value_ui {
struct Payload
{
    std::string type;
    std::vector<unsigned char> bytes;
    std::string text;
    int coordinateSpace = 0;
};
inline Payload& Clipboard() { static Payload value; return value; }
struct Scope;
inline Scope*& ActiveScope() { static Scope* scope = nullptr; return scope; }
struct Scope
{
    Scope* previous;
    std::function<void()> before, after;
    bool pasted = false;
    Scope(std::function<void()> begin, std::function<void()> end)
        : previous(ActiveScope()), before(std::move(begin)), after(std::move(end)) { ActiveScope() = this; }
    ~Scope() { ActiveScope() = previous; if (pasted && after) after(); }
    void Notify() { if (!pasted) { if (before) before(); pasted = true; } }
    Scope(const Scope&) = delete;
};
inline void NotifyPaste() { if (ActiveScope()) ActiveScope()->Notify(); }
inline void Copy(const std::string& type, const void* data, size_t size, int space = 0)
{
    auto& clip = Clipboard(); clip.type = type; clip.text.clear(); clip.coordinateSpace = space;
    const auto* bytes = static_cast<const unsigned char*>(data); clip.bytes.assign(bytes, bytes + size);
}
inline bool Compatible(const std::string& type, size_t size)
{ return Clipboard().type == type && Clipboard().bytes.size() == size; }
inline bool Paste(const std::string& type, void* data, size_t size, bool notify = true)
{
    if (!Compatible(type, size)) return false;
    if (std::memcmp(data, Clipboard().bytes.data(), size) == 0) return false;
    if (notify) NotifyPaste();
    std::memcpy(data, Clipboard().bytes.data(), size); return true;
}
inline void CopyText(const std::string& type, const std::string& value)
{ Clipboard() = Payload{type, {}, value, 0}; }
inline bool PasteText(const std::string& type, std::string& value, bool notify = true)
{
    if (Clipboard().type != type || value == Clipboard().text) return false;
    if (notify) NotifyPaste(); value = Clipboard().text; return true;
}
struct LabelRect { ImGuiWindow* window = nullptr; int frame = -1; ImRect rect; };
inline LabelRect& LastLabel() { static LabelRect label; return label; }
inline void RememberLabel() { LastLabel() = {ImGui::GetCurrentWindow(), ImGui::GetFrameCount(), GImGui->LastItemData.Rect}; }
inline void ForgetLabel() { LastLabel().frame = -1; }
inline void TextUnformatted(const char* text, const char* end = nullptr)
{ ImGui::TextUnformatted(text, end); RememberLabel(); }
inline void Text(const char* fmt, ...)
{ va_list args; va_start(args, fmt); ImGui::TextV(fmt, args); va_end(args); RememberLabel(); }
inline void TextDisabled(const char* fmt, ...)
{ va_list args; va_start(args, fmt); ImGui::TextDisabledV(fmt, args); va_end(args); RememberLabel(); }

// Preserve the original widget item for drag/drop, tooltips and edit-commit code.
inline bool Menu(const char* label, const std::function<void()>& copy,
                 bool compatible, const std::function<bool()>& paste, bool allowPaste = true)
{
    auto& g = *GImGui;
    const ImGuiLastItemData item = g.LastItemData;
    bool labelHovered = false;
    const auto& extra = LastLabel();
    if (label[0] == '#' && extra.frame == g.FrameCount && extra.window == g.CurrentWindow &&
        std::abs(extra.rect.Max.y - item.Rect.Min.y) < ImGui::GetFrameHeightWithSpacing() * 2.f)
        labelHovered = ImGui::IsMouseHoveringRect(extra.rect.Min, extra.rect.Max);
    const bool disabled = (item.ItemFlags & ImGuiItemFlags_Disabled) != 0;
    ImGui::PushID(label);
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Right) &&
        ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup) &&
        (labelHovered || ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup |
             ImGuiHoveredFlags_AllowWhenOverlappedByItem | ImGuiHoveredFlags_AllowWhenDisabled)))
        ImGui::OpenPopup("##value_clipboard");
    bool changed = false;
    if (ImGui::BeginPopup("##value_clipboard"))
    {
        if (ImGui::MenuItem("Wert kopieren")) copy();
        if (ImGui::MenuItem("Wert einfuegen", nullptr, false, compatible && allowPaste && !disabled)) changed = paste();
        if (!compatible && !Clipboard().type.empty()) ImGui::TextDisabled("Datentyp passt nicht");
        ImGui::EndPopup();
    }
    ImGui::PopID();
    g.LastItemData = item;
    if (changed)
    {
        g.LastItemData.StatusFlags |= ImGuiItemStatusFlags_Edited | ImGuiItemStatusFlags_HasDeactivated | ImGuiItemStatusFlags_Deactivated;
        g.DeactivatedItemData.HasBeenEditedBefore = true;
    }
    return changed;
}

template<typename T>
inline bool ScalarMenu(const char* label, T* data, const char* type, T min = T{}, T max = T{})
{
    return Menu(label, [=] { Copy(type, data, sizeof(T)); }, Compatible(type, sizeof(T)), [=] {
        T value = *data;
        if (!Paste(type, &value, sizeof(T), false)) return false;
        if (min < max) value = std::clamp(value, min, max);
        if (value == *data) return false;
        NotifyPaste(); *data = value; return true;
    });
}
inline bool FloatsMenu(const char* label, float* data, int count, const char* type, float min = 0, float max = 0)
{
    bool valid = Compatible(type, sizeof(float) * count);
    if (valid) for (int i = 0; i < count; ++i) { float v; std::memcpy(&v, Clipboard().bytes.data() + i*sizeof(float), sizeof(float)); valid &= std::isfinite(v); }
    return Menu(label, [=] { Copy(type, data, sizeof(float)*count); }, valid, [=] {
        std::array<float,4> values{}; std::copy_n(data, count, values.data());
        if (!Paste(type, values.data(), sizeof(float)*count, false)) return false;
        if (min < max) for (int i=0;i<count;++i) values[i] = std::clamp(values[i],min,max);
        if (std::equal(values.begin(), values.begin()+count, data)) return false;
        NotifyPaste(); std::copy_n(values.data(), count, data); return true;
    });
}
inline bool StringMenu(const char* label, std::string& value, const char* type = "string", bool allow = true)
{ return Menu(label, [&] { CopyText(type, value); }, Clipboard().type == type, [&] { return PasteText(type,value); }, allow); }
inline bool DragFloat(const char* label, float* value, float speed=1.f, float min=0.f, float max=0.f, const char* format="%.3f", ImGuiSliderFlags flags=0)
{
    const bool changed = ImGui::DragFloat(label, value, speed, min, max, format, flags);
    return FloatsMenu(label,value,1,"float",min,max) || changed;
}
inline bool DragFloat2(const char* label, float* value, float speed=1.f, float min=0.f, float max=0.f, const char* format="%.3f", ImGuiSliderFlags flags=0)
{
    const bool changed = ImGui::DragFloat2(label, value, speed, min, max, format, flags);
    return FloatsMenu(label,value,2,"float2",min,max) || changed;
}
inline bool DragFloat3(const char* label, float* value, float speed=1.f, float min=0.f, float max=0.f, const char* format="%.3f", ImGuiSliderFlags flags=0)
{
    const bool changed = ImGui::DragFloat3(label, value, speed, min, max, format, flags);
    if (std::strcmp(label,"##pos")==0 || std::strcmp(label,"##rot")==0 || std::strcmp(label,"##scl")==0) return changed;
    return FloatsMenu(label,value,3,"float3",min,max) || changed;
}
inline bool DragFloat4(const char* label, float* value, float speed=1.f, float min=0.f, float max=0.f, const char* format="%.3f", ImGuiSliderFlags flags=0)
{
    const bool changed = ImGui::DragFloat4(label, value, speed, min, max, format, flags);
    return FloatsMenu(label,value,4,"float4",min,max) || changed;
}
inline bool ColorEdit3(const char* label, float* value, ImGuiColorEditFlags flags=0)
{ const bool changed = ImGui::ColorEdit3(label,value,flags); return FloatsMenu(label,value,3,"color3",(flags & ImGuiColorEditFlags_HDR)?0.f:0.f,(flags & ImGuiColorEditFlags_HDR)?0.f:1.f) || changed; }
inline bool ColorEdit4(const char* label, float* value, ImGuiColorEditFlags flags=0)
{ const bool changed = ImGui::ColorEdit4(label,value,flags); return FloatsMenu(label,value,4,"color4",(flags & ImGuiColorEditFlags_HDR)?0.f:0.f,(flags & ImGuiColorEditFlags_HDR)?0.f:1.f) || changed; }
inline bool DragInt(const char* label, int* value, float speed=1, int min=0, int max=0, const char* format="%d", ImGuiSliderFlags flags=0)
{ const bool changed = ImGui::DragInt(label,value,speed,min,max,format,flags); return ScalarMenu(label,value,"int",min,max) || changed; }
inline bool InputInt(const char* label,int* value,int step=1,int fast=100,ImGuiInputTextFlags flags=0)
{ const bool changed = ImGui::InputInt(label,value,step,fast,flags); return ScalarMenu(label,value,"int") || changed; }
inline bool Checkbox(const char* label,bool* value)
{ const bool changed = ImGui::Checkbox(label,value); return ScalarMenu(label,value,"bool") || changed; }
inline bool SliderFloat(const char* label,float* value,float min,float max,const char* format="%.3f",ImGuiSliderFlags flags=0)
{ const bool changed = ImGui::SliderFloat(label,value,min,max,format,flags); return FloatsMenu(label,value,1,"float",min,max) || changed; }
inline bool SliderInt(const char* label,int* value,int min,int max,const char* format="%d",ImGuiSliderFlags flags=0)
{ const bool changed = ImGui::SliderInt(label,value,min,max,format,flags); return ScalarMenu(label,value,"int",min,max) || changed; }
inline bool InputText(const char* label,char* buffer,size_t capacity,ImGuiInputTextFlags flags=0,ImGuiInputTextCallback callback=nullptr,void* user=nullptr)
{
    const bool changed = ImGui::InputText(label,buffer,capacity,flags,callback,user);
    const bool pasted = Menu(label,[&] { CopyText("string",buffer); },Clipboard().type=="string" && Clipboard().text.size()<capacity,[&] {
        std::string value(buffer); if (!PasteText("string",value)) return false;
        std::memcpy(buffer,value.c_str(),value.size()+1); return true;
    }, (flags & ImGuiInputTextFlags_ReadOnly)==0);
    return pasted || changed;
}
inline bool Combo(const char* label,int* value,const char* const items[],int count,int height=-1)
{
    std::string type="enum:"; for(int i=0;i<count;++i) { type += items[i]; type.push_back('\x1f'); }
    const bool changed=ImGui::Combo(label,value,items,count,height);
    return ScalarMenu(label,value,type.c_str(),0,std::max(0,count-1)) || changed;
}
inline bool Combo(const char* label,int* value,const char* items,int height=-1)
{
    std::vector<const char*> entries;
    for(const char* p=items;*p;p+=std::strlen(p)+1) entries.push_back(p);
    return Combo(label,value,entries.data(),static_cast<int>(entries.size()),height);
}
} // namespace engine::renderer::addons::editor::value_ui
