// =============================================================================
// KROM Engine - addons/editor/EditorProfilerWindow.cpp
// Window "Profiler": frame-time graph, CPU breakdown, GPU time per pass and
// scene counters in one dockable window, plus the one-line overlay in the
// viewport. GPU pass timing runs only while the GPU section is visible
// (EditorState::profilerGpuTiming); the app switches it in the RenderSystem.
// =============================================================================
#include "addons/editor/EditorUIInternal.hpp"

#ifdef KROM_EDITOR_HAS_IMGUI

namespace engine::renderer::addons::editor::detail {

namespace {

constexpr size_t kHistorySize = 240u;
constexpr float  kBudget60Ms  = 1000.f / 60.f;
constexpr float  kBudget30Ms  = 1000.f / 30.f;
// Time constant of the smoothing for the CPU breakdown, in seconds.
constexpr float  kSegmentSmoothingSeconds = 0.25f;

// Bits of EditorState::profilerSections.
enum ProfilerBits : uint32_t
{
    kSectionOverview = 1u << 0,
    kSectionCpu      = 1u << 1,
    kSectionGpu      = 1u << 2,
    kSectionScene    = 1u << 3,
    kOptionSortGpu   = 1u << 4,
    kSectionCpuDetails = 1u << 5,
};

// Unsmoothed frame times of the last kHistorySize frames (ring buffer).
struct FrameHistory
{
    std::array<float, kHistorySize> frameMs{};
    std::array<float, kHistorySize> gpuMs{};
    size_t next  = 0u;
    size_t count = 0u;

    void Push(float frame, float gpu)
    {
        frameMs[next] = frame;
        gpuMs[next]   = gpu;
        next  = (next + 1u) % kHistorySize;
        count = std::min(count + 1u, kHistorySize);
    }

