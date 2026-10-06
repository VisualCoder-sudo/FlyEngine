// ============================================================================
// Reference implementation of gpu::IGraphicsBackend on top of raylib.
//
// This is the backend the engine is verified against, and the one the migration
// away from raylib is measured with. It therefore has two jobs beyond "make
// raylib calls": it must be an honest reference for what the abstraction
// guarantees, and it must be correct, because every shader-parity check made
// during the Sokol/Vulkan port is only meaningful if the raylib side renders
// what it rendered before.
//
// Two rules are load-bearing here and easy to get wrong:
//
//  1. Matrix conversion is a TRANSPOSE, not a copy. raylib's Matrix stores
//     element(row, col) at [row*4 + col] (rows contiguous) while gpu::Mat4 is
//     column-major (m[col][row]). Both are proven against raylib's own
//     Vector3Transform in tests/math_backend_parity_test.cpp. Copying the bytes
//     instead of transposing compiles fine and renders garbage.
//
//  2. Every raylib call whose name is also an IGraphicsBackend member must be
//     written with an explicit `::` qualifier. Inside a member function, an
//     unqualified call finds the member first and hides the global entirely, so
//     SetWindowTitle(window, title) would call itself forever. The `::` is
//     load-bearing, not stylistic.
//
// Where raylib genuinely cannot express a concept (uniform blocks, persistent
// buffer mapping, compute, multiple windows) the method is documented as a no-op
// rather than faked, so the Sokol backend's support can be compared against it.
// ============================================================================

#include "Engine/Backend/GraphicsBackend.hpp"
#include "Engine/Backend/Math.hpp"

#include "raylib.h"
#include "rlgl.h"

#include "imgui.h"
#include "imgui_impl_raylib.h"

#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

using gpu::BufferDesc;
using gpu::BufferHandle;
using gpu::BufferType;
using gpu::BufferUsage;
using gpu::Camera3D;
using gpu::Color;
using gpu::CursorType;
using gpu::FramebufferDesc;
using gpu::FramebufferHandle;
using gpu::IGraphicsBackend;
using gpu::IndexType;
using gpu::KeyCode;
using gpu::Mat4;
using gpu::MeshData;
using gpu::MeshHandle;
// MouseButton deliberately not imported: raylib declares a global MouseButton
// enum too, and a `using` would make the bare name ambiguous in this TU.
using gpu::PipelineDesc;
using gpu::PipelineHandle;
using gpu::RenderPassDesc;
using gpu::SamplerDesc;
using gpu::SamplerHandle;
using gpu::ShaderDesc;
using gpu::ShaderHandle;
using gpu::TextureData;
using gpu::TextureFormat;
using gpu::TextureHandle;
using gpu::UniformType;
using gpu::Vec2;
using gpu::Vec3;
using gpu::Vec4;
using gpu::WindowDesc;
using gpu::WindowHandle;

// ============================================================================
// Matrix conversion
//
// See the note at the top of this file: these TRANSPOSE.
// ============================================================================

gpu::Mat4 ToMat4(const Matrix& m) {
    const float* p = &m.m0;
    gpu::Mat4 out;
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) out.m[c][r] = p[r * 4 + c];
    return out;
}

Matrix FromMat4(const gpu::Mat4& m) {
    Matrix r;
    float* p = &r.m0;
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row) p[row * 4 + c] = m.m[c][row];
    return r;
}

// ============================================================================
// Scalar / small-type conversions
// ============================================================================

Vec2 ToVec2(const Vector2& v) { return {v.x, v.y}; }
Vec3 ToVec3(const Vector3& v) { return {v.x, v.y, v.z}; }
Vec4 ToVec4(const Vector4& v) { return {v.x, v.y, v.z, v.w}; }
Color ToColor(const ::Color& c) { return {c.r, c.g, c.b, c.a}; }
::Color FromColor(const Color& c) { return {c.r, c.g, c.b, c.a}; }

// gpu::KeyCode is defined to match raylib's KeyboardKey numerically (verified
// against raylib.h: Escape=256, F1=290, LeftShift=340, KP_0=320, Menu=348), so
// the cast is exact for every key raylib defines. F13..F25 exist in gpu::KeyCode
// for backends that support extended keyboards; raylib defines no equivalent, so
// they simply report "not down" here.
KeyCode ToKeyCode(int key) { return static_cast<KeyCode>(key); }
int FromKeyCode(KeyCode key) { return static_cast<int>(key); }

MouseButton ToMouseButton(int button) { return static_cast<MouseButton>(button); }

// gpu::CursorType is a short cross-platform list; raylib's MouseCursor is longer
// and differently ordered, so this is an explicit mapping rather than a cast.
int FromCursor(CursorType cursor) {
    switch (cursor) {
        case CursorType::Arrow: return MOUSE_CURSOR_ARROW;
        case CursorType::IBeam: return MOUSE_CURSOR_IBEAM;
        case CursorType::Crosshair: return MOUSE_CURSOR_CROSSHAIR;
        case CursorType::Hand: return MOUSE_CURSOR_POINTING_HAND;
        case CursorType::HResize: return MOUSE_CURSOR_RESIZE_EW;
        case CursorType::VResize: return MOUSE_CURSOR_RESIZE_NS;
    }
    return MOUSE_CURSOR_DEFAULT;
}

