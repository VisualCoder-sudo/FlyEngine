// raylib mesh API on sokol_gfx: UploadMesh/UpdateMeshBuffer/UnloadMesh and the
// mesh draw calls. Model loading, mesh generation and materials stay in
// raylib's own rmodels.c (src/Engine/RL/raylib), which calls into these.
#include "rl_internal.hpp"
#include "rlgl.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <unordered_map>

#ifndef MAX_MESH_VERTEX_BUFFERS
#define MAX_MESH_VERTEX_BUFFERS 7
#endif
#ifndef MAX_MATERIAL_MAPS
#define MAX_MATERIAL_MAPS 12
#endif

namespace rli {

namespace {

constexpr int kMeshAttribs = 6;   // position, texcoord, normal, color, tangent, texcoord2

struct MeshRec {
    sg_buffer vb[kMeshAttribs] = {};
    sg_buffer ib = {};
    sg_buffer lineIb = {};
    int lineIndexCount = 0;
    int vertexCount = 0;
    int indexCount = 0;
    bool dynamic = false;
    bool alive = false;
};

std::vector<MeshRec> g_meshes(1);
std::vector<unsigned int> g_freeMeshes;

const sg_vertex_format kAttribFormat[kMeshAttribs] = {
    SG_VERTEXFORMAT_FLOAT3, SG_VERTEXFORMAT_FLOAT2, SG_VERTEXFORMAT_FLOAT3,
    SG_VERTEXFORMAT_UBYTE4N, SG_VERTEXFORMAT_FLOAT4, SG_VERTEXFORMAT_FLOAT2,
};
const int kAttribSize[kMeshAttribs] = { 12, 8, 12, 4, 16, 8 };

MeshRec* GetMesh(unsigned int id) {
    if (id == 0 || id >= g_meshes.size() || !g_meshes[id].alive) return nullptr;
    return &g_meshes[id];
}

const void* MeshArray(const Mesh& mesh, int attrib) {
    switch (attrib) {
        case 0: return mesh.vertices;
        case 1: return mesh.texcoords;
        case 2: return mesh.normals;
        case 3: return mesh.colors;
        case 4: return mesh.tangents;
        case 5: return mesh.texcoords2;
        default: return nullptr;
    }
}

// Small-buffer pool.
//
// Every sokol buffer gets its own GPU allocation, and on Vulkan creating an
// immutable buffer also means a staging copy plus a queue wait. Systems that
// rebuild many small meshes per frame (water chunks stream in and out as the
// camera moves; terrain/city edits) spent most of the frame in
// vkAllocateMemory/vkFreeMemory. Small mesh buffers are therefore created as
// updatable buffers and recycled by size instead of destroyed: a released
// buffer is refilled with sg_update_buffer() by the next mesh of the same size.
// sokol allows one update per buffer per frame and keeps in-flight frames safe.
constexpr size_t kPooledMaxSize = 256 * 1024;
constexpr size_t kPoolMaxPerKey = 1024;
// Pooled buffers are cheap to keep but not free: terrain sculpting rebuilds
// meshes at constantly changing sizes, so without a limit every size ever used
// would stay resident. Entries unused for kPoolMaxAgeFrames are destroyed, and
// the pool as a whole is capped at kPoolMaxBytes (oldest first).
constexpr uint64_t kPoolMaxAgeFrames = 600;
constexpr size_t kPoolMaxBytes = 64u << 20;
size_t g_pooledBytes = 0;

struct PooledBuffer {
    sg_buffer buffer;
    uint64_t releasedFrame;
};
std::unordered_map<uint64_t, std::vector<PooledBuffer>> g_bufferPool;
std::unordered_map<uint32_t, uint64_t> g_pooledKeys;   // live pooled buffer id -> pool key
std::unordered_map<uint32_t, uint64_t> g_lastUpdate;   // pooled buffer id -> frame of last update

uint64_t PoolKey(size_t size, bool index) { return ((uint64_t)size << 1) | (index ? 1u : 0u); }

uint64_t FrameIndex() { return (uint64_t)Gfx().frameCounter; }

sg_buffer MakeBuffer(const void* data, size_t size, bool dynamic, bool index, const char* label) {
    const bool pooled = !dynamic && data && size <= kPooledMaxSize;
    if (pooled) {
        const uint64_t key = PoolKey(size, index);
        auto& free = g_bufferPool[key];
        const uint64_t frame = FrameIndex();
        for (size_t i = free.size(); i-- > 0;) {
            const PooledBuffer pb = free[i];
            // Not updated yet this frame (sokol allows one update per frame).
            if (g_lastUpdate[pb.buffer.id] == frame) continue;
            free.erase(free.begin() + (std::ptrdiff_t)i);
            g_pooledBytes -= size;
            sg_update_buffer(pb.buffer, sg_range{ data, size });
            g_lastUpdate[pb.buffer.id] = frame;
            g_pooledKeys[pb.buffer.id] = key;
            return pb.buffer;
        }
    }
    sg_buffer_desc d{};
    d.usage.vertex_buffer = !index;
    d.usage.index_buffer = index;
    if (dynamic || pooled) {
        d.usage.immutable = false;
        d.usage.dynamic_update = true;
        d.size = size;
    } else {
        d.data = { data, size };
    }
    d.label = label;
    sg_buffer b = sg_make_buffer(&d);
    if ((dynamic || pooled) && data) {
        sg_update_buffer(b, sg_range{ data, size });
        g_lastUpdate[b.id] = FrameIndex();
    }
    if (pooled) g_pooledKeys[b.id] = PoolKey(size, index);
    return b;
}

sg_buffer MakeVertexBuffer(const void* data, size_t size, bool dynamic, const char* label) {
    return MakeBuffer(data, size, dynamic, false, label);
}

void DestroyPooled(const PooledBuffer& pb) {
    g_lastUpdate.erase(pb.buffer.id);
    sg_destroy_buffer(pb.buffer);
}

void ReleaseBuffer(sg_buffer b) {
    if (!b.id) return;
    auto it = g_pooledKeys.find(b.id);
    if (it != g_pooledKeys.end()) {
        const uint64_t key = it->second;
        auto& free = g_bufferPool[key];
        g_pooledKeys.erase(it);
        const size_t size = (size_t)(key >> 1);
        if (free.size() < kPoolMaxPerKey) {
            free.push_back({ b, FrameIndex() });
            g_pooledBytes += size;
            return;
        }
    }
    g_lastUpdate.erase(b.id);
    sg_destroy_buffer(b);
}

// Shaders read attributes the mesh may not have (vertex colors on most meshes,
// for instance). raylib relies on GL's constant "default attribute" for that;
// sokol has none, so build a buffer of the default value on first use.
sg_buffer AttributeBuffer(MeshRec& m, int attrib) {
    if (m.vb[attrib].id) return m.vb[attrib];
    const int n = std::max(1, m.vertexCount);
    std::vector<uint8_t> data((size_t)n * (size_t)kAttribSize[attrib], 0);
    if (attrib == ATTR_COLOR) {
        std::fill(data.begin(), data.end(), 255);
    } else if (attrib == ATTR_NORMAL) {
        float* f = (float*)data.data();
        for (int i = 0; i < n; ++i) f[i * 3 + 1] = 1.0f;
    } else if (attrib == ATTR_TANGENT) {
        float* f = (float*)data.data();
        for (int i = 0; i < n; ++i) { f[i * 4 + 0] = 1.0f; f[i * 4 + 3] = 1.0f; }
    }
    m.vb[attrib] = MakeVertexBuffer(data.data(), data.size(), false, "fly-mesh-default-attr");
    return m.vb[attrib];
}

// Edge list for wireframe drawing (sokol has no polygon line mode).
void BuildLineIndices(MeshRec& m, const Mesh& mesh) {
    if (m.lineIb.id) return;
    std::vector<uint32_t> lines;
    const int triCount = mesh.indices ? m.indexCount / 3 : m.vertexCount / 3;
    lines.reserve((size_t)triCount * 6);
    for (int t = 0; t < triCount; ++t) {
        uint32_t a, b, c;
        if (mesh.indices) {
            a = mesh.indices[t * 3]; b = mesh.indices[t * 3 + 1]; c = mesh.indices[t * 3 + 2];
        } else {
            a = (uint32_t)(t * 3); b = a + 1; c = a + 2;
        }
        lines.insert(lines.end(), { a, b, b, c, c, a });
    }
    if (lines.empty()) return;
    sg_buffer_desc d{};
    d.usage.index_buffer = true;
    d.data = { lines.data(), lines.size() * sizeof(uint32_t) };
    d.label = "fly-mesh-lines";
    m.lineIb = sg_make_buffer(&d);
    m.lineIndexCount = (int)lines.size();
}

// Vertex buffer slots are packed contiguously in semantic order, instance data
// last. sokol's Vulkan backend stops reading buffer bindings at the first empty
// slot, so a gap (a shader that skips e.g. vertex colors but uses tangents)
// silently drops every buffer after it -- which made all instanced draws
// invisible on Vulkan.
int BufferSlot(const ShaderRec& sh, int semantic) {
    int slot = 0;
    for (int a = 0; a < semantic; ++a) {
        if (sh.attrSlot[a] >= 0) ++slot;
    }
    return slot;
}

int InstanceBufferSlot(const ShaderRec& sh) {
    return BufferSlot(sh, kMeshAttribs);
}

sg_pipeline_desc MeshPipelineTemplate(const ShaderRec& sh, bool instanced) {
    sg_pipeline_desc d{};
    d.shader = sh.shader;
    for (int a = 0; a < kMeshAttribs; ++a) {
        const int slot = sh.attrSlot[a];
        if (slot < 0) continue;
        d.layout.attrs[slot].buffer_index = BufferSlot(sh, a);
        d.layout.attrs[slot].format = kAttribFormat[a];
    }
    if (instanced) {
        const int ib = InstanceBufferSlot(sh);
        d.layout.buffers[ib].stride = 64;
        d.layout.buffers[ib].step_func = SG_VERTEXSTEP_PER_INSTANCE;
        for (int c = 0; c < 4; ++c) {
            const int slot = sh.attrSlot[ATTR_INSTANCE0 + c];
            if (slot < 0) continue;
            d.layout.attrs[slot].buffer_index = ib;
            d.layout.attrs[slot].offset = c * 16;
            d.layout.attrs[slot].format = SG_VERTEXFORMAT_FLOAT4;
        }
    }
    return d;
}

Vector4 ColorVec(Color c) {
    return { c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, c.a / 255.0f };
}

// Shared body of DrawMesh / DrawMeshInstanced*.
void DrawMeshInternal(const Mesh& mesh, const Material& material, const Matrix& transform,
                      sg_buffer instanceBuffer, int instanceOffset, int instances) {
    GfxState& g = Gfx();
    MeshRec* m = GetMesh(mesh.vaoId);
    if (!m || !g.initialized) return;
    ShaderRec* sh = GetShader(material.shader.id);
    if (!sh || !sh->shader.id) sh = GetShader(g.defaultShader);
    if (!sh || !sh->shader.id) return;

    const bool instanced = instances > 0;
    if (instanced && !(sh->attrMask & (1u << ATTR_INSTANCE0))) return;   // shader can't instance

    BatchFlush();
    EnsurePass();
    if (!g.passActive) return;

    // Matrices, as raylib's DrawMesh/DrawMeshInstanced compute them.
    const Matrix matView = g.modelview;
    const Matrix matProjection = g.projection;
    const Matrix matModel = instanced ? MatrixIdentity() : MatrixMultiply(transform, g.transform);
    const Matrix matModelView = instanced ? MatrixMultiply(g.transform, matView) : MatrixMultiply(matModel, matView);
    const Matrix mvp = MatrixMultiply(matModelView, matProjection);
    const int* locs = sh->locs;
    if (locs[SHADER_LOC_MATRIX_MVP] >= 0) SetUniformMatrix(*sh, locs[SHADER_LOC_MATRIX_MVP], mvp);
    if (locs[SHADER_LOC_MATRIX_VIEW] >= 0) SetUniformMatrix(*sh, locs[SHADER_LOC_MATRIX_VIEW], matView);
    if (locs[SHADER_LOC_MATRIX_PROJECTION] >= 0) SetUniformMatrix(*sh, locs[SHADER_LOC_MATRIX_PROJECTION], matProjection);
    if (locs[SHADER_LOC_MATRIX_MODEL] >= 0) SetUniformMatrix(*sh, locs[SHADER_LOC_MATRIX_MODEL], matModel);
    if (locs[SHADER_LOC_MATRIX_NORMAL] >= 0) SetUniformMatrix(*sh, locs[SHADER_LOC_MATRIX_NORMAL], MatrixTranspose(MatrixInvert(matModel)));
    if (material.maps) {
        if (locs[SHADER_LOC_COLOR_DIFFUSE] >= 0) {
            const Vector4 c = ColorVec(material.maps[MATERIAL_MAP_DIFFUSE].color);
            SetUniformValue(*sh, locs[SHADER_LOC_COLOR_DIFFUSE], &c, SHADER_UNIFORM_VEC4, 1);
        }
        if (locs[SHADER_LOC_COLOR_SPECULAR] >= 0) {
            const Vector4 c = ColorVec(material.maps[MATERIAL_MAP_SPECULAR].color);
            SetUniformValue(*sh, locs[SHADER_LOC_COLOR_SPECULAR], &c, SHADER_UNIFORM_VEC4, 1);
        }
    }

    // Material maps bind to texture units 0..N for this draw only.
    unsigned int drawUnits[GfxState::kMaxUnits] = {};
    if (material.maps) {
        for (int i = 0; i < MAX_MATERIAL_MAPS && i < GfxState::kMaxUnits; ++i) {
            const int loc = locs[SHADER_LOC_MAP_DIFFUSE + i];
            if (material.maps[i].texture.id > 0 && loc >= 0) {
                drawUnits[i] = material.maps[i].texture.id;
                SetUniformValue(*sh, loc, &i, SHADER_UNIFORM_INT, 1);
            }
        }
    }

    const bool wire = g.state.wireframe;
    if (wire) BuildLineIndices(*m, mesh);
    const bool useLines = wire && m->lineIb.id;

    PipelineKey key{};
    key.shader = sh->shader.id;
    key.layout = sh->attrMask & ((1u << kMeshAttribs) - 1);
    key.primitive = useLines ? SG_PRIMITIVETYPE_LINES : SG_PRIMITIVETYPE_TRIANGLES;
    key.indexType = useLines ? SG_INDEXTYPE_UINT32 : (m->ib.id ? SG_INDEXTYPE_UINT16 : SG_INDEXTYPE_NONE);
    key.blend = (uint8_t)(g.state.blendMode + 1);
    key.depthTest = g.state.depthTest;
    key.depthWrite = g.state.depthWrite;
    key.cull = useLines ? 0 : 1;
    key.instanced = instanced;
    key.format = g.passFormat;
    sg_apply_pipeline(GetPipeline(key, MeshPipelineTemplate(*sh, instanced)));

    sg_bindings bind{};
    for (int a = 0; a < kMeshAttribs; ++a) {
        if (sh->attrMask & (1u << a)) bind.vertex_buffers[BufferSlot(*sh, a)] = AttributeBuffer(*m, a);
    }
    if (instanced) {
        const int ib = InstanceBufferSlot(*sh);
        bind.vertex_buffers[ib] = instanceBuffer;
        bind.vertex_buffer_offsets[ib] = instanceOffset;
    }
    if (useLines) bind.index_buffer = m->lineIb;
    else if (m->ib.id) bind.index_buffer = m->ib;
    ResolveShaderTextures(*sh, drawUnits, bind);
    sg_apply_bindings(&bind);
    ApplyShaderUniforms(*sh);

    const int count = useLines ? m->lineIndexCount : (m->ib.id ? m->indexCount : m->vertexCount);
    sg_draw(0, count, instanced ? instances : 1);
}

} // namespace

// Called once per frame: drops pooled buffers that have gone unused for a while
// and enforces the overall size limit, oldest first.
void TrimBufferPool() {
    const uint64_t frame = FrameIndex();
    for (auto& [key, list] : g_bufferPool) {
        const size_t size = (size_t)(key >> 1);
        size_t keep = 0;
        for (size_t i = 0; i < list.size(); ++i) {
            if (frame - list[i].releasedFrame > kPoolMaxAgeFrames) {
                DestroyPooled(list[i]);
                g_pooledBytes -= size;
            } else {
                list[keep++] = list[i];
            }
        }
        list.resize(keep);
    }
    while (g_pooledBytes > kPoolMaxBytes) {
        uint64_t oldestKey = 0, oldestFrame = UINT64_MAX;
        size_t oldestIdx = 0;
        for (auto& [key, list] : g_bufferPool) {
            for (size_t i = 0; i < list.size(); ++i) {
                if (list[i].releasedFrame < oldestFrame) {
                    oldestFrame = list[i].releasedFrame; oldestKey = key; oldestIdx = i;
                }
            }
        }
        if (oldestFrame == UINT64_MAX) break;
        auto& list = g_bufferPool[oldestKey];
        DestroyPooled(list[oldestIdx]);
        g_pooledBytes -= (size_t)(oldestKey >> 1);
        list.erase(list.begin() + (std::ptrdiff_t)oldestIdx);
    }
}

void ModelsShutdown() {
    // sg_shutdown() releases the buffers; just forget the records.
    g_meshes.assign(1, MeshRec{});
    g_freeMeshes.clear();
    g_bufferPool.clear();
    g_pooledBytes = 0;
    g_pooledKeys.clear();
    g_lastUpdate.clear();
}

} // namespace rli

