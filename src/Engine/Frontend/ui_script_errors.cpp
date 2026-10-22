// ui_script_errors.cpp: the script build panel (compiling / failed with diagnostics), drawn with raylib.
#include "ui_internal.hpp"
#include "../../../include/Engine/Scripts/NativeScriptHost.hpp"
#include "../../../include/Engine/Scripts/ScriptLauncher.hpp"
#include "../../../include/Engine/Scripts/ScriptRuntime.hpp"

#include <algorithm>
#include <string>

namespace ui {

// Cuts `text` to fit `maxWidth`, ending in "..." when it had to be shortened.
static std::string FitTextToWidth(const std::string& text, float fontSize, float maxWidth) {
    if (MeasureTextArial(text.c_str(), fontSize) <= maxWidth) return text;
    std::string out = text;
    while (!out.empty() && MeasureTextArial((out + "...").c_str(), fontSize) > maxWidth) out.pop_back();
    return out + "...";
}

void DrawScriptErrorPanel() {
    Rectangle panel = GetScriptPanelBounds();
    if (panel.width <= 0.0f) return;

    constexpr float textSize = 12.0f;
    const Vector2 mouse = GetMousePosition();
    const bool failed = g_scriptBuildState == ScriptBuildState::Failed;

    int errors = 0, warnings = 0;
    for (const auto& d : g_scriptDiagnostics) {
        if (d.severity == ScriptDiagnostic::Severity::Error) ++errors;
        else if (d.severity == ScriptDiagnostic::Severity::Warning) ++warnings;
    }

    DrawRectangleRec({ panel.x + 3.0f, panel.y + 3.0f, panel.width, panel.height }, theme::SHADOW);
    DrawRectangleRec(panel, Color{ 20, 21, 26, 240 });
    DrawRectangleLinesEx(panel, 1.0f, failed ? theme::DANGER : theme::BORDER_STRONG);

    // --- header -----------------------------------------------------------
    std::string title;
    if (!failed) {
        title = "COMPILING SCRIPTS...";
    } else {
        title = "SCRIPT BUILD FAILED - " + std::to_string(errors) + (errors == 1 ? " error" : " errors");
        if (warnings > 0) title += ", " + std::to_string(warnings) + (warnings == 1 ? " warning" : " warnings");
    }
    DrawTextArial(title.c_str(), panel.x + 10.0f, panel.y + 6.0f, 13.0f, failed ? theme::DANGER : theme::ACCENT);

    if (failed) {
        // [Rebuild] and [X] on the right
        Rectangle closeBtn = { panel.x + panel.width - 24.0f, panel.y + 4.0f, 18.0f, 18.0f };
        Rectangle rebuildBtn = { closeBtn.x - 70.0f, panel.y + 4.0f, 64.0f, 18.0f };

        const bool rebuildHover = CheckCollisionPointRec(mouse, rebuildBtn);
        const float rebuildT = HoverProgress(9700, rebuildHover);
        DrawRectangleRounded(rebuildBtn, 0.2f, 4, Mix(theme::BG_WIDGET, theme::ACCENT_PRESSED, rebuildT));
        DrawRectangleLinesEx(rebuildBtn, 1.0f, Mix(theme::BORDER, theme::ACCENT, rebuildT));
        DrawCenteredTextArial("Rebuild", rebuildBtn, 12.0f, rebuildHover ? theme::TEXT : theme::TEXT_MUTED);
        if (rebuildHover) MarkHand();
        if (rebuildHover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            if (ScriptRuntime* rt = GetActiveRuntime())
                if (auto* host = rt->GetHost()) host->RequestScriptRebuild();
        }

        const bool closeHover = CheckCollisionPointRec(mouse, closeBtn);
        const float closeT = HoverProgress(9701, closeHover);
        DrawRectangleRounded(closeBtn, 0.2f, 4, Mix(theme::BG_WIDGET, theme::DANGER, closeT));
        DrawRectangleLinesEx(closeBtn, 1.0f, Mix(theme::BORDER, theme::DANGER_HOVER, closeT));
        DrawCenteredTextArial("X", closeBtn, 12.0f, closeHover ? theme::TEXT : theme::TEXT_MUTED);
        if (closeHover) MarkHand();
        if (closeHover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) g_scriptPanelDismissed = true;
    }
    DrawLine(static_cast<int>(panel.x + 6.0f), static_cast<int>(panel.y + kScriptPanelHeaderH),
             static_cast<int>(panel.x + panel.width - 6.0f), static_cast<int>(panel.y + kScriptPanelHeaderH), theme::DIVIDER);
    if (!failed) return;

    // --- rows -------------------------------------------------------------
    const int total = static_cast<int>(g_scriptDiagnostics.size());
    const int visible = std::min(kScriptPanelMaxRows, total);
    const int maxScroll = std::max(0, total - visible);
    const bool overPanel = CheckCollisionPointRec(mouse, panel);
    const float wheel = GetMouseWheelMove();
    if (wheel != 0.0f && overPanel) g_scriptPanelScroll -= static_cast<int>(wheel * 2.0f);
    g_scriptPanelScroll = std::clamp(g_scriptPanelScroll, 0, maxScroll);

    const float rowsTop = panel.y + kScriptPanelHeaderH + 2.0f;
    const float textRight = panel.x + panel.width - (maxScroll > 0 ? 16.0f : 10.0f);

    for (int i = 0; i < visible; ++i) {
        const ScriptDiagnostic& d = g_scriptDiagnostics[static_cast<size_t>(g_scriptPanelScroll + i)];
        const Rectangle row = { panel.x + 4.0f, rowsTop + static_cast<float>(i) * kScriptPanelRowH,
                                panel.width - 8.0f, kScriptPanelRowH };
        const bool clickable = !d.file.empty();
        const bool hover = clickable && CheckCollisionPointRec(mouse, row);
        if (hover) {
            DrawRectangleRec(row, theme::BG_ROW_HOVER);
            MarkHand();
        }

        Color sevColor = theme::TEXT_MUTED;
        const char* sev = "note";
        if (d.severity == ScriptDiagnostic::Severity::Error) { sevColor = theme::DANGER; sev = "error"; }
        else if (d.severity == ScriptDiagnostic::Severity::Warning) { sevColor = Color{ 226, 178, 70, 255 }; sev = "warning"; }
        const bool isNote = d.severity == ScriptDiagnostic::Severity::Note;
        const float ty = row.y + (row.height - textSize) * 0.5f;

        float x = row.x + 6.0f + (isNote ? 14.0f : 0.0f);
        DrawTextArial(sev, x, ty, textSize, sevColor);
        x += 52.0f;

        if (clickable) {
            std::string where = FileName(d.file);
            if (d.line > 0) where += ":" + std::to_string(d.line);
            DrawTextArial(where.c_str(), x, ty, textSize, theme::ACCENT);
            x += MeasureTextArial(where.c_str(), textSize) + 10.0f;
        }
        const std::string msg = FitTextToWidth(d.message, textSize, textRight - x);
        DrawTextArial(msg.c_str(), x, ty, textSize, isNote ? theme::TEXT_DIM : theme::TEXT);

        if (hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
            scriptLauncher::RequestOpenAt(d.file, d.line, d.column);
    }

    if (maxScroll > 0) {
        const float sbX = panel.x + panel.width - 10.0f;
        const float sbH = static_cast<float>(visible) * kScriptPanelRowH;
        DrawRectangle(static_cast<int>(sbX), static_cast<int>(rowsTop), 4, static_cast<int>(sbH), Color{ 255, 255, 255, 18 });
        float thumbH = std::max(12.0f, sbH * static_cast<float>(visible) / static_cast<float>(total));
        const float thumbY = rowsTop + (sbH - thumbH) * (static_cast<float>(g_scriptPanelScroll) / static_cast<float>(maxScroll));
        DrawRectangle(static_cast<int>(sbX), static_cast<int>(thumbY), 4, static_cast<int>(thumbH), theme::BORDER);
    }
}

} // namespace ui
