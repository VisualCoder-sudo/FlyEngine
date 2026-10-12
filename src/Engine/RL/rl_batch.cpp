// Immediate-mode vertex batch (rlBegin/rlVertex/rlEnd) on sokol_gfx.
//
// Vertices are collected on the CPU in draw commands (one per primitive type and
// texture) and uploaded in one sg_append_buffer() per flush. A flush happens
// before anything that is not part of the batch draws, and before any change to
// global state the batch depends on (matrices, scissor, depth, blending,
// render target), which is exactly when raylib flushes its own batch.
#include "rl_internal.hpp"
#include "rlgl.h"

#include <cstddef>
#include <cstring>

namespace rli {
namespace {

struct Command {
    bool lines = false;
    unsigned int texture = 0;
    int first = 0;
    int count = 0;
};

struct BatchState {
    std::vector<BatchVertex> vertices;
    std::vector<Command> commands;
    int mode = 0;                 // RL_LINES/RL_TRIANGLES/RL_QUADS, 0 outside rlBegin
    unsigned int texture = 0;     // rlSetTexture()
    float u = 0.0f, v = 0.0f;
    Color color = { 255, 255, 255, 255 };
    BatchVertex pending[4];       // primitive being assembled
    int pendingCount = 0;
    int locMvp = -1;
    int locColDiffuse = -1;
    bool flushing = false;
};
BatchState g_batch;

unsigned int EffectiveTexture() {
    return g_batch.texture ? g_batch.texture : Gfx().whiteTexture;
}

Command& CurrentCommand(bool lines) {
    const unsigned int tex = EffectiveTexture();
    if (g_batch.commands.empty() || g_batch.commands.back().lines != lines ||
        g_batch.commands.back().texture != tex) {
        Command c;
        c.lines = lines;
        c.texture = tex;
        c.first = (int)g_batch.vertices.size();
        g_batch.commands.push_back(c);
    }
    return g_batch.commands.back();
}

void Emit(const BatchVertex& vtx, bool lines) {
    Command& c = CurrentCommand(lines);
    g_batch.vertices.push_back(vtx);
    c.count++;
}

void EmitTriangle(const BatchVertex& a, const BatchVertex& b, const BatchVertex& c) {
    if (Gfx().state.wireframe) {
        Emit(a, true); Emit(b, true);
        Emit(b, true); Emit(c, true);
        Emit(c, true); Emit(a, true);
    } else {
        Emit(a, false); Emit(b, false); Emit(c, false);
    }
}

sg_pipeline_desc BatchPipelineTemplate(const ShaderRec& sh) {
    sg_pipeline_desc d{};
    d.shader = sh.shader;
    d.layout.buffers[0].stride = sizeof(BatchVertex);
    const struct { int semantic; int offset; sg_vertex_format format; } attrs[] = {
        { ATTR_POSITION, (int)offsetof(BatchVertex, x), SG_VERTEXFORMAT_FLOAT3 },
        { ATTR_TEXCOORD, (int)offsetof(BatchVertex, u), SG_VERTEXFORMAT_FLOAT2 },
        { ATTR_COLOR, (int)offsetof(BatchVertex, r), SG_VERTEXFORMAT_UBYTE4N },
    };
    for (const auto& a : attrs) {
        const int slot = sh.attrSlot[a.semantic];
        if (slot < 0) continue;
        d.layout.attrs[slot].buffer_index = 0;
        d.layout.attrs[slot].offset = a.offset;
        d.layout.attrs[slot].format = a.format;
    }
    return d;
}

} // namespace

void BatchInit() {
    g_batch = BatchState{};
    g_batch.vertices.reserve(1 << 16);
    ShaderRec* sh = GetShader(Gfx().defaultShader);
    if (sh) {
        Shader s{ Gfx().defaultShader, sh->locs };
        g_batch.locMvp = GetShaderLocation(s, "mvp");
        g_batch.locColDiffuse = GetShaderLocation(s, "colDiffuse");
    }
}

void BatchShutdown() {
    g_batch = BatchState{};
}

bool BatchHasPending() { return !g_batch.vertices.empty(); }

void BatchReserve(int) {}

void BatchBegin(bool lines, unsigned int texture) {
    g_batch.texture = texture;
    CurrentCommand(lines);
}

void BatchVertex3(float x, float y, float z, float u, float v, Color c) {
    BatchVertex vtx{ x, y, z, u, v, c.r, c.g, c.b, c.a };
    Emit(vtx, false);
}

void BatchFlush() {
    GfxState& g = Gfx();
    if (g_batch.flushing || g_batch.vertices.empty() || !g.initialized) {
        if (!g_batch.flushing) {
            g_batch.vertices.clear();
            g_batch.commands.clear();
        }
        return;
    }
    g_batch.flushing = true;
    EnsurePass();
    ShaderRec* sh = GetShader(g.defaultShader);
    const size_t bytes = g_batch.vertices.size() * sizeof(BatchVertex);
    if (!g.passActive || !sh) {
        // Nothing to draw into (no target bound).
    } else if (sg_query_buffer_will_overflow(g.batchBuffer, bytes)) {
        if (!g.batchOverflowWarned) {
            TraceLog(LOG_WARNING, "RL: immediate-mode batch exceeded %zu MB this frame; extra geometry dropped",
                     g.batchBufferSize >> 20);
            g.batchOverflowWarned = true;
        }
    } else {
        const int offset = sg_append_buffer(g.batchBuffer, sg_range{ g_batch.vertices.data(), bytes });
        const Matrix mvp = MatrixMultiply(g.modelview, g.projection);
        SetUniformMatrix(*sh, g_batch.locMvp, mvp);
        const float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        SetUniformValue(*sh, g_batch.locColDiffuse, white, SHADER_UNIFORM_VEC4, 1);
        SetLinearTargetUniform(*sh);
        const sg_pipeline_desc tmpl = BatchPipelineTemplate(*sh);

        for (const Command& c : g_batch.commands) {
            if (c.count == 0) continue;
            PipelineKey key{};
            key.shader = sh->shader.id;
            key.layout = 0x80000000u | 0x0B;   // batch vertex layout
            key.primitive = c.lines ? SG_PRIMITIVETYPE_LINES : SG_PRIMITIVETYPE_TRIANGLES;
            key.indexType = SG_INDEXTYPE_NONE;
            key.blend = (uint8_t)(g.state.blendMode + 1);
            key.depthTest = g.state.depthTest;
            key.depthWrite = g.state.depthWrite;
            key.cull = 0;
            key.format = g.passFormat;
            sg_apply_pipeline(GetPipeline(key, tmpl));

            sg_bindings bind{};
            bind.vertex_buffers[0] = g.batchBuffer;
            bind.vertex_buffer_offsets[0] = offset;
            unsigned int drawUnits[GfxState::kMaxUnits] = {};
            drawUnits[0] = c.texture;
            ResolveShaderTextures(*sh, drawUnits, bind);
            sg_apply_bindings(&bind);
            ApplyShaderUniforms(*sh);
            sg_draw(c.first, c.count, 1);
        }
    }
    g_batch.vertices.clear();
    g_batch.commands.clear();
    g_batch.flushing = false;
}

} // namespace rli