using namespace rli;

void UploadMesh(Mesh* mesh, bool dynamic) {
    if (!mesh || !Gfx().initialized) return;
    if (mesh->vaoId > 0) {
        TraceLog(LOG_WARNING, "VAO: [ID %i] Trying to re-load an already loaded mesh", mesh->vaoId);
        return;
    }
    unsigned int id;
    if (!g_freeMeshes.empty()) {
        id = g_freeMeshes.back();
        g_freeMeshes.pop_back();
    } else {
        g_meshes.emplace_back();
        id = (unsigned int)g_meshes.size() - 1;
    }
    MeshRec& m = g_meshes[id];
    m = MeshRec{};
    m.alive = true;
    m.dynamic = dynamic;
    m.vertexCount = mesh->vertexCount;
    if (!mesh->vboId) mesh->vboId = (unsigned int*)RL_CALLOC(MAX_MESH_VERTEX_BUFFERS, sizeof(unsigned int));

    for (int a = 0; a < kMeshAttribs; ++a) {
        const void* data = MeshArray(*mesh, a);
        if (!data || mesh->vertexCount <= 0) continue;
        m.vb[a] = MakeVertexBuffer(data, (size_t)mesh->vertexCount * (size_t)kAttribSize[a], dynamic, "fly-mesh");
        mesh->vboId[a] = m.vb[a].id;
    }
    if (mesh->indices && mesh->triangleCount > 0) {
        m.indexCount = mesh->triangleCount * 3;
        m.ib = MakeBuffer(mesh->indices, (size_t)m.indexCount * sizeof(unsigned short), false, true, "fly-mesh-indices");
        mesh->vboId[6] = m.ib.id;
    }
    mesh->vaoId = id;
}

