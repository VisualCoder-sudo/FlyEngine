#pragma once
// Internal state shared by the raylib-compatible layer (src/Engine/RL/*.cpp).
//
// The engine was written against raylib. Rather than rewrite ~2,700 call sites,
// this layer implements the subset of the raylib API the engine uses on top of
// sokol_app (window, input) and sokol_gfx (rendering). Rendering goes through:
//
//   * an offscreen "main target" the size of the window. Everything the engine
//     draws "to the screen" lands here, and EndDrawing() copies it to the
//     swapchain in a single pass. That keeps every sokol backend happy, including
//     Vulkan, which allows only one swapchain pass per frame, while the engine
//     freely interleaves screen drawing with render-to-texture passes;
//   * a CPU-side immediate-mode batcher (lines/triangles with a texture) for
//     raylib's shape, text and 3D debug-draw functions;
//   * mesh/material draws with shaders compiled offline by sokol-shdc. Uniforms
//     are still set by name (GetShaderLocation/SetShaderValue) using the
//     reflection data sokol-shdc generates, and GL texture-unit semantics
//     (rlActiveTextureSlot/rlEnableTexture, sampler uniforms holding a unit) are
//     emulated so engine code keeps working unchanged.
#include "raylib.h"
#include "raymath.h"
#include "sokol_gfx.h"

#include <cstdint>
#include <thread>
#include <string>
#include <unordered_map>
#include <vector>

struct sapp_event;

namespace rli {

// Vertex attribute semantics, identified in shaders by raylib's attribute names
// (vertexPosition, vertexTexCoord, ...) plus the four columns of a per-instance
// model matrix (instanceTransform0..3). Each shader gets its own contiguous
// attribute slots (sokol requires that); ShaderRec::attrSlot maps semantic to
// slot. A mesh binds one vertex buffer per semantic, at buffer index =
// semantic (instance data shares buffer index ATTR_INSTANCE0).
enum AttribSemantic {
    ATTR_POSITION = 0,
    ATTR_TEXCOORD = 1,
    ATTR_NORMAL = 2,
    ATTR_COLOR = 3,
    ATTR_TANGENT = 4,
    ATTR_TEXCOORD2 = 5,
    ATTR_INSTANCE0 = 6,   // 6..9
    ATTR_COUNT = 10,
};
extern const char* const kAttribNames[ATTR_COUNT];

// ---------------------------------------------------------------------------
// Core (rl_core.cpp)
// ---------------------------------------------------------------------------
constexpr int kMaxKeys = 512;
constexpr int kMaxMouseButtons = 8;

struct CoreState {
    bool windowOpen = false;
    bool shouldClose = false;
    unsigned int configFlags = 0;
    int exitKey = KEY_ESCAPE;
    std::string title;

    // Timing
    double timeStart = 0.0;     // stm ticks converted to seconds at InitWindow
    double frameStart = 0.0;    // GetTime() at the start of the current frame
    double prevFrameStart = 0.0;
    double frameTime = 0.0;     // duration of the previous frame (s)
    double targetFrameTime = 0.0;
    int fpsFrames = 0;
    double fpsAccum = 0.0;
    int fps = 0;

    // Input (current/previous frame)
    bool keyDown[kMaxKeys] = {};
    bool keyPrev[kMaxKeys] = {};
    bool keyRepeat[kMaxKeys] = {};
    bool keyReleasedEvt[kMaxKeys] = {};
    bool keyPressedEvt[kMaxKeys] = {};
    bool mouseDown[kMaxMouseButtons] = {};
    bool mousePrev[kMaxMouseButtons] = {};
    bool mousePressedEvt[kMaxMouseButtons] = {};
    bool mouseReleasedEvt[kMaxMouseButtons] = {};
    Vector2 mousePos = {};
    Vector2 mouseDelta = {};    // accumulated this frame
    Vector2 wheel = {};         // accumulated this frame
    std::vector<int> charQueue;
    std::vector<int> keyQueue;
    std::vector<std::string> droppedFiles;
    bool filesDropped = false;
    bool cursorLocked = false;
    bool cursorHidden = false;
    int cursorShape = 0;

