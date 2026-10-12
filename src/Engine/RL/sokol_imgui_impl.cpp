// sokol_imgui implementation and the rlimgui.h glue.
#include "imgui.h"

#include "sokol_app.h"
#include "sokol_gfx.h"
#include "sokol_log.h"
#define SOKOL_IMGUI_IMPL
#include "util/sokol_imgui.h"

#include "rl_internal.hpp"
#include "rlimgui.h"

using namespace rli;

namespace {

bool g_imguiActive = false;

bool ForwardEvent(const void* ev) {
    return g_imguiActive && simgui_handle_event((const sapp_event*)ev);
}

} // namespace

void rlImGuiSetup(void) {
    if (g_imguiActive) return;
    simgui_desc_t desc{};
    desc.color_format = SG_PIXELFORMAT_RGBA8;    // main target format
    desc.depth_format = SG_PIXELFORMAT_DEPTH;
    desc.sample_count = 1;
    desc.ini_filename = nullptr;
    // The caller adds its own fonts, and the engine owns the cursor shape
    // (rlImGuiUpdateMouseCursor() hands it to ImGui only over ImGui windows).
    desc.no_default_font = true;
    desc.disable_set_mouse_cursor = true;
    desc.logger.func = slog_func;
    simgui_setup(&desc);
    g_imguiActive = true;
    SetEventHook(ForwardEvent);
}

void rlImGuiShutdown(void) {
    if (!g_imguiActive) return;
    SetEventHook(nullptr);
    simgui_shutdown();
    g_imguiActive = false;
}

void rlImGuiNewFrame(float deltaTime) {
    if (!g_imguiActive) return;
    simgui_frame_desc_t fd{};
    fd.width = GetScreenWidth();
    fd.height = GetScreenHeight();
    fd.delta_time = deltaTime > 0.0f ? deltaTime : 1.0f / 60.0f;
    fd.dpi_scale = 1.0f;
    simgui_new_frame(&fd);
}

void rlImGuiRender(void) {
    if (!g_imguiActive) return;
    GfxState& g = Gfx();
    BatchFlush();
    if (g.currentTarget != g.mainTarget) {
        EndPass();
        g.currentTarget = g.mainTarget;
    }
    EnsurePass();
    if (!g.passActive) return;
    simgui_render();
    // sokol_imgui leaves its own scissor rect applied.
    ApplyScissor();
}

void rlImGuiUpdateMouseCursor(void) {
    if (!g_imguiActive) return;
    switch (ImGui::GetMouseCursor()) {
        case ImGuiMouseCursor_TextInput: SetMouseCursor(MOUSE_CURSOR_IBEAM); break;
        case ImGuiMouseCursor_ResizeAll: SetMouseCursor(MOUSE_CURSOR_RESIZE_ALL); break;
        case ImGuiMouseCursor_ResizeNS: SetMouseCursor(MOUSE_CURSOR_RESIZE_NS); break;
        case ImGuiMouseCursor_ResizeEW: SetMouseCursor(MOUSE_CURSOR_RESIZE_EW); break;
        case ImGuiMouseCursor_ResizeNESW: SetMouseCursor(MOUSE_CURSOR_RESIZE_NESW); break;
        case ImGuiMouseCursor_ResizeNWSE: SetMouseCursor(MOUSE_CURSOR_RESIZE_NWSE); break;
        case ImGuiMouseCursor_Hand: SetMouseCursor(MOUSE_CURSOR_POINTING_HAND); break;
        case ImGuiMouseCursor_NotAllowed: SetMouseCursor(MOUSE_CURSOR_NOT_ALLOWED); break;
        default: SetMouseCursor(MOUSE_CURSOR_DEFAULT); break;
    }
}

unsigned long long GetTextureImGuiId(Texture2D texture) {
    TextureRec* t = GetTextureForBinding(texture.id);
    if (!t) return 0;
    return (unsigned long long)simgui_imtextureid_with_sampler(t->view, GetSamplerFor(*t, false));
}
