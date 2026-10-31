// =============================================================================
// KROM Engine - addons/editor/EditorParticleEditor.cpp
// Partikeleditor-Fenster und gemeinsame Effekt-Felder (siehe Header).
// =============================================================================
#include "addons/editor/EditorParticleEditor.hpp"
#include "addons/editor/EditorFeature.hpp"
#include "addons/particles/ParticleEffect.hpp"
#include "assets/AssetPipeline.hpp"
#include "core/Debug.hpp"
#include "core/PathUtf8.hpp"

#ifdef KROM_EDITOR_HAS_IMGUI
#include "imgui.h"
#endif

#include <algorithm>
#include <cstdio>
#include <filesystem>

namespace engine::renderer::addons::editor {

std::unordered_set<std::string>& DirtyParticleEffects()
{
    static std::unordered_set<std::string> dirty;
    return dirty;
}

void OpenParticleEditor(EditorFrameContext& ctx, const std::string& effectPath)
{
    if (effectPath.empty())
        return;
    if (ctx.state.particleEditorPath != effectPath)
        ctx.state.particlePreviewRestart = true;
    ctx.state.particleEditorPath = effectPath;
    ctx.state.particleEditorOpen = true;
    ctx.state.particleEditorFocusRequest = true;
}

#ifdef KROM_EDITOR_HAS_IMGUI

void DrawParticleEffectFields(EditorFrameContext& ctx, ParticleEmitterComponent& fx)
{
    // Emission
    ImGui::SeparatorText("Emission");
    ImGui::DragFloat("Rate (pro s)##pe_rate", &fx.emitRate, 0.5f, 0.f, 10000.f, "%.1f");
    int burst = static_cast<int>(fx.burstCount);
    if (ImGui::DragInt("Burst beim Start##pe_burst", &burst, 1.f, 0, 100000))
        fx.burstCount = static_cast<uint32_t>(std::max(burst, 0));
    int maxParticles = static_cast<int>(fx.maxParticles);
    if (ImGui::DragInt("Max. Partikel##pe_max", &maxParticles, 10.f, 1, 100000))
        fx.maxParticles = static_cast<uint32_t>(std::max(maxParticles, 1));
    ImGui::DragFloat("Lebensdauer (s)##pe_life", &fx.particleLifetime, 0.01f, 0.01f, 60.f, "%.2f");
    ImGui::SliderFloat("Lebensdauer-Zufall##pe_liferand", &fx.lifetimeRandom, 0.f, 1.f, "%.2f");
    ImGui::DragFloat("Radius##pe_radius", &fx.emitterRadius, 0.01f, 0.f, 100.f, "%.2f");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Partikel entstehen zufaellig in einer Kugel um den Ursprung.");

    // Bewegung
    ImGui::SeparatorText("Bewegung");
    ImGui::DragFloat3("Startgeschwindigkeit##pe_vel", &fx.initialVelocity.x, 0.05f, -1000.f, 1000.f, "%.2f");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("In Ausrichtung des Objekts (dreht mit).");
    ImGui::DragFloat3("Geschw.-Zufall (+/-)##pe_velrand", &fx.velocityRandom.x, 0.05f, 0.f, 1000.f, "%.2f");
    ImGui::DragFloat3("Schwerkraft##pe_gravity", &fx.gravity.x, 0.05f, -1000.f, 1000.f, "%.2f");
    ImGui::DragFloat("Luftwiderstand##pe_drag", &fx.drag, 0.01f, 0.f, 50.f, "%.2f");

    // Boden
    ImGui::SeparatorText("Boden");
    ImGui::Checkbox("Bodenkollision##pe_floor", &fx.collideWithFloor);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Partikel prallen an einer waagrechten Ebene ab (im Viewport als oranges Gitter).");
    if (fx.collideWithFloor)
    {
        ImGui::Checkbox("Relativ zum Emitter##pe_floor_rel", &fx.floorRelative);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("An: Hoehe als Abstand zum Emitter (wandert mit). Aus: feste Welt-Hoehe (Y).");
        ImGui::DragFloat(fx.floorRelative ? "Hoehe (relativ)##pe_floor_h" : "Hoehe (Welt-Y)##pe_floor_h",
                         &fx.floorHeight, 0.01f, -1000.f, 1000.f, "%.2f m");
        ImGui::Checkbox("Beim Aufprall verschwinden##pe_floor_die", &fx.dieOnFloor);
        if (!fx.dieOnFloor)
        {
            ImGui::SliderFloat("Abprallen##pe_bounce", &fx.bounce, 0.f, 1.f, "%.2f");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("0 = bleibt liegen, 1 = springt voll zurueck.");
            ImGui::SliderFloat("Reibung##pe_floor_friction", &fx.floorFriction, 0.f, 1.f, "%.2f");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Bremst die seitliche Bewegung bei jedem Aufprall.");
        }
    }

    // Aussehen
    ImGui::SeparatorText("Aussehen");
    int blend = fx.blendMode == ParticleBlendMode::Additive ? 1 : 0;
    if (ImGui::Combo("Mischung##pe_blend", &blend, "Alpha (Rauch, Staub)\0Additiv (Feuer, Funken)\0"))
        fx.blendMode = blend == 1 ? ParticleBlendMode::Additive : ParticleBlendMode::Alpha;
    ImGui::DragFloat("Startgroesse##pe_size0", &fx.startSize, 0.005f, 0.f, 100.f, "%.3f");
    ImGui::DragFloat("Endgroesse##pe_size1", &fx.endSize, 0.005f, 0.f, 100.f, "%.3f");
    ImGui::ColorEdit4("Startfarbe##pe_col0", &fx.startColor.x);
    ImGui::ColorEdit4("Endfarbe##pe_col1", &fx.endColor.x);
    ImGui::DragFloat("Intensitaet##pe_intensity", &fx.intensity, 0.05f, 0.f, 100.f, "%.2f");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Werte ueber 1 leuchten (Bloom).");

    // Textur: Textfeld + Drop-Ziel fuer Bilder aus dem Asset-Browser.
    ImGui::SeparatorText("Textur");
    char textureBuffer[512];
    std::snprintf(textureBuffer, sizeof(textureBuffer), "%s", fx.texturePath.c_str());
    ImGui::SetNextItemWidth(-1.f);
    if (ImGui::InputText("##pe_texture", textureBuffer, sizeof(textureBuffer)))
        fx.texturePath = textureBuffer;
    if (ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("KROM_ASSET_TEXTURE"))
        {
            if (const char* path = static_cast<const char*>(p->Data); path && path[0] != '\0')
            {
                const std::filesystem::path dropped = PathFromUtf8(path).lexically_normal();
                fx.texturePath = ctx.assetPipeline ? ctx.assetPipeline->ToAssetRelative(dropped)
                                                   : PathToGenericUtf8(dropped);
            }
        }
        ImGui::EndDragDropTarget();
    }
    if (fx.texturePath.empty())
        ImGui::TextDisabled("Keine - weiche Scheibe. Bild aus dem Asset-Browser hierher ziehen.");
    else if (ImGui::SmallButton("Textur entfernen##pe_tex_clear"))
        fx.texturePath.clear();