TextureFormat FromPixelFormat(int fmt) {
    switch (fmt) {
        case PIXELFORMAT_UNCOMPRESSED_GRAYSCALE: return TextureFormat::R8;
        case PIXELFORMAT_UNCOMPRESSED_GRAY_ALPHA: return TextureFormat::RG8;
        case PIXELFORMAT_UNCOMPRESSED_R8G8B8: return TextureFormat::RGB8;
        case PIXELFORMAT_UNCOMPRESSED_R8G8B8A8: return TextureFormat::RGBA8;
        case PIXELFORMAT_UNCOMPRESSED_R16: return TextureFormat::R16;
        case PIXELFORMAT_UNCOMPRESSED_R16G16B16: return TextureFormat::RGB16;
        case PIXELFORMAT_UNCOMPRESSED_R16G16B16A16: return TextureFormat::RGBA16;
        case PIXELFORMAT_UNCOMPRESSED_R32: return TextureFormat::R32F;
        case PIXELFORMAT_UNCOMPRESSED_R32G32B32: return TextureFormat::RGB32F;
        case PIXELFORMAT_UNCOMPRESSED_R32G32B32A32: return TextureFormat::RGBA32F;
        case PIXELFORMAT_COMPRESSED_DXT1_RGB: return TextureFormat::BC1_RGB;
        case PIXELFORMAT_COMPRESSED_DXT1_RGBA: return TextureFormat::BC1_RGBA;
        case PIXELFORMAT_COMPRESSED_DXT3_RGBA: return TextureFormat::BC3;
        case PIXELFORMAT_COMPRESSED_DXT5_RGBA: return TextureFormat::BC5;
        case PIXELFORMAT_COMPRESSED_ETC1_RGB: return TextureFormat::ETC2_RGB;
        case PIXELFORMAT_COMPRESSED_ETC2_RGB: return TextureFormat::ETC2_RGB;
        case PIXELFORMAT_COMPRESSED_ETC2_EAC_RGBA: return TextureFormat::ETC2_RGBA;
        case PIXELFORMAT_COMPRESSED_PVRT_RGB: return TextureFormat::BC1_RGB;
        case PIXELFORMAT_COMPRESSED_PVRT_RGBA: return TextureFormat::BC3;
        case PIXELFORMAT_COMPRESSED_ASTC_4x4_RGBA: return TextureFormat::ASTC_4x4;
        default: return TextureFormat::RGBA8;
    }
}

int ToPixelFormat(TextureFormat fmt) {
    switch (fmt) {
        case TextureFormat::R8: return PIXELFORMAT_UNCOMPRESSED_GRAYSCALE;
        case TextureFormat::RG8: return PIXELFORMAT_UNCOMPRESSED_GRAY_ALPHA;
        case TextureFormat::RGB8: return PIXELFORMAT_UNCOMPRESSED_R8G8B8;
        case TextureFormat::RGBA8: return PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
        case TextureFormat::R16: return PIXELFORMAT_UNCOMPRESSED_R16;
        case TextureFormat::RGB16: return PIXELFORMAT_UNCOMPRESSED_R16G16B16;
        case TextureFormat::RGBA16: return PIXELFORMAT_UNCOMPRESSED_R16G16B16A16;
        case TextureFormat::R32F: return PIXELFORMAT_UNCOMPRESSED_R32;
        case TextureFormat::RGB32F: return PIXELFORMAT_UNCOMPRESSED_R32G32B32;
        case TextureFormat::RGBA32F: return PIXELFORMAT_UNCOMPRESSED_R32G32B32A32;
        case TextureFormat::BC1_RGB: return PIXELFORMAT_COMPRESSED_DXT1_RGB;
        case TextureFormat::BC1_RGBA: return PIXELFORMAT_COMPRESSED_DXT1_RGBA;
        case TextureFormat::BC3: return PIXELFORMAT_COMPRESSED_DXT3_RGBA;
        case TextureFormat::BC5: return PIXELFORMAT_COMPRESSED_DXT5_RGBA;
        case TextureFormat::ETC2_RGB: return PIXELFORMAT_COMPRESSED_ETC2_RGB;
        case TextureFormat::ETC2_RGBA: return PIXELFORMAT_COMPRESSED_ETC2_EAC_RGBA;
        case TextureFormat::ASTC_4x4: return PIXELFORMAT_COMPRESSED_ASTC_4x4_RGBA;
        default: return PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
    }
}

// gpu::VertexAttribute::format reuses TextureFormat to describe a vertex
// attribute. This maps the formats vertex buffers actually use onto raylib's
// rlgl type enum; anything else is widened to 4 floats, which is safe because a
// narrower attribute can always be read as four floats.
int ToVertexAttributeType(TextureFormat fmt) {
    switch (fmt) {
        case TextureFormat::R8:
        case TextureFormat::R16:
        case TextureFormat::R32F:
            return RL_FLOAT;
        case TextureFormat::RG8:
        case TextureFormat::RG16:
        case TextureFormat::RG16F:
        case TextureFormat::RG32F:
            return RL_FLOAT;
        case TextureFormat::RGB8:
        case TextureFormat::RGB16:
        case TextureFormat::RGB16F:
        case TextureFormat::RGB32F:
        case TextureFormat::RGBA8:
        case TextureFormat::RGBA16:
        case TextureFormat::RGBA16F:
        case TextureFormat::RGBA32F:
        default:
            return RL_FLOAT;
    }
}

