#pragma once

#include "addons/editor/EditorLocalization.hpp"
// =============================================================================
// KROM Engine - addons/editor/EditorComponentClipboard.hpp
// Zwischenablage fuer Komponenten im Inspector (wie Unity): Rechtsklick auf
// eine Komponente -> "Komponente kopieren"; am anderen Objekt "Werte
// einfuegen" (gleiche Komponente vorhanden) oder "Als neue Komponente
// einfuegen". Transform zusaetzlich teilweise (Position/Rotation/Skalierung),
// Scripts je Instanz. Alles laeuft ueber ExecuteSceneMutation (Undo).
// =============================================================================
#include "addons/camera/CameraComponents.hpp"
#include "addons/editor/EditorCommands.hpp"
#include "addons/probes/ProbeComponents.hpp"
#include "ecs/Components.hpp"

#include "imgui.h"

#include <functional>
#include <memory>
#include <string>

namespace engine::renderer::addons::editor {

// Inhalt der Zwischenablage (eine Komponente).
struct ComponentClip
{
    uint32_t    typeId = 0u;
    std::string label;
    // Kopie auf entity anwenden; asNew = hinzufuegen, wenn sie dort fehlt.
    std::function<bool(EditorFrameContext&, EntityID, bool asNew)> apply;
    std::function<bool(EditorFrameContext&, EntityID)>              presentOn;
};

[[nodiscard]] ComponentClip& ComponentClipboard();

// Each copied value remembers its coordinate space, independently of later UI changes.
enum class TransformClipboardPart { All, Position, Rotation, Scale };
void DrawTransformValueClipboardMenu(EditorFrameContext& ctx, EntityID entity, TransformClipboardPart part);
bool CopyTransformToClipboard(EditorFrameContext& ctx, EntityID entity,
                              TransformClipboardPart part = TransformClipboardPart::All);
bool PasteTransformFromClipboard(EditorFrameContext& ctx, EntityID entity,
                                 TransformClipboardPart part = TransformClipboardPart::All);


// Nachbearbeitung nach dem Einfuegen; previous = Wert am Ziel davor (oder null).
template<typename T>
void FixupPastedComponent(T&, const T*) {}
template<>
inline void FixupPastedComponent<TransformComponent>(TransformComponent& t, const TransformComponent*)
{
    t.dirty = true;
    ++t.localVersion;
}
template<>
inline void FixupPastedComponent<CameraComponent>(CameraComponent& c, const CameraComponent* previous)
{
    // Hauptkamera bleibt, wie sie am Ziel war (nie zwei Hauptkameras).
    c.isMainCamera = previous ? previous->isMainCamera : false;
}
template<>
inline void FixupPastedComponent<ReflectionProbeComponent>(ReflectionProbeComponent& p,
                                                           const ReflectionProbeComponent* previous)
{
    // The target keeps its own baked file; a new probe gets its own id.
    p.probeId = previous ? previous->probeId : engine::addons::probes::GenerateProbeId();
    p.bakedDataPath = previous ? previous->bakedDataPath : std::string{};
}

template<typename T>
void CopyComponentToClipboard(EditorFrameContext& ctx, EntityID entity, const char* label)
{
    const T* source = ctx.world.Get<T>(entity);
    if (!source)
        return;
    auto copy = std::make_shared<T>(*source);
    ComponentClip& clip = ComponentClipboard();
    clip.typeId = ComponentTypeID<T>::Get();
    clip.label = label;
    clip.apply = [copy](EditorFrameContext& target, EntityID e, bool asNew) {
        if (T* existing = target.world.Get<T>(e))
        {
            T pasted = *copy;
            FixupPastedComponent<T>(pasted, existing);
            *existing = std::move(pasted);
            return true;
        }
        if (!asNew)
            return false;
        T added = *copy;
        FixupPastedComponent<T>(added, nullptr);
        target.world.Add<T>(e, std::move(added));
        return true;
    };
    clip.presentOn = [](EditorFrameContext& target, EntityID e) { return target.world.Has<T>(e); };
    ctx.lastFileMessage = TranslateWithValue("clipboard.copied", Translate(label));
}

template<>
inline void CopyComponentToClipboard<TransformComponent>(EditorFrameContext& ctx, EntityID entity, const char*)
{
    (void)CopyTransformToClipboard(ctx, entity);
}

template<typename T>
[[nodiscard]] bool ClipboardHolds() noexcept
{
    const ComponentClip& clip = ComponentClipboard();
    return clip.apply && clip.typeId == ComponentTypeID<T>::Get();
}

// Eintraege fuer das Rechtsklick-Menue einer Komponente.
template<typename T>
void DrawComponentClipboardMenuItems(EditorFrameContext& ctx, EntityID entity, const char* label)
{
    if (ImGui::MenuItem(TranslateLabel("clipboard.copy_component", "Komponente kopieren").c_str()))
        CopyComponentToClipboard<T>(ctx, entity, label);
    const bool canPaste = ClipboardHolds<T>();
    if (!canPaste)
        ImGui::BeginDisabled();
    if (ImGui::MenuItem(TranslateLabel("clipboard.paste_values", "Werte einfuegen").c_str()))
    {
        const ComponentClip clip = ComponentClipboard();
        ExecuteSceneMutation(ctx, "history.paste_component_values",
                             [&]() { clip.apply(ctx, entity, false); });
    }
    if (!canPaste)
        ImGui::EndDisabled();
}

// "Als neue Komponente einfuegen" (im Menue "Komponente hinzufuegen"):
// nur sichtbar, wenn die kopierte Komponente am Ziel noch fehlt.
void DrawPasteComponentAsNewItem(EditorFrameContext& ctx, EntityID entity);

// Transform teilweise (Position, Rotation, Skalierung einzeln).
void DrawTransformClipboardMenuItems(EditorFrameContext& ctx, EntityID entity);

// Scripts: eine Instanz (Klasse, Anzeigename, Feldwerte).
void DrawScriptClipboardMenuItems(EditorFrameContext& ctx, EntityID entity, size_t index);
void DrawPasteScriptAsNewItem(EditorFrameContext& ctx, EntityID entity);

} // namespace engine::renderer::addons::editor
