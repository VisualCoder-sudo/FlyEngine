// sokol_gfx device, resources, render passes and the raylib/rlgl state API.
#include "rl_internal.hpp"
#include "rlgl.h"

#include "sokol_app.h"
#include "sokol_glue.h"
#include "sokol_log.h"

#include <algorithm>
#include <cstring>

namespace rli {

GfxState& Gfx() {
    static GfxState g;
    return g;
}

namespace {

constexpr size_t kBatchBufferSize = 32u << 20;
constexpr size_t kInstanceBufferSize = 16u << 20;

struct MatrixStackState {
    int mode = RL_MODELVIEW;
    Matrix stack[32];
    int depth = 0;
    bool transformRequired = false;
};
MatrixStackState g_ms;

uint64_t HashBytes(const void* data, size_t size, uint64_t h = 1469598103934665603ull) {
    const uint8_t* p = (const uint8_t*)data;
    for (size_t i = 0; i < size; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

sg_filter ToMinMag(int filter) {
    return filter == TEXTURE_FILTER_POINT ? SG_FILTER_NEAREST : SG_FILTER_LINEAR;
}

sg_wrap ToWrap(int wrap) {
    switch (wrap) {
        case TEXTURE_WRAP_CLAMP: return SG_WRAP_CLAMP_TO_EDGE;
        case TEXTURE_WRAP_MIRROR_REPEAT: return SG_WRAP_MIRRORED_REPEAT;
        case TEXTURE_WRAP_MIRROR_CLAMP: return SG_WRAP_MIRRORED_REPEAT;
        default: return SG_WRAP_REPEAT;
    }
}

unsigned int AllocTargetSlot() {
    GfxState& g = Gfx();
    if (!g.freeTargets.empty()) {
        const unsigned int id = g.freeTargets.back();
        g.freeTargets.pop_back();
        return id;
    }
    g.targets.emplace_back();
    return (unsigned int)g.targets.size() - 1;
}

// raylib pixel format -> the sokol format a render target of it uses.
sg_pixel_format TargetFormat(int format) {
    sg_pixel_format f = SG_PIXELFORMAT_RGBA8;
    switch (format) {
        case PIXELFORMAT_UNCOMPRESSED_R16G16B16A16: f = SG_PIXELFORMAT_RGBA16F; break;
        case PIXELFORMAT_UNCOMPRESSED_R32G32B32A32: f = SG_PIXELFORMAT_RGBA32F; break;
        case PIXELFORMAT_UNCOMPRESSED_R16: f = SG_PIXELFORMAT_R16F; break;
        case PIXELFORMAT_UNCOMPRESSED_R32: f = SG_PIXELFORMAT_R32F; break;
        case PIXELFORMAT_UNCOMPRESSED_GRAYSCALE: f = SG_PIXELFORMAT_R8; break;
        default: break;
    }
    // Every desktop backend renders to these; the check is for the odd driver that does not.
    if (!sg_query_pixelformat(f).render) {
        TraceLog(LOG_WARNING, "RL: render target format %i is not renderable here, using RGBA8", format);
        f = SG_PIXELFORMAT_RGBA8;
    }
    return f;
}

bool IsFloatFormat(sg_pixel_format f) {
    return f == SG_PIXELFORMAT_RGBA16F || f == SG_PIXELFORMAT_RGBA32F || f == SG_PIXELFORMAT_R16F || f == SG_PIXELFORMAT_R32F;
}

// Creates color/depth images and attachment views for a render target slot.
void BuildTarget(RenderTargetRec& t, int width, int height, bool color, bool depth, const char* label,
                 int format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8) {
    t = RenderTargetRec{};
    t.width = width;
    t.height = height;
    if (color) {
        sg_image_desc d{};
        d.usage.color_attachment = true;
        d.width = width;
        d.height = height;
        d.pixel_format = TargetFormat(format);
        d.label = label;
        t.colorFormat = d.pixel_format;
        t.linear = IsFloatFormat(d.pixel_format);
        t.colorTex = CreateTexture(d, t.colorFormat == SG_PIXELFORMAT_RGBA8 ? PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 : format, false, true);
        sg_view_desc vd{};
        vd.color_attachment.image = GetTexture(t.colorTex)->image;
        t.colorAtt = sg_make_view(&vd);
    }
    if (depth) {
        sg_image_desc d{};
        d.usage.depth_stencil_attachment = true;
        d.width = width;
        d.height = height;
        d.pixel_format = SG_PIXELFORMAT_DEPTH;
        d.label = label;
        t.depthTex = CreateTexture(d, 0, true, true);
        sg_view_desc vd{};
        vd.depth_stencil_attachment.image = GetTexture(t.depthTex)->image;
        t.depthAtt = sg_make_view(&vd);
    }
    t.alive = true;
}

void DestroyTarget(RenderTargetRec& t) {
    if (!t.alive) return;
    if (t.colorAtt.id) sg_destroy_view(t.colorAtt);
    if (t.depthAtt.id) sg_destroy_view(t.depthAtt);
    if (t.colorTex) DestroyTexture(t.colorTex);
    if (t.depthTex) DestroyTexture(t.depthTex);
    t = RenderTargetRec{};
}

Matrix Ortho2D(int w, int h) {
    return MatrixOrtho(0.0, (double)w, (double)h, 0.0, 0.0, 1.0);
}

void Reset2DMatrices() {
    GfxState& g = Gfx();
    g.projection = Ortho2D(g.currentWidth, g.currentHeight);
    g.modelview = MatrixIdentity();
    g.transform = MatrixIdentity();
    g_ms.depth = 0;
    g_ms.transformRequired = false;
    g_ms.mode = RL_MODELVIEW;
}

void EnsureMainTarget() {
    GfxState& g = Gfx();
    const int w = std::max(1, sapp_width());
    const int h = std::max(1, sapp_height());
    if (g.mainTarget && g.targets[g.mainTarget].alive &&
        g.targets[g.mainTarget].width == w && g.targets[g.mainTarget].height == h) {
        return;
    }
    if (!g.mainTarget) g.mainTarget = AllocTargetSlot();
    DestroyTarget(g.targets[g.mainTarget]);
    BuildTarget(g.targets[g.mainTarget], w, h, true, true, "fly-main-target");
    // A fresh target has undefined content; start it cleared.
    RenderTargetRec& t = g.targets[g.mainTarget];
    t.clearColorPending = true;
    t.clearDepthPending = true;
    t.clearColor = { 0, 0, 0, 1 };
    g.screenWidth = w;
    g.screenHeight = h;
}

bool g_dummyDepthCleared = false;
unsigned int g_dummyDepthTarget = 0;

} // namespace

// ---------------------------------------------------------------------------
// Device
// ---------------------------------------------------------------------------
void GfxInit(int width, int height) {
    GfxState& g = Gfx();
    sg_desc d{};
    d.environment = sglue_environment();
    d.logger.func = slog_func;
    d.buffer_pool_size = 65000;   // sokol caps pools below 65536
    d.image_pool_size = 16384;
    d.view_pool_size = 32768;
    d.sampler_pool_size = 256;
    d.shader_pool_size = 256;
    d.pipeline_pool_size = 2048;
    d.uniform_buffer_size = 64 << 20;
    d.vulkan.descriptor_buffer_size = 32 << 20;
    sg_setup(&d);

    g = GfxState{};
    g.initialized = true;
    g.mainThread = std::this_thread::get_id();
    g.backend = sg_query_backend();
    g.originTopLeft = sg_query_features().origin_top_left;
    g.textures.resize(1);
    g.targets.resize(1);

    // 1x1 white texture: raylib's default texture (rlGetTextureIdDefault).
    {
        const uint32_t white = 0xFFFFFFFFu;
        sg_image_desc id{};
        id.width = 1;
        id.height = 1;
        id.pixel_format = SG_PIXELFORMAT_RGBA8;
        id.data.mip_levels[0] = { &white, sizeof(white) };
        id.label = "fly-white";
        g.whiteTexture = CreateTexture(id, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, false, false);
    }
    // 1x1 depth texture bound to comparison samplers that have nothing else.
    {
        g_dummyDepthTarget = AllocTargetSlot();
        BuildTarget(g.targets[g_dummyDepthTarget], 1, 1, false, true, "fly-dummy-depth");
        g.dummyDepthTexture = g.targets[g_dummyDepthTarget].depthTex;
        g_dummyDepthCleared = false;
    }
    // 1x1x1 white volume, for texture3D slots with nothing bound.
    {
        const uint32_t white = 0xFFFFFFFFu;
        sg_image_desc id{};
        id.type = SG_IMAGETYPE_3D;
        id.width = 1;
        id.height = 1;
        id.num_slices = 1;
        id.pixel_format = SG_PIXELFORMAT_RGBA8;
        id.data.mip_levels[0] = { &white, sizeof(white) };
        id.label = "fly-white-3d";
        g.dummyVolumeTexture = CreateTexture(id, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, false, false);
        g.textures[g.dummyVolumeTexture].volume = true;
    }

    sg_buffer_desc bd{};
    bd.size = kBatchBufferSize;
    bd.usage.vertex_buffer = true;
    bd.usage.dynamic_update = true;
    bd.usage.immutable = false;
    bd.label = "fly-batch";
    g.batchBuffer = sg_make_buffer(&bd);
    g.batchBufferSize = kBatchBufferSize;

    bd.size = kInstanceBufferSize;
    bd.label = "fly-instances";
    g.instanceBuffer = sg_make_buffer(&bd);
    g.instanceBufferSize = kInstanceBufferSize;

    g.defaultShader = LoadProgram("rl_default");
    g.blitShader = LoadProgram("rl_blit");

    g.currentWidth = width;
    g.currentHeight = height;
    EnsureMainTarget();
    g.currentTarget = g.mainTarget;
    Reset2DMatrices();
    BatchInit();
}

void GfxShutdown() {
    GfxState& g = Gfx();
    if (!g.initialized) return;
    BatchShutdown();
    ResetTextureState();
    for (auto& kv : g.pipelines) sg_destroy_pipeline(kv.second);
    for (auto& kv : g.samplers) sg_destroy_sampler(kv.second);
    sg_shutdown();
    g = GfxState{};
}

void GfxBeginFrame() {
    GfxState& g = Gfx();
    if (!g.initialized) return;
    BatchFlush();
    EndPass();
    EnsureMainTarget();
    g.frameActive = true;
    g.currentTarget = g.mainTarget;
    g.currentWidth = g.screenWidth;
    g.currentHeight = g.screenHeight;
    g.in3D = false;
    g.state.depthTest = false;
    Reset2DMatrices();

    if (!g_dummyDepthCleared) {
        const unsigned int saved = g.currentTarget;
        g.currentTarget = g_dummyDepthTarget;
        g.targets[g_dummyDepthTarget].clearDepthPending = true;
        EnsurePass();
        EndPass();
        g.currentTarget = saved;
        g_dummyDepthCleared = true;
    }
}

void GfxEndFrame() {
    GfxState& g = Gfx();
    if (!g.initialized) return;
    BatchFlush();
    if (g.currentTarget != g.mainTarget) {
        TraceLog(LOG_WARNING, "RL: EndDrawing() called inside BeginTextureMode()");
        EndPass();
        g.currentTarget = g.mainTarget;
    }
    RenderTargetRec& main = g.targets[g.mainTarget];
    if (main.clearColorPending || main.clearDepthPending) EnsurePass();
    EndPass();

    // Copy the main target to the swapchain.
    sg_pass pass{};
    pass.action.colors[0].load_action = SG_LOADACTION_DONTCARE;
    pass.swapchain = sglue_swapchain();
    pass.label = "fly-present";
    sg_begin_pass(&pass);
    ShaderRec* blit = GetShader(g.blitShader);
    if (blit && blit->shader.id) {
        PipelineKey key{};
        key.shader = blit->shader.id;
        key.primitive = SG_PRIMITIVETYPE_TRIANGLES;
        key.indexType = SG_INDEXTYPE_NONE;
        key.format.color = sglue_environment().defaults.color_format;
        key.format.depth = sglue_environment().defaults.depth_format;
        key.format.colorCount = 1;
        key.layout = 0xFFFFFFFFu;   // no vertex input
        sg_pipeline_desc pd{};
        pd.shader = blit->shader;
        sg_pipeline pip = GetPipeline(key, pd);
        sg_apply_pipeline(pip);
        sg_bindings bind{};
        const TextureRec* tex = GetTextureForBinding(main.colorTex);
        bind.views[0] = tex->view;
        bind.samplers[0] = GetSampler(TEXTURE_FILTER_POINT, TEXTURE_WRAP_CLAMP, false, false);
        sg_apply_bindings(&bind);
        sg_draw(0, 3, 1);
    }
    sg_end_pass();
    sg_commit();
    g.frameActive = false;
    g.frameCounter++;
    ApplyDeferredTextureUpdates();
    TrimBufferPool();
    ProcessPendingScreenshots();
    // Upload textures that were loaded this frame but never drawn, so their
    // CPU copies do not linger.
    MaterializePendingTextures();
}

// ---------------------------------------------------------------------------
// Textures and samplers
// ---------------------------------------------------------------------------
TextureRec* GetTexture(unsigned int id) {
    GfxState& g = Gfx();
    if (id == 0 || id >= g.textures.size() || !g.textures[id].alive) return nullptr;
    return &g.textures[id];
}

unsigned int CreateTexture(const sg_image_desc& desc, int format, bool depth, bool attachment) {
    GfxState& g = Gfx();
    unsigned int id;
    if (!g.freeTextures.empty()) {
        id = g.freeTextures.back();
        g.freeTextures.pop_back();
    } else {
        g.textures.emplace_back();
        id = (unsigned int)g.textures.size() - 1;
    }
    TextureRec& t = g.textures[id];
    t = TextureRec{};
    t.image = sg_make_image(&desc);
    if (sg_query_image_state(t.image) != SG_RESOURCESTATE_VALID) {
        TraceLog(LOG_WARNING, "TEXTURE: Failed to create %ix%i image", desc.width, desc.height);
    }
    sg_view_desc vd{};
    vd.texture.image = t.image;
    t.view = sg_make_view(&vd);
    t.width = desc.width;
    t.height = desc.height;
    t.mipmaps = desc.num_mipmaps > 0 ? desc.num_mipmaps : 1;
    t.format = format;
    t.depth = depth;
    t.attachment = attachment;
    t.filter = depth ? TEXTURE_FILTER_BILINEAR : TEXTURE_FILTER_POINT;
    t.wrap = depth ? TEXTURE_WRAP_CLAMP : TEXTURE_WRAP_REPEAT;
    t.alive = true;
    return id;
}

void DestroyTexture(unsigned int id) {
    TextureRec* t = GetTexture(id);
    if (!t) return;
    GfxState& g = Gfx();
    for (unsigned int& u : g.units) {
        if (u == id) u = 0;
    }
    if (t->view.id) sg_destroy_view(t->view);
    if (t->image.id) sg_destroy_image(t->image);
    *t = TextureRec{};
    g.freeTextures.push_back(id);
}

sg_sampler GetSampler(int filter, int wrap, bool mipmapped, bool compare) {
    GfxState& g = Gfx();
    const uint32_t key = (uint32_t)filter | ((uint32_t)wrap << 4) | ((uint32_t)mipmapped << 8) | ((uint32_t)compare << 9);
    auto it = g.samplers.find(key);
    if (it != g.samplers.end()) return it->second;

    sg_sampler_desc d{};
    d.min_filter = ToMinMag(filter);
    d.mag_filter = ToMinMag(filter);
    const sg_wrap w = compare ? SG_WRAP_CLAMP_TO_EDGE : ToWrap(wrap);
    d.wrap_u = w;
    d.wrap_v = w;
    d.wrap_w = w;
    if (mipmapped && filter != TEXTURE_FILTER_POINT) {
        d.mipmap_filter = (filter == TEXTURE_FILTER_BILINEAR) ? SG_FILTER_NEAREST : SG_FILTER_LINEAR;
        if (filter == TEXTURE_FILTER_ANISOTROPIC_4X) d.max_anisotropy = 4;
        if (filter == TEXTURE_FILTER_ANISOTROPIC_8X) d.max_anisotropy = 8;
        if (filter == TEXTURE_FILTER_ANISOTROPIC_16X) d.max_anisotropy = 16;
    } else {
        d.mipmap_filter = SG_FILTER_NEAREST;
        d.max_lod = 0.0f;
    }
    if (compare) {
        d.compare = SG_COMPAREFUNC_LESS_EQUAL;
        d.min_filter = SG_FILTER_LINEAR;
        d.mag_filter = SG_FILTER_LINEAR;
    }
    d.label = "fly-sampler";
    sg_sampler s = sg_make_sampler(&d);
    g.samplers[key] = s;
    return s;
}

sg_sampler GetSamplerFor(const TextureRec& tex, bool compare) {
    return GetSampler(tex.filter, tex.wrap, tex.mipmaps > 1, compare);
}

// ---------------------------------------------------------------------------
// Pipelines
// ---------------------------------------------------------------------------
sg_pipeline GetPipeline(const PipelineKey& key, const sg_pipeline_desc& tmpl) {
    GfxState& g = Gfx();
    uint64_t h = HashBytes(&key.shader, sizeof(key.shader));
    h = HashBytes(&key.layout, sizeof(key.layout), h);
    const uint8_t small[] = { key.primitive, key.indexType, key.blend, key.depthTest, key.depthWrite, key.cull, key.instanced };
    h = HashBytes(small, sizeof(small), h);
    h = HashBytes(&key.format.color, sizeof(key.format.color), h);
    h = HashBytes(&key.format.depth, sizeof(key.format.depth), h);
    h = HashBytes(&key.format.colorCount, sizeof(key.format.colorCount), h);
    auto it = g.pipelines.find(h);
    if (it != g.pipelines.end()) return it->second;

    sg_pipeline_desc d = tmpl;
    d.primitive_type = (sg_primitive_type)key.primitive;
    d.index_type = (sg_index_type)key.indexType;
    d.cull_mode = key.cull ? SG_CULLMODE_BACK : SG_CULLMODE_NONE;
    d.face_winding = SG_FACEWINDING_CCW;
    d.color_count = key.format.colorCount;
    d.depth.pixel_format = key.format.depth;
    if (key.format.depth != SG_PIXELFORMAT_NONE) {
        d.depth.compare = key.depthTest ? SG_COMPAREFUNC_LESS_EQUAL : SG_COMPAREFUNC_ALWAYS;
        d.depth.write_enabled = key.depthTest && key.depthWrite;
    }
    if (key.format.colorCount == 0) {
        // Depth-only pass: sokol only accepts color_count 0 when the first
        // color format is explicitly NONE.
        d.colors[0].pixel_format = SG_PIXELFORMAT_NONE;
    } else {
        d.colors[0].pixel_format = key.format.color;
        if (key.blend) {
            sg_blend_state& b = d.colors[0].blend;
            b.enabled = true;
            switch (key.blend - 1) {
                case BLEND_ADDITIVE:
                    b.src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA;
                    b.dst_factor_rgb = SG_BLENDFACTOR_ONE;
                    break;
                case BLEND_MULTIPLIED:
                    b.src_factor_rgb = SG_BLENDFACTOR_DST_COLOR;
                    b.dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
                    break;
                case BLEND_ADD_COLORS:
                    b.src_factor_rgb = SG_BLENDFACTOR_ONE;
                    b.dst_factor_rgb = SG_BLENDFACTOR_ONE;
                    break;
                case BLEND_ALPHA_PREMULTIPLY:
                    b.src_factor_rgb = SG_BLENDFACTOR_ONE;
                    b.dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
                    break;
                default:   // BLEND_ALPHA
                    b.src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA;
                    b.dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
                    break;
            }
            b.src_factor_alpha = b.src_factor_rgb;
            b.dst_factor_alpha = b.dst_factor_rgb;
        }
    }
    d.label = "fly-pipeline";
    sg_pipeline p = sg_make_pipeline(&d);
    if (sg_query_pipeline_state(p) != SG_RESOURCESTATE_VALID) {
        TraceLog(LOG_WARNING, "RL: pipeline creation failed (shader %u)", key.shader);
    }
    g.pipelines[h] = p;
    return p;
}

// ---------------------------------------------------------------------------
// Passes
// ---------------------------------------------------------------------------
void EnsurePass() {
    GfxState& g = Gfx();
    if (g.passActive || !g.initialized) return;
    if (g.currentTarget == 0 || g.currentTarget >= g.targets.size()) return;
    RenderTargetRec& t = g.targets[g.currentTarget];
    if (!t.alive) return;

    sg_pass pass{};
    if (t.colorAtt.id) {
        pass.attachments.colors[0] = t.colorAtt;
        pass.action.colors[0].load_action = t.clearColorPending ? SG_LOADACTION_CLEAR : SG_LOADACTION_LOAD;
        pass.action.colors[0].store_action = SG_STOREACTION_STORE;
        pass.action.colors[0].clear_value = t.clearColor;
    }
    if (t.depthAtt.id) {
        pass.attachments.depth_stencil = t.depthAtt;
        pass.action.depth.load_action = t.clearDepthPending ? SG_LOADACTION_CLEAR : SG_LOADACTION_LOAD;
        pass.action.depth.store_action = SG_STOREACTION_STORE;
        pass.action.depth.clear_value = 1.0f;
    }
    pass.label = (g.currentTarget == g.mainTarget) ? "fly-main" : "fly-offscreen";
    sg_begin_pass(&pass);
    t.clearColorPending = false;
    t.clearDepthPending = false;
    if (TextureRec* c = GetTexture(t.colorTex)) c->rendered = true;
    if (TextureRec* d = GetTexture(t.depthTex)) d->rendered = true;

    g.passActive = true;
    g.passFormat.color = t.colorAtt.id ? t.colorFormat : SG_PIXELFORMAT_NONE;
    g.passFormat.colorCount = t.colorAtt.id ? 1 : 0;
    g.passFormat.depth = t.depthAtt.id ? SG_PIXELFORMAT_DEPTH : SG_PIXELFORMAT_NONE;
    ApplyScissor();
}

void EndPass() {
    GfxState& g = Gfx();
    if (!g.passActive) return;
    BatchFlush();
    sg_end_pass();
    g.passActive = false;
}

bool IsBoundAsAttachment(unsigned int textureId) {
    GfxState& g = Gfx();
    if (!g.passActive || textureId == 0) return false;
    const RenderTargetRec& t = g.targets[g.currentTarget];
    return t.colorTex == textureId || t.depthTex == textureId;
}

void ApplyScissor() {
    GfxState& g = Gfx();
    if (!g.passActive) return;
    if (g.state.scissor) {
        int x = std::clamp(g.state.scissorX, 0, g.currentWidth);
        int y = std::clamp(g.state.scissorY, 0, g.currentHeight);
        int w = std::clamp(g.state.scissorW, 0, g.currentWidth - x);
        int h = std::clamp(g.state.scissorH, 0, g.currentHeight - y);
        sg_apply_scissor_rect(x, y, w, h, true);
    } else {
        sg_apply_scissor_rect(0, 0, g.currentWidth, g.currentHeight, true);
    }
}

} // namespace rli

using namespace rli;

// ---------------------------------------------------------------------------
// raylib: drawing modes
// ---------------------------------------------------------------------------
void ClearBackground(Color color) {
    GfxState& g = Gfx();
    if (!g.initialized) return;
    BatchFlush();
    EndPass();
    RenderTargetRec& t = g.targets[g.currentTarget];
    t.clearColorPending = t.colorAtt.id != 0;
    t.clearDepthPending = t.depthAtt.id != 0;
    t.clearColor = { color.r / 255.0f, color.g / 255.0f, color.b / 255.0f, color.a / 255.0f };
    EnsurePass();
}

void BeginMode3D(Camera3D camera) {
    GfxState& g = Gfx();
    BatchFlush();
    const double aspect = (double)g.currentWidth / (double)std::max(1, g.currentHeight);
    if (camera.projection == CAMERA_ORTHOGRAPHIC) {
        const double top = camera.fovy / 2.0;
        const double right = top * aspect;
        g.projection = MatrixOrtho(-right, right, -top, top, g.cullNear, g.cullFar);
        g.projection.m12 += g.jitterX;
        g.projection.m13 += g.jitterY;
    } else {
        g.projection = MatrixPerspective(camera.fovy * DEG2RAD, aspect, g.cullNear, g.cullFar);
        // clip.w is -z here, so a shift of the picture goes into the z column.
        g.projection.m8 -= g.jitterX;
        g.projection.m9 -= g.jitterY;
    }
    g.modelview = MatrixLookAt(camera.position, camera.target, camera.up);
    g.transform = MatrixIdentity();
    g_ms.depth = 0;
    g_ms.transformRequired = false;
    g.in3D = true;
    g.state.depthTest = true;
}

void EndMode3D(void) {
    GfxState& g = Gfx();
    BatchFlush();
    Reset2DMatrices();
    g.in3D = false;
    g.state.depthTest = false;
}

void BeginTextureMode(RenderTexture2D target) {
    GfxState& g = Gfx();
    if (target.id == 0 || target.id >= g.targets.size() || !g.targets[target.id].alive) return;
    BatchFlush();
    EndPass();
    g.currentTarget = target.id;
    g.currentWidth = g.targets[target.id].width;
    g.currentHeight = g.targets[target.id].height;
    Reset2DMatrices();
}

void EndTextureMode(void) {
    GfxState& g = Gfx();
    BatchFlush();
    RenderTargetRec& t = g.targets[g.currentTarget];
    if (t.clearColorPending || t.clearDepthPending) EnsurePass();
    EndPass();
    g.currentTarget = g.mainTarget;
    g.currentWidth = g.screenWidth;
    g.currentHeight = g.screenHeight;
    Reset2DMatrices();
}

void BeginScissorMode(int x, int y, int width, int height) {
    GfxState& g = Gfx();
    BatchFlush();
    g.state.scissor = true;
    g.state.scissorX = x;
    g.state.scissorY = y;
    g.state.scissorW = std::max(0, width);
    g.state.scissorH = std::max(0, height);
    ApplyScissor();
}

void EndScissorMode(void) {
    GfxState& g = Gfx();
    BatchFlush();
    g.state.scissor = false;
    ApplyScissor();
}

void BeginBlendMode(int mode) {
    BatchFlush();
    Gfx().state.blendMode = mode;
}

void EndBlendMode(void) {
    BatchFlush();
    Gfx().state.blendMode = BLEND_ALPHA;
}

RenderTexture2D LoadRenderTexture(int width, int height) {
    RenderTexture2D rt{};
    if (!Gfx().initialized || width <= 0 || height <= 0) return rt;
    const unsigned int id = AllocTargetSlot();
    BuildTarget(Gfx().targets[id], width, height, true, true, "fly-render-texture");
    const RenderTargetRec& t = Gfx().targets[id];
    rt.id = id;
    rt.texture = { t.colorTex, width, height, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 };
    rt.depth = { t.depthTex, width, height, 1, 0 };
    GetTexture(t.colorTex)->filter = TEXTURE_FILTER_POINT;
    GetTexture(t.colorTex)->wrap = TEXTURE_WRAP_CLAMP;
    return rt;
}

RenderTexture2D LoadRenderTextureEx(int width, int height, int format, bool depth) {
    RenderTexture2D rt{};
    if (!Gfx().initialized || width <= 0 || height <= 0) return rt;
    const unsigned int id = AllocTargetSlot();
    BuildTarget(Gfx().targets[id], width, height, true, depth, "fly-render-texture-ex", format);
    const RenderTargetRec& t = Gfx().targets[id];
    rt.id = id;
    rt.texture = { t.colorTex, width, height, 1, GetTexture(t.colorTex)->format };
    rt.depth = { t.depthTex, width, height, 1, 0 };
    GetTexture(t.colorTex)->filter = TEXTURE_FILTER_BILINEAR;
    GetTexture(t.colorTex)->wrap = TEXTURE_WRAP_CLAMP;
    return rt;
}

RenderTexture2D LoadRenderTextureDepth(int width, int height) {
    RenderTexture2D rt{};
    if (!Gfx().initialized || width <= 0 || height <= 0) return rt;
    const unsigned int id = AllocTargetSlot();
    BuildTarget(Gfx().targets[id], width, height, false, true, "fly-depth-texture");
    const RenderTargetRec& t = Gfx().targets[id];
    rt.id = id;
    rt.texture = { 0, width, height, 1, 0 };
    rt.depth = { t.depthTex, width, height, 1, 0 };
    return rt;
}

bool IsRenderTextureValid(RenderTexture2D target) {
    GfxState& g = Gfx();
    return target.id > 0 && target.id < g.targets.size() && g.targets[target.id].alive;
}

void UnloadRenderTexture(RenderTexture2D target) {
    GfxState& g = Gfx();
    if (!IsRenderTextureValid(target) || target.id == g.mainTarget) return;
    if (g.currentTarget == target.id) EndTextureMode();
    DestroyTarget(g.targets[target.id]);
    g.freeTargets.push_back(target.id);
}

// One triangle that covers the current target, drawn with `shader` (its vertex
// stage is the fly_fullscreen_vs block). No depth test or write.
void DrawFullscreen(Shader shader, int blendMode) {
    GfxState& g = Gfx();
    ShaderRec* sh = GetShader(shader.id);
    if (!g.initialized || !sh || !sh->shader.id) return;
    BatchFlush();
    EnsurePass();
    if (!g.passActive) return;

    PipelineKey key{};
    key.shader = sh->shader.id;
    key.layout = 0xFFFFFFFFu;   // no vertex input
    key.primitive = SG_PRIMITIVETYPE_TRIANGLES;
    key.indexType = SG_INDEXTYPE_NONE;
    key.blend = blendMode < 0 ? 0 : (uint8_t)(blendMode + 1);
    key.format = g.passFormat;
    sg_pipeline_desc pd{};
    pd.shader = sh->shader;
    sg_apply_pipeline(GetPipeline(key, pd));

    SetLinearTargetUniform(*sh);
    sg_bindings bind{};
    ResolveShaderTextures(*sh, nullptr, bind);
    sg_apply_bindings(&bind);
    ApplyShaderUniforms(*sh);
    sg_draw(0, 3, 1);
}

void SetProjectionJitter(float x, float y) {
    BatchFlush();
    Gfx().jitterX = x;
    Gfx().jitterY = y;
}

Vector2 GetRenderTargetSize(void) {
    return { (float)Gfx().currentWidth, (float)Gfx().currentHeight };
}

Texture2D LoadTexture3D(const void* data, int width, int height, int depth, int format) {
    Texture2D tex{};
    GfxState& g = Gfx();
    if (!g.initialized || !data || width <= 0 || height <= 0 || depth <= 0) return tex;
    const bool gray = format == PIXELFORMAT_UNCOMPRESSED_GRAYSCALE;
    if (!gray && format != PIXELFORMAT_UNCOMPRESSED_R8G8B8A8) {
        TraceLog(LOG_WARNING, "TEXTURE: LoadTexture3D() takes GRAYSCALE or R8G8B8A8 data");
        return tex;
    }
    sg_image_desc d{};
    d.type = SG_IMAGETYPE_3D;
    d.width = width;
    d.height = height;
    d.num_slices = depth;
    d.pixel_format = gray ? SG_PIXELFORMAT_R8 : SG_PIXELFORMAT_RGBA8;
    d.data.mip_levels[0] = { data, (size_t)width * (size_t)height * (size_t)depth * (gray ? 1u : 4u) };
    d.label = "fly-texture-3d";
    tex.id = CreateTexture(d, format, false, false);
    TextureRec& t = g.textures[tex.id];
    t.volume = true;
    t.filter = TEXTURE_FILTER_BILINEAR;
    t.wrap = TEXTURE_WRAP_REPEAT;
    tex.width = width;
    tex.height = height;
    tex.mipmaps = 1;
    tex.format = format;
    return tex;
}

bool IsRenderOriginTopLeft(void) { return Gfx().originTopLeft; }

const char* GetGraphicsBackendName(void) {
    switch (Gfx().backend) {
        case SG_BACKEND_GLCORE: return "OpenGL";
        case SG_BACKEND_VULKAN: return "Vulkan";
        case SG_BACKEND_D3D11: return "D3D11";
        case SG_BACKEND_METAL_MACOS: return "Metal";
        default: return "unknown";
    }
}

// ---------------------------------------------------------------------------
// rlgl: matrices
// ---------------------------------------------------------------------------
static Matrix* CurrentMatrix() {
    GfxState& g = Gfx();
    if (g_ms.mode == RL_PROJECTION) return &g.projection;
    return g_ms.transformRequired ? &g.transform : &g.modelview;
}

void rlMatrixMode(int mode) { g_ms.mode = mode; }

void rlPushMatrix(void) {
    GfxState& g = Gfx();
    if (g_ms.depth >= 32) {
        TraceLog(LOG_ERROR, "RLGL: Matrix stack overflow (RL_MAX_MATRIX_STACK_SIZE)");
        return;
    }
    if (g_ms.mode == RL_MODELVIEW) g_ms.transformRequired = true;
    g_ms.stack[g_ms.depth++] = *CurrentMatrix();
    (void)g;
}

void rlPopMatrix(void) {
    if (g_ms.depth > 0) {
        *CurrentMatrix() = g_ms.stack[--g_ms.depth];
    }
    if (g_ms.depth == 0 && g_ms.mode == RL_MODELVIEW) {
        g_ms.transformRequired = false;
        Gfx().transform = MatrixIdentity();
    }
}

void rlLoadIdentity(void) { *CurrentMatrix() = MatrixIdentity(); }

void rlTranslatef(float x, float y, float z) {
    Matrix* m = CurrentMatrix();
    *m = MatrixMultiply(MatrixTranslate(x, y, z), *m);
}

void rlRotatef(float angle, float x, float y, float z) {
    Matrix* m = CurrentMatrix();
    Vector3 axis = Vector3Normalize({ x, y, z });
    *m = MatrixMultiply(MatrixRotate(axis, angle * DEG2RAD), *m);
}

void rlScalef(float x, float y, float z) {
    Matrix* m = CurrentMatrix();
    *m = MatrixMultiply(MatrixScale(x, y, z), *m);
}

void rlMultMatrixf(const float* f) {
    Matrix mat = { f[0], f[4], f[8], f[12],
                   f[1], f[5], f[9], f[13],
                   f[2], f[6], f[10], f[14],
                   f[3], f[7], f[11], f[15] };
    Matrix* m = CurrentMatrix();
    *m = MatrixMultiply(mat, *m);
}

Matrix rlGetMatrixModelview(void) { return Gfx().modelview; }
Matrix rlGetMatrixProjection(void) { return Gfx().projection; }
Matrix rlGetMatrixTransform(void) { return Gfx().transform; }

void rlSetMatrixProjection(Matrix proj) {
    BatchFlush();
    Gfx().projection = proj;
}

void rlSetMatrixModelview(Matrix view) {
    BatchFlush();
    Gfx().modelview = view;
}

void rlSetClipPlanes(double nearPlane, double farPlane) {
    Gfx().cullNear = nearPlane;
    Gfx().cullFar = farPlane;
}

double rlGetCullDistanceNear(void) { return Gfx().cullNear; }
double rlGetCullDistanceFar(void) { return Gfx().cullFar; }

// ---------------------------------------------------------------------------
// rlgl: state
// ---------------------------------------------------------------------------
void rlEnableDepthTest(void) { BatchFlush(); Gfx().state.depthTest = true; }
void rlDisableDepthTest(void) { BatchFlush(); Gfx().state.depthTest = false; }
void rlEnableDepthMask(void) { BatchFlush(); Gfx().state.depthWrite = true; }
void rlDisableDepthMask(void) { BatchFlush(); Gfx().state.depthWrite = false; }
void rlEnableWireMode(void) { BatchFlush(); Gfx().state.wireframe = true; }
void rlDisableWireMode(void) { BatchFlush(); Gfx().state.wireframe = false; }
bool rlIsWireMode(void) { return Gfx().state.wireframe; }
void rlSetBlendMode(int mode) { BatchFlush(); Gfx().state.blendMode = mode; }

void rlEnableScissorTest(void) {
    BatchFlush();
    Gfx().state.scissor = true;
    ApplyScissor();
}

void rlDisableScissorTest(void) {
    BatchFlush();
    Gfx().state.scissor = false;
    ApplyScissor();
}

void rlScissor(int x, int y, int width, int height) {
    // glScissor convention: origin bottom-left.
    GfxState& g = Gfx();
    BatchFlush();
    g.state.scissorX = x;
    g.state.scissorY = g.currentHeight - (y + height);
    g.state.scissorW = width;
    g.state.scissorH = height;
    ApplyScissor();
}

void rlActiveTextureSlot(int slot) {
    if (slot >= 0 && slot < GfxState::kMaxUnits) Gfx().activeUnit = slot;
}

void rlEnableTexture(unsigned int id) {
    GfxState& g = Gfx();
    g.units[g.activeUnit] = id;
}

void rlDisableTexture(void) {
    GfxState& g = Gfx();
    g.units[g.activeUnit] = 0;
}

unsigned int rlGetTextureIdDefault(void) { return Gfx().whiteTexture; }

int rlGetVersion(void) { return RL_OPENGL_33; }

unsigned int rlLoadVertexBuffer(const void* buffer, int size, bool dynamic) {
    if (!buffer || size <= 0) return 0;
    sg_buffer_desc d{};
    d.usage.vertex_buffer = true;
    if (dynamic) {
        d.usage.immutable = false;
        d.usage.dynamic_update = true;
        d.size = (size_t)size;
    } else {
        d.data = { buffer, (size_t)size };
    }
    d.label = "fly-vertex-buffer";
    sg_buffer b = sg_make_buffer(&d);
    if (dynamic) sg_update_buffer(b, sg_range{ buffer, (size_t)size });
    return sg_query_buffer_state(b) == SG_RESOURCESTATE_VALID ? b.id : 0;
}

void rlUnloadVertexBuffer(unsigned int vboId) {
    if (vboId) sg_destroy_buffer(sg_buffer{ vboId });
}