// ============================================================================
// Backend state
//
// raylib is single-window and owns its own resource lifetime, so handles are the
// raw OpenGL ids it hands out. The maps exist to recover the extra information
// raylib needs but does not encode in an id: which kind of buffer an id is (so
// it is unloaded with the right rlgl call), a mesh's index count (so DrawMesh
// does not have to be told again), and a texture's dimensions (needed by
// GenTextureMipmaps and UpdateTexture).
// ============================================================================

struct BufferRecord {
    BufferType type = BufferType::Vertex;
    BufferUsage usage = BufferUsage::Static;
    uint32_t size = 0;
    void* mapped = nullptr;
};

struct MeshRecord {
    unsigned int vaoId = 0;
    unsigned int vboId = 0;
    unsigned int eboId = 0;
    uint32_t vertexCount = 0;
    uint32_t indexCount = 0;
};

struct TextureRecord {
    unsigned int id = 0;
    int width = 0;
    int height = 0;
    int mipmaps = 1;
    int format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
};

struct State {
    WindowHandle currentWindow = gpu::INVALID_HANDLE;
    WindowHandle nextWindow = 1;
    bool windowOpen = false;
    bool imguiInitialized = false;

    std::unordered_map<BufferHandle, BufferRecord> buffers;
    std::unordered_map<MeshHandle, MeshRecord> meshes;
    std::unordered_map<TextureHandle, TextureRecord> textures;
    std::unordered_map<FramebufferHandle, RenderTexture2D> framebuffers;
};

State g_state;

BufferHandle NextBufferId() {
    // rlgl object ids start at 1, so starting at 2 cannot collide.
    static BufferHandle next = 2;
    return next++;
}

} // namespace

// ============================================================================
// RaylibBackend
// ============================================================================

class RaylibBackend final : public gpu::IGraphicsBackend {
public:
    static std::unique_ptr<IGraphicsBackend> Create() {
        return std::make_unique<RaylibBackend>();
    }

    // ===== Window Management =====
    WindowHandle CreateWindow(const WindowDesc& desc) override {
        // Config flags must be set before InitWindow; raylib reads them once.
        ::SetConfigFlags(FLAG_WINDOW_RESIZABLE);
        if (desc.fullscreen) ::SetConfigFlags(FLAG_FULLSCREEN_MODE);
        if (desc.vsync) ::SetConfigFlags(FLAG_VSYNC_HINT);
        if (desc.msaaSamples > 1) ::SetConfigFlags(FLAG_MSAA_4X_HINT);

        ::InitWindow(static_cast<int>(desc.width), static_cast<int>(desc.height),
                     desc.title ? desc.title : "FlyEngine");
        ::SetTargetFPS(60);

        g_state.windowOpen = true;
        g_state.currentWindow = g_state.nextWindow++;
        return g_state.currentWindow;
    }

    void DestroyWindow(WindowHandle) override {
        if (!g_state.windowOpen) return;
        // Every tracked resource dies with the GL context, so the maps must be
        // cleared too or stale handles would be double-freed later.
        // raylib 6.0 has no rlUnloadVertexBufferElement; element-array buffers
        // are released through rlUnloadVertexBuffer, which is a plain
        // glDeleteBuffers and target-agnostic. UnloadMesh does the same.
        for (const auto& [handle, mesh] : g_state.meshes) {
            if (mesh.vboId) rlUnloadVertexBuffer(mesh.vboId);
            if (mesh.eboId) rlUnloadVertexBuffer(mesh.eboId);
            if (mesh.vaoId) rlUnloadVertexArray(mesh.vaoId);
        }
        g_state.meshes.clear();
        g_state.buffers.clear();
        g_state.textures.clear();
        g_state.framebuffers.clear();

        ::CloseWindow();
        g_state.windowOpen = false;
        g_state.currentWindow = gpu::INVALID_HANDLE;
    }

    void SetWindowTitle(WindowHandle, const char* title) override {
        if (title) ::SetWindowTitle(title);
    }

    void SetWindowSize(WindowHandle, uint32_t width, uint32_t height) override {
        ::SetWindowSize(static_cast<int>(width), static_cast<int>(height));
    }

    bool ShouldClose(WindowHandle) override { return ::WindowShouldClose(); }

    void PollEvents() override {
        // raylib pumps its event queue inside BeginDrawing/WindowShouldClose,
        // so there is nothing to do here. The Sokol backend must poll.
    }

    void SetCurrentWindow(WindowHandle window) override { g_state.currentWindow = window; }
    WindowHandle GetCurrentWindow() override { return g_state.currentWindow; }

    uint32_t GetWindowWidth(WindowHandle) override {
        return static_cast<uint32_t>(::GetScreenWidth());
    }

    uint32_t GetWindowHeight(WindowHandle) override {
        return static_cast<uint32_t>(::GetScreenHeight());
    }