    // index 0 = oldest stored sample
    [[nodiscard]] size_t Slot(size_t index) const
    {
        return (next + kHistorySize - count + index) % kHistorySize;
    }
};

enum Segment : size_t
{
    SegUpdate, SegAssets, SegViews, SegGizmos, SegParticles, SegEditorUi,
    SegPrepare, SegRecord, SegSubmit, SegPresent, SegOther, SegCount
};

struct SegmentInfo
{
    const char* labelKey;
    unsigned    color;   // sRGB, converted by theme::Hex
};

constexpr std::array<SegmentInfo, SegCount> kSegments{{
    { "profiler.seg_update",    0x4E9BD8 },
    { "profiler.seg_assets",    0x6C7BD9 },
    { "profiler.seg_views",     0x9B6BD3 },
    { "profiler.seg_gizmos",    0xC76BB5 },
    { "profiler.seg_particles", 0xD9736B },
    { "profiler.seg_editor_ui", 0x8A96A3 },
    { "profiler.seg_prepare",   0x3BBF7A },
    { "profiler.seg_record",    0xA6C94A },
    { "profiler.seg_submit",    0xE8C53A },
    { "profiler.seg_present",   0xE8943A },
    { "profiler.seg_other",     0x55606B },
}};

FrameHistory                 g_history;
std::array<float, SegCount>  g_segmentMs{};

ImVec4 BudgetColor(float frameMs)
{
    if (frameMs <= kBudget60Ms * 1.05f)
        return theme::kSuccess;
    return frameMs <= kBudget30Ms * 1.05f ? theme::kWarning : theme::kDanger;
}

float SmoothedFrameMs()
{
    const float fps = ImGui::GetIO().Framerate;
    return fps > 0.f ? 1000.f / fps : 0.f;
}

// GPU time of the whole frame: from the backend if it reports one, otherwise
// the sum of the measured passes. Negative: not available.
float GpuFrameMs(const EditorFrameContext& ctx)
{
    if (ctx.renderStats && ctx.renderStats->backendGpuFrameMs > 0.f)
        return ctx.renderStats->backendGpuFrameMs;
    if (ctx.gpuPassTimings.enabled && !ctx.gpuPassTimings.passes.empty())
        return ctx.gpuPassTimings.totalMs;
    return -1.f;
}

void UpdateSegments(const EditorFrameContext& ctx, const EditorUiTimings& ui, float deltaSeconds)
{
    std::array<float, SegCount> now{};
    now[SegUpdate]    = static_cast<float>(ctx.appCpuTimings[0]);
    now[SegAssets]    = static_cast<float>(ctx.appCpuTimings[1]);
    now[SegViews]     = static_cast<float>(ctx.appCpuTimings[2]);
    now[SegGizmos]    = static_cast<float>(ctx.appCpuTimings[3]);
    now[SegParticles] = static_cast<float>(ctx.appCpuTimings[4]);
    now[SegEditorUi]  = static_cast<float>(ui.materials + ui.menus + ui.gizmos + ui.assets +
                                           ui.hierarchy + ui.inspector + ui.preview);
    if (const RenderStats* s = ctx.renderStats)
    {
        now[SegPrepare] = s->prepareFrameMs;
        now[SegRecord]  = s->executeRecordMs;
        now[SegSubmit]  = s->executeSubmitMs;
        now[SegPresent] = s->executePresentMs;
    }
    // The render tick contains the editor UI and the four render stages; the
    // rest of it (frame begin, acquire, graph build, uploads) is "other".
    now[SegOther] = std::max(0.f, static_cast<float>(ctx.appCpuTimings[5]) - now[SegEditorUi] -
                                  now[SegPrepare] - now[SegRecord] - now[SegSubmit] - now[SegPresent]);

    const float blend = 1.f - std::exp(-std::max(deltaSeconds, 0.f) / kSegmentSmoothingSeconds);
    for (size_t i = 0u; i < SegCount; ++i)
        g_segmentMs[i] += (now[i] - g_segmentMs[i]) * blend;
}

bool SectionHeader(EditorState& state, uint32_t bit, const char* labelKey, const char* id, bool treeNode = false)
{
    ImGui::SetNextItemOpen((state.profilerSections & bit) != 0u, ImGuiCond_Always);
    const std::string label = std::string(Translate(labelKey)) + "###" + id;
    const bool open = treeNode ? ImGui::TreeNodeEx(label.c_str(), ImGuiTreeNodeFlags_NoTreePushOnOpen)
                               : ImGui::CollapsingHeader(label.c_str());
    state.profilerSections = open ? (state.profilerSections | bit) : (state.profilerSections & ~bit);
    return open;
}

void BigNumber(const char* label, const ImVec4& color, const char* format, float value)
{
    ImGui::TableNextColumn();
    ImGui::TextDisabled("%s", label);
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.6f);
    if (format)
        ImGui::TextColored(color, format, value);
    else
        ImGui::TextDisabled("-");
    ImGui::PopFont();
}

void DrawFrameGraph()
{
    const float scale = ImGui::GetStyle().FontScaleMain;
    const ImVec2 size(std::max(ImGui::GetContentRegionAvail().x, 80.f * scale), 96.f * scale);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + size.x, p0.y + size.y);
    ImGui::InvisibleButton("##profiler_graph", size);
    const bool hovered = ImGui::IsItemHovered();