using namespace rli;

// ---------------------------------------------------------------------------
// rlgl immediate mode
// ---------------------------------------------------------------------------
void rlBegin(int mode) {
    g_batch.mode = mode;
    g_batch.pendingCount = 0;
}

void rlEnd(void) {
    g_batch.mode = 0;
    g_batch.pendingCount = 0;
}

void rlSetTexture(unsigned int id) {
    g_batch.texture = id;
}

void rlTexCoord2f(float x, float y) {
    g_batch.u = x;
    g_batch.v = y;
}

void rlNormal3f(float, float, float) {}

void rlColor4ub(unsigned char r, unsigned char g, unsigned char b, unsigned char a) {
    g_batch.color = { r, g, b, a };
}

void rlColor4f(float r, float g, float b, float a) {
    rlColor4ub((unsigned char)(r * 255), (unsigned char)(g * 255), (unsigned char)(b * 255), (unsigned char)(a * 255));
}

void rlColor3f(float r, float g, float b) { rlColor4f(r, g, b, 1.0f); }

void rlVertex3f(float x, float y, float z) {
    Vector3 p = { x, y, z };
    const GfxState& g = Gfx();
    // Only pay for the transform when a rlPushMatrix() block is active.
    const Matrix& t = g.transform;
    const bool identity = t.m0 == 1.0f && t.m5 == 1.0f && t.m10 == 1.0f && t.m15 == 1.0f &&
                          t.m12 == 0.0f && t.m13 == 0.0f && t.m14 == 0.0f &&
                          t.m1 == 0.0f && t.m2 == 0.0f && t.m4 == 0.0f && t.m6 == 0.0f && t.m8 == 0.0f && t.m9 == 0.0f;
    if (!identity) p = Vector3Transform(p, t);

    BatchVertex vtx{ p.x, p.y, p.z, g_batch.u, g_batch.v,
                     g_batch.color.r, g_batch.color.g, g_batch.color.b, g_batch.color.a };
    switch (g_batch.mode) {
        case RL_LINES:
            g_batch.pending[g_batch.pendingCount++] = vtx;
            if (g_batch.pendingCount == 2) {
                Emit(g_batch.pending[0], true);
                Emit(g_batch.pending[1], true);
                g_batch.pendingCount = 0;
            }
            break;
        case RL_TRIANGLES:
            g_batch.pending[g_batch.pendingCount++] = vtx;
            if (g_batch.pendingCount == 3) {
                EmitTriangle(g_batch.pending[0], g_batch.pending[1], g_batch.pending[2]);
                g_batch.pendingCount = 0;
            }
            break;
        case RL_QUADS:
            g_batch.pending[g_batch.pendingCount++] = vtx;
            if (g_batch.pendingCount == 4) {
                EmitTriangle(g_batch.pending[0], g_batch.pending[1], g_batch.pending[2]);
                EmitTriangle(g_batch.pending[0], g_batch.pending[2], g_batch.pending[3]);
                g_batch.pendingCount = 0;
            }
            break;
        default:
            break;
    }
}

void rlVertex2f(float x, float y) { rlVertex3f(x, y, 0.0f); }
void rlVertex2i(int x, int y) { rlVertex3f((float)x, (float)y, 0.0f); }

bool rlCheckRenderBatchLimit(int) { return false; }

void rlDrawRenderBatchActive(void) { BatchFlush(); }