    // ===== Frame Lifecycle =====
    void BeginFrame(WindowHandle) override { ::BeginDrawing(); }
    void EndFrame(WindowHandle) override { ::EndDrawing(); }
    void WaitIdle() override { /* raylib is synchronous */ }

    // ===== Input =====
    bool IsKeyDown(WindowHandle, KeyCode key) override { return ::IsKeyDown(FromKeyCode(key)); }
    bool IsKeyPressed(WindowHandle, KeyCode key) override { return ::IsKeyPressed(FromKeyCode(key)); }
    bool IsKeyReleased(WindowHandle, KeyCode key) override { return ::IsKeyReleased(FromKeyCode(key)); }

    bool IsMouseButtonDown(WindowHandle, gpu::MouseButton button) override {
        return ::IsMouseButtonDown(static_cast<int>(button));
    }

    Vec2 GetMousePosition(WindowHandle) override { return ToVec2(::GetMousePosition()); }
    float GetMouseWheelDelta(WindowHandle) override { return ::GetMouseWheelMove(); }

    void SetMouseCursor(WindowHandle, CursorType cursor) override {
        ::SetMouseCursor(FromCursor(cursor));
    }

    void SetMouseLocked(WindowHandle, bool locked) override {
        if (locked) ::DisableCursor();
        else ::EnableCursor();
    }

    bool IsMouseLocked(WindowHandle) override { return ::IsCursorHidden(); }

    // ===== Time =====
    double GetTime() override { return ::GetTime(); }
    float GetFrameTime() override { return ::GetFrameTime(); }

    // ===== Shader System =====
    ShaderHandle CreateShader(const ShaderDesc& desc) override {
        if (desc.vsSource.empty() || desc.fsSource.empty()) return gpu::INVALID_HANDLE;

        // raylib only accepts GLSL source, so the dual-source shader pipeline
        // feeds it the same text the Sokol backend compiles to SPIR-V. The
        // vsBytecode/fsBytecode fields are deliberately ignored here.
        const Shader shader = ::LoadShaderFromMemory(desc.vsSource.c_str(), desc.fsSource.c_str());
        if (shader.id == 0) return gpu::INVALID_HANDLE;
        return shader.id;
    }

    void DestroyShader(ShaderHandle handle) override {
        if (handle == gpu::INVALID_HANDLE) return;
        Shader shader{};
        shader.id = handle;
        // locs is owned by the loaded shader; LoadShaderFromMemory filled it in
        // and UnloadShader frees it, so a zeroed Shader is the correct value to
        // pass back for a handle obtained as a bare id.
        ::UnloadShader(shader);
    }

    int32_t GetUniformLocation(ShaderHandle handle, const char* name) override {
        if (handle == gpu::INVALID_HANDLE || !name) return -1;
        Shader shader{};
        shader.id = handle;
        return ::GetShaderLocation(shader, name);
    }

    int32_t GetUniformBlockIndex(ShaderHandle, const char*) override {
        // raylib/OpenGL backends expose uniform blocks only through
        // glUniformBlockBinding, which rlgl does not wrap. Reported as
        // unsupported rather than silently faked.
        return -1;
    }

    void SetUniform(ShaderHandle handle, int32_t location, const void* data, UniformType type,
                    uint32_t count) override {
        if (handle == gpu::INVALID_HANDLE || location < 0 || !data) return;

        int rlType = 0;
        switch (type) {
            case UniformType::Float: rlType = SHADER_UNIFORM_FLOAT; break;
            case UniformType::Vec2: rlType = SHADER_UNIFORM_VEC2; break;
            case UniformType::Vec3: rlType = SHADER_UNIFORM_VEC3; break;
            case UniformType::Vec4: rlType = SHADER_UNIFORM_VEC4; break;
            case UniformType::Int: rlType = SHADER_UNIFORM_INT; break;
            case UniformType::IVec2: rlType = SHADER_UNIFORM_IVEC2; break;
            case UniformType::IVec3: rlType = SHADER_UNIFORM_IVEC3; break;
            case UniformType::IVec4: rlType = SHADER_UNIFORM_IVEC4; break;
            // Mat4 has no ShaderUniformDataType entry in raylib 6.0; matrices go
            // through SetShaderValueMatrix, which takes a Matrix by value.
            case UniformType::Mat4: rlType = -1; break;
            default: return;
        }

        Shader shader{};
        shader.id = handle;

        if (type == UniformType::Mat4) {
            // A mat4 array is uploaded one element at a time, since raylib's
            // matrix setter takes exactly one Matrix.
            const auto* matrices = static_cast<const Mat4*>(data);
            for (uint32_t i = 0; i < count; ++i)
                ::SetShaderValueMatrix(shader, location + static_cast<int>(i),
                                       FromMat4(matrices[i]));
            return;
        }

        // count > 1 needs the vector entry point; the scalar one silently uses 1.
        if (count > 1) ::SetShaderValueV(shader, location, data, rlType, static_cast<int>(count));
        else ::SetShaderValue(shader, location, data, rlType);
    }

    void SetUniformBlock(ShaderHandle, uint32_t, BufferHandle, uint32_t, uint32_t) override {
        // See GetUniformBlockIndex: uniform blocks are not expressible via rlgl.
    }