    float peak = 0.f;
    float sum  = 0.f;
    bool  hasGpu = false;
    for (size_t i = 0u; i < g_history.count; ++i)
    {
        const size_t slot = g_history.Slot(i);
        peak = std::max(peak, g_history.frameMs[slot]);
        sum += g_history.frameMs[slot];
        hasGpu = hasGpu || g_history.gpuMs[slot] > 0.f;
    }
    const float top = std::max(kBudget30Ms * 1.2f, peak * 1.15f);
    const auto toY = [&](float ms) { return p1.y - std::clamp(ms / top, 0.f, 1.f) * size.y; };
    // Newest sample at the right edge.
    const float step = size.x / static_cast<float>(kHistorySize - 1u);
    const auto toX = [&](size_t index) {
        return p1.x - static_cast<float>(g_history.count - 1u - index) * step;
    };

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, p1, theme::U32(theme::kField), 3.f);
    dl->PushClipRect(p0, p1, true);
    const float budgets[] = { kBudget60Ms, kBudget30Ms };
    for (float budget : budgets)
    {
        const float y = toY(budget);
        dl->AddLine(ImVec2(p0.x, y), ImVec2(p1.x, y), theme::U32(theme::kBorder));
        char text[24];
        std::snprintf(text, sizeof(text), "%.1f ms", budget);
        dl->AddText(ImVec2(p0.x + 4.f, y - ImGui::GetFontSize() - 1.f), theme::U32(theme::kTextDisabled), text);
    }

    if (g_history.count >= 2u)
    {
        std::array<ImVec2, kHistorySize> points;
        if (hasGpu)
        {
            for (size_t i = 0u; i < g_history.count; ++i)
                points[i] = ImVec2(toX(i), toY(g_history.gpuMs[g_history.Slot(i)]));
            dl->AddPolyline(points.data(), static_cast<int>(g_history.count), theme::U32(theme::kWarning),
                            ImDrawFlags_None, 1.f);
        }
        for (size_t i = 0u; i < g_history.count; ++i)
            points[i] = ImVec2(toX(i), toY(g_history.frameMs[g_history.Slot(i)]));
        dl->AddPolyline(points.data(), static_cast<int>(g_history.count), theme::U32(theme::kAccentHover),
                        ImDrawFlags_None, 1.5f);
    }

    if (hovered && g_history.count > 0u)
    {
        const float fromRight = (p1.x - ImGui::GetIO().MousePos.x) / step;
        const size_t back = static_cast<size_t>(std::clamp(fromRight + 0.5f, 0.f,
                                                           static_cast<float>(g_history.count - 1u)));
        const size_t index = g_history.count - 1u - back;
        const size_t slot = g_history.Slot(index);
        const float x = toX(index);
        dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p1.y), theme::U32(theme::kTextSecondary));
        if (g_history.gpuMs[slot] > 0.f)
            ImGui::SetTooltip("Frame %.2f ms\nGPU %.2f ms", g_history.frameMs[slot], g_history.gpuMs[slot]);
        else
            ImGui::SetTooltip("Frame %.2f ms", g_history.frameMs[slot]);
    }
    dl->PopClipRect();

    ImGui::TextColored(theme::kAccentHover, "Frame");
    if (hasGpu)
    {
        ImGui::SameLine();
        ImGui::TextColored(theme::kWarning, "GPU");
    }
    if (g_history.count > 0u)
    {
        ImGui::SameLine();
        ImGui::TextDisabled(Translate("profiler.graph_stats"),
                            sum / static_cast<float>(g_history.count), peak);
    }
}

void DrawOverview(const EditorFrameContext& ctx)
{
    const float frameMs = SmoothedFrameMs();
    const float gpuMs = GpuFrameMs(ctx);
    if (ImGui::BeginTable("##profiler_numbers", 3, ImGuiTableFlags_SizingStretchSame))
    {
        const ImVec4 color = BudgetColor(frameMs);
        BigNumber("FPS", color, "%.0f", ImGui::GetIO().Framerate);
        BigNumber(Translate("profiler.frame_ms"), color, "%.1f", frameMs);
        BigNumber(Translate("profiler.gpu_ms"), theme::kText, gpuMs >= 0.f ? "%.1f" : nullptr, gpuMs);
        ImGui::EndTable();
    }
    DrawFrameGraph();
}