    int columns = static_cast<int>(fx.flipbookColumns);
    int rows = static_cast<int>(fx.flipbookRows);
    ImGui::SetNextItemWidth(ImGui::CalcItemWidth() * 0.5f);
    if (ImGui::DragInt("##pe_fb_cols", &columns, 0.1f, 1, 64))
        fx.flipbookColumns = static_cast<uint32_t>(std::clamp(columns, 1, 64));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::CalcItemWidth() * 0.5f);
    if (ImGui::DragInt("Flipbook (Spalten x Zeilen)##pe_fb_rows", &rows, 0.1f, 1, 64))
        fx.flipbookRows = static_cast<uint32_t>(std::clamp(rows, 1, 64));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Bild aus mehreren Einzelbildern (Zeile fuer Zeile).\n1 x 1 = ganzes Bild.");
    if (fx.flipbookColumns * fx.flipbookRows > 1u)
    {
        ImGui::Checkbox("Zufaelliges Einzelbild##pe_fb_random", &fx.flipbookRandomFrame);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("An: jedes Partikel zeigt ein festes, zufaelliges Bild (z. B. Rauchwolken).\n"
                              "Aus: die Bilder laufen ueber die Lebensdauer ab (z. B. Explosion).");
    }
}

namespace {

bool s_editPending = false;   // Feld geaendert, Neustart nach dem Loslassen

// Vorschau-Bild mit Orbit (rechte Maustaste) und Zoom (Mausrad), wie beim Material.
void DrawParticlePreview(EditorFrameContext& ctx, float size)
{
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const ImVec2 end{start.x + size, start.y + size};

    ImGui::InvisibleButton("##particlePreviewImage", ImVec2(size, size));
    if (ImGui::IsItemHovered())
    {
        const ImGuiIO& io = ImGui::GetIO();
        if (io.MouseWheel != 0.f)
            ctx.state.particlePreviewDistance =
                std::clamp(ctx.state.particlePreviewDistance - io.MouseWheel * 0.5f, 1.f, 30.f);
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Right))
        {
            const ImVec2 delta = io.MouseDelta;
            ctx.state.particlePreviewYawDeg += delta.x * 0.35f;
            ctx.state.particlePreviewPitchDeg =
                std::clamp(ctx.state.particlePreviewPitchDeg + delta.y * 0.35f, -85.f, 85.f);
        }
        ImGui::SetTooltip("Rechte Maustaste: drehen, Mausrad: Abstand");
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(start, end, IM_COL32(30, 31, 34, 255));
    void* texId = ctx.particlePreviewTexture ? ctx.particlePreviewTexture() : nullptr;
    if (texId)
    {
        const ImVec2 uv0 = ctx.particlePreviewFlipY ? ImVec2(0.f, 1.f) : ImVec2(0.f, 0.f);
        const ImVec2 uv1 = ctx.particlePreviewFlipY ? ImVec2(1.f, 0.f) : ImVec2(1.f, 1.f);
        dl->AddImage(reinterpret_cast<ImTextureID>(texId), start, end, uv0, uv1);
    }
    else
    {
        const char* label = "Keine Vorschau";
        const ImVec2 textSize = ImGui::CalcTextSize(label);
        dl->AddText(ImVec2(start.x + (size - textSize.x) * 0.5f, start.y + (size - textSize.y) * 0.5f),
                    IM_COL32(170, 170, 170, 255), label);
    }
    dl->AddRect(start, end, IM_COL32(95, 98, 104, 255));
    char info[64];
    std::snprintf(info, sizeof(info), "%u Partikel", ctx.particlePreviewCount);
    dl->AddText(ImVec2(start.x + 6.f, end.y - ImGui::GetTextLineHeight() - 4.f),
                IM_COL32(200, 200, 200, 220), info);
}

} // namespace