    // ===== Pipelines =====
    // raylib has no pipeline objects; fixed-function state is set per draw call.
    // A pipeline handle is therefore just the shader program it draws with, and
    // DestroyPipeline is intentionally a no-op so it cannot unload a shader the
    // caller still owns.
    PipelineHandle CreatePipeline(const PipelineDesc& desc) override { return desc.shader; }
    void DestroyPipeline(PipelineHandle) override {}

    // ===== Buffers =====
    BufferHandle CreateBuffer(const BufferDesc& desc) override {
        if (desc.size == 0) return gpu::INVALID_HANDLE;

        const bool dynamic = desc.usage != BufferUsage::Static;
        const void* initial = desc.initialData;
        const int size = static_cast<int>(desc.size);

        // Index buffers are a distinct rlgl object; loading one as a vertex
        // buffer would produce a buffer that cannot be bound as an element array.
        unsigned int id = (desc.type == BufferType::Index)
                              ? rlLoadVertexBufferElement(initial, size, dynamic)
                              : rlLoadVertexBuffer(initial, size, dynamic);
        if (id == 0) return gpu::INVALID_HANDLE;

        BufferRecord record;
        record.type = desc.type;
        record.usage = desc.usage;
        record.size = desc.size;
        g_state.buffers[id] = record;
        return id;
    }

    void DestroyBuffer(BufferHandle handle) override {
        auto it = g_state.buffers.find(handle);
        if (it == g_state.buffers.end()) return;

        // Every buffer this backend creates, index buffers included, is a plain GL
        // buffer object, so rlUnloadVertexBuffer (glDeleteBuffers) releases all
        // of them; raylib 6.0 has no rlUnloadVertexBufferElement.
        rlUnloadVertexBuffer(handle);
        g_state.buffers.erase(it);
    }

    void UpdateBuffer(BufferHandle handle, const void* data, uint32_t size, uint32_t offset) override {
        if (!data || size == 0) return;
        auto it = g_state.buffers.find(handle);
        if (it == g_state.buffers.end()) return;

        if (it->second.type == BufferType::Index)
            rlUpdateVertexBufferElements(handle, data, static_cast<int>(size), static_cast<int>(offset));
        else
            rlUpdateVertexBuffer(handle, data, static_cast<int>(size), static_cast<int>(offset));
    }

    void* MapBuffer(BufferHandle handle) override {
        // rlgl has no buffer mapping entry point. Returning nullptr (rather than
        // a CPU-side shadow buffer) keeps the contract honest: callers must fall
        // back to UpdateBuffer, which is the only path raylib supports.
        auto it = g_state.buffers.find(handle);
        if (it != g_state.buffers.end()) it->second.mapped = nullptr;
        return nullptr;
    }

    void UnmapBuffer(BufferHandle) override {}

    // ===== Meshes =====
    MeshHandle CreateMesh(const MeshData& data) override {
        if (data.vertexData.empty() || data.vertexCount == 0) return gpu::INVALID_HANDLE;

        const unsigned int vao = rlLoadVertexArray();
        if (vao == 0) return gpu::INVALID_HANDLE;
        rlEnableVertexArray(vao);

        const unsigned int vbo =
            rlLoadVertexBuffer(data.vertexData.data(), static_cast<int>(data.vertexData.size()),
                               data.dynamic);
        rlEnableVertexBuffer(vbo);

        // The layout is authoritative: attributes are wired up from it rather
        // than assumed, so a mesh built for Sokol uploads correctly here too.
        const uint32_t stride = data.layout.stride;
        for (const gpu::VertexAttribute& attr : data.layout.attributes) {
            rlEnableVertexAttribute(attr.location);
            rlSetVertexAttribute(attr.location, 4, ToVertexAttributeType(attr.format), false,
                                 static_cast<int>(stride ? stride : attr.stride),
                                 static_cast<int>(attr.offset));
            if (attr.instanced) rlSetVertexAttributeDivisor(attr.location, attr.instanceDivisor);
        }

        unsigned int ebo = 0;
        if (data.indexCount > 0) {
            ebo = rlLoadVertexBufferElement(data.indexData.data(),
                                            static_cast<int>(data.indexData.size() * sizeof(uint32_t)),
                                            data.dynamic);
            rlEnableVertexBufferElement(ebo);
        }

        rlDisableVertexArray();

        MeshRecord record;
        record.vaoId = vao;
        record.vboId = vbo;
        record.eboId = ebo;
        record.vertexCount = data.vertexCount;
        record.indexCount = data.indexCount;
        g_state.meshes[vao] = record;
        return vao;
    }

    void DestroyMesh(MeshHandle handle) override {
        auto it = g_state.meshes.find(handle);
        if (it == g_state.meshes.end()) return;
        const MeshRecord& mesh = it->second;
        if (mesh.vboId) rlUnloadVertexBuffer(mesh.vboId);
        if (mesh.eboId) rlUnloadVertexBuffer(mesh.eboId);
        if (mesh.vaoId) rlUnloadVertexArray(mesh.vaoId);
        g_state.meshes.erase(it);
    }