void DrawCpuDetails(const EditorFrameContext& ctx, const EditorUiTimings& ui)
{
    if (!ctx.renderStats)
        return;
    const RenderStats& s = *ctx.renderStats;
    ImGui::Text(Translate("perf.render"), ctx.appCpuTimings[5], s.backendBeginFrameMs, s.backendAcquireMs);
    ImGui::Text(Translate("perf.prepare"), s.prepareFrameMs, s.collectShadersMs, s.collectMaterialsMs);
    ImGui::Text(Translate("perf.cpu_execute"), s.executeMs);
    ImGui::Text(Translate("perf.opaque_setup"), s.opaqueCpu.setupMs);
    ImGui::Text(Translate("perf.material"), s.opaqueCpu.materialMs, s.opaqueCpu.materialBinds);
    ImGui::Text(Translate("perf.draw_calls"), s.opaqueCpu.drawMs, s.opaqueCpu.draws);
    for (const auto& pass : s.slowestPasses)
    {
        if (pass.name[0] != '\0')
            ImGui::Text("%s  %.1f ms", pass.name.data(), pass.cpuMs);
    }
    ImGui::Text(Translate("perf.build_graph"), s.buildGraphMs);
    ImGui::Text(Translate("perf.parallel"), s.parallelSectionMs, s.peakActiveWorkers);
    ImGui::Text("Upload       %5.1f ms  (%u KB)",
        s.collectUploadsMs + s.commitUploadsMs,
        static_cast<uint32_t>(s.uploadedBytes / 1024u));
    ImGui::Text(Translate("perf.inspector"), ui.inspector, ui.gizmos);
    ImGui::Text(Translate("perf.hierarchy"), ui.assets, ui.hierarchy);
    ImGui::Text(Translate("perf.materials"), ui.materials, ui.menus, ui.preview);
}

void DrawCpuSection(EditorFrameContext& ctx, const EditorUiTimings& ui)
{
    const float scale = ImGui::GetStyle().FontScaleMain;
    float total = 0.f;
    for (float ms : g_segmentMs)
        total += ms;

    // Stacked bar: one coloured piece per segment, width = share of the sum.
    const ImVec2 size(std::max(ImGui::GetContentRegionAvail().x, 80.f * scale), 18.f * scale);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##profiler_cpu_bar", size);
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, ImVec2(p0.x + size.x, p0.y + size.y), theme::U32(theme::kField), 3.f);
    if (total > 0.f)
    {
        const float mouseX = ImGui::GetIO().MousePos.x;
        float x = p0.x;
        for (size_t i = 0u; i < SegCount; ++i)
        {
            const float width = size.x * g_segmentMs[i] / total;
            if (width <= 0.f)
                continue;
            dl->AddRectFilled(ImVec2(x, p0.y), ImVec2(x + width, p0.y + size.y),
                              theme::U32(theme::Hex(kSegments[i].color)));
            if (hovered && mouseX >= x && mouseX < x + width)
                ImGui::SetTooltip("%s\n%.2f ms (%.0f %%)", Translate(kSegments[i].labelKey),
                                  g_segmentMs[i], 100.f * g_segmentMs[i] / total);
            x += width;
        }
    }

    // Legend with the values, two segments per row.
    const ImGuiTableFlags flags = ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("##profiler_cpu_legend", 4, flags))
    {
        const float valueWidth = 60.f * scale;
        ImGui::TableSetupColumn("##name0", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("##ms0", ImGuiTableColumnFlags_WidthFixed, valueWidth);
        ImGui::TableSetupColumn("##name1", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("##ms1", ImGuiTableColumnFlags_WidthFixed, valueWidth);
        const float box = ImGui::GetFontSize() * 0.7f;
        for (size_t i = 0u; i < SegCount; ++i)
        {
            ImGui::TableNextColumn();
            const ImVec2 cursor = ImGui::GetCursorScreenPos();
            const float top = cursor.y + (ImGui::GetTextLineHeight() - box) * 0.5f;
            ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(cursor.x, top), ImVec2(cursor.x + box, top + box),
                                                      theme::U32(theme::Hex(kSegments[i].color)), 2.f);
            ImGui::Dummy(ImVec2(box, box));
            ImGui::SameLine();
            ImGui::TextUnformatted(Translate(kSegments[i].labelKey));
            ImGui::TableNextColumn();
            ImGui::Text("%6.2f", g_segmentMs[i]);
        }
        ImGui::EndTable();
    }
    ImGui::TextColored(theme::kWarning, Translate("profiler.cpu_total"), total, SmoothedFrameMs());
    ImGui::TextDisabled("%s", Translate("profiler.cpu_note"));

    if (SectionHeader(ctx.state, kSectionCpuDetails, "profiler.details", "profiler_cpu_details", true))
    {
        ImGui::Indent();
        DrawCpuDetails(ctx, ui);
        ImGui::Unindent();
    }
}