void DrawParticleEditorWindow(EditorFrameContext& ctx)
{
    if (!ctx.state.particleEditorOpen)
        return;

    ImGui::SetNextWindowSize(ImVec2(900.f, 560.f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(560.f, 360.f), ImVec2(2400.f, 1600.f));
    if (ctx.state.particleEditorFocusRequest)
    {
        ImGui::SetNextWindowFocus();
        ImGui::SetNextWindowCollapsed(false);
    }
    const bool visible = ImGui::Begin("Partikeleditor##editor", &ctx.state.particleEditorOpen);
    ctx.state.particleEditorFocusRequest = false;
    if (!visible)
    {
        ImGui::End();
        return;
    }

    const std::string path = ctx.state.particleEditorPath;
    particles::ParticleEffectLibrary* library = ctx.particleEffects;
    ParticleEmitterComponent* fx = library ? library->FindMutable(path) : nullptr;

    ImGui::Text("Datei: %s", path.c_str());
    if (!fx)
    {
        ImGui::TextColored(ImVec4(1.f, 0.6f, 0.3f, 1.f), "Effekt nicht ladbar (Datei fehlt oder ist kaputt).");
        if (library && ImGui::Button("Neu laden"))
            library->Reload(path);
        ImGui::End();
        return;
    }

    const bool dirty = DirtyParticleEffects().count(path) != 0u;
    if (ImGui::Button(dirty ? "Speichern *" : "Speichern"))
    {
        std::string error;
        if (library->Save(path, &error))
            DirtyParticleEffects().erase(path);
        else
            Debug::LogError("Particles: Effekt nicht gespeichert (%s)", error.c_str());
    }
    ImGui::SameLine();
    if (ImGui::Button("Neu laden"))
    {
        library->Reload(path);
        DirtyParticleEffects().erase(path);
        ctx.state.particlePreviewRestart = true;
        fx = library->FindMutable(path);
        if (!fx)
        {
            ImGui::End();
            return;
        }
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Datei neu einlesen, ungespeicherte Aenderungen verwerfen.");
    ImGui::SameLine();
    if (ImGui::Button("Neu starten"))
        ctx.state.particlePreviewRestart = true;
    ImGui::SameLine();
    if (ImGui::Button("Pfad kopieren"))
        ImGui::SetClipboardText(path.c_str());
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Fuer Scripts: ctx.SpawnEffect(\"%s\", position)", path.c_str());
    ImGui::SameLine();
    ImGui::Checkbox("Bei Aenderung neu starten", &ctx.state.particlePreviewRestartOnEdit);
    ImGui::Separator();

    // Links Vorschau, rechts die Felder (eigener Scrollbereich).
    const float available = ImGui::GetContentRegionAvail().x;
    const float previewSize = std::clamp(std::min(available * 0.5f, ImGui::GetContentRegionAvail().y),
                                         200.f, 720.f);
    ImGui::BeginGroup();
    DrawParticlePreview(ctx, previewSize);
    ImGui::EndGroup();
    ImGui::SameLine();

    ImGui::BeginChild("##particle_fields", ImVec2(0.f, 0.f));
    const ParticleEmitterComponent before = *fx;
    ImGui::SeparatorText("Wiedergabe");
    ImGui::Checkbox("Schleife##pe_loop", &fx->looping);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Aus: einmaliger Effekt (SpawnEffect spielt ihn immer einmal ab).\n"
                          "Die Vorschau wiederholt ihn trotzdem.");
    if (!fx->looping)
        ImGui::DragFloat("Dauer (s)##pe_duration", &fx->duration, 0.05f, 0.f, 600.f, "%.2f");
    DrawParticleEffectFields(ctx, *fx);
    ImGui::EndChild();

    if (!(*fx == before))
    {
        DirtyParticleEffects().insert(path);
        s_editPending = true;
    }
    // Erst nach dem Loslassen neu starten, nicht bei jedem Zieh-Schritt.
    if (s_editPending && !ImGui::IsAnyItemActive())
    {
        s_editPending = false;
        if (ctx.state.particlePreviewRestartOnEdit)
            ctx.state.particlePreviewRestart = true;
    }

    ImGui::End();
}

#else

void DrawParticleEffectFields(EditorFrameContext&, ParticleEmitterComponent&) {}
void DrawParticleEditorWindow(EditorFrameContext&) {}

#endif

} // namespace engine::renderer::addons::editor
