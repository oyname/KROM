#pragma once
// =============================================================================
// KROM Engine - addons/editor/EditorParticleEditor.hpp
// Partikeleditor fuer .particle-Assets (eigenes Fenster mit Vorschau, wie der
// Materialeditor) und die gemeinsamen Effekt-Felder fuer Inspector und Editor.
// Die Vorschau rendert die App in ein eigenes Render-Target
// (EditorFrameContext::particlePreviewTexture).
// =============================================================================
#include "addons/particles/ParticleComponents.hpp"

#include <string>
#include <unordered_set>

namespace engine::renderer::addons::editor {

struct EditorFrameContext;

// Effekt-Felder ab "Emission" (Wiedergabe-Zeile zeichnet der Aufrufer).
void DrawParticleEffectFields(EditorFrameContext& ctx, ParticleEmitterComponent& fx);

// Asset-Pfade mit Aenderungen, die noch nicht in der .particle-Datei stehen.
[[nodiscard]] std::unordered_set<std::string>& DirtyParticleEffects();

// Oeffnet (oder fokussiert) den Partikeleditor fuer ein Asset relativ zum Asset-Root.
void OpenParticleEditor(EditorFrameContext& ctx, const std::string& effectPath);
void DrawParticleEditorWindow(EditorFrameContext& ctx);

} // namespace engine::renderer::addons::editor