void UpdateMeshBuffer(Mesh mesh, int index, const void* data, int dataSize, int offset) {
    MeshRec* m = GetMesh(mesh.vaoId);
    if (!m || !data || dataSize <= 0 || index < 0 || index >= kMeshAttribs) return;
    const size_t full = (size_t)mesh.vertexCount * (size_t)kAttribSize[index];
    if (m->dynamic && m->vb[index].id && offset == 0) {
        sg_update_buffer(m->vb[index], sg_range{ data, std::min((size_t)dataSize, full) });
        return;
    }
    // Immutable buffer: rebuild it. The mesh's CPU array supplies the bytes
    // outside the updated range.
    std::vector<uint8_t> bytes(full, 0);
    const void* cpu = MeshArray(mesh, index);
    if (cpu) std::memcpy(bytes.data(), cpu, full);
    if ((size_t)offset < full) {
        std::memcpy(bytes.data() + offset, data, std::min((size_t)dataSize, full - (size_t)offset));
    }
    ReleaseBuffer(m->vb[index]);
    m->vb[index] = MakeVertexBuffer(bytes.data(), bytes.size(), false, "fly-mesh");
    if (mesh.vboId) mesh.vboId[index] = m->vb[index].id;
}

void UnloadMesh(Mesh mesh) {
    MeshRec* m = GetMesh(mesh.vaoId);
    if (m) {
        for (sg_buffer& b : m->vb) ReleaseBuffer(b);
        ReleaseBuffer(m->ib);
        ReleaseBuffer(m->lineIb);
        *m = MeshRec{};
        g_freeMeshes.push_back(mesh.vaoId);
    }
    RL_FREE(mesh.vboId);
    RL_FREE(mesh.vertices);
    RL_FREE(mesh.texcoords);
    RL_FREE(mesh.normals);
    RL_FREE(mesh.colors);
    RL_FREE(mesh.tangents);
    RL_FREE(mesh.texcoords2);
    RL_FREE(mesh.indices);
    RL_FREE(mesh.boneWeights);
    RL_FREE(mesh.boneIndices);
    RL_FREE(mesh.animVertices);
    RL_FREE(mesh.animNormals);
}

