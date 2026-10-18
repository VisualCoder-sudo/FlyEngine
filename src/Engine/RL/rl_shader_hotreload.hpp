#pragma once
// Watches shaders/*.glsl for edits and hot-swaps the changed program(s) in
// place (see rl_shader_hotreload.cpp for how). Call TickShaderHotReload()
// once a frame (done from PollEvents() in rl_core.cpp); SetShaderHotReloadEnabled()
// backs the public SetShaderHotReload() in raylib.h.
namespace rli {

void SetShaderHotReloadEnabled(bool enabled);
void TickShaderHotReload();

} // namespace rli
