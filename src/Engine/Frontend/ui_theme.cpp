// Dear ImGui styling: the dark palette and the accent re-skin.
#include "ui_internal.hpp"

namespace ui {

// Points every accent-coloured ImGui style entry at theme::ACCENT. No-op until
// the ImGui context exists; ApplyImGuiTheme calls it again when it is created.
void ApplyAccentToImGuiStyle() {
    if (!ImGui::GetCurrentContext()) return;
    ImVec4* c = ImGui::GetStyle().Colors;
    const ImVec4 hover(theme::ACCENT_HOVER.r / 255.0f, theme::ACCENT_HOVER.g / 255.0f,
                       theme::ACCENT_HOVER.b / 255.0f, 1.0f);
    c[ImGuiCol_TextSelectedBg]     = theme::AccentVec(64 / 255.0f);
    c[ImGuiCol_CheckMark]          = theme::AccentVec();
    c[ImGuiCol_SliderGrab]         = theme::AccentVec();
    c[ImGuiCol_SliderGrabActive]   = hover;
    c[ImGuiCol_SeparatorActive]    = theme::AccentVec();
    c[ImGuiCol_TabHovered]         = theme::AccentVec(100 / 255.0f);
    c[ImGuiCol_TabActive]          = theme::AccentVec(55 / 255.0f);
    c[ImGuiCol_TabUnfocusedActive] = theme::AccentVec(35 / 255.0f);
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

    // Mirrors the app's Dark Theme palette in theme:: (BG_*/TEXT_*/ACCENT...)
    auto rgba = [](int r, int g, int b, int a = 255) -> ImVec4 {
        return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f);
    };

    ImVec4* c = s.Colors;
    c[ImGuiCol_Text]                 = rgba(232, 232, 238);   // theme::TEXT
    c[ImGuiCol_TextDisabled]         = rgba(122, 128, 140);   // theme::TEXT_DIM
    c[ImGuiCol_TextSelectedBg]       = rgba(0, 190, 200, 64); // theme::ACCENT_SOFT
    c[ImGuiCol_WindowBg]             = rgba(30, 32, 38);      // theme::BG_PANEL
    c[ImGuiCol_ChildBg]              = rgba(30, 32, 38);
    c[ImGuiCol_PopupBg]              = rgba(35, 37, 43, 250); // theme::BG_MENU
    c[ImGuiCol_Border]               = rgba(72, 77, 88);      // theme::BORDER
    c[ImGuiCol_BorderShadow]         = rgba(0, 0, 0, 60);
    c[ImGuiCol_FrameBg]              = rgba(21, 23, 28);      // theme::BG_INPUT
    c[ImGuiCol_FrameBgHovered]       = rgba(27, 29, 36);      // theme::BG_INPUT_HOVER
    c[ImGuiCol_FrameBgActive]        = rgba(40, 43, 51);      // theme::BG_WIDGET_PRESSED
    c[ImGuiCol_TitleBg]              = rgba(38, 41, 48);      // theme::BG_TITLE
    c[ImGuiCol_TitleBgActive]        = rgba(38, 41, 48);
    c[ImGuiCol_TitleBgCollapsed]     = rgba(25, 27, 32);      // theme::BG_TOP_BAR
    c[ImGuiCol_MenuBarBg]            = rgba(25, 27, 32);
    c[ImGuiCol_ScrollbarBg]          = rgba(21, 23, 28, 180);
    c[ImGuiCol_ScrollbarGrab]        = rgba(72, 77, 88);
    c[ImGuiCol_ScrollbarGrabHovered] = rgba(104, 110, 122);   // theme::BORDER_STRONG
    c[ImGuiCol_ScrollbarGrabActive]  = rgba(122, 128, 140);
    c[ImGuiCol_CheckMark]            = rgba(0, 190, 200);     // theme::ACCENT
    c[ImGuiCol_SliderGrab]           = rgba(0, 190, 200);
    c[ImGuiCol_SliderGrabActive]     = rgba(40, 210, 220);    // theme::ACCENT_HOVER
    c[ImGuiCol_Button]               = rgba(48, 51, 60);      // theme::BG_WIDGET
    c[ImGuiCol_ButtonHovered]        = rgba(63, 67, 78);      // theme::BG_WIDGET_HOVER
    c[ImGuiCol_ButtonActive]         = rgba(40, 43, 51);
    c[ImGuiCol_Header]               = rgba(48, 51, 60);
    c[ImGuiCol_HeaderHovered]        = rgba(63, 67, 78);
    c[ImGuiCol_HeaderActive]         = rgba(40, 43, 51);
    c[ImGuiCol_Separator]            = rgba(56, 60, 70);      // theme::DIVIDER
    c[ImGuiCol_SeparatorHovered]     = rgba(72, 77, 88);
    c[ImGuiCol_SeparatorActive]      = rgba(0, 190, 200);
    c[ImGuiCol_ResizeGrip]           = rgba(56, 60, 70);
    c[ImGuiCol_ResizeGripHovered]    = rgba(72, 77, 88);
    c[ImGuiCol_ResizeGripActive]     = rgba(104, 110, 122);
    c[ImGuiCol_Tab]                  = rgba(35, 37, 43);
    c[ImGuiCol_TabHovered]           = rgba(0, 190, 200, 100);
    c[ImGuiCol_TabActive]            = rgba(0, 190, 200, 55);
    c[ImGuiCol_TabUnfocused]         = rgba(35, 37, 43);
    c[ImGuiCol_TabUnfocusedActive]   = rgba(0, 190, 200, 35);
    c[ImGuiCol_TableHeaderBg]        = rgba(38, 41, 48);
    c[ImGuiCol_TableBorderStrong]    = rgba(72, 77, 88);
    c[ImGuiCol_TableBorderLight]     = rgba(56, 60, 70);
    c[ImGuiCol_TableRowBg]           = rgba(30, 32, 38);
    c[ImGuiCol_TableRowBgAlt]        = rgba(35, 37, 43);
    c[ImGuiCol_PlotLines]            = rgba(0, 190, 200);
    c[ImGuiCol_PlotHistogram]        = rgba(0, 190, 200);
    c[ImGuiCol_ModalWindowDimBg]     = rgba(0, 0, 0, 110);

    ApplyAccentToImGuiStyle();
}

} // namespace ui
