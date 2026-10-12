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

} // namespace rli
