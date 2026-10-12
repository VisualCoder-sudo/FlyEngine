// Dear ImGui platform glue for raylib, matching the "custom input bridge + imgui_impl_opengl3"
// approach used by Flyengine.
//
// Rendering is handled by the OpenGL3 backend (imgui_impl_opengl3.cpp) which shares
// raylib's OpenGL 3.3 core context. This backend only bridges the *platform* side:
//   - feeds raylib input / window state into ImGuiIO every frame,
//   - manages the render flush so raylib's internal render batch never interleaves
//     with ImGui's raw GL calls.
//
// It has no dependency on GLFW or any windowing system, so it keeps working no
// matter which backend raylib itself was built with.
#pragma once

#include "imgui.h"

bool  ImGui_ImplRaylib_Init(void);                // Create/register context hooks. Call after the raylib window + GL context exist.
void  ImGui_ImplRaylib_NewFrame(void);            // Call once per frame, BEFORE ImGui::NewFrame()/widgets.
void  ImGui_ImplRaylib_RenderDrawData(ImDrawData* draw_data); // Flush raylib's batch, then render ImGui via OpenGL3.
void  ImGui_ImplRaylib_UpdateMouseCursor(void);   // Apply ImGui's requested cursor through raylib's SetMouseCursor().
                                                  // The app decides when to call it (e.g. only when io.WantCaptureMouse) so
                                                  // raylib-driven cursors (hand/ibeam over engine panels) keep working.
void  ImGui_ImplRaylib_Shutdown(void);