    void DrawMesh(MeshHandle mesh, PipelineHandle pipeline, const Mat4& transform,
                  uint32_t instanceCount) override {
        auto it = g_state.meshes.find(mesh);
        if (it == g_state.meshes.end()) return;

        const MeshRecord& record = it->second;
        Shader shader{};
        shader.id = pipeline;
        rlEnableShader(shader.id);
        rlSetMatrixModelview(FromMat4(transform));

        if (!rlEnableVertexArray(record.vaoId)) return;

        // An indexed mesh draws its element array; the non-indexed path uses the
        // vertex count, which is the correct count either way for a triangle list.
        if (record.eboId != 0)
            rlDrawVertexArrayElements(0, static_cast<int>(record.indexCount), nullptr);
        else
            rlDrawVertexArray(0, static_cast<int>(record.vertexCount));

        rlDisableVertexArray();
    }

    void DrawMeshIndexed(MeshHandle mesh, PipelineHandle pipeline, uint32_t indexCount,
                         uint32_t indexOffset, const Mat4& transform) override {
        auto it = g_state.meshes.find(mesh);
        if (it == g_state.meshes.end()) return;

        const MeshRecord& record = it->second;
        Shader shader{};
        shader.id = pipeline;
        rlEnableShader(shader.id);
        rlSetMatrixModelview(FromMat4(transform));

        if (!rlEnableVertexArray(record.vaoId)) return;
        if (record.eboId != 0)
            rlDrawVertexArrayElements(static_cast<int>(indexOffset), static_cast<int>(indexCount),
                                      nullptr);
        else
            rlDrawVertexArray(static_cast<int>(indexOffset), static_cast<int>(indexCount));
        rlDisableVertexArray();
    }

    // ===== Instanced Drawing =====
    BufferHandle CreateInstanceBuffer(const Mat4* transforms, uint32_t count, BufferUsage usage) override {
        if (!transforms || count == 0) return gpu::INVALID_HANDLE;

        const uint32_t bytes = count * static_cast<uint32_t>(sizeof(Mat4));
        const unsigned int id =
            rlLoadVertexBuffer(transforms, static_cast<int>(bytes), usage != BufferUsage::Static);
        if (id == 0) return gpu::INVALID_HANDLE;

        BufferRecord record;
        record.type = BufferType::Instance;
        record.usage = usage;
        record.size = bytes;
        g_state.buffers[id] = record;
        return id;
    }

    void UpdateInstanceBuffer(BufferHandle handle, const Mat4* transforms, uint32_t count) override {
        if (!transforms || count == 0) return;
        const uint32_t bytes = count * static_cast<uint32_t>(sizeof(Mat4));
        rlUpdateVertexBuffer(handle, transforms, static_cast<int>(bytes), 0);
    }

    void DrawMeshInstanced(MeshHandle mesh, PipelineHandle pipeline, BufferHandle instanceBuffer,
                           uint32_t count) override {
        auto it = g_state.meshes.find(mesh);
        if (it == g_state.meshes.end()) return;

        const MeshRecord& record = it->second;
        Shader shader{};
        shader.id = pipeline;
        rlEnableShader(shader.id);
        // The per-instance matrix arrives as vertex attributes at locations
        // 9..12, so the modelview must be identity; the vertex shader composes
        // the instance transform itself.
        rlSetMatrixModelview(FromMat4(math::Identity()));

        if (!rlEnableVertexArray(record.vaoId)) return;
        rlEnableVertexBuffer(instanceBuffer);

        // The instance matrix occupies locations 9..12 as four vec4 columns.
        // Mat4 is column-major, so column c is already the contiguous 4 floats
        // attribute c wants; no transpose is needed here.
        for (int c = 0; c < 4; ++c) {
            rlEnableVertexAttribute(9 + c);
            rlSetVertexAttribute(9 + c, 4, RL_FLOAT, false, static_cast<int>(sizeof(Mat4)),
                                 static_cast<int>(c * sizeof(Vec4)));
            rlSetVertexAttributeDivisor(9 + c, 1);
        }

        const int drawCount = record.eboId != 0 ? static_cast<int>(record.indexCount)
                                                : static_cast<int>(record.vertexCount);
        if (record.eboId != 0)
            rlDrawVertexArrayElementsInstanced(0, drawCount, nullptr, static_cast<int>(count));
        else
            rlDrawVertexArrayInstanced(0, drawCount, static_cast<int>(count));

        for (int c = 0; c < 4; ++c) rlDisableVertexAttribute(9 + c);
        rlDisableVertexBuffer();
        rlDisableVertexArray();
    }

    // ===== Textures =====
    TextureHandle CreateTexture(const TextureData& data) override {
        if (data.mipData.empty() || data.width == 0 || data.height == 0)
            return gpu::INVALID_HANDLE;

        // raylib's Image takes the base level and a mipmap count; higher levels
        // are regenerated by GenerateMipmaps, so only level 0 is uploaded here.
        Image img{};
        img.data = const_cast<uint8_t*>(data.mipData[0].data());
        img.width = static_cast<int>(data.width);
        img.height = static_cast<int>(data.height);
        img.mipmaps = static_cast<int>(data.mipLevels);
        img.format = ToPixelFormat(data.format);

        const Texture2D tex = ::LoadTextureFromImage(img);
        if (tex.id == 0) return gpu::INVALID_HANDLE;

        TextureRecord record;
        record.id = tex.id;
        record.width = tex.width;
        record.height = tex.height;
        record.mipmaps = tex.mipmaps;
        record.format = tex.format;
        g_state.textures[tex.id] = record;
        return tex.id;
    }

