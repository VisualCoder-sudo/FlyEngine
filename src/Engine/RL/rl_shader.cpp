// raylib shader API on sokol-shdc programs: uniforms by name, emulated
// texture units.
#include "rl_internal.hpp"
#include "rl_programs.hpp"
#include "rlgl.h"

#include <algorithm>
#include <cstring>
#include <string>

namespace rli {

const char* const kAttribNames[ATTR_COUNT] = {
    "vertexPosition", "vertexTexCoord", "vertexNormal", "vertexColor", "vertexTangent", "vertexTexCoord2",
    "instanceTransform0", "instanceTransform1", "instanceTransform2", "instanceTransform3",
};

namespace {

constexpr int kMaxShaderLocations = 32;   // RL_MAX_SHADER_LOCATIONS

// One sg_shader per program, shared by every Shader loaded from it (each
// Shader still has its own uniform values, like separate GL programs would).
std::unordered_map<std::string, sg_shader> g_programShaders;

int UniformTypeSize(int sgType) {
    switch (sgType) {
        case SG_UNIFORMTYPE_FLOAT: case SG_UNIFORMTYPE_INT: return 4;
        case SG_UNIFORMTYPE_FLOAT2: case SG_UNIFORMTYPE_INT2: return 8;
        case SG_UNIFORMTYPE_FLOAT3: case SG_UNIFORMTYPE_INT3: return 12;
        case SG_UNIFORMTYPE_FLOAT4: case SG_UNIFORMTYPE_INT4: return 16;
        case SG_UNIFORMTYPE_MAT4: return 64;
        default: return 0;
    }
}

int RaylibTypeSize(int type) {
    switch (type) {
        case SHADER_UNIFORM_FLOAT: case SHADER_UNIFORM_INT: case SHADER_UNIFORM_UINT:
        case SHADER_UNIFORM_SAMPLER2D: return 4;
        case SHADER_UNIFORM_VEC2: case SHADER_UNIFORM_IVEC2: case SHADER_UNIFORM_UIVEC2: return 8;
        case SHADER_UNIFORM_VEC3: case SHADER_UNIFORM_IVEC3: case SHADER_UNIFORM_UIVEC3: return 12;
        case SHADER_UNIFORM_VEC4: case SHADER_UNIFORM_IVEC4: case SHADER_UNIFORM_UIVEC4: return 16;
        default: return 0;
    }
}

int FindUniform(const ShaderRec& sh, const std::string& name) {
    for (size_t i = 0; i < sh.uniforms.size(); ++i) {
        if (sh.uniforms[i].name == name) return (int)i;
    }
    return -1;
}

// Writes `count` source elements of `srcSize` bytes into a uniform-block
// target, following std140 array rules (array elements are 16-byte aligned).
void WriteTarget(std::vector<uint8_t>& block, const UniformTarget& t, const uint8_t* src, int srcSize, int count) {
    const int elemSize = UniformTypeSize(t.type);
    if (elemSize == 0 || srcSize <= 0) return;
    const int maxElems = t.arrayCount > 0 ? t.arrayCount : 1;
    const int stride = t.arrayCount > 0 ? ((elemSize + 15) / 16) * 16 : elemSize;
    const size_t limit = std::min(block.size(), (size_t)t.offset + (size_t)stride * (size_t)maxElems);
    if ((size_t)t.offset >= limit) return;

    const size_t total = (size_t)srcSize * (size_t)count;
    if (t.arrayCount == 0) {
        // Scalar/vector/matrix: copy what fits (e.g. 4 floats into a vec4).
        std::memcpy(block.data() + t.offset, src, std::min(total, limit - (size_t)t.offset));
        return;
    }
    if (srcSize == stride || (srcSize < elemSize && elemSize == stride)) {
        // Layout already matches (e.g. vec4[] from vec4s, or ivec4[] filled with ints).
        std::memcpy(block.data() + t.offset, src, std::min(total, limit - (size_t)t.offset));
        return;
    }
    for (int i = 0; i < count && i < maxElems; ++i) {
        const size_t dst = (size_t)t.offset + (size_t)i * (size_t)stride;
        const size_t n = std::min((size_t)std::min(srcSize, elemSize), limit - dst);
        if (dst >= limit) break;
        std::memcpy(block.data() + dst, src + (size_t)i * (size_t)srcSize, n);
    }
}

sg_shader ProgramShader(const ProgramInfo* prog) {
    auto it = g_programShaders.find(prog->name);
    if (it != g_programShaders.end()) return it->second;
    const sg_shader_desc* desc = prog->desc(sg_query_backend());
    sg_shader shd{};
    if (desc) {
        shd = sg_make_shader(desc);
        if (sg_query_shader_state(shd) != SG_RESOURCESTATE_VALID) {
            TraceLog(LOG_WARNING, "SHADER: [%s] failed to compile for %s", prog->name, GetGraphicsBackendName());
        }
    } else {
        TraceLog(LOG_WARNING, "SHADER: [%s] has no code for %s", prog->name, GetGraphicsBackendName());
    }
    g_programShaders[prog->name] = shd;
    return shd;
}

} // namespace

std::vector<ShaderRec>& Shaders() {
    static std::vector<ShaderRec> s(1);
    return s;
}

ShaderRec* GetShader(unsigned int id) {
    auto& s = Shaders();
    if (id == 0 || id >= s.size() || !s[id].alive) return nullptr;
    return &s[id];
}

unsigned int LoadProgram(const char* name) {
    const ProgramInfo* prog = name ? FindProgram(name) : nullptr;
    if (!prog) {
        TraceLog(LOG_WARNING, "SHADER: program '%s' is not compiled into this build", name ? name : "(null)");
        return 0;
    }
    auto& shaders = Shaders();
    unsigned int id = 0;
    for (size_t i = 1; i < shaders.size(); ++i) {
        if (!shaders[i].alive) { id = (unsigned int)i; break; }
    }
    if (id == 0) {
        shaders.emplace_back();
        id = (unsigned int)shaders.size() - 1;
    }
    ShaderRec& sh = shaders[id];
    sh = ShaderRec{};
    sh.program = prog;
    sh.shader = ProgramShader(prog);
    const sg_shader_desc* desc = prog->desc(sg_query_backend());

    // Uniforms (a name may live in both the vertex and fragment blocks).
    for (const char* const* p = prog->uniforms; p && p[0]; p += 2) {
        const char* ub = p[0];
        const char* un = p[1];
        const int slot = prog->uniformblockSlot(ub);
        if (slot < 0 || slot >= SG_MAX_UNIFORMBLOCK_BINDSLOTS) continue;
        if (sh.ubData[slot].empty()) sh.ubData[slot].assign(prog->uniformblockSize(ub), 0);
        const sg_glsl_shader_uniform ud = prog->uniformDesc(ub, un);
        UniformTarget t;
        t.ub = slot;
        t.offset = prog->uniformOffset(ub, un);
        t.type = (int)ud.type;
        t.arrayCount = ud.array_count;
        int idx = FindUniform(sh, un);
        if (idx < 0) {
            sh.uniforms.emplace_back();
            sh.uniforms.back().name = un;
            idx = (int)sh.uniforms.size() - 1;
        }
        ShaderUniform& u = sh.uniforms[idx];
        if (u.targetCount < 2) u.targets[u.targetCount++] = t;
    }

    // Textures, with their sampler slot and sampler type from the desc.
    for (const char* const* p = prog->textures; p && p[0]; ++p) {
        ShaderUniform u;
        u.name = *p;
        u.isTexture = true;
        u.viewSlot = prog->textureSlot(*p);
        if (desc) {
            for (int i = 0; i < SG_MAX_TEXTURE_SAMPLER_PAIRS; ++i) {
                const sg_shader_texture_sampler_pair& pair = desc->texture_sampler_pairs[i];
                if (pair.stage == SG_SHADERSTAGE_NONE) continue;
                if (pair.view_slot == u.viewSlot) {
                    u.samplerSlot = pair.sampler_slot;
                    u.compare = desc->samplers[pair.sampler_slot].sampler_type == SG_SAMPLERTYPE_COMPARISON;
                    u.nonfiltering = desc->samplers[pair.sampler_slot].sampler_type == SG_SAMPLERTYPE_NONFILTERING;
                    break;
                }
            }
            if (u.viewSlot >= 0 && u.viewSlot < SG_MAX_VIEW_BINDSLOTS)
                u.volume = desc->views[u.viewSlot].texture.image_type == SG_IMAGETYPE_3D;
        }
        if (u.name == "texture1") u.unit = 1;
        if (u.name == "texture2") u.unit = 2;
        sh.uniforms.push_back(u);
    }

    for (int a = 0; a < ATTR_COUNT; ++a) {
        sh.attrSlot[a] = prog->attrSlot(kAttribNames[a]);
        if (sh.attrSlot[a] >= 0) sh.attrMask |= 1u << a;
    }

    // Default locations, as raylib's LoadShader() assigns them.
    sh.locs = (int*)RL_MALLOC(kMaxShaderLocations * sizeof(int));
    for (int i = 0; i < kMaxShaderLocations; ++i) sh.locs[i] = -1;
    auto attr = [&](const char* n) { const int s = prog->attrSlot(n); return s; };
    auto uni = [&](const char* n) { return FindUniform(sh, n); };
    sh.locs[SHADER_LOC_VERTEX_POSITION] = attr("vertexPosition");
    sh.locs[SHADER_LOC_VERTEX_TEXCOORD01] = attr("vertexTexCoord");
    sh.locs[SHADER_LOC_VERTEX_TEXCOORD02] = attr("vertexTexCoord2");
    sh.locs[SHADER_LOC_VERTEX_NORMAL] = attr("vertexNormal");
    sh.locs[SHADER_LOC_VERTEX_TANGENT] = attr("vertexTangent");
    sh.locs[SHADER_LOC_VERTEX_COLOR] = attr("vertexColor");
    sh.locs[SHADER_LOC_VERTEX_INSTANCETRANSFORM] = sh.attrSlot[ATTR_INSTANCE0];
    sh.locs[SHADER_LOC_MATRIX_MVP] = uni("mvp");
    sh.locs[SHADER_LOC_MATRIX_VIEW] = uni("matView");
    sh.locs[SHADER_LOC_MATRIX_PROJECTION] = uni("matProjection");
    sh.locs[SHADER_LOC_MATRIX_MODEL] = uni("matModel");
    sh.locs[SHADER_LOC_MATRIX_NORMAL] = uni("matNormal");
    sh.locs[SHADER_LOC_COLOR_DIFFUSE] = uni("colDiffuse");
    sh.locs[SHADER_LOC_MAP_DIFFUSE] = uni("texture0");
    sh.locs[SHADER_LOC_MAP_SPECULAR] = uni("texture1");
    sh.locs[SHADER_LOC_MAP_NORMAL] = uni("texture2");
    sh.locLinearTarget = uni("flyLinearTarget");

    // Uniform blocks start zeroed; colDiffuse defaults to white like GL's
    // "uniform not set" path would leave it after raylib's first draw.
    const float white[4] = { 1, 1, 1, 1 };
    if (sh.locs[SHADER_LOC_COLOR_DIFFUSE] >= 0) SetUniformValue(sh, sh.locs[SHADER_LOC_COLOR_DIFFUSE], white, SHADER_UNIFORM_VEC4, 1);

    sh.alive = true;
    return id;
}

void ShadersShutdown() {
    auto& shaders = Shaders();
    for (ShaderRec& sh : shaders) {
        if (sh.locs) RL_FREE(sh.locs);
        sh = ShaderRec{};
    }
    shaders.assign(1, ShaderRec{});
    // sg_shutdown() releases the sg_shader objects themselves.
    g_programShaders.clear();
}

void SetUniformMatrix(ShaderRec& sh, int loc, const Matrix& m) {
    if (loc < 0 || loc >= (int)sh.uniforms.size()) return;
    const float16 f = MatrixToFloatV(m);
    ShaderUniform& u = sh.uniforms[loc];
    for (int i = 0; i < u.targetCount; ++i) {
        WriteTarget(sh.ubData[u.targets[i].ub], u.targets[i], (const uint8_t*)f.v, 64, 1);
    }
}

void SetUniformValue(ShaderRec& sh, int loc, const void* value, int type, int count) {
    if (loc < 0 || loc >= (int)sh.uniforms.size() || !value) return;
    ShaderUniform& u = sh.uniforms[loc];
    if (u.isTexture) {
        // A sampler uniform holds the texture unit it reads from.
        if (type == SHADER_UNIFORM_INT || type == SHADER_UNIFORM_SAMPLER2D) {
            const int unit = *(const int*)value;
            if (unit >= 0 && unit < GfxState::kMaxUnits) u.unit = unit;
        }
        return;
    }
    const int size = RaylibTypeSize(type);
    for (int i = 0; i < u.targetCount; ++i) {
        WriteTarget(sh.ubData[u.targets[i].ub], u.targets[i], (const uint8_t*)value, size, std::max(1, count));
    }
}

void ResolveShaderTextures(const ShaderRec& sh, const unsigned int* drawUnits, sg_bindings& bind) {
    GfxState& g = Gfx();
    for (const ShaderUniform& u : sh.uniforms) {
        if (!u.isTexture || u.viewSlot < 0 || u.samplerSlot < 0) continue;
        unsigned int id = u.explicitTexture;
        if (!id && drawUnits) id = drawUnits[u.unit];
        if (!id) id = g.units[u.unit];
        TextureRec* t = GetTextureForBinding(id);
        // A depth texture is read through a comparison sampler (shadow maps) or,
        // for its raw values, a nearest one (@sampler_type nonfiltering).
        bool usable = t && !IsBoundAsAttachment(id) && t->volume == u.volume && (!t->attachment || t->rendered);
        if (usable) usable = u.compare ? t->depth : (!t->depth || u.nonfiltering);
        if (!usable) {
            t = GetTextureForBinding(u.volume ? g.dummyVolumeTexture : u.compare ? g.dummyDepthTexture : g.whiteTexture);
        }
        if (!t) continue;
        bind.views[u.viewSlot] = t->view;
        bind.samplers[u.samplerSlot] = u.nonfiltering
            ? GetSampler(TEXTURE_FILTER_POINT, t->depth ? TEXTURE_WRAP_CLAMP : t->wrap, false, false)
            : GetSamplerFor(*t, u.compare);
    }
}

void SetLinearTargetUniform(ShaderRec& sh) {
    if (sh.locLinearTarget < 0) return;
    const GfxState& g = Gfx();
    const float linear = (g.currentTarget < g.targets.size() && g.targets[g.currentTarget].linear) ? 1.0f : 0.0f;
    SetUniformValue(sh, sh.locLinearTarget, &linear, SHADER_UNIFORM_FLOAT, 1);
}

void ApplyShaderUniforms(const ShaderRec& sh) {
    for (int i = 0; i < SG_MAX_UNIFORMBLOCK_BINDSLOTS; ++i) {
        if (!sh.ubData[i].empty()) {
            sg_apply_uniforms(i, sg_range{ sh.ubData[i].data(), sh.ubData[i].size() });
        }
    }
}

} // namespace rli

