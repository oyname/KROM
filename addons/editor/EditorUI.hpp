#pragma once

namespace engine::renderer::addons::editor {

struct EditorFrameContext;

// Zeichnet die eingebauten Editor-Panels (Entity-Liste + Transform-Inspector).
// Wird pro Frame vom EditorPass aufgerufen, bevor der optionale drawUI-Callback laeuft.
// Benoetigt imgui.h im Include-Pfad (addons/editor/imgui/).
void DrawEditorPanels(EditorFrameContext& ctx);

// Traegt Debug-Linien fuer das selektierte Entity in ctx.debugDraw ein
// (Licht-Gizmos, OBB-Drahtgitter, Handle-Marker).
// Muss VOR dem DebugDraw-Pass aufgerufen werden (Frame-Build-Phase, vor ImGui::NewFrame),
// da DebugDraw vor dem UI-Pass geflusht wird. Kein ImGui-Input hier.
void DrawLightDebugLines(EditorFrameContext& ctx);

} // namespace engine::renderer::addons::editor