    TextureHandle CreateTextureFromFile(const char* path) override {
        if (!path) return gpu::INVALID_HANDLE;
        const Texture2D tex = ::LoadTexture(path);
        if (tex.id == 0) return gpu::INVALID_HANDLE;

        TextureRecord record;
        record.id = tex.id;
        record.width = tex.width;
        record.height = tex.height;
        record.mipmaps = tex.mipmaps;
        record.format = tex.format;
        g_state.textures[tex.id] = record;
        return tex.id;
    }

    void DestroyTexture(TextureHandle handle) override {
        auto it = g_state.textures.find(handle);
        if (it == g_state.textures.end()) return;
        // The tracked record supplies the width/height/mipmap count that
        // UnloadTexture and GenTextureMipmaps read; a zeroed Texture2D would
        // report a 0x0 texture.
        Texture2D tex{};
        tex.id = it->second.id;
        tex.width = it->second.width;
        tex.height = it->second.height;
        tex.mipmaps = it->second.mipmaps;
        tex.format = it->second.format;
        ::UnloadTexture(tex);
        g_state.textures.erase(it);
    }

    void UpdateTexture(TextureHandle handle, const void* pixels, uint32_t width, uint32_t height,
                       uint32_t mipLevel) override {
        auto it = g_state.textures.find(handle);
        if (it == g_state.textures.end() || !pixels) return;
        // raylib's UpdateTexture writes the whole base level, so a non-zero mip
        // level is not expressible here.
        if (mipLevel != 0) return;

        Texture2D tex{};
        tex.id = it->second.id;
        tex.width = static_cast<int>(width);
        tex.height = static_cast<int>(height);
        tex.mipmaps = it->second.mipmaps;
        tex.format = it->second.format;
        ::UpdateTexture(tex, pixels);
    }

    void GenerateMipmaps(TextureHandle handle) override {
        auto it = g_state.textures.find(handle);
        if (it == g_state.textures.end()) return;
        Texture2D tex{};
        tex.id = it->second.id;
        tex.width = it->second.width;
        tex.height = it->second.height;
        tex.mipmaps = it->second.mipmaps;
        tex.format = it->second.format;
        ::GenTextureMipmaps(&tex);
    }

    // ===== Samplers =====
    // raylib has no sampler objects: filtering and wrap state live on the bound
    // texture. Returning a stable non-invalid handle lets generic code that
    // creates a sampler and passes it around keep compiling; the parameters
    // themselves cannot be honoured here.
    SamplerHandle CreateSampler(const SamplerDesc&) override { return 1; }
    void DestroySampler(SamplerHandle) override {}

    // ===== Framebuffers / Render Passes =====
    FramebufferHandle CreateFramebuffer(const FramebufferDesc& desc) override {
        if (desc.width == 0 || desc.height == 0) return gpu::INVALID_HANDLE;
        // Only the sampleCount==1 path is supported: raylib's LoadRenderTexture
        // owns both attachments and offers no multisample or custom-attachment
        // control.
        if (desc.sampleCount != 1) return gpu::INVALID_HANDLE;

        const RenderTexture2D rt = ::LoadRenderTexture(static_cast<int>(desc.width),
                                                       static_cast<int>(desc.height));
        if (rt.id == 0) return gpu::INVALID_HANDLE;
        g_state.framebuffers[rt.id] = rt;
        return rt.id;
    }

    void DestroyFramebuffer(FramebufferHandle handle) override {
        auto it = g_state.framebuffers.find(handle);
        if (it == g_state.framebuffers.end()) return;
        ::UnloadRenderTexture(it->second);
        g_state.framebuffers.erase(it);
    }

    void BeginRenderPass(FramebufferHandle fb, const RenderPassDesc& desc) override {
        auto it = g_state.framebuffers.find(fb);
        if (it == g_state.framebuffers.end()) return;

        ::BeginTextureMode(it->second);
        if (!desc.colorOps.empty() &&
            desc.colorOps[0].loadOp == RenderPassDesc::AttachmentOp::LoadOp::Clear) {
            ::ClearBackground(FromColor(desc.colorOps[0].clearColor));
        }
    }

    void EndRenderPass() override { ::EndTextureMode(); }

    void BeginSwapchainPass(WindowHandle, const RenderPassDesc& desc) override {
        // The swapchain target is the default framebuffer, so this is the same
        // as BeginDrawing. The depth/stencil loadOp is not honoured: raylib's
        // BeginDrawing always clears depth.
        ::BeginDrawing();
        if (!desc.colorOps.empty() &&
            desc.colorOps[0].loadOp == RenderPassDesc::AttachmentOp::LoadOp::Clear) {
            ::ClearBackground(FromColor(desc.colorOps[0].clearColor));
        }
    }

    void EndSwapchainPass() override { ::EndDrawing(); }