using namespace rli;

// ---------------------------------------------------------------------------
// raylib shader API
// ---------------------------------------------------------------------------
Shader LoadShaderProgram(const char* programName) {
    Shader s{};
    s.id = LoadProgram(programName);
    if (s.id == 0) {
        // Fall back to the default shader, as raylib does for a failed load.
        s.id = LoadProgram("rl_default");
    }
    ShaderRec* sh = GetShader(s.id);
    s.locs = sh ? sh->locs : nullptr;
    return s;
}

// Shader files are compiled into the binary; the program is named after the
// vertex shader's file stem (shaders/water.vert -> "water").
Shader LoadShader(const char* vsFileName, const char* fsFileName) {
    const char* path = vsFileName ? vsFileName : fsFileName;
    if (!path || !*path) return LoadShaderProgram("rl_default");
    std::string stem = GetFileName(path);
    const size_t dot = stem.find('.');
    if (dot != std::string::npos) stem = stem.substr(0, dot);
    return LoadShaderProgram(stem.c_str());
}

Shader LoadShaderFromMemory(const char*, const char*) {
    TraceLog(LOG_WARNING, "SHADER: runtime GLSL compilation is not available; use LoadShaderProgram()");
    return LoadShaderProgram("rl_default");
}

bool IsShaderValid(Shader shader) {
    ShaderRec* sh = GetShader(shader.id);
    return sh && sh->shader.id != 0 && sg_query_shader_state(sh->shader) == SG_RESOURCESTATE_VALID;
}