void DrawGpuSection(EditorFrameContext& ctx)
{
    const GpuPassTimingReport& report = ctx.gpuPassTimings;
    if (!report.supported)
    {
        ImGui::TextWrapped("%s", Translate("gpu_timing.unsupported"));
        return;
    }
    if (report.passes.empty())
    {
        ImGui::TextDisabled("%s", Translate("gpu_timing.waiting"));
        return;
    }

    bool sortByTime = (ctx.state.profilerSections & kOptionSortGpu) != 0u;
    if (ImGui::Checkbox(Translate("gpu_timing.sort"), &sortByTime))
        ctx.state.profilerSections ^= kOptionSortGpu;

    std::vector<const GpuPassTiming*> rows;
    rows.reserve(report.passes.size());
    for (const GpuPassTiming& pass : report.passes)
        rows.push_back(&pass);
    if (sortByTime)
        std::stable_sort(rows.begin(), rows.end(),
                         [](const GpuPassTiming* a, const GpuPassTiming* b) { return a->ms > b->ms; });

    const float scale = ImGui::GetStyle().FontScaleMain;
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                  ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("##gpu_pass_table", 3, flags))
    {
        ImGui::TableSetupColumn(Translate("gpu_timing.pass"), ImGuiTableColumnFlags_WidthStretch, 1.f);
        ImGui::TableSetupColumn(Translate("gpu_timing.share"), ImGuiTableColumnFlags_WidthStretch, 1.f);
        ImGui::TableSetupColumn("ms", ImGuiTableColumnFlags_WidthFixed, 70.f * scale);
        ImGui::TableHeadersRow();
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, theme::kAccent);
        for (const GpuPassTiming* pass : rows)
        {
            const float share = report.totalMs > 0.f ? pass->ms / report.totalMs : 0.f;
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(pass->name.c_str());
            ImGui::TableSetColumnIndex(1);
            char overlay[16];
            std::snprintf(overlay, sizeof(overlay), "%.0f %%", share * 100.f);
            ImGui::ProgressBar(share, ImVec2(-FLT_MIN, ImGui::GetTextLineHeight()), overlay);
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%7.3f", pass->ms);
        }
        ImGui::PopStyleColor();
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextColored(theme::kWarning, "%s", Translate("gpu_timing.total"));
        ImGui::TableSetColumnIndex(2);
        ImGui::TextColored(theme::kWarning, "%7.3f", report.totalMs);
        ImGui::EndTable();
    }
    ImGui::PushTextWrapPos(0.f);
    ImGui::TextDisabled("%s", Translate("gpu_timing.note"));
    ImGui::PopTextWrapPos();
}

void DrawSceneSection(const EditorFrameContext& ctx)
{
    if (!ctx.renderStats)
        return;
    const RenderStats& s = *ctx.renderStats;
    const auto* lighting = ctx.frameData.GetFrameData<engine::addons::lighting::LightingFrameData>();
    const auto* shadow = ctx.frameData.GetFrameData<engine::addons::shadow::ShadowFrameData>();

    ImGui::Text(Translate("perf.draws"), s.opaqueDraws, s.shadowDraws, s.transparentDraws);
    ImGui::Text(Translate("perf.instancing"), s.instancedDraws, s.instancedObjects, s.instancingSavedDraws);
    if (lighting)
    {
        ImGui::Text(Translate("perf.lights"),
            lighting->extractedCount, lighting->gpu.lightCount, lighting->shadowCastingCount);
    }
    if (shadow)
    {
        uint32_t shadowViewCount = 0u;
        for (size_t requestIndex : shadow->currentRenderPath.requestIndices)
        {
            if (requestIndex < shadow->requests.size())
                shadowViewCount += static_cast<uint32_t>(shadow->requests[requestIndex].views.size());
        }
        ImGui::Text(Translate("perf.shadow_requests"),
            static_cast<uint32_t>(shadow->requests.size()),
            static_cast<uint32_t>(shadow->currentRenderPath.requestIndices.size()),
            shadowViewCount);
        ImGui::Text(Translate("perf.shadow_culled"), shadow->culledReceiverCount, shadow->culledBudgetCount);
    }
    ImGui::Text(Translate("perf.shadow_draws"), s.shadowPass.views, s.shadowPass.drawn, s.shadowPass.culled);
    ImGui::Text(Translate("perf.passes"), s.graphPassCount, s.graphTransitionCount);
}

} // namespace

