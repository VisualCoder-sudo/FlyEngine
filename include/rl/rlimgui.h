#pragma once
// Dear ImGui integration for the sokol backend (sokol_imgui.h).
//
// Replaces imgui_impl_raylib + imgui_impl_opengl3. Window events reach ImGui
// through the raylib event hook (SetEventHook), and ImGui draws into the same
// offscreen main target as everything else, so it composes with raylib-style
// drawing exactly as before.

// Creates the ImGui context (like ImGui::CreateContext) and the sokol renderer.
void rlImGuiSetup(void);
void rlImGuiShutdown(void);
// Starts an ImGui frame for the current window size (calls ImGui::NewFrame()).
void rlImGuiNewFrame(float deltaTime);
// Applies ImGui's requested mouse cursor (call when io.WantCaptureMouse).
void rlImGuiUpdateMouseCursor(void);
// Renders ImGui::GetDrawData() into the current frame (call between
// BeginDrawing and EndDrawing, after ImGui::Render()).
void rlImGuiRender(void);
