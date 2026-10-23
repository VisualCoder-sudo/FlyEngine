#pragma once
// Registry of shader programs compiled offline by sokol-shdc.
//
// shaders/*.glsl are compiled at build time into headers holding the
// sg_shader_desc for every backend plus reflection functions. The build also
// generates shaders_registry.cpp, which lists every program with the names of
// its uniforms, textures and vertex attributes (scraped from those reflection
// functions), so uniforms can still be looked up by name at runtime.
#include "sokol_gfx.h"

#include <cstddef>

namespace rli {

struct ProgramInfo {
    const char* name;
    const sg_shader_desc* (*desc)(sg_backend backend);
    int (*uniformblockSlot)(const char* ubName);
    size_t (*uniformblockSize)(const char* ubName);
    int (*uniformOffset)(const char* ubName, const char* uName);
    sg_glsl_shader_uniform (*uniformDesc)(const char* ubName, const char* uName);
    int (*textureSlot)(const char* texName);
    int (*attrSlot)(const char* attrName);
    const char* const* uniforms;   // { ub0, name0, ub1, name1, ..., nullptr }
    const char* const* textures;   // { tex0, tex1, ..., nullptr }
    const char* const* attrs;      // { attr0, attr1, ..., nullptr }
};

// Defined in the generated shaders_registry.cpp.
const ProgramInfo* FindProgram(const char* name);

// Hot-reload override: once set, FindProgramLive() returns `info` instead of
// the build-time entry for `name`, until ClearProgramOverride(). `info.name`
// and the uniforms/textures/attrs name arrays must stay valid for as long as
// the override is installed (rl_shader_hotreload.cpp keeps them pointing at
// the original, build-time ProgramInfo's arrays, since those names don't
// change across a hot reload).
void SetProgramOverride(const char* name, const ProgramInfo& info);
void ClearProgramOverride(const char* name);

// FindProgram(), but returns a hot-reload override if one is installed.
const ProgramInfo* FindProgramLive(const char* name);

// Drops the cached sg_shader for `name` (destroying it) so the next
// LoadProgram()/ProgramShader() call rebuilds it from FindProgramLive().
// Returns false (keeping the old shader) if the new program does not compile or
// changed the uniform/texture/attribute layout live Shaders were built with.
bool InvalidateProgramShader(const char* name);

} // namespace rli