    float masterVolume = 1.0f;
};
CoreState& Core();

// Optional hook that sees every window event before the raylib input state does
// (the ImGui integration uses it). Set through SetEventHook() in raylib.h.
void DispatchEvent(const sapp_event* ev);

// ---------------------------------------------------------------------------
// Graphics (rl_gfx.cpp)
// ---------------------------------------------------------------------------
struct TextureRec {
    sg_image image{};
    sg_view view{};
    int width = 0;
    int height = 0;
    int mipmaps = 1;
    int format = 0;
    int filter = TEXTURE_FILTER_POINT;
    int wrap = TEXTURE_WRAP_REPEAT;
    bool depth = false;
    bool volume = false;       // 3D texture (LoadTexture3D)
    bool attachment = false;   // owned by a render target
    // A render target's image cannot be sampled before a pass has drawn into it
    // (the Vulkan backend has no image layout for it yet).
    bool rendered = false;
    bool alive = false;
    // Textures loaded from images are created lazily, on first use: until then
    // the pixels stay on the CPU, so GenTextureMipmaps() (which sokol cannot do on
    // the GPU) can still build the mip chain, and UpdateTexture() is free.
    std::vector<uint8_t> pending;
    sg_pixel_format sgFormat = SG_PIXELFORMAT_RGBA8;
    bool pendingMips = false;
    bool dynamic = false;          // created with dynamic_update (UpdateTexture)
    uint32_t lastUpdateFrame = 0;
};

struct RenderTargetRec {
    unsigned int colorTex = 0;   // 0 for depth-only targets
    unsigned int depthTex = 0;
    sg_view colorAtt{};
    sg_view depthAtt{};
    sg_pixel_format colorFormat = SG_PIXELFORMAT_RGBA8;
    // Float targets hold scene-linear light: sRGB colours drawn into them by the
    // default shader are linearized first (see flyLinearTarget in rl_default.glsl).
    bool linear = false;
    int width = 0;
    int height = 0;
    bool alive = false;
    // Deferred clear requested by ClearBackground() while this target is bound.
    bool clearColorPending = false;
    bool clearDepthPending = false;
    sg_color clearColor{ 0, 0, 0, 1 };
};

// Pixel formats of the pass currently being recorded; pipelines must match them.
struct PassFormat {
    sg_pixel_format color = SG_PIXELFORMAT_NONE;
    sg_pixel_format depth = SG_PIXELFORMAT_NONE;
    int colorCount = 0;
    bool operator==(const PassFormat& o) const {
        return color == o.color && depth == o.depth && colorCount == o.colorCount;
    }
};

// Render state that raylib keeps globally and applies to every draw.
enum class Cull : uint8_t { None, Back };

struct DrawState {
    bool depthTest = false;
    bool depthWrite = true;
    int blendMode = BLEND_ALPHA;
    bool wireframe = false;
    bool scissor = false;
    int scissorX = 0, scissorY = 0, scissorW = 0, scissorH = 0;
};

struct GfxState {
    bool initialized = false;
    sg_backend backend = SG_BACKEND_GLCORE;
    bool originTopLeft = false;

    std::vector<TextureRec> textures;          // index = Texture.id (0 unused)
    std::vector<unsigned int> freeTextures;
    std::vector<RenderTargetRec> targets;      // index = RenderTexture.id (0 = main)
    std::vector<unsigned int> freeTargets;

    unsigned int whiteTexture = 0;             // 1x1 white, rlGetTextureIdDefault()
    unsigned int dummyDepthTexture = 0;        // bound to comparison samplers with no texture
    unsigned int dummyVolumeTexture = 0;       // bound to 3D samplers with no texture

    // Main target: offscreen copy of the window, blitted to the swapchain.
    unsigned int mainTarget = 0;               // index into targets
    int screenWidth = 0;
    int screenHeight = 0;

    // Current render target (index into targets) and whether a sokol pass is open.
    unsigned int currentTarget = 0;
    bool passActive = false;
    PassFormat passFormat;
    bool frameActive = false;
    uint64_t frameCounter = 0;
    std::thread::id mainThread;    // incremented by GfxEndFrame (sokol's frame index)

    // Matrix state (raylib's rlgl stack, reduced to what the engine uses).
    Matrix projection = MatrixIdentity();
    Matrix modelview = MatrixIdentity();
    Matrix transform = MatrixIdentity();
    bool in3D = false;
    int currentWidth = 0;    // size of the current target, used for 2D projection
    int currentHeight = 0;
    double cullNear = 0.05;
    double cullFar = 4000.0;

    DrawState state;

    // Emulated GL texture units (rlActiveTextureSlot/rlEnableTexture).
    static constexpr int kMaxUnits = 16;
    unsigned int units[kMaxUnits] = {};
    int activeUnit = 0;

    // Streaming buffers.
    sg_buffer batchBuffer{};
    size_t batchBufferSize = 0;
    sg_buffer instanceBuffer{};
    size_t instanceBufferSize = 0;
    bool batchOverflowWarned = false;
    bool instanceOverflowWarned = false;

    // Samplers keyed by filter/wrap/mipmapped/compare.
    std::unordered_map<uint32_t, sg_sampler> samplers;
    // Pipelines keyed by a hash of every state that is baked into a sokol pipeline.
    std::unordered_map<uint64_t, sg_pipeline> pipelines;

