// Dear ImGui styling: the dark palette and the accent re-skin.
#include "ui_internal.hpp"

namespace ui {

// Points every accent-coloured ImGui style entry at theme::ACCENT. No-op until
// the ImGui context exists; ApplyImGuiTheme calls it again when it is created.
void ApplyAccentToImGuiStyle() {
    if (!ImGui::GetCurrentContext()) return;
    ImVec4* c = ImGui::GetStyle().Colors;
    const ImVec4 hover = theme::Vec(theme::ACCENT_HOVER, 255);
    c[ImGuiCol_TextSelectedBg]     = theme::Vec(theme::ACCENT, 64);
    c[ImGuiCol_CheckMark]          = theme::AccentVec();
    c[ImGuiCol_SliderGrab]         = theme::AccentVec();
    c[ImGuiCol_SliderGrabActive]   = hover;
    c[ImGuiCol_SeparatorActive]    = theme::AccentVec();
    c[ImGuiCol_TabHovered]         = theme::Vec(theme::ACCENT, 100);
    c[ImGuiCol_TabActive]          = theme::Vec(theme::ACCENT, 55);
    c[ImGuiCol_TabUnfocusedActive] = theme::Vec(theme::ACCENT, 35);
    c[ImGuiCol_PlotLines]          = theme::AccentVec();
    c[ImGuiCol_PlotHistogram]      = theme::AccentVec();
}

// ---------------------------------------------------------------------------
// Dear ImGui integration
// ---------------------------------------------------------------------------

void ApplyImGuiTheme() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding     = 6.0f;
    s.PopupRounding      = 4.0f;
    s.FrameRounding      = 4.0f;
    s.ScrollbarRounding  = 4.0f;
    s.GrabRounding       = 4.0f;
    s.TabRounding        = 4.0f;
    s.WindowBorderSize   = 1.0f;
    s.WindowPadding      = ImVec2(8.0f, 8.0f);
    s.FramePadding       = ImVec2(6.0f, 4.0f);
    s.ItemSpacing        = ImVec2(6.0f, 5.0f);
    s.ItemInnerSpacing   = ImVec2(6.0f, 4.0f);
    s.ScrollbarSize      = 13.0f;

    // Every colour comes from theme:: so the ImGui panels and the raylib-drawn ones stay in step.
    // Accent-coloured entries are set by ApplyAccentToImGuiStyle below.
    using theme::Vec;
    ImVec4* c = s.Colors;
    c[ImGuiCol_Text]                 = Vec(theme::TEXT);
    c[ImGuiCol_TextDisabled]         = Vec(theme::TEXT_DIM);
    c[ImGuiCol_WindowBg]             = Vec(theme::BG_PANEL);
    c[ImGuiCol_ChildBg]              = Vec(theme::BG_PANEL);
    c[ImGuiCol_PopupBg]              = Vec(theme::BG_MENU);
    c[ImGuiCol_Border]               = Vec(theme::BORDER);
    c[ImGuiCol_BorderShadow]         = Vec(theme::SHADOW, 60);
    c[ImGuiCol_FrameBg]              = Vec(theme::BG_INPUT);
    c[ImGuiCol_FrameBgHovered]       = Vec(theme::BG_INPUT_HOVER);
    c[ImGuiCol_FrameBgActive]        = Vec(theme::BG_WIDGET_PRESSED);
    c[ImGuiCol_TitleBg]              = Vec(theme::BG_TITLE);
    c[ImGuiCol_TitleBgActive]        = Vec(theme::BG_TITLE);
    c[ImGuiCol_TitleBgCollapsed]     = Vec(theme::BG_TOP_BAR);
    c[ImGuiCol_MenuBarBg]            = Vec(theme::BG_TOP_BAR);
    c[ImGuiCol_ScrollbarBg]          = Vec(theme::BG_INPUT, 180);
    c[ImGuiCol_ScrollbarGrab]        = Vec(theme::BORDER);
    c[ImGuiCol_ScrollbarGrabHovered] = Vec(theme::BORDER_STRONG);
    c[ImGuiCol_ScrollbarGrabActive]  = Vec(theme::TEXT_DIM);
    c[ImGuiCol_Button]               = Vec(theme::BG_WIDGET);
    c[ImGuiCol_ButtonHovered]        = Vec(theme::BG_WIDGET_HOVER);
    c[ImGuiCol_ButtonActive]         = Vec(theme::BG_WIDGET_PRESSED);
    c[ImGuiCol_Header]               = Vec(theme::BG_WIDGET);
    c[ImGuiCol_HeaderHovered]        = Vec(theme::BG_WIDGET_HOVER);
    c[ImGuiCol_HeaderActive]         = Vec(theme::BG_WIDGET_PRESSED);
    c[ImGuiCol_Separator]            = Vec(theme::DIVIDER);
    c[ImGuiCol_SeparatorHovered]     = Vec(theme::BORDER);
    c[ImGuiCol_ResizeGrip]           = Vec(theme::DIVIDER);
    c[ImGuiCol_ResizeGripHovered]    = Vec(theme::BORDER);
    c[ImGuiCol_ResizeGripActive]     = Vec(theme::BORDER_STRONG);
    c[ImGuiCol_Tab]                  = Vec(theme::BG_MENU, 255);
    c[ImGuiCol_TabUnfocused]         = Vec(theme::BG_MENU, 255);
    c[ImGuiCol_TableHeaderBg]        = Vec(theme::BG_TITLE);
    c[ImGuiCol_TableBorderStrong]    = Vec(theme::BORDER);
    c[ImGuiCol_TableBorderLight]     = Vec(theme::DIVIDER);
    c[ImGuiCol_TableRowBg]           = Vec(theme::BG_PANEL);
    c[ImGuiCol_TableRowBgAlt]        = Vec(theme::BG_MENU, 255);
    c[ImGuiCol_ModalWindowDimBg]     = Vec(theme::SHADOW, 110);

    ApplyAccentToImGuiStyle();
}

} // namespace ui