void DrawMesh(Mesh mesh, Material material, Matrix transform) {
    DrawMeshInternal(mesh, material, transform, sg_buffer{}, 0, 0);
}

void DrawMeshInstanced(Mesh mesh, Material material, const Matrix* transforms, int instances) {
    GfxState& g = Gfx();
    if (!transforms || instances <= 0 || !g.initialized) return;
    std::vector<float16> data((size_t)instances);
    for (int i = 0; i < instances; ++i) data[(size_t)i] = MatrixToFloatV(transforms[i]);
    const size_t bytes = data.size() * sizeof(float16);
    if (sg_query_buffer_will_overflow(g.instanceBuffer, bytes)) {
        if (!g.instanceOverflowWarned) {
            TraceLog(LOG_WARNING, "RL: per-frame instance data exceeded %zu MB; extra instances dropped",
                     g.instanceBufferSize >> 20);
            g.instanceOverflowWarned = true;
        }
        return;
    }
    const int offset = sg_append_buffer(g.instanceBuffer, sg_range{ data.data(), bytes });
    DrawMeshInternal(mesh, material, MatrixIdentity(), g.instanceBuffer, offset, instances);
}

void DrawMeshInstancedBuffer(Mesh mesh, Material material, unsigned int instanceBuffer, int instances) {
    if (instanceBuffer == 0 || instances <= 0) return;
    DrawMeshInternal(mesh, material, MatrixIdentity(), sg_buffer{ instanceBuffer }, 0, instances);
}

// ---------------------------------------------------------------------------
// rlgl pieces used by raylib's model module
// ---------------------------------------------------------------------------
unsigned int rlGetShaderIdDefault(void) { return Gfx().defaultShader; }

int* rlGetShaderLocsDefault(void) {
    ShaderRec* sh = GetShader(Gfx().defaultShader);
    return sh ? sh->locs : nullptr;
}

void rlUpdateVertexBuffer(unsigned int bufferId, const void* data, int dataSize, int) {
    if (bufferId == 0 || !data || dataSize <= 0) return;
    sg_buffer b{ bufferId };
    if (sg_query_buffer_usage(b).dynamic_update) sg_update_buffer(b, sg_range{ data, (size_t)dataSize });
}

void rlEnableShader(unsigned int) {}
void rlDisableShader(void) {}
void rlSetUniformMatrices(int, const Matrix*, int) {}
bool rlEnableVertexArray(unsigned int) { return false; }
void rlDisableVertexArray(void) {}
void rlEnableVertexAttribute(unsigned int) {}
void rlSetVertexAttribute(unsigned int, int, int, bool, int, int) {}