    unsigned int defaultShader = 0;
    unsigned int blitShader = 0;
};
GfxState& Gfx();

void GfxInit(int width, int height);
void GfxShutdown();
void GfxBeginFrame();
void GfxEndFrame();      // blits the main target to the swapchain and commits
void ProcessPendingScreenshots();   // rl_misc.cpp; called after each commit

TextureRec* GetTexture(unsigned int id);
// Like GetTexture(), but makes sure the sokol image exists (see TextureRec::pending).
TextureRec* GetTextureForBinding(unsigned int id);
void MaterializePendingTextures();
void ResetTextureState();
void ApplyDeferredTextureUpdates();   // applies same-frame repeat updates at the start of the next frame
unsigned int CreateTexture(const sg_image_desc& desc, int format, bool depth, bool attachment);
unsigned int CreatePendingTexture(const Image& image);
void DestroyTexture(unsigned int id);
sg_sampler GetSampler(int filter, int wrap, bool mipmapped, bool compare);
sg_sampler GetSamplerFor(const TextureRec& tex, bool compare);

// Makes sure a sokol pass is open on the current target (flushing nothing).
void EnsurePass();
// Closes the open pass (after flushing the batch).
void EndPass();
// Current pass attachments, so a draw never samples the image it renders into.
bool IsBoundAsAttachment(unsigned int textureId);
void ApplyScissor();

// Pipeline lookup. `layoutKey` identifies the vertex layout; the descriptor is
// only consulted when the pipeline does not exist yet.
struct PipelineKey {
    uint32_t shader = 0;
    uint32_t layout = 0;          // bitmask of bound attributes + formats
    uint8_t primitive = 0;        // sg_primitive_type
    uint8_t indexType = 0;        // sg_index_type
    uint8_t blend = 0;            // raylib BlendMode + 1, 0 = no blending
    uint8_t depthTest = 0;
    uint8_t depthWrite = 0;
    uint8_t cull = 0;
    uint8_t instanced = 0;
    PassFormat format;
};
sg_pipeline GetPipeline(const PipelineKey& key, const sg_pipeline_desc& descTemplate);

// ---------------------------------------------------------------------------
// Immediate-mode batch (rl_batch.cpp)
// ---------------------------------------------------------------------------
struct BatchVertex {
    float x, y, z;
    float u, v;
    uint8_t r, g, b, a;
};

void BatchInit();
void BatchShutdown();
// Starts or continues a draw command. `lines` selects SG_PRIMITIVETYPE_LINES.
void BatchBegin(bool lines, unsigned int texture);
void BatchVertex3(float x, float y, float z, float u, float v, Color c);
// Uploads and draws every pending command. Called before any non-batch draw,
// before passes end, and whenever global state (scissor, matrices) changes.
void BatchFlush();
bool BatchHasPending();
// Equivalent of rlCheckRenderBatchLimit: make room for n vertices.
void BatchReserve(int vertexCount);

// ---------------------------------------------------------------------------
// Shaders (rl_shader.cpp)
// ---------------------------------------------------------------------------
struct ProgramInfo;   // generated registry (shaders_registry.cpp)

struct UniformTarget {
    int ub = -1;
    int offset = 0;
    int type = 0;          // sg_uniform_type
    int arrayCount = 0;    // 0 = not an array
};

struct ShaderUniform {
    std::string name;
    UniformTarget targets[2];
    int targetCount = 0;
    bool isTexture = false;
    int viewSlot = -1;
    int samplerSlot = -1;
    bool compare = false;
    bool nonfiltering = false;       // nearest-only sampler: the one way to read a depth texture's values
    bool volume = false;             // texture3D
    int unit = 0;                    // GL texture unit this sampler reads
    unsigned int explicitTexture = 0;  // SetShaderValueTexture()
};

struct ShaderRec {
    const ProgramInfo* program = nullptr;
    sg_shader shader{};
    std::vector<ShaderUniform> uniforms;
    std::vector<uint8_t> ubData[SG_MAX_UNIFORMBLOCK_BINDSLOTS];
    uint32_t attrMask = 0;           // bit s set: shader consumes semantic s
    int attrSlot[ATTR_COUNT];        // semantic -> shader attribute slot, -1 if unused
    int locLinearTarget = -1;        // "flyLinearTarget": 1 while drawing into a float (scene-linear) target
    int* locs = nullptr;
    bool alive = false;
};

std::vector<ShaderRec>& Shaders();
ShaderRec* GetShader(unsigned int id);
unsigned int LoadProgram(const char* name);
void ShadersShutdown();
void SetUniformMatrix(ShaderRec& sh, int loc, const Matrix& m);
void SetUniformValue(ShaderRec& sh, int loc, const void* value, int type, int count);

// Binds the textures a shader samples, resolving units and per-draw overrides.
// `drawUnits` holds textures bound for this draw only (material maps).
void ResolveShaderTextures(const ShaderRec& sh, const unsigned int* drawUnits, sg_bindings& bind);
void ApplyShaderUniforms(const ShaderRec& sh);
// Tells a shader that declares flyLinearTarget whether the current target is scene-linear.
void SetLinearTargetUniform(ShaderRec& sh);

// ---------------------------------------------------------------------------
// Models (rl_models.cpp)
// ---------------------------------------------------------------------------
void ModelsShutdown();
void TrimBufferPool();   // once per frame: age/size limits on the recycled-buffer pool

// ---------------------------------------------------------------------------
// Text (rl_text.cpp)
// ---------------------------------------------------------------------------
void TextInit();
void TextShutdown();

} // namespace rli