void UnloadShader(Shader shader) {
    ShaderRec* sh = GetShader(shader.id);
    if (!sh || shader.id == Gfx().defaultShader || shader.id == Gfx().blitShader) return;
    if (sh->locs) RL_FREE(sh->locs);
    *sh = ShaderRec{};
}

int GetShaderLocation(Shader shader, const char* uniformName) {
    ShaderRec* sh = GetShader(shader.id);
    if (!sh || !uniformName) return -1;
    std::string name = uniformName;
    int idx = FindUniform(*sh, name);
    if (idx >= 0) return idx;
    // GLSL array syntax: "tileSize[0]" -> "tileSize", "albedoTex[2]" -> "albedoTex2"
    // (sokol-shdc has no texture arrays, so those are declared as numbered textures).
    const size_t br = name.find('[');
    if (br != std::string::npos) {
        const std::string base = name.substr(0, br);
        const std::string index = name.substr(br + 1, name.find(']') - br - 1);
        if (index == "0") {
            idx = FindUniform(*sh, base);
            if (idx >= 0) return idx;
        }
        idx = FindUniform(*sh, base + index);
        if (idx >= 0) return idx;
    }
    return -1;
}

int GetShaderLocationAttrib(Shader shader, const char* attribName) {
    ShaderRec* sh = GetShader(shader.id);
    if (!sh || !attribName || !sh->program) return -1;
    return sh->program->attrSlot(attribName);
}