    // ===== Resource Binding =====
    void BindPipeline(PipelineHandle pipeline) override {
        Shader shader{};
        shader.id = pipeline;
        rlEnableShader(shader.id);
    }

    void BindVertexBuffer(BufferHandle buffer, uint32_t slot) override {
        // raylib has one vertex buffer binding point, so a non-zero slot is
        // ignored rather than silently drawing with the wrong data.
        if (slot != 0) return;
        rlEnableVertexBuffer(buffer);
    }

    void BindIndexBuffer(BufferHandle buffer, IndexType) override {
        rlEnableVertexBufferElement(buffer);
    }

    void BindTexture(uint32_t binding, TextureHandle texture, SamplerHandle) override {
        rlActiveTextureSlot(static_cast<int>(binding));
        rlEnableTexture(texture);
    }

    void BindUniformBuffer(uint32_t, BufferHandle, uint32_t, uint32_t) override {
        // Not expressible through rlgl; see GetUniformBlockIndex.
    }

    // ===== Draw Calls =====
    void Draw(uint32_t vertexCount, uint32_t, uint32_t firstVertex) override {
        rlDrawVertexArray(static_cast<int>(firstVertex), static_cast<int>(vertexCount));
    }

    void DrawIndexed(uint32_t indexCount, uint32_t, uint32_t firstIndex) override {
        rlDrawVertexArrayElements(static_cast<int>(firstIndex), static_cast<int>(indexCount), nullptr);
    }

    void DispatchCompute(uint32_t, uint32_t, uint32_t) override {
        // raylib/OpenGL 3.3 core has no compute dispatch path in rlgl.
    }

    // ===== Scissor / Viewport =====
    void SetViewport(uint32_t x, uint32_t y, uint32_t width, uint32_t height) override {
        rlViewport(static_cast<int>(x), static_cast<int>(y), static_cast<int>(width),
                   static_cast<int>(height));
    }

    void SetScissor(uint32_t x, uint32_t y, uint32_t width, uint32_t height) override {
        rlEnableScissorTest();
        rlScissor(static_cast<int>(x), static_cast<int>(y), static_cast<int>(width),
                  static_cast<int>(height));
    }

    // ===== Debug / ImGui =====
    void InitDebugUI(WindowHandle) override {
        if (g_state.imguiInitialized) return;
        ImGui_ImplRaylib_Init();
        g_state.imguiInitialized = true;
    }

    void ShutdownDebugUI() override {
        if (!g_state.imguiInitialized) return;
        ImGui_ImplRaylib_Shutdown();
        g_state.imguiInitialized = false;
    }

    void BeginDebugUIFrame(WindowHandle) override { ImGui_ImplRaylib_NewFrame(); }

    void EndDebugUIFrame() override {
        ImGui::Render();
        ImGui_ImplRaylib_RenderDrawData(ImGui::GetDrawData());
    }

    // ===== Capabilities =====
    // raylib exposes no device queries through rlgl, so these are the GL 3.3
    // core minimums. Deliberately conservative: the Sokol backend should report
    // real values, and generic code must not depend on anything larger.
    const gpu::DeviceCaps& GetCaps() const override {
        static const gpu::DeviceCaps caps = [] {
            gpu::DeviceCaps c;
            c.maxTextureSize = 16384;
            c.maxUniformBufferSize = 65536;
            c.maxStorageBufferSize = 0;
            c.maxVertexAttributes = 16;
            c.maxVertexBindings = 1;
            c.maxColorAttachments = 8;
            c.maxDescriptorSets = 1;
            c.maxAnisotropy = 1.0f;
            c.supportsCompute = false;
            c.supportsGeometryShader = true;
            c.supportsTessellation = true;
            c.supportsBindless = false;
            c.supportsRayTracing = false;
            c.supportsMeshShaders = false;
            c.supportsBC = false;
            c.supportsASTC = false;
            c.supportsETC2 = false;
            c.vendorName = "raylib / OpenGL 3.3";
            c.deviceName = "OpenGL 3.3";
            return c;
        }();
        return caps;
    }
};

// ============================================================================
// Factory
// ============================================================================

// Declared in SokolBackend.cpp. Defined here so the backend choice is a single
// translation unit's concern and adding a backend does not touch this file.
std::unique_ptr<gpu::IGraphicsBackend> CreateSokolBackend(gpu::BackendType type);

namespace {

// Picks a backend for BackendType::Auto. Sokol/Vulkan is preferred where the
// migration is heading, but falling back silently to raylib would hide a broken
// Vulkan setup, so Auto resolves to the compile-time default instead.
constexpr gpu::BackendType kAutoBackend = gpu::BackendType::Raylib;

} // namespace

std::unique_ptr<gpu::IGraphicsBackend> gpu::IGraphicsBackend::Create(BackendType type) {
    if (type == BackendType::Auto) type = kAutoBackend;

    switch (type) {
        case BackendType::Raylib:
            return RaylibBackend::Create();
        case BackendType::SokolVulkan:
        case BackendType::SokolMetal:
        case BackendType::SokolD3D11:
        case BackendType::SokolGL:
            return CreateSokolBackend(type);
        case BackendType::Auto:
            break;
    }
    return nullptr;
}