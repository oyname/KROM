// =============================================================================
// KROM Engine - addons/editor/EditorGpuTimingWindow.cpp
// Window "GPU-Zeit pro Pass": one row per render-graph pass with the smoothed
// GPU time, plus the sum. The measurement runs only while the window is open
// (EditorState::showGpuPassTiming); the app switches it in the RenderSystem.
// =============================================================================
#include "addons/editor/EditorUIInternal.hpp"

#ifdef KROM_EDITOR_HAS_IMGUI

namespace engine::renderer::addons::editor::detail {

void DrawGpuPassTimingWindow(EditorFrameContext& ctx)
{
    if (!ctx.state.showGpuPassTiming)
        return;

    ImGui::SetNextWindowSize(ImVec2(320.f * ImGui::GetStyle().FontScaleMain, 360.f * ImGui::GetStyle().FontScaleMain), ImGuiCond_FirstUseEver);
    bool open = true;
    const std::string title = std::string(Translate("gpu_timing.title")) + "###gpu_pass_timing";
    if (ImGui::Begin(title.c_str(), &open))
    {
        const GpuPassTimingReport& report = ctx.gpuPassTimings;
        if (!report.supported)
        {
            ImGui::TextWrapped("%s", Translate("gpu_timing.unsupported"));
        }
        else if (report.passes.empty())
        {
            ImGui::TextDisabled("%s", Translate("gpu_timing.waiting"));
        }
        else
        {
            const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                          ImGuiTableFlags_SizingStretchProp;
            if (ImGui::BeginTable("##gpu_pass_table", 2, flags))
            {
                ImGui::TableSetupColumn(Translate("gpu_timing.pass"), ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("ms", ImGuiTableColumnFlags_WidthFixed, 70.f * ImGui::GetStyle().FontScaleMain);
                ImGui::TableHeadersRow();
                for (const GpuPassTiming& pass : report.passes)
                {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(pass.name.c_str());
                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%7.3f", pass.ms);
                }
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextColored(ImVec4(1.f, 0.85f, 0.2f, 1.f), "%s", Translate("gpu_timing.total"));
                ImGui::TableSetColumnIndex(1);
                ImGui::TextColored(ImVec4(1.f, 0.85f, 0.2f, 1.f), "%7.3f", report.totalMs);
                ImGui::EndTable();
            }
            const ImGuiIO& io = ImGui::GetIO();
            ImGui::TextDisabled(Translate("gpu_timing.frame"), io.Framerate > 0.f ? 1000.f / io.Framerate : 0.f);
            ImGui::TextDisabled("%s", Translate("gpu_timing.note"));
        }
    }
    ImGui::End();
    if (!open)
        ctx.state.showGpuPassTiming = false;
}

} // namespace engine::renderer::addons::editor::detail

#endif // KROM_EDITOR_HAS_IMGUI