void DrawProfilerWindow(EditorFrameContext& ctx, const EditorUiTimings& ui)
{
    // Recorded every frame, so the graph is already filled when the window opens.
    const ImGuiIO& io = ImGui::GetIO();
    g_history.Push(io.DeltaTime * 1000.f, ctx.renderStats ? ctx.renderStats->backendGpuFrameMs : 0.f);
    UpdateSegments(ctx, ui, io.DeltaTime);

    ctx.state.profilerGpuTiming = false;
    if (!ctx.state.showProfiler)
        return;

    const float scale = ImGui::GetStyle().FontScaleMain;
    ImGui::SetNextWindowSize(ImVec2(400.f * scale, 600.f * scale), ImGuiCond_FirstUseEver);
    const std::string title = std::string(Translate("profiler.title")) + "###profiler";
    auto& placement = ctx.state.profilerWindowPlacement;
    const ImGuiWindowFlags flags = PrepareEditorWindowPlacement(placement);
    if (ImGui::Begin(title.c_str(), &ctx.state.showProfiler, flags))
    {
        DrawEditorWindowSizeButton(placement);
        if (SectionHeader(ctx.state, kSectionOverview, "profiler.overview", "profiler_overview"))
            DrawOverview(ctx);
        if (SectionHeader(ctx.state, kSectionCpu, "profiler.cpu", "profiler_cpu"))
            DrawCpuSection(ctx, ui);
        if (SectionHeader(ctx.state, kSectionGpu, "profiler.gpu", "profiler_gpu"))
        {
            ctx.state.profilerGpuTiming = true;
            DrawGpuSection(ctx);
        }
        if (SectionHeader(ctx.state, kSectionScene, "profiler.scene", "profiler_scene"))
            DrawSceneSection(ctx);
    }
    ImGui::End();
}

void DrawPerfMiniOverlay(const EditorFrameContext& ctx)
{
    if (!ctx.state.showPerfOverlay)
        return;

    const ImGuiIO& io = ImGui::GetIO();
    const float pad = 8.f;
    const ImVec2 pos = ctx.sceneViewRectValid
        ? ImVec2(ctx.sceneViewX + ctx.sceneViewWidth - pad, ctx.sceneViewY + pad)
        : EditorWindowPos(io.DisplaySize.x - pad, (kEditorTopHeight * ImGui::GetStyle().FontScaleMain) + pad);
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always, ImVec2(1.f, 0.f));
    ImGui::SetNextWindowBgAlpha(0.55f);
    ImGui::SetNextWindowSize(ImVec2(0.f, 0.f)); // auto-size
    // Without NoBringToFrontOnFocus: ImGui creates such windows at the very back,
    // where the viewport panel covers them. NoInputs prevents any focus anyway.
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
        ImGuiWindowFlags_NoMove       | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing;

    KeepNextWindowInMainViewport("##perfoverlay");
    if (ImGui::Begin("##perfoverlay", nullptr, flags))
    {
        const float frameMs = SmoothedFrameMs();
        const float gpuMs = GpuFrameMs(ctx);
        ImGui::TextColored(BudgetColor(frameMs), "%.0f FPS", io.Framerate);
        ImGui::SameLine();
        ImGui::Text("%.1f ms", frameMs);
        if (gpuMs >= 0.f)
        {
            ImGui::SameLine();
            ImGui::TextDisabled("GPU %.1f ms", gpuMs);
        }
    }
    ImGui::End();
}

} // namespace engine::renderer::addons::editor::detail

#endif // KROM_EDITOR_HAS_IMGUI