void SetShaderValue(Shader shader, int locIndex, const void* value, int uniformType) {
    SetShaderValueV(shader, locIndex, value, uniformType, 1);
}

void SetShaderValueV(Shader shader, int locIndex, const void* value, int uniformType, int count) {
    ShaderRec* sh = GetShader(shader.id);
    if (!sh) return;
    BatchFlush();
    SetUniformValue(*sh, locIndex, value, uniformType, count);
}

void SetShaderValueMatrix(Shader shader, int locIndex, Matrix mat) {
    ShaderRec* sh = GetShader(shader.id);
    if (!sh) return;
    BatchFlush();
    SetUniformMatrix(*sh, locIndex, mat);
}

void SetShaderValueTexture(Shader shader, int locIndex, Texture2D texture) {
    ShaderRec* sh = GetShader(shader.id);
    if (!sh || locIndex < 0 || locIndex >= (int)sh->uniforms.size()) return;
    BatchFlush();
    ShaderUniform& u = sh->uniforms[locIndex];
    if (u.isTexture) u.explicitTexture = texture.id;
}

// raylib's BeginShaderMode() swaps the shader used by its immediate-mode batch.
// Only the engine's 3D shader-mode draws use it, and they draw meshes, which
// carry their own shader; the batch keeps the default shader.
void BeginShaderMode(Shader) { BatchFlush(); }
void EndShaderMode(void) { BatchFlush(); }
